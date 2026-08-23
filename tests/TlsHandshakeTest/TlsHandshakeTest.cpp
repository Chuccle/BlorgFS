// Stage 3 test: drives a real TLS 1.3 handshake (ClientHello through
// Finished, plus an application-data round trip) against a live
// `openssl s_server -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256
// -groups P-256`, using plain Winsock -- not WSK. This is deliberately
// the same handshake code shape (Tls.c) that will later be driven by
// WSK in the kernel driver; only the socket I/O in this file is
// throwaway test scaffolding.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "..\..\src\Tls.h"

#pragma comment(lib, "ws2_32.lib")

// Resolves a path under <repo root>\FuzzSeeds\, with repo root found by
// walking up from this exe's own path until a directory containing
// BlorgFS.sln turns up, rather than any machine- or session-specific
// location or a hardcoded directory-depth guess -- MSBuild's actual
// output directory for a project built as part of the .sln (the normal
// way to build this) is the solution root's x64\<Config>\, not a
// project-nested TlsHandshakeTest\x64\<Config>\, so a fixed "N
// components up" count silently breaks depending on how the exe
// happened to be built.
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

// Dumps real captured handshake bytes as fuzz seeds -- mutating actual
// wire data from a live handshake is a better starting corpus than
// synthetic buffers, since it's already structurally valid and a fuzzer
// only needs to explore nearby mutations, not stumble onto validity from
// scratch.
static void DumpSeed(const char* name, const void* data, unsigned long len)
{
    char path[MAX_PATH];
    GetFuzzSeedPath(path, sizeof(path), name);

    FILE* f = nullptr;
    if (0 == fopen_s(&f, path, "wb") && f)
    {
        fwrite(data, 1, len, f);
        fclose(f);
        printf("  [SEED] wrote %s (%lu bytes)\n", name, len);
    }
}

typedef struct _TEST_STATE
{
    int Failures;
} TEST_STATE;

static TEST_STATE Test = { 0 };

static void Check(const char* label, bool ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) Test.Failures++;
}

static void CheckStatus(const char* label, NTSTATUS status)
{
    if (!NT_SUCCESS(status))
    {
        printf("  [FAIL] %s -- NTSTATUS 0x%08lX\n", label, C_CAST(unsigned long, status));
        Test.Failures++;
    }
    else
    {
        printf("  [PASS] %s\n", label);
    }
}

// --- TCP record-framing helpers (test-only; the driver's equivalent is
// WSK-based buffering in Client.c, not this) -----------------------------

struct RECV_BUFFER
{
    unsigned char data[16384];
    unsigned long len;
};

static bool FillMore(SOCKET s, RECV_BUFFER* rb)
{
    int n = recv(s, reinterpret_cast<char*>(rb->data) + rb->len, C_CAST(int, sizeof(rb->data) - rb->len), 0);
    if (n <= 0) return false;
    rb->len += C_CAST(unsigned long, n);
    return true;
}

// Reads one TLS record (5-byte header + declared-length payload) off the
// socket, buffering across recv() calls as needed and handling multiple
// records arriving in one recv() (leftover bytes are shifted down, not
// dropped).
static bool ReadRecord(SOCKET s, RECV_BUFFER* rb, unsigned char header[5], unsigned char* payload, unsigned long payloadCapacity, unsigned long* payloadLen)
{
    while (rb->len < 5)
    {
        if (!FillMore(s, rb)) return false;
    }

    unsigned long declaredLen = (C_CAST(unsigned long, rb->data[3]) << 8) | rb->data[4];

    while (rb->len < 5u + declaredLen)
    {
        if (!FillMore(s, rb)) return false;
    }

    memcpy(header, rb->data, 5);

    if (declaredLen > payloadCapacity) return false;

    memcpy(payload, rb->data + 5, declaredLen);
    *payloadLen = declaredLen;

    unsigned long consumed = 5 + declaredLen;
    memmove(rb->data, rb->data + consumed, rb->len - consumed);
    rb->len -= consumed;

    return true;
}

// TLSInnerPlaintext = content || content_type || zeros*. Strips the
// trailing zero padding and returns the real content type + content
// length.
static bool StripInnerPlaintext(const unsigned char* plaintext, unsigned long plaintextLen, unsigned char* contentTypeOut, unsigned long* contentLenOut)
{
    unsigned long i = plaintextLen;

    while (i > 0 && 0 == plaintext[i - 1]) i--;

    if (0 == i) return false;

    *contentTypeOut = plaintext[i - 1];
    *contentLenOut = i - 1;
    return true;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0); // see output up to the crash point, not lost in a buffer
    printf("=== Stage 3: live TLS 1.3 handshake vs openssl s_server ===\n\n");

    WSADATA wsaData;
    if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData))
    {
        printf("WSAStartup failed\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (INVALID_SOCKET == sock)
    {
        printf("socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(14448);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (0 != connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
    {
        printf("connect() failed: %d -- is openssl s_server running on port 14443?\n", WSAGetLastError());
        return 1;
    }

    printf("Connected to 127.0.0.1:%u\n\n", ntohs(addr.sin_port));

    // Transcript accumulates raw handshake MESSAGE bytes (header + body)
    // in order -- not record bytes -- per RFC 8446 4.4.1. static: this and
    // the other large scratch buffers below (rb, payload, flight,
    // decrypted) run once in a single-threaded harness, so there's no
    // reason to pay ~70KB of concurrent stack for them -- same reasoning
    // as why the real driver keeps its equivalents in a heap-allocated
    // TLS_HANDSHAKE_CONTEXT rather than as locals.
    static unsigned char transcript[8192];
    unsigned long transcriptLen = 0;

    // --- ClientHello ---

    unsigned char clientPrivate[TLS_ECC_COORD_LEN];
    unsigned char clientPublic[TLS_ECC_PUBKEY_LEN];
    CheckStatus("BlorgTlsEcdhGenerateKeyPair", BlorgTlsEcdhGenerateKeyPair(clientPrivate, clientPublic));

    unsigned char clientRandom[TLS_HANDSHAKE_RANDOM_LEN];
    CheckStatus("BCryptGenRandom(client random)", BCryptGenRandom(NULL, clientRandom, TLS_HANDSHAKE_RANDOM_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG));

    unsigned char clientHello[TLS_CLIENT_HELLO_MAX_LEN];
    unsigned long clientHelloLen = 0;
    CheckStatus("BlorgTlsBuildClientHello",
        BlorgTlsBuildClientHello(clientRandom, clientPublic, "localhost", 9, clientHello, sizeof(clientHello), &clientHelloLen));

    memcpy(transcript + transcriptLen, clientHello, clientHelloLen);
    transcriptLen += clientHelloLen;

    {
        unsigned char record[5 + TLS_CLIENT_HELLO_MAX_LEN];
        record[0] = 0x16; // handshake
        record[1] = 0x03;
        record[2] = 0x01; // legacy_record_version: 0x0301 for the first record only, per real-world convention
        record[3] = C_CAST(unsigned char, clientHelloLen >> 8);
        record[4] = C_CAST(unsigned char, clientHelloLen & 0xFF);
        memcpy(record + 5, clientHello, clientHelloLen);

        int sent = send(sock, reinterpret_cast<const char*>(record), C_CAST(int, 5 + clientHelloLen), 0);
        Check("send(ClientHello)", sent == C_CAST(int, 5 + clientHelloLen));
    }

    // --- ServerHello ---

    static RECV_BUFFER rb = { {0}, 0 };
    unsigned char header[5];
    static unsigned char payload[16384];
    unsigned long payloadLen = 0;

    if (!ReadRecord(sock, &rb, header, payload, sizeof(payload), &payloadLen))
    {
        printf("Failed to read ServerHello record\n");
        return 1;
    }

    Check("ServerHello record type == handshake (0x16)", 0x16 == header[0]);

    // payload is the ServerHello handshake message (header + body) directly, unencrypted.
    memcpy(transcript + transcriptLen, payload, payloadLen);
    transcriptLen += payloadLen;

    unsigned long shBodyLen = (C_CAST(unsigned long, payload[1]) << 16) | (C_CAST(unsigned long, payload[2]) << 8) | payload[3];
    Check("ServerHello msg type == 0x02", 0x02 == payload[0]);
    Check("ServerHello length fits in record", 4 + shBodyLen == payloadLen);

    DumpSeed("server_hello_body", payload + 4, shBodyLen);

    unsigned char serverRandom[TLS_HANDSHAKE_RANDOM_LEN];
    unsigned char serverPublic[TLS_ECC_PUBKEY_LEN];
    CheckStatus("BlorgTlsParseServerHello", BlorgTlsParseServerHello(payload + 4, shBodyLen, serverRandom, serverPublic));

    // --- Key schedule through the handshake secret ---

    unsigned char zero32[32] = { 0 };
    unsigned char sharedSecret[TLS_ECC_COORD_LEN];
    CheckStatus("BlorgTlsEcdhComputeSharedSecret", BlorgTlsEcdhComputeSharedSecret(clientPrivate, clientPublic, serverPublic, sharedSecret));

    unsigned char transcriptHashChSh[TLS_HASH_LEN];
    CheckStatus("BlorgTlsSha256(ClientHello||ServerHello)", BlorgTlsSha256(transcript, transcriptLen, transcriptHashChSh));

    unsigned char emptyHash[TLS_HASH_LEN];
    CheckStatus("BlorgTlsSha256(empty)", BlorgTlsSha256(NULL, 0, emptyHash));

    unsigned char earlySecret[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExtract(early)", BlorgTlsHkdfExtract(zero32, 32, zero32, 32, earlySecret));

    unsigned char derivedForHandshake[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(derived)", BlorgTlsHkdfExpandLabel(earlySecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForHandshake));

    unsigned char handshakeSecret[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExtract(handshake)", BlorgTlsHkdfExtract(derivedForHandshake, TLS_HASH_LEN, sharedSecret, TLS_ECC_COORD_LEN, handshakeSecret));

    unsigned char clientHsTraffic[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(c hs traffic)", BlorgTlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "c hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, clientHsTraffic));

    unsigned char serverHsTraffic[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(s hs traffic)", BlorgTlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "s hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, serverHsTraffic));

    unsigned char serverHsKey[TLS_KEY_LEN];
    unsigned char serverHsIv[TLS_IV_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(server key)", BlorgTlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, serverHsKey));
    CheckStatus("BlorgTlsHkdfExpandLabel(server iv)", BlorgTlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, serverHsIv));

    unsigned char clientHsKey[TLS_KEY_LEN];
    unsigned char clientHsIv[TLS_IV_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(client key)", BlorgTlsHkdfExpandLabel(clientHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, clientHsKey));
    CheckStatus("BlorgTlsHkdfExpandLabel(client iv)", BlorgTlsHkdfExpandLabel(clientHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, clientHsIv));

    // --- Encrypted flight: EncryptedExtensions, Certificate, CertificateVerify, Finished ---

    static unsigned char flight[16384];
    unsigned long flightLen = 0;
    ULONGLONG serverSeq = 0;

    unsigned char serverLongTermKey[TLS_ECC_PUBKEY_LEN] = { 0 };
    bool sawCertificate = false;
    bool sawCertVerify = false;
    bool sawFinished = false;
    unsigned char transcriptHashThroughCert[TLS_HASH_LEN] = { 0 };
    unsigned char transcriptHashThroughCertVerify[TLS_HASH_LEN] = { 0 };

    while (!sawFinished)
    {
        if (!ReadRecord(sock, &rb, header, payload, sizeof(payload), &payloadLen))
        {
            printf("Failed to read encrypted handshake record (seq=%llu)\n", serverSeq);
            return 1;
        }

        if (0x15 == header[0]) // alert
        {
            printf("  [FAIL] server sent an alert instead of a handshake record: level=%u description=%u\n",
                payloadLen > 0 ? payload[0] : 0, payloadLen > 1 ? payload[1] : 0);
            return 1;
        }

        if (0x14 == header[0]) // change_cipher_spec
        {
            // RFC 8446 Appendix D.4: a vestigial 1-byte record some
            // servers still send for middlebox compatibility -- no
            // semantic meaning in TLS 1.3, not part of the encrypted
            // flight, not counted in the AEAD sequence number.
            printf("  [SKIP] change_cipher_spec (middlebox-compat, ignored)\n");
            continue;
        }

        Check("encrypted record type == application_data (0x17)", 0x17 == header[0]);

        if (payloadLen < TLS_TAG_LEN)
        {
            printf("  [FAIL] record payload (%lu bytes) shorter than the AEAD tag -- not a valid encrypted record\n", payloadLen);
            return 1;
        }

        // Ciphertext and tag are passed separately to BlorgTlsAeadDecrypt: the
        // tag is the trailing TLS_TAG_LEN bytes of the record payload.
        static unsigned char decrypted[16384];
        unsigned long ciphertextLen = payloadLen - TLS_TAG_LEN;
        CheckStatus("BlorgTlsAeadDecrypt(handshake record)",
            BlorgTlsAeadDecrypt(serverHsKey, serverHsIv, serverSeq, header, 5, payload, ciphertextLen, payload + ciphertextLen, decrypted));

        if (0 == serverSeq) // only need one real record as an AEAD fuzz seed
        {
            DumpSeed("aead_key", serverHsKey, TLS_KEY_LEN);
            DumpSeed("aead_iv", serverHsIv, TLS_IV_LEN);
            DumpSeed("aead_aad", header, 5);
            DumpSeed("aead_ciphertext", payload, ciphertextLen);
            DumpSeed("aead_tag", payload + ciphertextLen, TLS_TAG_LEN);
        }

        serverSeq++;

        unsigned char contentType;
        unsigned long contentLen;
        if (!StripInnerPlaintext(decrypted, ciphertextLen, &contentType, &contentLen))
        {
            printf("Malformed TLSInnerPlaintext (all-zero)\n");
            return 1;
        }

        Check("inner content type == handshake (0x16)", 0x16 == contentType);

        memcpy(flight + flightLen, decrypted, contentLen);
        flightLen += contentLen;

        // Drain as many complete handshake messages as are now available
        // from the front of the accumulated flight buffer.
        unsigned long msgOffset = 0;

        while (msgOffset + 4 <= flightLen)
        {
            unsigned char msgType = flight[msgOffset];
            unsigned long msgBodyLen = (C_CAST(unsigned long, flight[msgOffset + 1]) << 16) |
                                        (C_CAST(unsigned long, flight[msgOffset + 2]) << 8) |
                                        flight[msgOffset + 3];
            unsigned long msgTotalLen = 4 + msgBodyLen;

            if (msgOffset + msgTotalLen > flightLen)
            {
                break; // incomplete message, need another record
            }

            const unsigned char* msgBody = flight + msgOffset + 4;

            // Append this message's raw bytes to the transcript BEFORE
            // acting on it -- every transcript-hash checkpoint below is
            // "everything up to and including the message just parsed".
            memcpy(transcript + transcriptLen, flight + msgOffset, msgTotalLen);
            transcriptLen += msgTotalLen;

            if (0x08 == msgType) // EncryptedExtensions
            {
                // Nothing this driver needs from it.
            }
            else if (0x0B == msgType) // Certificate
            {
                DumpSeed("certificate_message_body", msgBody, msgBodyLen);

                const unsigned char* leafCert;
                unsigned long leafCertLen;
                CheckStatus("BlorgTlsParseCertificateMessage", BlorgTlsParseCertificateMessage(msgBody, msgBodyLen, &leafCert, &leafCertLen));

                DumpSeed("leaf_cert_der", leafCert, leafCertLen);

                const unsigned char* spki;
                unsigned long spkiLen;
                CheckStatus("BlorgTlsExtractSpkiFromCertificate", BlorgTlsExtractSpkiFromCertificate(leafCert, leafCertLen, &spki, &spkiLen));
                Check("extracted SPKI length == TLS_SPKI_DER_LEN", TLS_SPKI_DER_LEN == spkiLen);

                DumpSeed("spki_der", spki, spkiLen);

                CheckStatus("BlorgTlsDecodeP256SubjectPublicKeyInfo", BlorgTlsDecodeP256SubjectPublicKeyInfo(spki, spkiLen, serverLongTermKey));

                CheckStatus("BlorgTlsSha256(transcript through Certificate)", BlorgTlsSha256(transcript, transcriptLen, transcriptHashThroughCert));
                sawCertificate = true;
            }
            else if (0x0F == msgType) // CertificateVerify
            {
                Check("Certificate seen before CertificateVerify", sawCertificate);

                DumpSeed("certificate_verify_message_body", msgBody, msgBodyLen);

                unsigned char rawSignature[64];
                CheckStatus("BlorgTlsParseCertificateVerifyMessage", BlorgTlsParseCertificateVerifyMessage(msgBody, msgBodyLen, rawSignature));

                unsigned char verifyContent[TLS_CERT_VERIFY_CONTENT_LEN];
                CheckStatus("BlorgTlsBuildServerCertVerifyContent", BlorgTlsBuildServerCertVerifyContent(transcriptHashThroughCert, verifyContent));

                unsigned char verifyDigest[TLS_HASH_LEN];
                CheckStatus("BlorgTlsSha256(CertVerify content)", BlorgTlsSha256(verifyContent, TLS_CERT_VERIFY_CONTENT_LEN, verifyDigest));

                CheckStatus("BlorgTlsEcdsaVerify(server CertificateVerify)", BlorgTlsEcdsaVerify(serverLongTermKey, verifyDigest, TLS_HASH_LEN, rawSignature));

                CheckStatus("BlorgTlsSha256(transcript through CertificateVerify)", BlorgTlsSha256(transcript, transcriptLen, transcriptHashThroughCertVerify));
                sawCertVerify = true;
            }
            else if (0x14 == msgType) // Finished
            {
                Check("CertificateVerify seen before Finished", sawCertVerify);
                Check("Finished body length == 32", 32 == msgBodyLen);

                unsigned char serverFinishedKey[TLS_HASH_LEN];
                CheckStatus("BlorgTlsHkdfExpandLabel(server finished key)", BlorgTlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, serverFinishedKey));

                unsigned char expectedFinishedMac[TLS_HASH_LEN];
                CheckStatus("BlorgTlsHmacSha256(expected server finished)", BlorgTlsHmacSha256(serverFinishedKey, TLS_HASH_LEN, transcriptHashThroughCertVerify, TLS_HASH_LEN, expectedFinishedMac));

                Check("server Finished MAC matches", BlorgTlsConstantTimeEqual(expectedFinishedMac, msgBody, TLS_HASH_LEN));

                sawFinished = true;
            }
            else
            {
                printf("  [FAIL] unexpected handshake message type 0x%02x\n", msgType);
                Test.Failures++;
            }

            msgOffset += msgTotalLen;
        }

        // Shift any leftover partial message to the front for next round.
        memmove(flight, flight + msgOffset, flightLen - msgOffset);
        flightLen -= msgOffset;
    }

    // --- Master secret + application traffic secrets (not exercised further here,
    // but derived to confirm the schedule completes correctly end to end) ---

    unsigned char derivedForMaster[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(derived, master)", BlorgTlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForMaster));

    unsigned char masterSecret[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExtract(master)", BlorgTlsHkdfExtract(derivedForMaster, TLS_HASH_LEN, zero32, 32, masterSecret));

    unsigned char transcriptHashThroughServerFinished[TLS_HASH_LEN];
    CheckStatus("BlorgTlsSha256(transcript through server Finished)", BlorgTlsSha256(transcript, transcriptLen, transcriptHashThroughServerFinished));

    unsigned char clientAppTraffic[TLS_HASH_LEN];
    unsigned char serverAppTraffic[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(c ap traffic)", BlorgTlsHkdfExpandLabel(masterSecret, TLS_HASH_LEN, "c ap traffic", transcriptHashThroughServerFinished, TLS_HASH_LEN, TLS_HASH_LEN, clientAppTraffic));
    CheckStatus("BlorgTlsHkdfExpandLabel(s ap traffic)", BlorgTlsHkdfExpandLabel(masterSecret, TLS_HASH_LEN, "s ap traffic", transcriptHashThroughServerFinished, TLS_HASH_LEN, TLS_HASH_LEN, serverAppTraffic));

    // --- Client Finished: send it, encrypted under the client handshake traffic key ---

    unsigned char clientFinishedKey[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHkdfExpandLabel(client finished key)", BlorgTlsHkdfExpandLabel(clientHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, clientFinishedKey));

    unsigned char clientFinishedMac[TLS_HASH_LEN];
    CheckStatus("BlorgTlsHmacSha256(client finished)", BlorgTlsHmacSha256(clientFinishedKey, TLS_HASH_LEN, transcriptHashThroughServerFinished, TLS_HASH_LEN, clientFinishedMac));

    {
        unsigned char finishedMsg[4 + TLS_HASH_LEN];
        finishedMsg[0] = 0x14;
        finishedMsg[1] = 0x00;
        finishedMsg[2] = 0x00;
        finishedMsg[3] = TLS_HASH_LEN;
        memcpy(finishedMsg + 4, clientFinishedMac, TLS_HASH_LEN);

        unsigned char plaintext[4 + TLS_HASH_LEN + 1];
        memcpy(plaintext, finishedMsg, sizeof(finishedMsg));
        plaintext[sizeof(finishedMsg)] = 0x16; // inner content type: handshake

        unsigned char aad[5] = { 0x17, 0x03, 0x03, 0x00, 0x00 };
        unsigned long recordLen = C_CAST(unsigned long, sizeof(plaintext)) + TLS_TAG_LEN;
        aad[3] = C_CAST(unsigned char, recordLen >> 8);
        aad[4] = C_CAST(unsigned char, recordLen & 0xFF);

        unsigned char ciphertext[sizeof(plaintext)];
        unsigned char tag[TLS_TAG_LEN];
        CheckStatus("BlorgTlsAeadEncrypt(client Finished)", BlorgTlsAeadEncrypt(clientHsKey, clientHsIv, 0, aad, 5, plaintext, C_CAST(unsigned long, sizeof(plaintext)), ciphertext, tag));

        unsigned char record[5 + sizeof(plaintext) + TLS_TAG_LEN];
        memcpy(record, aad, 5);
        memcpy(record + 5, ciphertext, sizeof(ciphertext));
        memcpy(record + 5 + sizeof(ciphertext), tag, TLS_TAG_LEN);

        int sent = send(sock, reinterpret_cast<const char*>(record), C_CAST(int, sizeof(record)), 0);
        Check("send(client Finished)", sent == C_CAST(int, sizeof(record)));
    }

    printf("\n=== %s (%d failure%s) ===\n",
        0 == Test.Failures ? "ALL TESTS PASSED" : "TESTS FAILED",
        Test.Failures, 1 == Test.Failures ? "" : "s");

    closesocket(sock);
    WSACleanup();

    return 0 == Test.Failures ? 0 : 1;
}
