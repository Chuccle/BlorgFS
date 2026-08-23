//
// Kernel-behaviour tests for the real TlsHandshake.c, run against the
// kernel rule model with a scriptable WSK peer (WskModel.h) -- the same
// substrate SocketKernelTest.cpp uses, since the handshake drives the
// exact same async WSK primitives Socket.c exposes (BlorgSendWskAsync,
// BlorgReceiveWskAsyncMdl).
//
// TlsHandshake.c had zero measured coverage before this file existed: the
// only other project that compiles it is DispatchSandbox (built for
// unrelated IRP-dispatch coverage), and the one thing that actually drives
// it end to end -- TlsHandshakeTest.cpp -- is an integration test against a
// live openssl s_server, excluded from the Fast tier for exactly that
// reason (see tools\Invoke-BlorgChecks.ps1).
//
// These tests act as the TLS *server* side of a handshake instead of a
// live peer: BlorgTlsStartHandshakeAsync generates its own ephemeral ECDHE key
// pair and client random internally, so a script can't be built in advance
// the way SocketKernelTest.cpp's fixed byte payloads can. Instead each test
// captures the real ClientHello the driver sends (WskModelLastSendBytes),
// derives the same key schedule from it using the crypto primitives Tls.h
// already exposes, builds a throwaway self-signed leaf certificate and
// signs CertificateVerify with a fresh ECDSA key, and scripts the result
// back through WskModelSetReceiveBehaviour -- the same receive-callback
// path TlsHandshakeIssueReceiveRecordHeader drains in the driver.
//
// BlorgTlsCheckPin is the priority: it's the actual security boundary (a
// MITM'd certificate -- cryptographically valid, just not the pinned one
// -- must be rejected) and had no verification that it did anything.
//

#include <gtest/gtest.h>

#include <vector>

// bcrypt.h itself comes in transitively, inside the extern "C" block below,
// via Driver.h -> Tls.h -- which is the only place its prerequisite NT
// types (ULONG, WINAPI, ...) are already correctly set up in this
// kernel-model sandbox build. Its declarations stay visible after the
// extern "C" block closes, same as every other type this file uses from it.
extern "C" {
#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
#include "..\..\src\TlsHandshake.h"
}

namespace
{

///////////////////////////////////////////////////////////////////////////
// Minimal DER helpers -- test-only, mirrors what BlorgTlsParseCertificateVerifyMessage
// (Tls.c) decodes back out.
///////////////////////////////////////////////////////////////////////////

// Encodes a 32-byte unsigned big-endian value as a DER INTEGER: strips
// redundant leading zero bytes, then re-adds exactly one if the remaining
// high bit is set (DER INTEGERs are signed; a top bit would otherwise read
// as negative). Mirrors, in reverse, the padding BlorgTlsParseCertificateVerifyMessage
// strips when decoding r/s back out of the wire signature.
ULONG DerEncodeUnsignedInteger(const UCHAR value32[32], UCHAR* out)
{
    const UCHAR* p = value32;
    ULONG len = 32;

    while (len > 1 && 0 == p[0])
    {
        p++;
        len--;
    }

    BOOLEAN pad = (p[0] & 0x80) != 0;

    ULONG o = 0;
    out[o++] = 0x02;
    out[o++] = C_CAST(UCHAR, len + (pad ? 1 : 0));

    if (pad)
    {
        out[o++] = 0x00;
    }

    memcpy(out + o, p, len);
    o += len;

    return o;
}

// SEQUENCE { INTEGER r, INTEGER s } from BCryptSignHash's raw r||s (64
// bytes) -- the wire form CertificateVerify carries and
// BlorgTlsParseCertificateVerifyMessage expects.
ULONG DerEncodeEcdsaSignature(const UCHAR rawSig[64], UCHAR* out)
{
    UCHAR rEnc[35];
    UCHAR sEnc[35];
    ULONG rLen = DerEncodeUnsignedInteger(rawSig, rEnc);
    ULONG sLen = DerEncodeUnsignedInteger(rawSig + 32, sEnc);
    ULONG contentLen = rLen + sLen;

    ULONG o = 0;
    out[o++] = 0x30;
    out[o++] = C_CAST(UCHAR, contentLen); // r and s are each <= 35 bytes DER-encoded, so this never needs long form
    memcpy(out + o, rEnc, rLen); o += rLen;
    memcpy(out + o, sEnc, sLen); o += sLen;

    return o;
}

//
// A fake but structurally valid leaf certificate DER carrying the given
// P-256 SPKI. BlorgTlsExtractSpkiFromCertificate only walks TLV *lengths* to
// reach subjectPublicKeyInfo and never validates the issuing CA's
// signature -- this driver pins the leaf's SPKI directly, it doesn't
// verify a chain -- so every field ahead of the SPKI just needs to be a
// well-formed, correctly-lengthed TLV, not a semantically real X.509 field.
//
ULONG BuildFakeLeafCertificate(const UCHAR spki[TLS_SPKI_DER_LEN], UCHAR* out)
{
    static const UCHAR serial[] = { 0x02, 0x01, 0x01 };
    static const UCHAR emptySeq[] = { 0x30, 0x00 };

    UCHAR tbsContent[3 + 2 + 2 + 2 + 2 + TLS_SPKI_DER_LEN];
    ULONG o = 0;
    memcpy(tbsContent + o, serial, sizeof(serial)); o += sizeof(serial);        // serialNumber
    memcpy(tbsContent + o, emptySeq, sizeof(emptySeq)); o += sizeof(emptySeq);  // signature AlgorithmIdentifier
    memcpy(tbsContent + o, emptySeq, sizeof(emptySeq)); o += sizeof(emptySeq);  // issuer
    memcpy(tbsContent + o, emptySeq, sizeof(emptySeq)); o += sizeof(emptySeq);  // validity
    memcpy(tbsContent + o, emptySeq, sizeof(emptySeq)); o += sizeof(emptySeq);  // subject
    memcpy(tbsContent + o, spki, TLS_SPKI_DER_LEN); o += TLS_SPKI_DER_LEN;      // subjectPublicKeyInfo

    UCHAR tbs[2 + sizeof(tbsContent)];
    ULONG to = 0;
    tbs[to++] = 0x30;
    tbs[to++] = C_CAST(UCHAR, o); // < 128 for every field size above -- single-byte DER length
    memcpy(tbs + to, tbsContent, o); to += o;

    static const UCHAR sigAlg[] = { 0x30, 0x00 };
    static const UCHAR sigVal[] = { 0x03, 0x01, 0x00 }; // empty BIT STRING, 0 unused bits

    ULONG certContentLen = to + C_CAST(ULONG, sizeof(sigAlg)) + C_CAST(ULONG, sizeof(sigVal));

    ULONG oo = 0;
    out[oo++] = 0x30;
    out[oo++] = C_CAST(UCHAR, certContentLen); // also < 128
    memcpy(out + oo, tbs, to); oo += to;
    memcpy(out + oo, sigAlg, sizeof(sigAlg)); oo += sizeof(sigAlg);
    memcpy(out + oo, sigVal, sizeof(sigVal)); oo += sizeof(sigVal);

    return oo;
}

///////////////////////////////////////////////////////////////////////////
// The fake TLS 1.3 server: builds a real, self-consistent ServerHello and
// encrypted flight in answer to whatever ClientHello the driver actually
// sent, using the same crypto/message primitives (Tls.h) the driver
// itself uses to build and parse them.
///////////////////////////////////////////////////////////////////////////

enum FLIGHT_VARIANT
{
    FlightNormal,
    FlightCertVerifyBeforeCertificate, // wire order only -- transcript/MAC stay logically correct
    FlightCorruptFinishedMac,

    //
    // Zero-padded (RFC 8446 5.4) out to the largest record a compliant
    // server may send: TLSInnerPlaintext of 2^14 content bytes plus the
    // content-type trailer, giving a 2^14 + 1 + 16 = 16401-byte
    // TLSCiphertext. This is what a server fragmenting a large certificate
    // chain at the plaintext maximum actually puts on the wire.
    //
    FlightMaxSizedRecord
};

struct FakeTlsServer
{
    UCHAR ServerEcdhPrivate[TLS_ECC_COORD_LEN] = {};
    UCHAR ServerEcdhPublic[TLS_ECC_PUBKEY_LEN] = {};
    UCHAR ServerRandom[TLS_HANDSHAKE_RANDOM_LEN] = {};

    BCRYPT_ALG_HANDLE EcdsaAlg = nullptr;
    BCRYPT_KEY_HANDLE EcdsaKey = nullptr;
    UCHAR ServerLongTermPublic[TLS_ECC_PUBKEY_LEN] = {};
    UCHAR Spki[TLS_SPKI_DER_LEN] = {};
    UCHAR LeafCertDer[256] = {};
    ULONG LeafCertDerLen = 0;

    UCHAR ClientRandom[TLS_HANDSHAKE_RANDOM_LEN] = {};
    UCHAR ClientPublicKey[TLS_ECC_PUBKEY_LEN] = {};
    UCHAR ClientHelloMsg[512] = {};
    ULONG ClientHelloMsgLen = 0;

    UCHAR Transcript[4096] = {};
    ULONG TranscriptLen = 0;

    UCHAR HandshakeSecret[TLS_HASH_LEN] = {};
    UCHAR ClientHsTraffic[TLS_HASH_LEN] = {};
    UCHAR ServerHsTraffic[TLS_HASH_LEN] = {};
    UCHAR ServerHsKey[TLS_KEY_LEN] = {};
    UCHAR ServerHsIv[TLS_IV_LEN] = {};

    ~FakeTlsServer()
    {
        if (EcdsaKey) BCryptDestroyKey(EcdsaKey);
        if (EcdsaAlg) BCryptCloseAlgorithmProvider(EcdsaAlg, 0);
    }

    void AppendToTranscript(const UCHAR* bytes, ULONG len)
    {
        memcpy(Transcript + TranscriptLen, bytes, len);
        TranscriptLen += len;
    }

    // Ephemeral ECDH key pair (for the shared secret), a throwaway ECDSA
    // signing key (stands in for a CA-issued leaf key), and the fake
    // certificate carrying its SPKI.
    bool GenerateKeys()
    {
        if (!NT_SUCCESS(BlorgTlsEcdhGenerateKeyPair(ServerEcdhPrivate, ServerEcdhPublic))) return false;
        if (!NT_SUCCESS(BCryptGenRandom(NULL, ServerRandom, TLS_HANDSHAKE_RANDOM_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;

        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&EcdsaAlg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0))) return false;
        if (!NT_SUCCESS(BCryptGenerateKeyPair(EcdsaAlg, &EcdsaKey, 256, 0))) return false;
        if (!NT_SUCCESS(BCryptFinalizeKeyPair(EcdsaKey, 0))) return false;

        UCHAR pubBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN];
        ULONG resultLen = 0;
        if (!NT_SUCCESS(BCryptExportKey(EcdsaKey, NULL, BCRYPT_ECCPUBLIC_BLOB, pubBlob, sizeof(pubBlob), &resultLen, 0))) return false;

        ServerLongTermPublic[0] = 0x04;
        memcpy(ServerLongTermPublic + 1, pubBlob + sizeof(BCRYPT_ECCKEY_BLOB), 2 * TLS_ECC_COORD_LEN);

        if (!NT_SUCCESS(BlorgTlsEncodeP256SubjectPublicKeyInfo(ServerLongTermPublic, Spki))) return false;

        LeafCertDerLen = BuildFakeLeafCertificate(Spki, LeafCertDer);
        return LeafCertDerLen > 0;
    }

    //
    // Reads back the ClientHello the driver actually sent (captured at
    // issue time by WskModel.c, even while the send is still deferred) and
    // pulls out the two fields the rest of the key schedule needs: the
    // client random and the client's ephemeral key_share public point.
    // Walks the wire format generically rather than assuming fixed offsets,
    // since whether SNI is present depends on global.RemoteHostSniAnsi.
    //
    bool CaptureClientHello()
    {
        SIZE_T len = 0;
        const unsigned char* bytes = WskModelLastSendBytes(&len);

        if (len < 5 + 4 + 2 + TLS_HANDSHAKE_RANDOM_LEN + 1 + 2 + 1 + 2) return false;

        ULONG msgLen = C_CAST(ULONG, len - 5);
        if (msgLen > sizeof(ClientHelloMsg)) return false;
        memcpy(ClientHelloMsg, bytes + 5, msgLen);
        ClientHelloMsgLen = msgLen;

        const UCHAR* body = ClientHelloMsg + 4;
        ULONG offset = 2; // legacy_version

        memcpy(ClientRandom, body + offset, TLS_HANDSHAKE_RANDOM_LEN);
        offset += TLS_HANDSHAKE_RANDOM_LEN;

        ULONG sessionIdLen = body[offset]; offset += 1 + sessionIdLen;
        ULONG cipherSuitesLen = (C_CAST(ULONG, body[offset]) << 8) | body[offset + 1]; offset += 2 + cipherSuitesLen;
        ULONG compressionLen = body[offset]; offset += 1 + compressionLen;
        ULONG extLen = (C_CAST(ULONG, body[offset]) << 8) | body[offset + 1]; offset += 2;
        ULONG extEnd = offset + extLen;

        bool foundKeyShare = false;

        while (offset + 4 <= extEnd)
        {
            USHORT extType = C_CAST(USHORT, (C_CAST(ULONG, body[offset]) << 8) | body[offset + 1]);
            USHORT thisExtLen = C_CAST(USHORT, (C_CAST(ULONG, body[offset + 2]) << 8) | body[offset + 3]);
            offset += 4;

            if (0x0033 == extType) // key_share: client_shares<0..2^16-1> list length(2), then group(2)+keylen(2)+point
            {
                memcpy(ClientPublicKey, body + offset + 6, TLS_ECC_PUBKEY_LEN);
                foundKeyShare = true;
            }

            offset += thisExtLen;
        }

        return foundKeyShare;
    }

    //
    // Builds the ServerHello record and, on the first call, seeds the
    // transcript with the captured ClientHello ahead of it -- transcript
    // order is ClientHello then ServerHello, per RFC 8446 4.4.1.
    //
    ULONG BuildServerHelloRecord(UCHAR* out, bool wrongCipherSuite = false)
    {
        if (0 == TranscriptLen)
        {
            AppendToTranscript(ClientHelloMsg, ClientHelloMsgLen);
        }

        UCHAR body[256];
        ULONG o = 0;
        body[o++] = 0x03; body[o++] = 0x03; // legacy_version -- not authoritative, not checked by the parser
        memcpy(body + o, ServerRandom, TLS_HANDSHAKE_RANDOM_LEN); o += TLS_HANDSHAKE_RANDOM_LEN;
        body[o++] = 0x00; // legacy_session_id_echo, empty

        if (wrongCipherSuite) { body[o++] = 0x13; body[o++] = 0x02; } // TLS_AES_256_GCM_SHA384 -- this driver only speaks 0x1301
        else { body[o++] = 0x13; body[o++] = 0x01; }

        body[o++] = 0x00; // legacy_compression_method

        ULONG extLenOffset = o; o += 2;
        ULONG extStart = o;

        body[o++] = 0x00; body[o++] = 0x2B; // supported_versions
        body[o++] = 0x00; body[o++] = 0x02;
        body[o++] = 0x03; body[o++] = 0x04; // TLS 1.3

        body[o++] = 0x00; body[o++] = 0x33; // key_share
        body[o++] = 0x00; body[o++] = 0x45; // extension length = 69
        body[o++] = 0x00; body[o++] = 0x17; // group: secp256r1
        body[o++] = 0x00; body[o++] = 0x41; // key length = 65
        memcpy(body + o, ServerEcdhPublic, TLS_ECC_PUBKEY_LEN); o += TLS_ECC_PUBKEY_LEN;

        ULONG extLen = o - extStart;
        body[extLenOffset] = C_CAST(UCHAR, extLen >> 8);
        body[extLenOffset + 1] = C_CAST(UCHAR, extLen & 0xFF);

        UCHAR msg[4 + sizeof(body)];
        ULONG msgLen = 0;
        msg[msgLen++] = 0x02; // ServerHello
        msg[msgLen++] = C_CAST(UCHAR, o >> 16); msg[msgLen++] = C_CAST(UCHAR, o >> 8); msg[msgLen++] = C_CAST(UCHAR, o & 0xFF);
        memcpy(msg + msgLen, body, o); msgLen += o;

        AppendToTranscript(msg, msgLen);

        out[0] = 0x16; out[1] = 0x03; out[2] = 0x03;
        out[3] = C_CAST(UCHAR, msgLen >> 8); out[4] = C_CAST(UCHAR, msgLen & 0xFF);
        memcpy(out + 5, msg, msgLen);

        return 5 + msgLen;
    }

    // Requires the transcript to already hold ClientHello + ServerHello.
    bool DeriveHandshakeSecrets()
    {
        UCHAR sharedSecret[TLS_ECC_COORD_LEN];
        if (!NT_SUCCESS(BlorgTlsEcdhComputeSharedSecret(ServerEcdhPrivate, ServerEcdhPublic, ClientPublicKey, sharedSecret))) return false;

        UCHAR transcriptHashChSh[TLS_HASH_LEN];
        if (!NT_SUCCESS(BlorgTlsSha256(Transcript, TranscriptLen, transcriptHashChSh))) return false;

        UCHAR emptyHash[TLS_HASH_LEN];
        BlorgTlsSha256(NULL, 0, emptyHash);

        UCHAR zero32[32] = { 0 };
        UCHAR earlySecret[TLS_HASH_LEN];
        BlorgTlsHkdfExtract(zero32, 32, zero32, 32, earlySecret);

        UCHAR derivedForHandshake[TLS_HASH_LEN];
        BlorgTlsHkdfExpandLabel(earlySecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForHandshake);

        BlorgTlsHkdfExtract(derivedForHandshake, TLS_HASH_LEN, sharedSecret, TLS_ECC_COORD_LEN, HandshakeSecret);

        BlorgTlsHkdfExpandLabel(HandshakeSecret, TLS_HASH_LEN, "c hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, ClientHsTraffic);
        BlorgTlsHkdfExpandLabel(HandshakeSecret, TLS_HASH_LEN, "s hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, ServerHsTraffic);

        BlorgTlsHkdfExpandLabel(ServerHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, ServerHsKey);
        BlorgTlsHkdfExpandLabel(ServerHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, ServerHsIv);

        return true;
    }

    //
    // Builds EncryptedExtensions, Certificate, CertificateVerify and
    // Finished, appended to the transcript and signed/MACed in the correct
    // logical order regardless of Variant -- only the bytes actually placed
    // on the wire are reordered for FlightCertVerifyBeforeCertificate, and
    // only the Finished MAC's first byte is flipped after computing the
    // real one for FlightCorruptFinishedMac. Encrypted as a single
    // application_data record at sequence 0.
    //
    ULONG BuildEncryptedFlightRecord(UCHAR* out, FLIGHT_VARIANT variant = FlightNormal)
    {
        UCHAR eeMsg[8];
        ULONG eeLen = 0;
        eeMsg[eeLen++] = 0x08;
        eeMsg[eeLen++] = 0x00; eeMsg[eeLen++] = 0x00; eeMsg[eeLen++] = 0x02;
        eeMsg[eeLen++] = 0x00; eeMsg[eeLen++] = 0x00; // extensions, empty
        AppendToTranscript(eeMsg, eeLen);

        UCHAR certBody[256];
        ULONG cbo = 0;
        certBody[cbo++] = 0x00; // certificate_request_context, empty
        ULONG certListLenOffset = cbo; cbo += 3;
        ULONG certListStart = cbo;
        certBody[cbo++] = C_CAST(UCHAR, LeafCertDerLen >> 16); certBody[cbo++] = C_CAST(UCHAR, LeafCertDerLen >> 8); certBody[cbo++] = C_CAST(UCHAR, LeafCertDerLen & 0xFF);
        memcpy(certBody + cbo, LeafCertDer, LeafCertDerLen); cbo += LeafCertDerLen;
        certBody[cbo++] = 0x00; certBody[cbo++] = 0x00; // per-certificate extensions, empty
        ULONG certListLen = cbo - certListStart;
        certBody[certListLenOffset] = C_CAST(UCHAR, certListLen >> 16);
        certBody[certListLenOffset + 1] = C_CAST(UCHAR, certListLen >> 8);
        certBody[certListLenOffset + 2] = C_CAST(UCHAR, certListLen & 0xFF);

        UCHAR certMsg[4 + sizeof(certBody)];
        ULONG certMsgLen = 0;
        certMsg[certMsgLen++] = 0x0B;
        certMsg[certMsgLen++] = C_CAST(UCHAR, cbo >> 16); certMsg[certMsgLen++] = C_CAST(UCHAR, cbo >> 8); certMsg[certMsgLen++] = C_CAST(UCHAR, cbo & 0xFF);
        memcpy(certMsg + certMsgLen, certBody, cbo); certMsgLen += cbo;

        AppendToTranscript(certMsg, certMsgLen);

        UCHAR transcriptHashThroughCert[TLS_HASH_LEN];
        BlorgTlsSha256(Transcript, TranscriptLen, transcriptHashThroughCert);

        UCHAR verifyContent[TLS_CERT_VERIFY_CONTENT_LEN];
        BlorgTlsBuildServerCertVerifyContent(transcriptHashThroughCert, verifyContent);
        UCHAR verifyDigest[TLS_HASH_LEN];
        BlorgTlsSha256(verifyContent, TLS_CERT_VERIFY_CONTENT_LEN, verifyDigest);

        UCHAR rawSig[64];
        ULONG sigResultLen = 0;
        if (!NT_SUCCESS(BCryptSignHash(EcdsaKey, NULL, verifyDigest, TLS_HASH_LEN, rawSig, sizeof(rawSig), &sigResultLen, 0))) return 0;

        UCHAR sigDer[80];
        ULONG sigDerLen = DerEncodeEcdsaSignature(rawSig, sigDer);

        UCHAR cvBody[4 + sizeof(sigDer)];
        ULONG cvbo = 0;
        cvBody[cvbo++] = 0x04; cvBody[cvbo++] = 0x03; // ecdsa_secp256r1_sha256
        cvBody[cvbo++] = C_CAST(UCHAR, sigDerLen >> 8); cvBody[cvbo++] = C_CAST(UCHAR, sigDerLen & 0xFF);
        memcpy(cvBody + cvbo, sigDer, sigDerLen); cvbo += sigDerLen;

        UCHAR cvMsg[4 + sizeof(cvBody)];
        ULONG cvMsgLen = 0;
        cvMsg[cvMsgLen++] = 0x0F;
        cvMsg[cvMsgLen++] = C_CAST(UCHAR, cvbo >> 16); cvMsg[cvMsgLen++] = C_CAST(UCHAR, cvbo >> 8); cvMsg[cvMsgLen++] = C_CAST(UCHAR, cvbo & 0xFF);
        memcpy(cvMsg + cvMsgLen, cvBody, cvbo); cvMsgLen += cvbo;

        AppendToTranscript(cvMsg, cvMsgLen);

        UCHAR transcriptHashThroughCertVerify[TLS_HASH_LEN];
        BlorgTlsSha256(Transcript, TranscriptLen, transcriptHashThroughCertVerify);

        UCHAR serverFinishedKey[TLS_HASH_LEN];
        BlorgTlsHkdfExpandLabel(ServerHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, serverFinishedKey);
        UCHAR finishedMac[TLS_HASH_LEN];
        BlorgTlsHmacSha256(serverFinishedKey, TLS_HASH_LEN, transcriptHashThroughCertVerify, TLS_HASH_LEN, finishedMac);

        if (FlightCorruptFinishedMac == variant)
        {
            finishedMac[0] ^= 0xFF;
        }

        UCHAR finMsg[4 + TLS_HASH_LEN];
        ULONG finMsgLen = 0;
        finMsg[finMsgLen++] = 0x14;
        finMsg[finMsgLen++] = 0x00; finMsg[finMsgLen++] = 0x00; finMsg[finMsgLen++] = TLS_HASH_LEN;
        memcpy(finMsg + finMsgLen, finishedMac, TLS_HASH_LEN); finMsgLen += TLS_HASH_LEN;

        std::vector<UCHAR> flightPlainBuf(TLS_RECORD_CIPHERTEXT_MAX, 0);
        UCHAR* flightPlain = flightPlainBuf.data();
        ULONG fpo = 0;
        memcpy(flightPlain + fpo, eeMsg, eeLen); fpo += eeLen;

        if (FlightCertVerifyBeforeCertificate == variant)
        {
            memcpy(flightPlain + fpo, cvMsg, cvMsgLen); fpo += cvMsgLen;
            memcpy(flightPlain + fpo, certMsg, certMsgLen); fpo += certMsgLen;
        }
        else
        {
            memcpy(flightPlain + fpo, certMsg, certMsgLen); fpo += certMsgLen;
            memcpy(flightPlain + fpo, cvMsg, cvMsgLen); fpo += cvMsgLen;
        }

        memcpy(flightPlain + fpo, finMsg, finMsgLen); fpo += finMsgLen;
        flightPlain[fpo++] = 0x16; // TLSInnerPlaintext content-type trailer: handshake

        //
        // Zero padding after the content-type trailer, which RFC 8446 5.4
        // allows and BlorgTlsStripInnerPlaintext scans back over. Grown until the
        // ciphertext hits exactly the largest a compliant server may emit,
        // so the record's declared length is the real-world maximum rather
        // than an arbitrary large number.
        //
        if (FlightMaxSizedRecord == variant)
        {
            while (fpo + TLS_TAG_LEN < TLS_RECORD_CIPHERTEXT_MAX)
            {
                flightPlain[fpo++] = 0x00;
            }
        }

        UCHAR aad[5];
        ULONG recordLen = fpo + TLS_TAG_LEN;
        aad[0] = 0x17; aad[1] = 0x03; aad[2] = 0x03;
        aad[3] = C_CAST(UCHAR, recordLen >> 8); aad[4] = C_CAST(UCHAR, recordLen & 0xFF);

        std::vector<UCHAR> ciphertextBuf(TLS_RECORD_CIPHERTEXT_MAX, 0);
        UCHAR* ciphertext = ciphertextBuf.data();
        UCHAR tag[TLS_TAG_LEN];
        if (!NT_SUCCESS(BlorgTlsAeadEncrypt(ServerHsKey, ServerHsIv, 0, aad, 5, flightPlain, fpo, ciphertext, tag))) return 0;

        memcpy(out, aad, 5);
        memcpy(out + 5, ciphertext, fpo);
        memcpy(out + 5 + fpo, tag, TLS_TAG_LEN);

        return 5 + fpo + TLS_TAG_LEN;
    }
};

///////////////////////////////////////////////////////////////////////////
// Fixture
///////////////////////////////////////////////////////////////////////////

class TlsHandshakeKernelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        WskModelReset();
        ASSERT_EQ(STATUS_SUCCESS, BlorgInitialiseWskClient());
        ASSERT_EQ(STATUS_SUCCESS, BlorgTlsGlobalInit());
        BlorgTlsHandshakeGlobalInit(); // TlsPin.Lock must be initialized before BlorgTlsSetPin/BlorgTlsCheckPin -- normally DriverEntry's job
        Result = {};
        AcquireResult = {};
    }

    void TearDown() override
    {
        BlorgTlsGlobalCleanup();
        BlorgCleanupWskClient();

        //
        // Nothing may outlive a test -- same discipline as
        // SocketKernelTest.cpp, and for the same reason: an IRP, MDL or
        // pool block still live here is a leak in TlsHandshake.c or
        // Socket.c, not in the harness.
        //
        KmAssertQuiescent("TlsHandshakeKernelTest teardown");
    }

    struct AcquireRecord
    {
        int Calls = 0;
        NTSTATUS Status = STATUS_SUCCESS;
        PKSOCKET Socket = nullptr;
    };

    static AcquireRecord AcquireResult;

    static void RecordAcquire(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID Context)
    {
        (void)Reused;
        (void)Context;
        AcquireResult.Calls++;
        AcquireResult.Status = Status;
        AcquireResult.Socket = Socket;
    }

    // ForceFresh=TRUE throughout: these tests never release a socket back
    // to Socket.c's connection pool (only BlorgCloseWskSocketAsync), so there is
    // nothing there for a later test -- in this file or any other sharing
    // the process -- to collide with.
    PKSOCKET AcquireSocket()
    {
        SOCKADDR_IN address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(443);

        AcquireResult = {};

        NTSTATUS status = BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, TRUE, RecordAcquire, nullptr);

        EXPECT_EQ(STATUS_PENDING, status);
        EXPECT_TRUE(NT_SUCCESS(AcquireResult.Status));

        return AcquireResult.Socket;
    }

    struct HandshakeRecord
    {
        int Calls = 0;
        NTSTATUS Status = STATUS_SUCCESS;
    };

    static HandshakeRecord Result;

    static void OnHandshakeDone(NTSTATUS Status, PVOID Context)
    {
        (void)Context;
        Result.Calls++;
        Result.Status = Status;
    }

    //
    // Starts the handshake with the ClientHello send deliberately
    // WskModelDeferred, so the real bytes the driver generated can be
    // captured (WskModelLastSendBytes) before anything completes --
    // otherwise BlorgTlsStartHandshakeAsync's send completes inline and issues
    // the ServerHello receive in the same call, leaving no window to
    // script a response that depends on what was just generated.
    //
    PKSOCKET StartAndCaptureClientHello(FakeTlsServer* server)
    {
        PKSOCKET socket = AcquireSocket();
        EXPECT_NE(nullptr, socket);

        WSK_MODEL_BEHAVIOUR deferredSend = {};
        deferredSend.Completion = WskModelDeferred;
        deferredSend.Status = STATUS_SUCCESS;
        WskModelSetSendBehaviour(&deferredSend);

        BlorgTlsStartHandshakeAsync(socket, OnHandshakeDone, nullptr);

        EXPECT_TRUE(server->CaptureClientHello());

        return socket;
    }

    //
    // Scripts Response as the answer to the next receive, resets the send
    // behaviour to a plain inline success (for the client Finished the
    // driver sends once it accepts the flight), then releases the deferred
    // ClientHello completion and drains the one PASSIVE bounce every
    // record-payload completion takes here: this model's completions run
    // "at DISPATCH_LEVEL", and TlsHandshakeOnReceiveRecordPayload bounces
    // to a work item unconditionally the first time it sees that.
    //
    void DeliverServerResponse(const UCHAR* response, ULONG responseLen)
    {
        WSK_MODEL_BEHAVIOUR recv = {};
        recv.Completion = WskModelInline;
        recv.Status = STATUS_SUCCESS;
        recv.Bytes = responseLen;
        recv.Payload = response;
        recv.PayloadLength = responseLen;
        WskModelSetReceiveBehaviour(&recv);

        WSK_MODEL_BEHAVIOUR sendOk = {};
        sendOk.Completion = WskModelInline;
        sendOk.Status = STATUS_SUCCESS;
        WskModelSetSendBehaviour(&sendOk);

        WskModelReleaseDeferred();
        ShimDrainWorkItems();
    }
};

TlsHandshakeKernelTest::AcquireRecord TlsHandshakeKernelTest::AcquireResult;
TlsHandshakeKernelTest::HandshakeRecord TlsHandshakeKernelTest::Result;

///////////////////////////////////////////////////////////////////////////
// BlorgTlsCheckPin -- the priority: this is the actual security boundary
///////////////////////////////////////////////////////////////////////////

TEST_F(TlsHandshakeKernelTest, HandshakeSucceedsAndTlsCheckPinAcceptsMatchingCertificate)
{
    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    UCHAR pin[TLS_HASH_LEN];
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSha256(server.Spki, TLS_SPKI_DER_LEN, pin));
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSetPin(pin));

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    UCHAR response[2048];
    ULONG responseLen = server.BuildServerHelloRecord(response);
    ASSERT_TRUE(server.DeriveHandshakeSecrets());
    responseLen += server.BuildEncryptedFlightRecord(response + responseLen);
    ASSERT_GT(responseLen, 0u);

    DeliverServerResponse(response, responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_EQ(STATUS_SUCCESS, Result.Status);
    EXPECT_EQ(TlsHandshakeComplete, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

//
// A server fragmenting its flight at the plaintext maximum emits a
// TLSCiphertext of 2^14 + 1 + 16 = 16401 bytes (RFC 8446 5.2 caps the
// declared length at 2^14 + 256, and this is what the padding rules
// actually produce). The handshake's own receive path capped a record at
// 2^14 = 16384 -- the *plaintext* maximum from 5.1 -- so it rejected a
// legal record 17 bytes over, failing the handshake outright with
// STATUS_INVALID_PARAMETER before decryption was ever attempted. The
// post-handshake drain in Client.c already used the correct
// TLS_RECORD_CIPHERTEXT_MAX, so the two halves of the same connection
// disagreed on what a valid record was.
//
// Nothing here was memory-unsafe: the check rejected rather than
// overflowed. The consequence is interop -- a server with a large
// certificate chain simply cannot complete a handshake -- and it would
// present as a chain-size-dependent connect failure, which is a
// thoroughly unpleasant thing to diagnose from the outside.
//
TEST_F(TlsHandshakeKernelTest, HandshakeAcceptsAMaximallySizedFlightRecord)
{
    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    UCHAR pin[TLS_HASH_LEN];
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSha256(server.Spki, TLS_SPKI_DER_LEN, pin));
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSetPin(pin));

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    std::vector<UCHAR> response(2048 + TLS_RECORD_CIPHERTEXT_MAX + 16, 0);
    ULONG responseLen = server.BuildServerHelloRecord(response.data());
    ASSERT_TRUE(server.DeriveHandshakeSecrets());

    ULONG flightLen = server.BuildEncryptedFlightRecord(
        response.data() + responseLen, FlightMaxSizedRecord);
    ASSERT_GT(flightLen, 0u);

    ULONG declaredLen = ((ULONG)response[responseLen + 3] << 8) | response[responseLen + 4];
    ASSERT_EQ((ULONG)TLS_RECORD_CIPHERTEXT_MAX, declaredLen)
        << "the fixture must actually produce a maximum-sized record for this to prove anything";

    responseLen += flightLen;

    DeliverServerResponse(response.data(), responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_EQ(STATUS_SUCCESS, Result.Status)
        << "a maximum-sized but entirely legal handshake record must not be rejected -- "
           "the cap belongs at TLS_RECORD_CIPHERTEXT_MAX (2^14 + 1 + tag), not at the "
           "2^14 plaintext maximum";
    EXPECT_EQ(TlsHandshakeComplete, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

TEST_F(TlsHandshakeKernelTest, HandshakeFailsOnCertificatePinMismatch)
{
    //
    // Mirrors TlsHandshake.c's private STATUS_BLORGFS_CERT_PIN_MISMATCH
    // (0xE0080001) -- not exported via TlsHandshake.h since nothing outside
    // that file needs to distinguish it from any other handshake failure.
    // Duplicated here deliberately, the same way SocketKernelTest.cpp
    // duplicates Socket.c's timeout constants: if this value ever changes,
    // this assertion failing is what should make someone come re-read it.
    //
    const NTSTATUS kCertPinMismatch = C_CAST(NTSTATUS, 0xE0080001L);

    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    UCHAR wrongPin[TLS_HASH_LEN];
    memset(wrongPin, 0xAA, sizeof(wrongPin));
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSetPin(wrongPin));

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    UCHAR response[2048];
    ULONG responseLen = server.BuildServerHelloRecord(response);
    ASSERT_TRUE(server.DeriveHandshakeSecrets());
    responseLen += server.BuildEncryptedFlightRecord(response + responseLen);
    ASSERT_GT(responseLen, 0u);

    DeliverServerResponse(response, responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_EQ(kCertPinMismatch, Result.Status)
        << "a certificate that doesn't match the configured pin must be rejected with "
           "the pin-specific status, not laundered through a generic parse failure";
    EXPECT_EQ(TlsHandshakeFailed, socket->Tls.State)
        << "a socket that failed pin checking must never be reused or pooled";

    BlorgCloseWskSocketAsync(socket);
}

///////////////////////////////////////////////////////////////////////////
// Malformed flights -- shapes a well-behaved server never produces
///////////////////////////////////////////////////////////////////////////

TEST_F(TlsHandshakeKernelTest, HandshakeFailsOnOutOfOrderCertificateVerify)
{
    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    UCHAR pin[TLS_HASH_LEN];
    BlorgTlsSha256(server.Spki, TLS_SPKI_DER_LEN, pin);
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSetPin(pin));

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    UCHAR response[2048];
    ULONG responseLen = server.BuildServerHelloRecord(response);
    ASSERT_TRUE(server.DeriveHandshakeSecrets());
    responseLen += server.BuildEncryptedFlightRecord(response + responseLen, FlightCertVerifyBeforeCertificate);
    ASSERT_GT(responseLen, 0u);

    DeliverServerResponse(response, responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_FALSE(NT_SUCCESS(Result.Status)) << "CertificateVerify before Certificate must be rejected";
    EXPECT_EQ(TlsHandshakeFailed, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

TEST_F(TlsHandshakeKernelTest, HandshakeFailsOnCorruptFinishedMac)
{
    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    UCHAR pin[TLS_HASH_LEN];
    BlorgTlsSha256(server.Spki, TLS_SPKI_DER_LEN, pin);
    ASSERT_EQ(STATUS_SUCCESS, BlorgTlsSetPin(pin));

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    UCHAR response[2048];
    ULONG responseLen = server.BuildServerHelloRecord(response);
    ASSERT_TRUE(server.DeriveHandshakeSecrets());
    responseLen += server.BuildEncryptedFlightRecord(response + responseLen, FlightCorruptFinishedMac);
    ASSERT_GT(responseLen, 0u);

    DeliverServerResponse(response, responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_FALSE(NT_SUCCESS(Result.Status)) << "a wrong server Finished MAC must be rejected, not silently accepted";
    EXPECT_EQ(TlsHandshakeFailed, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

TEST_F(TlsHandshakeKernelTest, HandshakeFailsOnUnsupportedServerHelloCipherSuite)
{
    FakeTlsServer server;
    ASSERT_TRUE(server.GenerateKeys());

    PKSOCKET socket = StartAndCaptureClientHello(&server);

    UCHAR response[256];
    ULONG responseLen = server.BuildServerHelloRecord(response, /*wrongCipherSuite=*/true);
    ASSERT_GT(responseLen, 0u);

    DeliverServerResponse(response, responseLen);

    EXPECT_EQ(1, Result.Calls);
    EXPECT_FALSE(NT_SUCCESS(Result.Status));
    EXPECT_EQ(TlsHandshakeFailed, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

TEST_F(TlsHandshakeKernelTest, HandshakeFailsWhenServerSendsAlertInsteadOfServerHello)
{
    FakeTlsServer server;
    PKSOCKET socket = StartAndCaptureClientHello(&server);

    static const UCHAR alertRecord[] = { 0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x28 }; // fatal, handshake_failure

    DeliverServerResponse(alertRecord, sizeof(alertRecord));

    EXPECT_EQ(1, Result.Calls);
    EXPECT_FALSE(NT_SUCCESS(Result.Status));
    EXPECT_EQ(TlsHandshakeFailed, socket->Tls.State);

    BlorgCloseWskSocketAsync(socket);
}

} // namespace
