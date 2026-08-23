#pragma once

//
// Shared TLS 1.3 record-layer/key-schedule crypto core. Single ciphersuite:
// ECDHE secp256r1 + ECDSA P-256 + AES-128-GCM-SHA256, implemented with
// kernel-mode CNG (BCrypt) since kernel drivers can't use SChannel/Crypt32.
//
// Compiled into two places:
//  - BlorgFS.vcxproj (kernel driver), which defines BLORGFS_KERNEL_BUILD
//    and includes this after Driver.h's ntifs.h/wsk.h chain, so kernel
//    base types (NTSTATUS, UCHAR, ULONG, ...) are already visible.
//  - TlsTest.vcxproj (usermode console harness), a plain Win32 project
//    that needs <windows.h> first.
// Both link the same BCrypt API surface; only the import library differs
// (cng.lib vs bcrypt.lib), set per-project. Everything below this guard
// is environment-independent C.
//
#ifndef BLORGFS_KERNEL_BUILD
#include <windows.h>
#endif
#include <bcrypt.h>

//
// C_CAST (Driver.h) isn't visible in the usermode build -- same
// definition, guarded, so both builds get it.
//
#ifndef C_CAST
#define C_CAST(T, expr) ((T)(expr))
#endif

//
// NT_SUCCESS and the two STATUS_ codes used here come from ntstatus.h in
// the kernel build. A plain usermode TU with only <windows.h> doesn't have
// them, so the same values are defined here, guarded, so both builds agree.
//
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (C_CAST(NTSTATUS, Status) >= 0)
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS C_CAST(NTSTATUS, 0x00000000L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER C_CAST(NTSTATUS, 0xC000000DL)
#endif

#define TLS_HASH_LEN 32   // SHA-256
#define TLS_KEY_LEN  16   // AES-128
#define TLS_IV_LEN   12   // GCM nonce/static IV
#define TLS_TAG_LEN  16   // GCM authentication tag

//
// __restrict (not plain `restrict`): MSVC recognizes __restrict in both C
// and C++ modes, needed since this header is also included from usermode
// .cpp harnesses. Applied only to plain-pointer parameters, not fixed-size
// array ones (e.g. UCHAR Key[TLS_KEY_LEN]), to keep the size-documenting
// array syntax intact. All call sites pass distinct, non-overlapping
// buffers for these parameters.
//
#define TLS_RESTRICT __restrict

#ifdef __cplusplus
extern "C" {
#endif

//
// Plain SHA-256 (unkeyed) -- used for the transcript hash and for the
// "empty hash" input to the two "derived" key-schedule steps.
//
NTSTATUS BlorgTlsSha256(const UCHAR* TLS_RESTRICT Data, ULONG DataLen, UCHAR Out[TLS_HASH_LEN]);

//
// HMAC-SHA256 -- the primitive both HKDF-Extract and HKDF-Expand are built
// from (RFC 5869), and also used directly for the Finished MAC.
//
NTSTATUS BlorgTlsHmacSha256(
    const UCHAR* TLS_RESTRICT Key, ULONG KeyLen,
    const UCHAR* TLS_RESTRICT Data, ULONG DataLen,
    UCHAR Out[TLS_HASH_LEN]);

//
// RFC 5869 HKDF-Extract: PRK = HMAC-Hash(salt, IKM). Salt of length 0 is
// valid and means "use a zero-filled salt of hash length" per the RFC --
// callers pass a 32-byte zero buffer explicitly rather than SaltLen==0,
// to keep this function a direct, unconditional HMAC call.
//
NTSTATUS BlorgTlsHkdfExtract(
    const UCHAR* TLS_RESTRICT Salt, ULONG SaltLen,
    const UCHAR* TLS_RESTRICT Ikm, ULONG IkmLen,
    UCHAR Out[TLS_HASH_LEN]);

//
// RFC 5869 HKDF-Expand: OKM = T(1) | T(2) | ... truncated to OutLen, where
// T(0) = empty and T(n) = HMAC-Hash(PRK, T(n-1) | info | n). Every TLS 1.3
// key-schedule use requests <= TLS_HASH_LEN bytes (secrets are exactly one
// hash length; key/iv are shorter), so in practice this only ever runs a
// single iteration -- the loop exists for RFC-correctness, not because
// multi-block output is exercised anywhere in this driver.
//
NTSTATUS BlorgTlsHkdfExpand(
    const UCHAR* TLS_RESTRICT Prk, ULONG PrkLen,
    const UCHAR* TLS_RESTRICT Info, ULONG InfoLen,
    UCHAR* TLS_RESTRICT Out, ULONG OutLen);

//
// RFC 8446 7.1 HKDF-Expand-Label: builds the HkdfLabel structure
// (uint16 Length | opaque8 "tls13 "+Label | opaque8 Context) and calls
// BlorgTlsHkdfExpand with it as Info. Label is a plain C string (e.g. "c hs
// traffic") -- the "tls13 " prefix is added internally, callers never
// include it themselves.
//
NTSTATUS BlorgTlsHkdfExpandLabel(
    const UCHAR* TLS_RESTRICT Secret, ULONG SecretLen,
    const char* TLS_RESTRICT Label,
    const UCHAR* TLS_RESTRICT Context, ULONG ContextLen,
    ULONG Length,
    UCHAR* TLS_RESTRICT Out);

//
// AES-128-GCM record encrypt/decrypt. Nonce is the per-record TLS 1.3
// construction (RFC 8446 5.3): the static IV XORed with the 64-bit
// SeqNum, big-endian, left-padded with zeros to IV length. CiphertextOut
// must have room for PlaintextLen bytes (GCM is not length-expanding
// beyond the separately-returned tag). AAD is the 5-byte TLSCiphertext
// header (opaque_type | legacy_record_version | length).
//
NTSTATUS BlorgTlsAeadEncrypt(
    const UCHAR Key[TLS_KEY_LEN], const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Plaintext, ULONG PlaintextLen,
    UCHAR* TLS_RESTRICT CiphertextOut, UCHAR TagOut[TLS_TAG_LEN]);

//
// Decrypt + verify in one call (BCrypt's GCM decrypt checks the tag
// internally and fails closed on mismatch -- STATUS_AUTH_TAG_MISMATCH --
// rather than returning unauthenticated plaintext).
//
NTSTATUS BlorgTlsAeadDecrypt(
    const UCHAR Key[TLS_KEY_LEN], const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Ciphertext, ULONG CiphertextLen, const UCHAR Tag[TLS_TAG_LEN],
    UCHAR* TLS_RESTRICT PlaintextOut);

//
// BlorgTlsAeadEncrypt/BlorgTlsAeadDecrypt's per-call BCryptOpenAlgorithmProvider +
// BCryptGenerateSymmetricKey are PASSIVE_LEVEL-only, which would force a
// PASSIVE bounce on every record-layer read/write. Instead the AES-GCM
// provider is opened once, for the driver's lifetime, with
// BCRYPT_PROV_DISPATCH -- that flag makes subsequent operations on handles
// derived from it (BCryptGenerateSymmetricKey, BCryptEncrypt, BCryptDecrypt,
// BCryptDestroyKey) usable at DISPATCH_LEVEL. BlorgTlsGlobalInit/BlorgTlsGlobalCleanup
// own that handle (called from DriverEntry/DriverUnload, always PASSIVE);
// BlorgTlsImportKeyHandle mints a per-connection, DISPATCH-usable
// BCRYPT_KEY_HANDLE from it once the handshake derives application traffic
// keys (TlsHandshakeSendClientFinished, already at PASSIVE);
// BlorgTlsAeadEncryptKeyed/BlorgTlsAeadDecryptKeyed then run per-record crypto
// against that cached handle with no PASSIVE-only calls. The plain,
// raw-key-bytes BlorgTlsAeadEncrypt/BlorgTlsAeadDecrypt above remain used by the
// handshake itself, which runs only a handful of times per connection and
// is already PASSIVE.
//
NTSTATUS BlorgTlsGlobalInit(VOID);
VOID BlorgTlsGlobalCleanup(VOID);

NTSTATUS BlorgTlsImportKeyHandle(const UCHAR Key[TLS_KEY_LEN], BCRYPT_KEY_HANDLE* KeyHandleOut);

NTSTATUS BlorgTlsAeadEncryptKeyed(
    BCRYPT_KEY_HANDLE KeyHandle, const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Plaintext, ULONG PlaintextLen,
    UCHAR* TLS_RESTRICT CiphertextOut, UCHAR TagOut[TLS_TAG_LEN]);

//
// Ciphertext/PlaintextOut are deliberately NOT restrict-qualified here,
// unlike BlorgTlsAeadDecrypt/BlorgTlsAeadEncrypt above -- the record-layer receive
// path calls this with Ciphertext == PlaintextOut, decrypting a TLS record
// in place straight into the destination MDL to avoid a copy. BCryptDecrypt
// allows this ("pbInput and pbOutput can be equal, in which case this
// function will perform the decryption in place"); they may only be
// unequal if fully disjoint, with no partial overlap.
//
NTSTATUS BlorgTlsAeadDecryptKeyed(
    BCRYPT_KEY_HANDLE KeyHandle, const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* Ciphertext, ULONG CiphertextLen, const UCHAR Tag[TLS_TAG_LEN],
    UCHAR* PlaintextOut);

#define TLS_ECC_COORD_LEN   32  // P-256 coordinate/private-scalar size
#define TLS_ECC_PUBKEY_LEN  65  // uncompressed SEC1 point: 0x04 || X || Y

//
// Generate a fresh ephemeral P-256 key pair for our side of ECDHE.
// PublicKeyOut is the uncompressed SEC1 point (0x04 || X || Y).
//
NTSTATUS BlorgTlsEcdhGenerateKeyPair(UCHAR PrivateKeyOut[TLS_ECC_COORD_LEN], UCHAR PublicKeyOut[TLS_ECC_PUBKEY_LEN]);

//
// ECDH shared secret. Needs our own key pair's private scalar AND public
// point (BCrypt's private-key blob format requires both to reconstruct a
// full key) plus the peer's public point. Output is the shared point's
// X coordinate, big-endian, 32 bytes -- matching every other byte string
// in this module (and what HKDF-Extract expects as IKM). CNG's
// BCRYPT_KDF_RAW_SECRET returns this byte-reversed relative to that; see
// the reversal in Tls.c.
//
NTSTATUS BlorgTlsEcdhComputeSharedSecret(
    const UCHAR OwnPrivateKey[TLS_ECC_COORD_LEN],
    const UCHAR OwnPublicKey[TLS_ECC_PUBKEY_LEN],
    const UCHAR PeerPublicKey[TLS_ECC_PUBKEY_LEN],
    UCHAR SharedSecretOut[TLS_ECC_COORD_LEN]);

//
// ECDSA P-256 signature verify. Signature is the raw fixed-size r || s
// concatenation (64 bytes) BCryptVerifySignature expects for this
// algorithm -- NOT the DER SEQUENCE{r,s} encoding TLS puts on the wire in
// CertificateVerify. Converting that wire encoding to this raw form is
// the message parser's job, not this primitive's.
//
NTSTATUS BlorgTlsEcdsaVerify(
    const UCHAR PublicKey[TLS_ECC_PUBKEY_LEN],
    const UCHAR* Hash, ULONG HashLen,
    const UCHAR Signature[64]);

//
// Minimal, fixed-shape DER encode/decode for a P-256 SubjectPublicKeyInfo
// (RFC 5480) -- exactly the ASN.1 shape a real X.509 certificate's SPKI
// has for an ECDSA P-256 key:
//   SEQUENCE {
//     SEQUENCE { OID id-ecPublicKey, OID prime256v1 },
//     BIT STRING (0x04 || X || Y)
//   }
// Not a general X.509/ASN.1 library: the decoder fails closed on
// anything that doesn't match this exact byte-for-byte shape (wrong key
// type, unrecognised OID, wrong tag/length structure) rather than trying
// to parse it. TLS_SPKI_DER_LEN is fixed because every field in this
// shape has a fixed, known length -- no variable-length integers appear
// anywhere in a P-256 SPKI.
//
#define TLS_SPKI_DER_LEN 91

NTSTATUS BlorgTlsEncodeP256SubjectPublicKeyInfo(const UCHAR PublicKey[TLS_ECC_PUBKEY_LEN], UCHAR DerOut[TLS_SPKI_DER_LEN]);
NTSTATUS BlorgTlsDecodeP256SubjectPublicKeyInfo(const UCHAR* Der, ULONG DerLen, UCHAR PublicKeyOut[TLS_ECC_PUBKEY_LEN]);

//
// Handshake message construction/parsing, scoped tightly to the single
// ciphersuite this driver supports -- not a general TLS message library.
// Every function here operates on a handshake message BODY (the bytes
// after the 4-byte Handshake header: 1-byte msg type + 3-byte length),
// never on TLS records themselves -- record framing/encryption is the
// caller's job (HttpStageTlsHandshake in Client.c, or the TlsTest
// harness), keeping this module WSK/IRP-agnostic.
//

#define TLS_HANDSHAKE_RANDOM_LEN 32
#define TLS_CLIENT_HELLO_MAX_LEN 300

//
// Builds a minimal ClientHello: exactly one cipher suite
// (TLS_AES_128_GCM_SHA256), one group (secp256r1), one signature
// algorithm (ecdsa_secp256r1_sha256), supported_versions={TLS 1.3} --
// no negotiation matrix, because there's nothing to negotiate. ServerName
// may be NULL/0-length to omit the SNI extension. Buffer must be at
// least TLS_CLIENT_HELLO_MAX_LEN bytes.
//
NTSTATUS BlorgTlsBuildClientHello(
    const UCHAR Random[TLS_HANDSHAKE_RANDOM_LEN],
    const UCHAR ClientPublicKey[TLS_ECC_PUBKEY_LEN],
    const char* ServerName, ULONG ServerNameLen,
    UCHAR* Buffer, ULONG BufferLen, ULONG* MessageLenOut);

//
// Parses a ServerHello body: fails closed unless it selected TLS 1.3
// (supported_versions extension) and our one cipher suite, then extracts
// the server's key_share public point and the server random.
//
NTSTATUS BlorgTlsParseServerHello(
    const UCHAR* Message, ULONG MessageLen,
    UCHAR ServerRandomOut[TLS_HANDSHAKE_RANDOM_LEN],
    UCHAR ServerPublicKeyOut[TLS_ECC_PUBKEY_LEN]);

//
// Extracts the leaf certificate's raw DER bytes from a Certificate
// message body -- ignores any chain certs after it and all per-cert
// extensions, since pinning only needs the leaf's key. LeafCertOut
// points into Message, no copy.
//
NTSTATUS BlorgTlsParseCertificateMessage(
    const UCHAR* Message, ULONG MessageLen,
    const UCHAR** LeafCertOut, ULONG* LeafCertLenOut);

//
// Locates a leaf certificate's SubjectPublicKeyInfo span within its DER
// bytes (still DER -- caller feeds the span to
// BlorgTlsDecodeP256SubjectPublicKeyInfo separately, and to the pin-hash
// comparison). Walks exactly the TBSCertificate fields RFC 5280 defines
// ahead of subjectPublicKeyInfo (an optional version, serialNumber,
// signature AlgorithmIdentifier, issuer, validity, subject) via a generic
// DER TLV skip -- their semantic content is never interpreted, only their
// length, so this works regardless of issuer/subject/validity contents.
//
NTSTATUS BlorgTlsExtractSpkiFromCertificate(
    const UCHAR* CertDer, ULONG CertDerLen,
    const UCHAR** SpkiOut, ULONG* SpkiLenOut);

//
// Parses a CertificateVerify body: fails closed unless the signature
// scheme is ecdsa_secp256r1_sha256 (the only one this driver supports),
// then DER-decodes SEQUENCE{INTEGER r, INTEGER s} into the raw r||s form
// BlorgTlsEcdsaVerify expects (stripping/padding the leading zero DER uses to
// keep an INTEGER non-negative, which raw r||s must not carry).
//
NTSTATUS BlorgTlsParseCertificateVerifyMessage(
    const UCHAR* Message, ULONG MessageLen,
    UCHAR RawSignatureOut[64]);

//
// RFC 8446 4.4.3: the content actually covered by a server
// CertificateVerify signature isn't the transcript hash directly, but
// 64 spaces (0x20) || "TLS 1.3, server CertificateVerify" || 0x00 ||
// Transcript-Hash(Handshake Context, Certificate) -- 130 bytes total.
// This driver only ever verifies a *server's* CertificateVerify (no
// client certificates), so this builds that one fixed context; the
// caller SHA-256s the result and feeds the digest to BlorgTlsEcdsaVerify
// (ECDSA verifies a hash, it doesn't hash internally).
//
#define TLS_CERT_VERIFY_CONTENT_LEN 130

NTSTATUS BlorgTlsBuildServerCertVerifyContent(const UCHAR TranscriptHash[TLS_HASH_LEN], UCHAR ContentOut[TLS_CERT_VERIFY_CONTENT_LEN]);

//
// XOR-accumulate comparison, no early exit -- used for the server
// Finished MAC check, to avoid a timing side channel on that comparison.
//
BOOLEAN BlorgTlsConstantTimeEqual(const UCHAR* A, const UCHAR* B, ULONG Len);

//
// TLSInnerPlaintext = content || content_type || zeros* (RFC 8446 5.2) --
// strips a decrypted record's trailing zero padding and returns the
// real content type plus the actual content length. Returns FALSE if
// the plaintext is all zero bytes (malformed -- there is no content
// type byte to recover).
//
BOOLEAN BlorgTlsStripInnerPlaintext(const UCHAR* Plaintext, ULONG PlaintextLen, UCHAR* ContentTypeOut, ULONG* ContentLenOut);

//
// Per-connection TLS state, embedded in KSOCKET (Socket.h) -- lives for
// exactly as long as the underlying TCP connection, surviving pooled
// reuse, same lifetime as KSOCKET.RemoteAddress. Zero-cost when unused:
// State == TlsHandshakeNotStarted is a plain socket, and every pre-TLS
// code path never touches this struct at all.
//
// WriteKey/IV/Seq and ReadKey/IV/Seq hold whatever the *current* traffic
// keys are -- handshake-traffic keys during the handshake, replaced with
// application-traffic keys once it completes (RFC 8446 7.2's key
// schedule transition). This struct is just storage; whoever drives the
// handshake (HttpStageTlsHandshake in Client.c) owns updating it.
//

// Handshake progress for a TLS connection.
typedef enum _TLS_HANDSHAKE_STATE
{
    TlsHandshakeNotStarted = 0, // matches a zero-initialized KSOCKET: inert plain socket
    TlsHandshakeInProgress,
    TlsHandshakeComplete,
    TlsHandshakeFailed          // never reuse/pool a socket in this state
} TLS_HANDSHAKE_STATE;

// Per-connection TLS key material and handshake state.
typedef struct _TLS_CONNECTION_STATE
{
    TLS_HANDSHAKE_STATE State; // current handshake progress

    UCHAR WriteKey[TLS_KEY_LEN];  // current outbound traffic key
    UCHAR WriteIv[TLS_IV_LEN];    // current outbound static IV
    ULONGLONG WriteSeq;           // outbound record sequence number

    UCHAR ReadKey[TLS_KEY_LEN];   // current inbound traffic key
    UCHAR ReadIv[TLS_IV_LEN];     // current inbound static IV
    ULONGLONG ReadSeq;            // inbound record sequence number

    //
    //  Cached BCRYPT_KEY_HANDLEs imported (BlorgTlsImportKeyHandle) from
    //  WriteKey/ReadKey once the handshake derives them
    //  (TlsHandshakeSendClientFinished) -- what makes the record-layer
    //  hot path DISPATCH-safe; see BlorgTlsAeadEncryptKeyed/BlorgTlsAeadDecryptKeyed
    //  above. NULL until the handshake completes. Destroyed by
    //  BlorgTlsDestroyConnectionState.
    //
    BCRYPT_KEY_HANDLE WriteKeyHandle; // imported handle for WriteKey, DISPATCH-usable
    BCRYPT_KEY_HANDLE ReadKeyHandle;  // imported handle for ReadKey, DISPATCH-usable
} TLS_CONNECTION_STATE, *PTLS_CONNECTION_STATE;

//
// RFC 8446 5.2: TLSInnerPlaintext (content || content_type || padding)
// MUST NOT exceed 2^14 + 1 octets; the AEAD tag adds TLS_TAG_LEN on top
// of that for the on-wire ciphertext. Used to size the record-layer
// receive scratch buffer (Client.c) for the largest legal record.
//
#define TLS_RECORD_CIPHERTEXT_MAX (16384 + 1 + TLS_TAG_LEN)

//
// Zero-inits and sets State = TlsHandshakeNotStarted. Called once when a
// fresh KSOCKET is allocated (Socket.c's slow/connect path) -- a pooled
// socket handed back for reuse already has whatever state its prior
// handshake left it in and must NOT be re-initialized.
//
VOID BlorgTlsInitializeConnectionState(PTLS_CONNECTION_STATE State);

// Zeroes the struct; key material doesn't need to outlive the connection.
VOID BlorgTlsDestroyConnectionState(PTLS_CONNECTION_STATE State);

#ifdef __cplusplus
}
#endif
