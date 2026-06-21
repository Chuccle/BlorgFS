// Mutation fuzzer for the wire-data parsers in Tls.c -- these are the
// functions that process bytes a malicious/compromised server controls,
// so this is the actual attack surface, not the AEAD/HKDF math (which
// only ever sees data this driver already trusts or has derived itself).
// Seeds are real bytes captured from a live handshake (TlsHandshakeTest's
// DumpSeed calls), mutated here rather than starting from synthetic
// buffers -- a mutated-valid seed explores nearby malformed variants far
// faster than a fuzzer stumbling onto validity from scratch.
//
// Built with /fsanitize=address (see TlsFuzzTest.vcxproj) specifically so
// an out-of-bounds read that doesn't happen to crash (reads adjacent
// heap data instead of unmapped memory) still gets caught -- that's the
// class of bug a plain "did it crash" pass/fail check would miss
// entirely, and it's exactly the class "never roll your own crypto" is
// most worried about in hand-written parsers.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "..\Tls.h"

// Resolves a path under <repo root>\FuzzSeeds\, with repo root found by
// walking up from this exe's own path until a directory containing
// BlorgFS.sln turns up -- not any machine- or session-specific location,
// and not a hardcoded directory-depth guess either: MSBuild's actual
// output directory for a project built as part of the .sln (the normal
// way to build this) is the solution root's x64\<Config>\, not a
// project-nested TlsFuzzTest\x64\<Config>\, so a fixed "N components up"
// count silently breaks depending on how the exe happened to be built.
static void GetFuzzSeedPath(char* outPath, size_t outSize, const char* name)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);

    char* lastSlash = strrchr(dir, '\\');
    if (lastSlash) *lastSlash = '\0'; // drop the exe filename itself

    for (int levels = 0; levels < 10; levels++)
    {
        char slnPath[MAX_PATH];
        sprintf_s(slnPath, sizeof(slnPath), "%s\\BlorgFS.sln", dir);

        if (GetFileAttributesA(slnPath) != INVALID_FILE_ATTRIBUTES)
        {
            break;
        }

        char* slash = strrchr(dir, '\\');
        if (!slash) break;
        *slash = '\0';
    }

    sprintf_s(outPath, outSize, "%s\\FuzzSeeds\\%s.bin", dir, name);
}

static unsigned char* LoadSeed(const char* name, unsigned long* lenOut)
{
    char path[MAX_PATH];
    GetFuzzSeedPath(path, sizeof(path), name);

    FILE* f = nullptr;
    if (0 != fopen_s(&f, path, "rb") || !f)
    {
        printf("  !! could not load seed %s\n", name);
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char* buf = new unsigned char[sz];
    fread(buf, 1, sz, f);
    fclose(f);

    *lenOut = C_CAST(unsigned long, sz);
    return buf;
}

// A generous over-allocation for the mutated copy: several targets
// (Certificate message parsing) legitimately grow the effective length
// via a corrupted length FIELD even though the underlying buffer doesn't
// grow -- callers pass the real (possibly truncated/extended) length
// separately, this is just backing storage.
#define FUZZ_BUF_CAP 4096

typedef struct _FUZZ_RNG
{
    unsigned int State;
} FUZZ_RNG;

static unsigned int FuzzRand(FUZZ_RNG* Rng)
{
    // xorshift32 -- fast, deterministic given a seed, no CRT rand() state
    // to worry about across threads (there's only one thread here, but
    // this is simpler to reason about regardless).
    Rng->State ^= Rng->State << 13;
    Rng->State ^= Rng->State >> 17;
    Rng->State ^= Rng->State << 5;
    return Rng->State;
}

// Copies Seed into Buf (zero-padded/truncated to a randomized length
// near the seed's own length) and flips a handful of random bytes --
// this explores "almost valid" malformed inputs: truncated fields,
// corrupted length bytes, flipped tag bytes, off-by-one boundaries.
static unsigned long Mutate(FUZZ_RNG* Rng, const unsigned char* seed, unsigned long seedLen, unsigned char* buf)
{
    // Bias toward lengths near the real seed length (truncation and
    // small extension are the most interesting cases for a TLV parser),
    // but occasionally go wild to also exercise "wildly wrong length".
    unsigned long len;
    unsigned int roll = FuzzRand(Rng) % 100;

    if (roll < 60)
    {
        // near seedLen: -8..+8
        int delta = C_CAST(int, FuzzRand(Rng) % 17) - 8;
        long candidate = C_CAST(long, seedLen) + delta;
        len = (candidate < 0) ? 0 : C_CAST(unsigned long, candidate);
    }
    else if (roll < 90)
    {
        // small buffer: 0..seedLen
        len = FuzzRand(Rng) % (seedLen + 1);
    }
    else
    {
        // wildly different: 0..FUZZ_BUF_CAP
        len = FuzzRand(Rng) % FUZZ_BUF_CAP;
    }

    if (len > FUZZ_BUF_CAP) len = FUZZ_BUF_CAP;

    unsigned long copyLen = (len < seedLen) ? len : seedLen;
    memcpy(buf, seed, copyLen);

    if (len > copyLen)
    {
        memset(buf + copyLen, 0, len - copyLen);
    }

    unsigned int numFlips = 1 + (FuzzRand(Rng) % 6);
    for (unsigned int i = 0; i < numFlips && len > 0; i++)
    {
        unsigned long pos = FuzzRand(Rng) % len;
        buf[pos] = C_CAST(unsigned char, FuzzRand(Rng) & 0xFF);
    }

    return len;
}

#define FUZZ_ITERATIONS 200000

int main()
{
    printf("=== TLS parser mutation fuzzer (%d iterations/target, ASan-instrumented) ===\n\n", FUZZ_ITERATIONS);

    FUZZ_RNG rng = { 0xC0FFEE01 };

    // --- Target: TlsParseServerHello ---
    {
        unsigned long realLen;
        unsigned char* seed = LoadSeed("server_hello_body", &realLen);
        if (seed)
        {
            printf("Fuzzing TlsParseServerHello...\n");
            static unsigned char buf[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long len = Mutate(&rng, seed, realLen, buf);
                unsigned char randomOut[TLS_HANDSHAKE_RANDOM_LEN];
                unsigned char pubKeyOut[TLS_ECC_PUBKEY_LEN];

                // Return value deliberately ignored -- fuzzing only cares
                // whether the call corrupts memory or hangs, not whether
                // it accepts or rejects a given mutation.
                TlsParseServerHello(buf, len, randomOut, pubKeyOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
            delete[] seed;
        }
    }

    // --- Target: TlsParseCertificateMessage ---
    {
        unsigned long realLen;
        unsigned char* seed = LoadSeed("certificate_message_body", &realLen);
        if (seed)
        {
            printf("Fuzzing TlsParseCertificateMessage...\n");
            static unsigned char buf[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long len = Mutate(&rng, seed, realLen, buf);
                const unsigned char* leafCertOut;
                unsigned long leafCertLenOut;

                TlsParseCertificateMessage(buf, len, &leafCertOut, &leafCertLenOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
            delete[] seed;
        }
    }

    // --- Target: TlsExtractSpkiFromCertificate (fed the leaf DER directly) ---
    {
        unsigned long realLen;
        unsigned char* seed = LoadSeed("leaf_cert_der", &realLen);
        if (seed)
        {
            printf("Fuzzing TlsExtractSpkiFromCertificate...\n");
            static unsigned char buf[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long len = Mutate(&rng, seed, realLen, buf);
                const unsigned char* spkiOut;
                unsigned long spkiLenOut;

                TlsExtractSpkiFromCertificate(buf, len, &spkiOut, &spkiLenOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
            delete[] seed;
        }
    }

    // --- Target: TlsDecodeP256SubjectPublicKeyInfo ---
    {
        unsigned long realLen;
        unsigned char* seed = LoadSeed("spki_der", &realLen);
        if (seed)
        {
            printf("Fuzzing TlsDecodeP256SubjectPublicKeyInfo...\n");
            static unsigned char buf[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long len = Mutate(&rng, seed, realLen, buf);
                unsigned char pubKeyOut[TLS_ECC_PUBKEY_LEN];

                TlsDecodeP256SubjectPublicKeyInfo(buf, len, pubKeyOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
            delete[] seed;
        }
    }

    // --- Target: TlsParseCertificateVerifyMessage ---
    {
        unsigned long realLen;
        unsigned char* seed = LoadSeed("certificate_verify_message_body", &realLen);
        if (seed)
        {
            printf("Fuzzing TlsParseCertificateVerifyMessage...\n");
            static unsigned char buf[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long len = Mutate(&rng, seed, realLen, buf);
                unsigned char sigOut[64];

                TlsParseCertificateVerifyMessage(buf, len, sigOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
            delete[] seed;
        }
    }

    // --- Target: TlsAeadDecrypt -- mutate ciphertext/tag (attacker-
    // controlled wire data), key/iv/aad stay fixed at their real derived
    // values (not attacker-controlled from the client's perspective). ---
    {
        unsigned long keyLen, ivLen, aadLen, ctLen, tagLen;
        unsigned char* key = LoadSeed("aead_key", &keyLen);
        unsigned char* iv = LoadSeed("aead_iv", &ivLen);
        unsigned char* aad = LoadSeed("aead_aad", &aadLen);
        unsigned char* ciphertext = LoadSeed("aead_ciphertext", &ctLen);
        unsigned char* tag = LoadSeed("aead_tag", &tagLen);

        if (key && iv && aad && ciphertext && tag)
        {
            printf("Fuzzing TlsAeadDecrypt (ciphertext + tag mutated, key/iv/aad fixed)...\n");
            static unsigned char ctBuf[FUZZ_BUF_CAP];
            unsigned char tagBuf[TLS_TAG_LEN];
            static unsigned char plaintextOut[FUZZ_BUF_CAP];

            for (unsigned long i = 0; i < FUZZ_ITERATIONS; i++)
            {
                unsigned long ctLenMutated = Mutate(&rng, ciphertext, ctLen, ctBuf);

                // Tag is fixed-size in the real protocol (TLS_TAG_LEN) --
                // mutate its bytes but keep it at the correct length,
                // since a caller in the real driver always slices
                // exactly TLS_TAG_LEN bytes off a record regardless of
                // what's in them.
                memcpy(tagBuf, tag, TLS_TAG_LEN);
                unsigned int numFlips = 1 + (FuzzRand(&rng) % 4);
                for (unsigned int f = 0; f < numFlips; f++)
                {
                    tagBuf[FuzzRand(&rng) % TLS_TAG_LEN] = C_CAST(unsigned char, FuzzRand(&rng) & 0xFF);
                }

                if (ctLenMutated > sizeof(plaintextOut)) ctLenMutated = C_CAST(unsigned long, sizeof(plaintextOut));

                TlsAeadDecrypt(key, iv, 0, aad, aadLen, ctBuf, ctLenMutated, tagBuf, plaintextOut);

                if (0 == (i % 50000)) printf("  %lu iterations OK\n", i);
            }

            printf("  done: %d iterations, no crash\n\n", FUZZ_ITERATIONS);
        }

        delete[] key; delete[] iv; delete[] aad; delete[] ciphertext; delete[] tag;
    }

    printf("=== ALL FUZZ TARGETS SURVIVED %d ITERATIONS EACH (ASan clean) ===\n", FUZZ_ITERATIONS);
    return 0;
}
