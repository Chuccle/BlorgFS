// Stage 1 usermode test harness: validates Tls.c's HKDF key schedule and
// AES-128-GCM record layer against RFC 8448 section 3's published test
// vectors before any of this code is wired into the kernel driver. See
// Rfc8448Vectors.h for where the numbers come from (extracted
// programmatically from the RFC text, not hand-transcribed) and Tls.h for
// why this file links the same Tls.c the driver will eventually use.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "..\Tls.h"
#include "Rfc8448Vectors.h"

typedef struct _TEST_STATE
{
    int Failures;
} TEST_STATE;

static TEST_STATE Test = { 0 };

static bool CompareBytes(const char* label, const unsigned char* actual, const unsigned char* expected, unsigned long len)
{
    if (memcmp(actual, expected, len) == 0)
    {
        printf("  [PASS] %s (%lu bytes)\n", label, len);
        return true;
    }

    printf("  [FAIL] %s (%lu bytes) -- mismatch\n", label, len);
    printf("    actual:   ");
    for (unsigned long i = 0; i < len; i++) printf("%02x", actual[i]);
    printf("\n    expected: ");
    for (unsigned long i = 0; i < len; i++) printf("%02x", expected[i]);
    printf("\n");
    Test.Failures++;
    return false;
}

static void CheckStatus(const char* label, NTSTATUS status)
{
    if (!NT_SUCCESS(status))
    {
        printf("  [FAIL] %s -- NTSTATUS 0x%08lX\n", label, C_CAST(unsigned long, status));
        Test.Failures++;
    }
}

int main()
{
    printf("=== Stage 1: HKDF key schedule + AES-128-GCM vs RFC 8448 ===\n\n");

    unsigned char zero32[32] = { 0 };

    // 1. empty_hash = SHA-256("")
    unsigned char emptyHash[TLS_HASH_LEN];
    CheckStatus("TlsSha256(empty)", TlsSha256(NULL, 0, emptyHash));
    CompareBytes("empty_hash", emptyHash, Rfc8448.EmptyHash, TLS_HASH_LEN);

    // 1b. Sanity check on TlsSha256 over real (non-empty) input, independent
    // of the key schedule: transcript_hash_ch_sh should equal
    // SHA-256(ClientHello || ServerHello) directly.
    {
        unsigned char buf[196 + 90];
        memcpy(buf, Rfc8448.ClientHello196, sizeof(Rfc8448.ClientHello196));
        memcpy(buf + sizeof(Rfc8448.ClientHello196), Rfc8448.ServerHello90, sizeof(Rfc8448.ServerHello90));
        unsigned char hash[TLS_HASH_LEN];
        CheckStatus("TlsSha256(ClientHello||ServerHello)", TlsSha256(buf, sizeof(buf), hash));
        CompareBytes("transcript_hash_ch_sh (recomputed)", hash, Rfc8448.TranscriptHashChSh, TLS_HASH_LEN);
    }

    // 2. early_secret = HKDF-Extract(salt=0, ikm=0)
    unsigned char earlySecret[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExtract(early)", TlsHkdfExtract(zero32, 32, zero32, 32, earlySecret));
    CompareBytes("early_secret", earlySecret, Rfc8448.EarlySecret, TLS_HASH_LEN);

    // 3. derived_for_handshake = HKDF-Expand-Label(early_secret, "derived", empty_hash, 32)
    unsigned char derivedForHandshake[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExpandLabel(derived, from early)",
        TlsHkdfExpandLabel(earlySecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForHandshake));
    CompareBytes("derived_for_handshake", derivedForHandshake, Rfc8448.DerivedForHandshake, TLS_HASH_LEN);

    // 4. handshake_secret = HKDF-Extract(salt=derived_for_handshake, ikm=shared_secret)
    unsigned char handshakeSecret[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExtract(handshake)",
        TlsHkdfExtract(derivedForHandshake, TLS_HASH_LEN, Rfc8448.SharedSecretIkm, TLS_HASH_LEN, handshakeSecret));
    CompareBytes("handshake_secret", handshakeSecret, Rfc8448.HandshakeSecret, TLS_HASH_LEN);

    // 5. client_handshake_traffic_secret = HKDF-Expand-Label(handshake_secret, "c hs traffic", transcript_hash, 32)
    unsigned char clientHsTraffic[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExpandLabel(c hs traffic)",
        TlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "c hs traffic", Rfc8448.TranscriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, clientHsTraffic));
    CompareBytes("client_handshake_traffic_secret", clientHsTraffic, Rfc8448.ClientHsTrafficSecret, TLS_HASH_LEN);

    // 6. server_handshake_traffic_secret = HKDF-Expand-Label(handshake_secret, "s hs traffic", transcript_hash, 32)
    unsigned char serverHsTraffic[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExpandLabel(s hs traffic)",
        TlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "s hs traffic", Rfc8448.TranscriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, serverHsTraffic));
    CompareBytes("server_handshake_traffic_secret", serverHsTraffic, Rfc8448.ServerHsTrafficSecret, TLS_HASH_LEN);

    // 7. derived_for_master = HKDF-Expand-Label(handshake_secret, "derived", empty_hash, 32)
    unsigned char derivedForMaster[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExpandLabel(derived, from handshake)",
        TlsHkdfExpandLabel(handshakeSecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForMaster));
    CompareBytes("derived_for_master", derivedForMaster, Rfc8448.DerivedForMaster, TLS_HASH_LEN);

    // 8. master_secret = HKDF-Extract(salt=derived_for_master, ikm=0)
    unsigned char masterSecret[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExtract(master)",
        TlsHkdfExtract(derivedForMaster, TLS_HASH_LEN, zero32, 32, masterSecret));
    CompareBytes("master_secret", masterSecret, Rfc8448.MasterSecret, TLS_HASH_LEN);

    // 9/10. server hs write key/iv = HKDF-Expand-Label(server_handshake_traffic_secret, "key"/"iv", "", 16/12)
    unsigned char serverKey[TLS_KEY_LEN];
    CheckStatus("TlsHkdfExpandLabel(key)",
        TlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, serverKey));
    CompareBytes("server_hs_write_key", serverKey, Rfc8448.ServerHsWriteKey, TLS_KEY_LEN);

    unsigned char serverIv[TLS_IV_LEN];
    CheckStatus("TlsHkdfExpandLabel(iv)",
        TlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, serverIv));
    CompareBytes("server_hs_write_iv", serverIv, Rfc8448.ServerHsWriteIv, TLS_IV_LEN);

    // 11. finished_key = HKDF-Expand-Label(server_handshake_traffic_secret, "finished", "", 32)
    unsigned char finishedKey[TLS_HASH_LEN];
    CheckStatus("TlsHkdfExpandLabel(finished)",
        TlsHkdfExpandLabel(serverHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, finishedKey));
    CompareBytes("finished_key", finishedKey, Rfc8448.FinishedKey, TLS_HASH_LEN);

    // 12. finished MAC = HMAC-SHA256(finished_key, transcript_hash_through_certverify).
    // NOT the same transcript hash as the traffic secrets above -- RFC 8446
    // 4.4.4 covers ClientHello..CertificateVerify for the server's Finished,
    // one message later than ClientHello..ServerHello. RFC 8448 doesn't
    // print this intermediate hash directly, so it's recomputed here from
    // the raw handshake messages: Payload657 is
    // EncryptedExtensions(40)+Certificate(445)+CertificateVerify(136)+Finished(36),
    // so the first 621 bytes are everything through CertificateVerify.
    unsigned char transcriptThroughCertVerify[TLS_HASH_LEN];
    {
        unsigned char buf[196 + 90 + 621];
        memcpy(buf, Rfc8448.ClientHello196, sizeof(Rfc8448.ClientHello196));
        memcpy(buf + sizeof(Rfc8448.ClientHello196), Rfc8448.ServerHello90, sizeof(Rfc8448.ServerHello90));
        memcpy(buf + sizeof(Rfc8448.ClientHello196) + sizeof(Rfc8448.ServerHello90), Rfc8448.Payload657, 621);
        CheckStatus("TlsSha256(transcript through CertificateVerify)",
            TlsSha256(buf, sizeof(buf), transcriptThroughCertVerify));
    }

    unsigned char finishedMac[TLS_HASH_LEN];
    CheckStatus("TlsHmacSha256(finished)",
        TlsHmacSha256(finishedKey, TLS_HASH_LEN, transcriptThroughCertVerify, TLS_HASH_LEN, finishedMac));
    CompareBytes("finished_mac", finishedMac, Rfc8448.FinishedMac, TLS_HASH_LEN);

    // 13. AES-128-GCM encrypt of the server's first handshake record (seq=0):
    // plaintext = payload (657 octets) + TLSInnerPlaintext content type 0x16
    // (handshake). AAD = the 5-byte TLSCiphertext header. Ciphertext+tag
    // must equal CompleteRecord679[5..] (674 bytes: 658 ciphertext + 16 tag).
    unsigned char plaintext[658];
    memcpy(plaintext, Rfc8448.Payload657, sizeof(Rfc8448.Payload657));
    plaintext[657] = 0x16; // handshake content type, appended before encryption per RFC 8446 5.2

    unsigned char aad[5];
    memcpy(aad, Rfc8448.CompleteRecord679, 5); // 17 03 03 02 a2

    unsigned char ciphertext[658];
    unsigned char tag[TLS_TAG_LEN];
    CheckStatus("TlsAeadEncrypt(server record 0)",
        TlsAeadEncrypt(Rfc8448.ServerHsWriteKey, Rfc8448.ServerHsWriteIv, 0, aad, 5, plaintext, 658, ciphertext, tag));
    CompareBytes("encrypted record ciphertext", ciphertext, Rfc8448.CompleteRecord679 + 5, 658);
    CompareBytes("encrypted record tag", tag, Rfc8448.CompleteRecord679 + 5 + 658, TLS_TAG_LEN);

    // 14. Round-trip: decrypt what we just encrypted (using the RFC's own
    // ciphertext/tag straight from CompleteRecord679, not our own output --
    // this is the actual "does decrypt agree with a real TLS stack" check).
    unsigned char decrypted[658];
    CheckStatus("TlsAeadDecrypt(server record 0)",
        TlsAeadDecrypt(Rfc8448.ServerHsWriteKey, Rfc8448.ServerHsWriteIv, 0, aad, 5,
            Rfc8448.CompleteRecord679 + 5, 658, Rfc8448.CompleteRecord679 + 5 + 658, decrypted));
    CompareBytes("decrypted plaintext", decrypted, plaintext, 658);

    // 15. Negative test: corrupt one tag byte, confirm decrypt fails closed
    // rather than silently returning unauthenticated plaintext.
    unsigned char badTag[TLS_TAG_LEN];
    memcpy(badTag, Rfc8448.CompleteRecord679 + 5 + 658, TLS_TAG_LEN);
    badTag[0] ^= 0xFF;
    NTSTATUS tamperStatus = TlsAeadDecrypt(Rfc8448.ServerHsWriteKey, Rfc8448.ServerHsWriteIv, 0, aad, 5,
        Rfc8448.CompleteRecord679 + 5, 658, badTag, decrypted);
    if (NT_SUCCESS(tamperStatus))
    {
        printf("  [FAIL] tampered-tag decrypt should have failed, returned success\n");
        Test.Failures++;
    }
    else
    {
        printf("  [PASS] tampered-tag decrypt correctly rejected (0x%08lX)\n", C_CAST(unsigned long, tamperStatus));
    }

    printf("\n=== Stage 2: ECDH P-256 (real vector) + ECDSA/DER (self-consistency) ===\n\n");

    // 16. ECDH shared secret vs a real external vector: RFC 8448 5's
    // HelloRetryRequest example uses real P-256 key pairs (not this
    // driver's usual x25519-example source, since section 3 doesn't use
    // P-256 at all). Both directions must agree with each other AND with
    // the RFC's own IKM value -- this also confirms the CNG
    // BCRYPT_KDF_RAW_SECRET byte-reversal in TlsEcdhComputeSharedSecret
    // is the right way round, rather than an assumption.
    unsigned char sharedFromClient[TLS_ECC_COORD_LEN];
    CheckStatus("TlsEcdhComputeSharedSecret(client side)",
        TlsEcdhComputeSharedSecret(Rfc8448.P256ClientPrivate, Rfc8448.P256ClientPublic, Rfc8448.P256ServerPublic, sharedFromClient));
    CompareBytes("ECDH shared secret (computed by client)", sharedFromClient, Rfc8448.P256SharedSecretIkm, TLS_ECC_COORD_LEN);

    unsigned char sharedFromServer[TLS_ECC_COORD_LEN];
    CheckStatus("TlsEcdhComputeSharedSecret(server side)",
        TlsEcdhComputeSharedSecret(Rfc8448.P256ServerPrivate, Rfc8448.P256ServerPublic, Rfc8448.P256ClientPublic, sharedFromServer));
    CompareBytes("ECDH shared secret (computed by server)", sharedFromServer, Rfc8448.P256SharedSecretIkm, TLS_ECC_COORD_LEN);

    // 17. ECDSA P-256 verify + the fixed-shape DER SPKI encode/decode,
    // self-consistency only: there's no RFC 8448 ECDSA P-256 vector to
    // check against (its certificates are all RSA), and hand-typing a
    // NIST CAVP vector from memory would reintroduce exactly the
    // transcription-risk problem this session has been avoiding
    // throughout. So this generates a real key pair and signature via
    // BCrypt directly (not through Tls.c -- the driver only ever
    // verifies server signatures, it never signs, so TlsEcdsaSign doesn't
    // exist and shouldn't), round-trips the public key through
    // TlsEncodeP256SubjectPublicKeyInfo/TlsDecodeP256SubjectPublicKeyInfo,
    // and confirms TlsEcdsaVerify accepts the real signature and rejects
    // a tampered one. This validates the encode/decode/verify plumbing
    // precisely, not "does this match an externally-issued certificate"
    // -- Stage 3's live openssl s_server test is the first time this
    // code parses a real external certificate.
    {
        BCRYPT_ALG_HANDLE ecdsaAlg = NULL;
        BCRYPT_KEY_HANDLE ecdsaKey = NULL;
        NTSTATUS status;

        status = BCryptOpenAlgorithmProvider(&ecdsaAlg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
        CheckStatus("BCryptOpenAlgorithmProvider(ECDSA P256)", status);

        status = BCryptGenerateKeyPair(ecdsaAlg, &ecdsaKey, TLS_ECC_COORD_LEN * 8, 0);
        CheckStatus("BCryptGenerateKeyPair(ECDSA)", status);

        status = BCryptFinalizeKeyPair(ecdsaKey, 0);
        CheckStatus("BCryptFinalizeKeyPair(ECDSA)", status);

        UCHAR pubBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN];
        ULONG pubBlobLen = 0;
        status = BCryptExportKey(ecdsaKey, NULL, BCRYPT_ECCPUBLIC_BLOB, pubBlob, sizeof(pubBlob), &pubBlobLen, 0);
        CheckStatus("BCryptExportKey(ECDSA public)", status);

        unsigned char publicKey[TLS_ECC_PUBKEY_LEN];
        publicKey[0] = 0x04;
        memcpy(publicKey + 1, pubBlob + sizeof(BCRYPT_ECCKEY_BLOB), TLS_ECC_COORD_LEN);
        memcpy(publicKey + 1 + TLS_ECC_COORD_LEN, pubBlob + sizeof(BCRYPT_ECCKEY_BLOB) + TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);

        // Sign an arbitrary (test) SHA-256 hash -- stands in for a real
        // CertificateVerify's transcript-hash-derived signature input,
        // which is a Stage 3 wire-format concern, not a primitive one.
        unsigned char testHash[TLS_HASH_LEN];
        CheckStatus("TlsSha256(test message)", TlsSha256(reinterpret_cast<const unsigned char*>("stage 2 test"), 12, testHash));

        unsigned char signature[64];
        ULONG sigLen = 0;
        status = BCryptSignHash(ecdsaKey, NULL, testHash, TLS_HASH_LEN, signature, sizeof(signature), &sigLen, 0);
        CheckStatus("BCryptSignHash", status);

        // DER encode -> decode round trip.
        unsigned char der[TLS_SPKI_DER_LEN];
        CheckStatus("TlsEncodeP256SubjectPublicKeyInfo", TlsEncodeP256SubjectPublicKeyInfo(publicKey, der));

        unsigned char decodedPublicKey[TLS_ECC_PUBKEY_LEN];
        CheckStatus("TlsDecodeP256SubjectPublicKeyInfo", TlsDecodeP256SubjectPublicKeyInfo(der, TLS_SPKI_DER_LEN, decodedPublicKey));
        CompareBytes("SPKI round-trip public key", decodedPublicKey, publicKey, TLS_ECC_PUBKEY_LEN);

        // Verify using the round-tripped (decoded) key, not the original --
        // this is the actual "did the DER parser extract usable key
        // material" check, not just a byte-equality assertion.
        CheckStatus("TlsEcdsaVerify(real signature)", TlsEcdsaVerify(decodedPublicKey, testHash, TLS_HASH_LEN, signature));

        unsigned char tamperedSig[64];
        memcpy(tamperedSig, signature, 64);
        tamperedSig[0] ^= 0xFF;
        NTSTATUS verifyTamperedStatus = TlsEcdsaVerify(decodedPublicKey, testHash, TLS_HASH_LEN, tamperedSig);
        if (NT_SUCCESS(verifyTamperedStatus))
        {
            printf("  [FAIL] tampered-signature verify should have failed, returned success\n");
            Test.Failures++;
        }
        else
        {
            printf("  [PASS] tampered-signature verify correctly rejected (0x%08lX)\n", C_CAST(unsigned long, verifyTamperedStatus));
        }

        // Reject a DER blob with a flipped OID byte too (wrong key type /
        // unrecognised algorithm) -- the decoder must fail closed, not
        // partially parse.
        unsigned char corruptDer[TLS_SPKI_DER_LEN];
        memcpy(corruptDer, der, TLS_SPKI_DER_LEN);
        corruptDer[6] ^= 0xFF; // inside the id-ecPublicKey OID bytes
        NTSTATUS decodeCorruptStatus = TlsDecodeP256SubjectPublicKeyInfo(corruptDer, TLS_SPKI_DER_LEN, decodedPublicKey);
        if (NT_SUCCESS(decodeCorruptStatus))
        {
            printf("  [FAIL] corrupted-OID SPKI decode should have failed, returned success\n");
            Test.Failures++;
        }
        else
        {
            printf("  [PASS] corrupted-OID SPKI decode correctly rejected (0x%08lX)\n", C_CAST(unsigned long, decodeCorruptStatus));
        }

        if (ecdsaKey) BCryptDestroyKey(ecdsaKey);
        if (ecdsaAlg) BCryptCloseAlgorithmProvider(ecdsaAlg, 0);
    }

    printf("\n=== %s (%d failure%s) ===\n",
        Test.Failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
        Test.Failures, Test.Failures == 1 ? "" : "s");

    return Test.Failures == 0 ? 0 : 1;
}
