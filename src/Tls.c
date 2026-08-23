#ifdef BLORGFS_KERNEL_BUILD
#include "Driver.h"
#else
#include "Tls.h"
#endif

//
// TLS 1.3 crypto primitives and handshake message construction/parsing:
// SHA-256/HMAC/HKDF, AES-GCM record encrypt/decrypt, ECDH key exchange,
// ECDSA verify, DER/SPKI helpers, and ClientHello/ServerHello/Certificate/
// CertificateVerify message codecs. Compiled unmodified into both the
// kernel driver and the usermode TlsTest harness (see Tls.h).
//

//
// Process-wide algorithm provider handles, opened once by BlorgTlsGlobalInit
// and shared by every connection. Opening a provider is a name-resolution
// and refcount round trip that a single TLS 1.3 handshake would otherwise
// pay about thirty times over -- one per transcript hash, one per
// HKDF-Expand-Label HMAC, plus the ECDH and ECDSA operations -- all of it
// on the connection-establishment critical path that a fresh stream walks
// once per pooled connection it opens. Only AES-GCM is opened
// BCRYPT_PROV_DISPATCH (its keys are used from the DISPATCH-level record
// path); the rest are handshake-only and therefore PASSIVE-only. BCrypt
// algorithm handles are safe to create hashes and keys from concurrently,
// which is what makes one shared handle per algorithm correct here.
//
static BCRYPT_ALG_HANDLE TlsAesGcmProvider = NULL;
static BCRYPT_ALG_HANDLE TlsSha256Provider = NULL;
static BCRYPT_ALG_HANDLE TlsHmacSha256Provider = NULL;
static BCRYPT_ALG_HANDLE TlsEcdhP256Provider = NULL;
static BCRYPT_ALG_HANDLE TlsEcdsaP256Provider = NULL;

//
// Yields the shared provider for AlgId when BlorgTlsGlobalInit opened one, in
// which case *OwnedOut is FALSE and the caller must not close it; falls
// back to opening a private one the caller does close. The fallback is
// what keeps the usermode test harnesses working (this file compiles into
// them unmodified, see Tls.h, and they have no driver load to call
// BlorgTlsGlobalInit from) -- in the driver the shared handle is always
// present and nothing is opened per call.
//
static NTSTATUS TlsResolveProvider(
    BCRYPT_ALG_HANDLE Shared,
    LPCWSTR AlgId,
    ULONG Flags,
    BCRYPT_ALG_HANDLE* HandleOut,
    BOOLEAN* OwnedOut)
{
    if (Shared)
    {
        *HandleOut = Shared;
        *OwnedOut = FALSE;
        return STATUS_SUCCESS;
    }

    *OwnedOut = TRUE;
    return BCryptOpenAlgorithmProvider(HandleOut, AlgId, NULL, Flags);
}

NTSTATUS BlorgTlsSha256(const UCHAR* TLS_RESTRICT Data, ULONG DataLen, UCHAR Out[TLS_HASH_LEN])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_HASH_HANDLE hashHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;

    status = TlsResolveProvider(TlsSha256Provider, BCRYPT_SHA256_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptCreateHash(algHandle, &hashHandle, NULL, 0, NULL, 0, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    if (DataLen > 0)
    {
        status = BCryptHashData(hashHandle, C_CAST(PUCHAR, Data), DataLen, 0);

        if (!NT_SUCCESS(status))
        {
            goto cleanup;
        }
    }

    status = BCryptFinishHash(hashHandle, Out, TLS_HASH_LEN, 0);

cleanup:

    if (hashHandle)
    {
        BCryptDestroyHash(hashHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

NTSTATUS BlorgTlsHmacSha256(
    const UCHAR* TLS_RESTRICT Key, ULONG KeyLen,
    const UCHAR* TLS_RESTRICT Data, ULONG DataLen,
    UCHAR Out[TLS_HASH_LEN])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_HASH_HANDLE hashHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;

    status = TlsResolveProvider(
        TlsHmacSha256Provider, BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptCreateHash(algHandle, &hashHandle, NULL, 0, C_CAST(PUCHAR, Key), KeyLen, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    if (DataLen > 0)
    {
        status = BCryptHashData(hashHandle, C_CAST(PUCHAR, Data), DataLen, 0);

        if (!NT_SUCCESS(status))
        {
            goto cleanup;
        }
    }

    status = BCryptFinishHash(hashHandle, Out, TLS_HASH_LEN, 0);

cleanup:

    if (hashHandle)
    {
        BCryptDestroyHash(hashHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

// RFC 5869 extract: PRK = HMAC-Hash(salt, IKM), with salt as the HMAC key.
NTSTATUS BlorgTlsHkdfExtract(
    const UCHAR* TLS_RESTRICT Salt, ULONG SaltLen,
    const UCHAR* TLS_RESTRICT Ikm, ULONG IkmLen,
    UCHAR Out[TLS_HASH_LEN])
{
    return BlorgTlsHmacSha256(Salt, SaltLen, Ikm, IkmLen, Out);
}

//
// RFC 5869 expand. scratch holds the previous T (<= TLS_HASH_LEN) + info +
// a 1-byte counter; 256 bytes is generous headroom for every TLS 1.3 info
// blob BlorgTlsHkdfExpandLabel builds, not a tight fit.
//
NTSTATUS BlorgTlsHkdfExpand(
    const UCHAR* TLS_RESTRICT Prk, ULONG PrkLen,
    const UCHAR* TLS_RESTRICT Info, ULONG InfoLen,
    UCHAR* TLS_RESTRICT Out, ULONG OutLen)
{
    UCHAR t[TLS_HASH_LEN];
    ULONG tLen = 0;
    ULONG generated = 0;
    UCHAR counter = 1;
    NTSTATUS status;
    UCHAR scratch[256];

    if (InfoLen + TLS_HASH_LEN + 1 > sizeof(scratch))
    {
        return STATUS_INVALID_PARAMETER;
    }

    while (generated < OutLen)
    {
        ULONG scratchLen = 0;
        ULONG copyLen;

        if (tLen > 0)
        {
            RtlCopyMemory(scratch, t, tLen);
            scratchLen = tLen;
        }

        RtlCopyMemory(scratch + scratchLen, Info, InfoLen);
        scratchLen += InfoLen;

        scratch[scratchLen] = counter;
        scratchLen += 1;

        status = BlorgTlsHmacSha256(Prk, PrkLen, scratch, scratchLen, t);

        if (!NT_SUCCESS(status))
        {
            return status;
        }

        tLen = TLS_HASH_LEN;

        copyLen = (OutLen - generated < TLS_HASH_LEN) ? (OutLen - generated) : TLS_HASH_LEN;
        RtlCopyMemory(Out + generated, t, copyLen);
        generated += copyLen;

        counter++;
    }

    return STATUS_SUCCESS;
}

//
// Builds HkdfLabel = uint16(Length) || opaque8("tls13 " + Label) ||
// opaque8(Context) and expands it. Label length is counted manually
// rather than via strlen, keeping this file free of any dependency on
// which CRT string routines a given build (kernel vs usermode) links.
//
NTSTATUS BlorgTlsHkdfExpandLabel(
    const UCHAR* TLS_RESTRICT Secret, ULONG SecretLen,
    const char* TLS_RESTRICT Label,
    const UCHAR* TLS_RESTRICT Context, ULONG ContextLen,
    ULONG Length,
    UCHAR* TLS_RESTRICT Out)
{
    UCHAR info[2 + 1 + 6 + 255 + 1 + 255];
    ULONG infoLen = 0;
    ULONG labelLen = 0;
    ULONG fullLabelLen;

    while (Label[labelLen] != '\0')
    {
        labelLen++;
    }

    fullLabelLen = 6 + labelLen;

    if (fullLabelLen > 255 || ContextLen > 255)
    {
        return STATUS_INVALID_PARAMETER;
    }

    info[0] = C_CAST(UCHAR, Length >> 8);
    info[1] = C_CAST(UCHAR, Length & 0xFF);
    infoLen = 2;

    info[infoLen] = C_CAST(UCHAR, fullLabelLen);
    infoLen += 1;

    RtlCopyMemory(info + infoLen, "tls13 ", 6);
    infoLen += 6;

    RtlCopyMemory(info + infoLen, Label, labelLen);
    infoLen += labelLen;

    info[infoLen] = C_CAST(UCHAR, ContextLen);
    infoLen += 1;

    if (ContextLen > 0)
    {
        RtlCopyMemory(info + infoLen, Context, ContextLen);
        infoLen += ContextLen;
    }

    return BlorgTlsHkdfExpand(Secret, SecretLen, info, infoLen, Out, Length);
}

//
// RFC 8446 5.3: nonce = static_iv XOR (seq_num, left-padded with zeros to
// iv_length). iv_length is 12 here and seq_num is 64 bits, so only the
// last 8 bytes of the IV are ever touched -- the first 4 stay as-is.
//
static VOID TlsBuildNonce(const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum, UCHAR NonceOut[TLS_IV_LEN])
{
    ULONG i;

    RtlCopyMemory(NonceOut, StaticIv, TLS_IV_LEN);

    for (i = 0; i < 8; i++)
    {
        UCHAR seqByte = C_CAST(UCHAR, SeqNum >> (8 * (7 - i)));
        NonceOut[TLS_IV_LEN - 8 + i] ^= seqByte;
    }
}

NTSTATUS BlorgTlsAeadEncrypt(
    const UCHAR Key[TLS_KEY_LEN], const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Plaintext, ULONG PlaintextLen,
    UCHAR* TLS_RESTRICT CiphertextOut, UCHAR TagOut[TLS_TAG_LEN])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_KEY_HANDLE keyHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;
    UCHAR nonce[TLS_IV_LEN];
    ULONG resultLen = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;

    TlsBuildNonce(StaticIv, SeqNum, nonce);

    status = TlsResolveProvider(TlsAesGcmProvider, BCRYPT_AES_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    if (ownedProvider)
    {
        status = BCryptSetProperty(algHandle, BCRYPT_CHAINING_MODE,
            C_CAST(PUCHAR, BCRYPT_CHAIN_MODE_GCM), sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

        if (!NT_SUCCESS(status))
        {
            goto cleanup;
        }
    }

    status = BCryptGenerateSymmetricKey(algHandle, &keyHandle, NULL, 0, C_CAST(PUCHAR, Key), TLS_KEY_LEN, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = TLS_IV_LEN;
    authInfo.pbAuthData = C_CAST(PUCHAR, Aad);
    authInfo.cbAuthData = AadLen;
    authInfo.pbTag = TagOut;
    authInfo.cbTag = TLS_TAG_LEN;

    status = BCryptEncrypt(keyHandle, C_CAST(PUCHAR, Plaintext), PlaintextLen, &authInfo,
        NULL, 0, CiphertextOut, PlaintextLen, &resultLen, 0);

cleanup:

    if (keyHandle)
    {
        BCryptDestroyKey(keyHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

NTSTATUS BlorgTlsAeadDecrypt(
    const UCHAR Key[TLS_KEY_LEN], const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Ciphertext, ULONG CiphertextLen, const UCHAR Tag[TLS_TAG_LEN],
    UCHAR* TLS_RESTRICT PlaintextOut)
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_KEY_HANDLE keyHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;
    UCHAR nonce[TLS_IV_LEN];
    ULONG resultLen = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;

    TlsBuildNonce(StaticIv, SeqNum, nonce);

    status = TlsResolveProvider(TlsAesGcmProvider, BCRYPT_AES_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    if (ownedProvider)
    {
        status = BCryptSetProperty(algHandle, BCRYPT_CHAINING_MODE,
            C_CAST(PUCHAR, BCRYPT_CHAIN_MODE_GCM), sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

        if (!NT_SUCCESS(status))
        {
            goto cleanup;
        }
    }

    status = BCryptGenerateSymmetricKey(algHandle, &keyHandle, NULL, 0, C_CAST(PUCHAR, Key), TLS_KEY_LEN, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = TLS_IV_LEN;
    authInfo.pbAuthData = C_CAST(PUCHAR, Aad);
    authInfo.cbAuthData = AadLen;
    authInfo.pbTag = C_CAST(PUCHAR, Tag);
    authInfo.cbTag = TLS_TAG_LEN;

    status = BCryptDecrypt(keyHandle, C_CAST(PUCHAR, Ciphertext), CiphertextLen, &authInfo,
        NULL, 0, PlaintextOut, CiphertextLen, &resultLen, 0);

cleanup:

    if (keyHandle)
    {
        BCryptDestroyKey(keyHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

//
// Opens the process-wide algorithm providers every connection shares (see
// their declarations at the top of this file). Called once at driver load.
// All-or-nothing: any failure closes whatever was already opened and
// returns, leaving every handle NULL, so the TlsResolveProvider fallback
// keeps the crypto working -- just at the old per-call open cost -- rather
// than leaving a half-initialized set behind. AES-GCM is the only one that
// needs BCRYPT_PROV_DISPATCH (Socket.c destroys its keys from the WSK
// close-completion path) and the only one carrying a chaining mode.
//
// BCRYPT_PROV_DISPATCH is what makes keys derived from that provider usable
// at DISPATCH_LEVEL (see BlorgTlsImportKeyHandle) -- required in the real driver,
// where the record-layer hot path runs there, but usermode CNG rejects it
// outright (STATUS_INVALID_PARAMETER). The AES-GCM open is retried without
// the flag on that specific failure, so the usermode test harnesses this
// file also compiles into (Tls.h) can still exercise BlorgTlsImportKeyHandle:
// none of them run anything at a real DISPATCH_LEVEL, so a
// non-DISPATCH-safe handle costs them nothing. In the real driver that
// retry is never reached -- BCRYPT_PROV_DISPATCH always succeeds there.
//
NTSTATUS BlorgTlsGlobalInit(VOID)
{
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &TlsAesGcmProvider, BCRYPT_AES_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);

    if (!NT_SUCCESS(status))
    {
        status = BCryptOpenAlgorithmProvider(&TlsAesGcmProvider, BCRYPT_AES_ALGORITHM, NULL, 0);
    }

    if (!NT_SUCCESS(status))
    {
        TlsAesGcmProvider = NULL;
        return status;
    }

    status = BCryptSetProperty(TlsAesGcmProvider, BCRYPT_CHAINING_MODE,
        C_CAST(PUCHAR, BCRYPT_CHAIN_MODE_GCM), sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

    if (NT_SUCCESS(status))
    {
        status = BCryptOpenAlgorithmProvider(&TlsSha256Provider, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    }

    if (NT_SUCCESS(status))
    {
        status = BCryptOpenAlgorithmProvider(
            &TlsHmacSha256Provider, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    }

    if (NT_SUCCESS(status))
    {
        status = BCryptOpenAlgorithmProvider(&TlsEcdhP256Provider, BCRYPT_ECDH_P256_ALGORITHM, NULL, 0);
    }

    if (NT_SUCCESS(status))
    {
        status = BCryptOpenAlgorithmProvider(&TlsEcdsaP256Provider, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    }

    if (!NT_SUCCESS(status))
    {
        BlorgTlsGlobalCleanup();
        return status;
    }

    return STATUS_SUCCESS;
}

//
// Closes the process-wide providers opened by BlorgTlsGlobalInit. Also the
// unwind path for a partially successful BlorgTlsGlobalInit, hence the
// per-handle NULL checks and the NULLing afterwards.
//
VOID BlorgTlsGlobalCleanup(VOID)
{
    BCRYPT_ALG_HANDLE* providers[] =
    {
        &TlsAesGcmProvider,
        &TlsSha256Provider,
        &TlsHmacSha256Provider,
        &TlsEcdhP256Provider,
        &TlsEcdsaP256Provider
    };

    for (ULONG i = 0; i < RTL_NUMBER_OF(providers); ++i)
    {
        if (*providers[i])
        {
            BCryptCloseAlgorithmProvider(*providers[i], 0);
            *providers[i] = NULL;
        }
    }
}

//
// Wraps a raw AES-GCM key into a BCRYPT_KEY_HANDLE bound to the shared
// dispatch-safe provider, so the returned handle can be used for
// encrypt/decrypt from completion routines running at DISPATCH_LEVEL.
//
NTSTATUS BlorgTlsImportKeyHandle(const UCHAR Key[TLS_KEY_LEN], BCRYPT_KEY_HANDLE* KeyHandleOut)
{
    if (!TlsAesGcmProvider)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return BCryptGenerateSymmetricKey(
        TlsAesGcmProvider, KeyHandleOut, NULL, 0, C_CAST(PUCHAR, Key), TLS_KEY_LEN, 0);
}

NTSTATUS BlorgTlsAeadEncryptKeyed(
    BCRYPT_KEY_HANDLE KeyHandle, const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* TLS_RESTRICT Plaintext, ULONG PlaintextLen,
    UCHAR* TLS_RESTRICT CiphertextOut, UCHAR TagOut[TLS_TAG_LEN])
{
    UCHAR nonce[TLS_IV_LEN];
    ULONG resultLen = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;

    TlsBuildNonce(StaticIv, SeqNum, nonce);

    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = TLS_IV_LEN;
    authInfo.pbAuthData = C_CAST(PUCHAR, Aad);
    authInfo.cbAuthData = AadLen;
    authInfo.pbTag = TagOut;
    authInfo.cbTag = TLS_TAG_LEN;

    return BCryptEncrypt(KeyHandle, C_CAST(PUCHAR, Plaintext), PlaintextLen, &authInfo,
        NULL, 0, CiphertextOut, PlaintextLen, &resultLen, 0);
}

NTSTATUS BlorgTlsAeadDecryptKeyed(
    BCRYPT_KEY_HANDLE KeyHandle, const UCHAR StaticIv[TLS_IV_LEN], ULONGLONG SeqNum,
    const UCHAR* TLS_RESTRICT Aad, ULONG AadLen,
    const UCHAR* Ciphertext, ULONG CiphertextLen, const UCHAR Tag[TLS_TAG_LEN],
    UCHAR* PlaintextOut)
{
    UCHAR nonce[TLS_IV_LEN];
    ULONG resultLen = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;

    TlsBuildNonce(StaticIv, SeqNum, nonce);

    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = TLS_IV_LEN;
    authInfo.pbAuthData = C_CAST(PUCHAR, Aad);
    authInfo.cbAuthData = AadLen;
    authInfo.pbTag = C_CAST(PUCHAR, Tag);
    authInfo.cbTag = TLS_TAG_LEN;

    return BCryptDecrypt(KeyHandle, C_CAST(PUCHAR, Ciphertext), CiphertextLen, &authInfo,
        NULL, 0, PlaintextOut, CiphertextLen, &resultLen, 0);
}

//
// Generates an ephemeral ECDH P-256 key pair, exporting the public point in
// uncompressed X9.62 form (0x04 || X || Y) and the raw private scalar.
//
NTSTATUS BlorgTlsEcdhGenerateKeyPair(UCHAR PrivateKeyOut[TLS_ECC_COORD_LEN], UCHAR PublicKeyOut[TLS_ECC_PUBKEY_LEN])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_KEY_HANDLE keyHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;
    ULONG resultLen;
    UCHAR blob[sizeof(BCRYPT_ECCKEY_BLOB) + 3 * TLS_ECC_COORD_LEN];

    status = TlsResolveProvider(TlsEcdhP256Provider, BCRYPT_ECDH_P256_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptGenerateKeyPair(algHandle, &keyHandle, TLS_ECC_COORD_LEN * 8, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptFinalizeKeyPair(keyHandle, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptExportKey(keyHandle, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof(blob), &resultLen, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    PublicKeyOut[0] = 0x04;
    RtlCopyMemory(PublicKeyOut + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), TLS_ECC_COORD_LEN);
    RtlCopyMemory(PublicKeyOut + 1 + TLS_ECC_COORD_LEN, blob + sizeof(BCRYPT_ECCKEY_BLOB) + TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);
    RtlCopyMemory(PrivateKeyOut, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);

cleanup:

    if (keyHandle)
    {
        BCryptDestroyKey(keyHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

//
// Computes the ECDH P-256 shared secret's X coordinate. The own-key private
// blob must carry the public point (X, Y) alongside the private scalar (d)
// because BCrypt's private-key import has no "scalar-only" form. BCrypt
// returns the X coordinate byte-reversed relative to the big-endian
// convention used everywhere else in this module and in TLS, so the result
// is reversed back before returning.
//
NTSTATUS BlorgTlsEcdhComputeSharedSecret(
    const UCHAR OwnPrivateKey[TLS_ECC_COORD_LEN],
    const UCHAR OwnPublicKey[TLS_ECC_PUBKEY_LEN],
    const UCHAR PeerPublicKey[TLS_ECC_PUBKEY_LEN],
    UCHAR SharedSecretOut[TLS_ECC_COORD_LEN])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_KEY_HANDLE ownKeyHandle = NULL;
    BCRYPT_KEY_HANDLE peerKeyHandle = NULL;
    BCRYPT_SECRET_HANDLE secretHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;
    ULONG resultLen;
    UCHAR ownPrivBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 3 * TLS_ECC_COORD_LEN];
    BCRYPT_ECCKEY_BLOB* ownHeader = C_CAST(BCRYPT_ECCKEY_BLOB*, ownPrivBlob);
    UCHAR peerPubBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN];
    BCRYPT_ECCKEY_BLOB* peerHeader = C_CAST(BCRYPT_ECCKEY_BLOB*, peerPubBlob);

    ownHeader->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    ownHeader->cbKey = TLS_ECC_COORD_LEN;
    RtlCopyMemory(ownPrivBlob + sizeof(BCRYPT_ECCKEY_BLOB), OwnPublicKey + 1, TLS_ECC_COORD_LEN);
    RtlCopyMemory(ownPrivBlob + sizeof(BCRYPT_ECCKEY_BLOB) + TLS_ECC_COORD_LEN, OwnPublicKey + 1 + TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);
    RtlCopyMemory(ownPrivBlob + sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN, OwnPrivateKey, TLS_ECC_COORD_LEN);

    peerHeader->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    peerHeader->cbKey = TLS_ECC_COORD_LEN;
    RtlCopyMemory(peerPubBlob + sizeof(BCRYPT_ECCKEY_BLOB), PeerPublicKey + 1, TLS_ECC_COORD_LEN);
    RtlCopyMemory(peerPubBlob + sizeof(BCRYPT_ECCKEY_BLOB) + TLS_ECC_COORD_LEN, PeerPublicKey + 1 + TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);

    status = TlsResolveProvider(TlsEcdhP256Provider, BCRYPT_ECDH_P256_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptImportKeyPair(algHandle, NULL, BCRYPT_ECCPRIVATE_BLOB, &ownKeyHandle, ownPrivBlob, sizeof(ownPrivBlob), 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptImportKeyPair(algHandle, NULL, BCRYPT_ECCPUBLIC_BLOB, &peerKeyHandle, peerPubBlob, sizeof(peerPubBlob), 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptSecretAgreement(ownKeyHandle, peerKeyHandle, &secretHandle, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptDeriveKey(secretHandle, BCRYPT_KDF_RAW_SECRET, NULL, SharedSecretOut, TLS_ECC_COORD_LEN, &resultLen, 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    {
        ULONG i;

        for (i = 0; i < TLS_ECC_COORD_LEN / 2; i++)
        {
            UCHAR tmp = SharedSecretOut[i];
            SharedSecretOut[i] = SharedSecretOut[TLS_ECC_COORD_LEN - 1 - i];
            SharedSecretOut[TLS_ECC_COORD_LEN - 1 - i] = tmp;
        }
    }

cleanup:

    if (secretHandle)
    {
        BCryptDestroySecret(secretHandle);
    }

    if (peerKeyHandle)
    {
        BCryptDestroyKey(peerKeyHandle);
    }

    if (ownKeyHandle)
    {
        BCryptDestroyKey(ownKeyHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

NTSTATUS BlorgTlsEcdsaVerify(
    const UCHAR PublicKey[TLS_ECC_PUBKEY_LEN],
    const UCHAR* Hash, ULONG HashLen,
    const UCHAR Signature[64])
{
    BCRYPT_ALG_HANDLE algHandle = NULL;
    BCRYPT_KEY_HANDLE keyHandle = NULL;
    BOOLEAN ownedProvider = FALSE;
    NTSTATUS status;
    UCHAR pubBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * TLS_ECC_COORD_LEN];
    BCRYPT_ECCKEY_BLOB* header = C_CAST(BCRYPT_ECCKEY_BLOB*, pubBlob);

    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = TLS_ECC_COORD_LEN;
    RtlCopyMemory(pubBlob + sizeof(BCRYPT_ECCKEY_BLOB), PublicKey + 1, TLS_ECC_COORD_LEN);
    RtlCopyMemory(pubBlob + sizeof(BCRYPT_ECCKEY_BLOB) + TLS_ECC_COORD_LEN, PublicKey + 1 + TLS_ECC_COORD_LEN, TLS_ECC_COORD_LEN);

    status = TlsResolveProvider(TlsEcdsaP256Provider, BCRYPT_ECDSA_P256_ALGORITHM, 0, &algHandle, &ownedProvider);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptImportKeyPair(algHandle, NULL, BCRYPT_ECCPUBLIC_BLOB, &keyHandle, pubBlob, sizeof(pubBlob), 0);

    if (!NT_SUCCESS(status))
    {
        goto cleanup;
    }

    status = BCryptVerifySignature(keyHandle, NULL, C_CAST(PUCHAR, Hash), HashLen, C_CAST(PUCHAR, Signature), 64, 0);

cleanup:

    if (keyHandle)
    {
        BCryptDestroyKey(keyHandle);
    }

    if (algHandle && ownedProvider)
    {
        BCryptCloseAlgorithmProvider(algHandle, 0);
    }

    return status;
}

//
// Fills in the fixed RFC 5480 SubjectPublicKeyInfo template for id-ecPublicKey
// + prime256v1 (SEQUENCE(89) { SEQUENCE(19) { OID id-ecPublicKey(9), OID
// prime256v1(10) }, BIT STRING(66) { 0 unused bits, 0x04||X||Y(65) } }) with
// the given uncompressed public point. Every field has a fixed, known length
// -- no ASN.1 variable-length integers appear anywhere in a P-256 SPKI, so
// this is a template fill-in, not a general encoder.
//
NTSTATUS BlorgTlsEncodeP256SubjectPublicKeyInfo(const UCHAR PublicKey[TLS_ECC_PUBKEY_LEN], UCHAR DerOut[TLS_SPKI_DER_LEN])
{
    static const UCHAR header[] =
    {
        0x30, 0x59,
        0x30, 0x13,
        0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
        0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
        0x03, 0x42, 0x00
    };

    RtlCopyMemory(DerOut, header, sizeof(header));
    RtlCopyMemory(DerOut + sizeof(header), PublicKey, TLS_ECC_PUBKEY_LEN);

    return STATUS_SUCCESS;
}

//
// Validates a DER SubjectPublicKeyInfo against the exact fixed P-256 header
// and extracts the uncompressed public point. Fails closed on anything that
// doesn't match this exact shape -- wrong key type, unrecognised OID, or
// malformed structure all land here rather than being partially parsed --
// and rejects any point not in uncompressed (0x04-prefixed) form.
//
NTSTATUS BlorgTlsDecodeP256SubjectPublicKeyInfo(const UCHAR* Der, ULONG DerLen, UCHAR PublicKeyOut[TLS_ECC_PUBKEY_LEN])
{
    static const UCHAR expectedHeader[] =
    {
        0x30, 0x59,
        0x30, 0x13,
        0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
        0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
        0x03, 0x42, 0x00
    };

    if (DerLen != TLS_SPKI_DER_LEN)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!RtlEqualMemory(Der, expectedHeader, sizeof(expectedHeader)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(PublicKeyOut, Der + sizeof(expectedHeader), TLS_ECC_PUBKEY_LEN);

    if (0x04 != PublicKeyOut[0])
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

//
// Handshake message construction/parsing (see Tls.h). Wire-format
// constants are file-local -- callers only ever deal in parsed values or
// opaque buffers, never these numbers directly.
//
#define TLS_EXT_SERVER_NAME          0x0000
#define TLS_EXT_SUPPORTED_GROUPS     0x000A
#define TLS_EXT_SIGNATURE_ALGORITHMS 0x000D
#define TLS_EXT_SUPPORTED_VERSIONS   0x002B
#define TLS_EXT_KEY_SHARE            0x0033

#define TLS_GROUP_SECP256R1                 0x0017
#define TLS_CIPHER_SUITE_AES_128_GCM_SHA256 0x1301
#define TLS_SIGALG_ECDSA_SECP256R1_SHA256    0x0403
#define TLS_VERSION_1_3                     0x0304

// Writes Value as big-endian (network order), matching TLS wire format.
static VOID TlsPutUint16(UCHAR* Buffer, USHORT Value)
{
    Buffer[0] = C_CAST(UCHAR, Value >> 8);
    Buffer[1] = C_CAST(UCHAR, Value & 0xFF);
}

// Reads a big-endian (network order) 16-bit value, matching TLS wire format.
static USHORT TlsGetUint16(const UCHAR* Buffer)
{
    return C_CAST(USHORT, (C_CAST(USHORT, Buffer[0]) << 8) | Buffer[1]);
}

//
// Writes the low 24 bits of Value as big-endian, matching the 3-byte length
// fields used throughout TLS handshake message framing.
//
static VOID TlsPutUint24(UCHAR* Buffer, ULONG Value)
{
    Buffer[0] = C_CAST(UCHAR, (Value >> 16) & 0xFF);
    Buffer[1] = C_CAST(UCHAR, (Value >> 8) & 0xFF);
    Buffer[2] = C_CAST(UCHAR, Value & 0xFF);
}

//
// Builds a ClientHello with a single cipher suite (AES-128-GCM-SHA256),
// group (secp256r1), and signature algorithm (ecdsa_secp256r1_sha256), plus
// an optional SNI extension. Every length field is computed from what was
// actually written, never hand-counted. Unlike every other extension here,
// supported_versions' version list uses a single-BYTE length prefix (max
// 254 bytes / 127 versions per RFC 8446 4.2.1), not uint16 -- do not
// "normalize" it to match the others.
//
NTSTATUS BlorgTlsBuildClientHello(
    const UCHAR Random[TLS_HANDSHAKE_RANDOM_LEN],
    const UCHAR ClientPublicKey[TLS_ECC_PUBKEY_LEN],
    const char* ServerName, ULONG ServerNameLen,
    UCHAR* Buffer, ULONG BufferLen, ULONG* MessageLenOut)
{
    ULONG offset = 0;
    ULONG lengthFieldOffset;
    ULONG bodyStart;
    ULONG extensionsLenOffset;
    ULONG extensionsStart;

    if (BufferLen < 4)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Buffer[offset++] = 0x01;
    lengthFieldOffset = offset;
    offset += 3;
    bodyStart = offset;

    if (offset + 2 + TLS_HANDSHAKE_RANDOM_LEN + 1 + 2 + 2 + 1 + 1 + 2 > BufferLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Buffer[offset++] = 0x03;
    Buffer[offset++] = 0x03;

    RtlCopyMemory(Buffer + offset, Random, TLS_HANDSHAKE_RANDOM_LEN);
    offset += TLS_HANDSHAKE_RANDOM_LEN;

    Buffer[offset++] = 0x00;

    TlsPutUint16(Buffer + offset, 2); offset += 2;
    TlsPutUint16(Buffer + offset, TLS_CIPHER_SUITE_AES_128_GCM_SHA256); offset += 2;

    Buffer[offset++] = 0x01;
    Buffer[offset++] = 0x00;

    extensionsLenOffset = offset;
    offset += 2;
    extensionsStart = offset;

    if (NULL != ServerName && ServerNameLen > 0)
    {
        ULONG extContentLen = 2 + 1 + 2 + ServerNameLen;

        if (offset + 4 + extContentLen > BufferLen)
        {
            return STATUS_INVALID_PARAMETER;
        }

        TlsPutUint16(Buffer + offset, TLS_EXT_SERVER_NAME); offset += 2;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, extContentLen)); offset += 2;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, 1 + 2 + ServerNameLen)); offset += 2;
        Buffer[offset++] = 0x00;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, ServerNameLen)); offset += 2;
        RtlCopyMemory(Buffer + offset, ServerName, ServerNameLen);
        offset += ServerNameLen;
    }

    if (offset + 8 > BufferLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    TlsPutUint16(Buffer + offset, TLS_EXT_SUPPORTED_GROUPS); offset += 2;
    TlsPutUint16(Buffer + offset, 4); offset += 2;
    TlsPutUint16(Buffer + offset, 2); offset += 2;
    TlsPutUint16(Buffer + offset, TLS_GROUP_SECP256R1); offset += 2;

    {
        ULONG listContentLen = 2 + 2 + TLS_ECC_PUBKEY_LEN;
        ULONG extContentLen = 2 + listContentLen;

        if (offset + 4 + extContentLen > BufferLen)
        {
            return STATUS_INVALID_PARAMETER;
        }

        TlsPutUint16(Buffer + offset, TLS_EXT_KEY_SHARE); offset += 2;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, extContentLen)); offset += 2;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, listContentLen)); offset += 2;
        TlsPutUint16(Buffer + offset, TLS_GROUP_SECP256R1); offset += 2;
        TlsPutUint16(Buffer + offset, C_CAST(USHORT, TLS_ECC_PUBKEY_LEN)); offset += 2;
        RtlCopyMemory(Buffer + offset, ClientPublicKey, TLS_ECC_PUBKEY_LEN);
        offset += TLS_ECC_PUBKEY_LEN;
    }

    if (offset + 7 + 8 > BufferLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    TlsPutUint16(Buffer + offset, TLS_EXT_SUPPORTED_VERSIONS); offset += 2;
    TlsPutUint16(Buffer + offset, 3); offset += 2;
    Buffer[offset++] = 2;
    TlsPutUint16(Buffer + offset, TLS_VERSION_1_3); offset += 2;

    TlsPutUint16(Buffer + offset, TLS_EXT_SIGNATURE_ALGORITHMS); offset += 2;
    TlsPutUint16(Buffer + offset, 4); offset += 2;
    TlsPutUint16(Buffer + offset, 2); offset += 2;
    TlsPutUint16(Buffer + offset, TLS_SIGALG_ECDSA_SECP256R1_SHA256); offset += 2;

    TlsPutUint16(Buffer + extensionsLenOffset, C_CAST(USHORT, offset - extensionsStart));
    TlsPutUint24(Buffer + lengthFieldOffset, offset - bodyStart);

    *MessageLenOut = offset;
    return STATUS_SUCCESS;
}

//
// Parses a ServerHello, requiring the AES-128-GCM-SHA256 cipher suite and
// confirming TLS 1.3 + a usable key share via extensions (legacy_version is
// not authoritative; the supported_versions extension is). Unlike
// ClientHello, ServerHello's supported_versions and key_share extensions
// each carry a single value, not a length-prefixed list. Unrecognized
// extensions are ignored, not an error -- a ServerHello may legitimately
// include extensions this driver has no need to act on.
//
NTSTATUS BlorgTlsParseServerHello(
    const UCHAR* Message, ULONG MessageLen,
    UCHAR ServerRandomOut[TLS_HANDSHAKE_RANDOM_LEN],
    UCHAR ServerPublicKeyOut[TLS_ECC_PUBKEY_LEN])
{
    ULONG offset = 0;
    ULONG extensionsLen;
    ULONG extEnd;
    BOOLEAN foundVersion = FALSE;
    BOOLEAN foundKeyShare = FALSE;

    if (MessageLen < 2 + TLS_HANDSHAKE_RANDOM_LEN + 1 + 2 + 1 + 2)
    {
        return STATUS_INVALID_PARAMETER;
    }

    offset += 2;

    RtlCopyMemory(ServerRandomOut, Message + offset, TLS_HANDSHAKE_RANDOM_LEN);
    offset += TLS_HANDSHAKE_RANDOM_LEN;

    {
        ULONG sessionIdLen = Message[offset];
        offset += 1;

        if (offset + sessionIdLen > MessageLen)
        {
            return STATUS_INVALID_PARAMETER;
        }

        offset += sessionIdLen;
    }

    if (offset + 2 > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (TLS_CIPHER_SUITE_AES_128_GCM_SHA256 != TlsGetUint16(Message + offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    offset += 2;
    offset += 1;

    if (offset + 2 > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    extensionsLen = TlsGetUint16(Message + offset);
    offset += 2;

    if (offset + extensionsLen > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    extEnd = offset + extensionsLen;

    while (offset + 4 <= extEnd)
    {
        USHORT extType = TlsGetUint16(Message + offset);
        USHORT extLen = TlsGetUint16(Message + offset + 2);
        offset += 4;

        if (offset + extLen > extEnd)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (TLS_EXT_SUPPORTED_VERSIONS == extType)
        {
            if (2 != extLen || TLS_VERSION_1_3 != TlsGetUint16(Message + offset))
            {
                return STATUS_INVALID_PARAMETER;
            }

            foundVersion = TRUE;
        }
        else if (TLS_EXT_KEY_SHARE == extType)
        {
            if (2 + 2 + TLS_ECC_PUBKEY_LEN != extLen ||
                TLS_GROUP_SECP256R1 != TlsGetUint16(Message + offset) ||
                TLS_ECC_PUBKEY_LEN != TlsGetUint16(Message + offset + 2))
            {
                return STATUS_INVALID_PARAMETER;
            }

            RtlCopyMemory(ServerPublicKeyOut, Message + offset + 4, TLS_ECC_PUBKEY_LEN);

            if (0x04 != ServerPublicKeyOut[0])
            {
                return STATUS_INVALID_PARAMETER;
            }

            foundKeyShare = TRUE;
        }

        offset += extLen;
    }

    if (!foundVersion || !foundKeyShare)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

//
// Parses a Certificate message and returns only the first (leaf) entry's
// DER bytes. Any further chain certs after the leaf are intentionally
// ignored -- pinning only needs the leaf's key, not the rest of the chain.
//
NTSTATUS BlorgTlsParseCertificateMessage(
    const UCHAR* Message, ULONG MessageLen,
    const UCHAR** LeafCertOut, ULONG* LeafCertLenOut)
{
    ULONG offset;
    ULONG contextLen;
    ULONG certListLen;
    ULONG certDataLen;

    if (MessageLen < 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    contextLen = Message[0];
    offset = 1 + contextLen;

    if (offset + 3 > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    certListLen = (C_CAST(ULONG, Message[offset]) << 16) | (C_CAST(ULONG, Message[offset + 1]) << 8) | Message[offset + 2];
    offset += 3;

    if (certListLen < 3 || offset + certListLen > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    certDataLen = (C_CAST(ULONG, Message[offset]) << 16) | (C_CAST(ULONG, Message[offset + 1]) << 8) | Message[offset + 2];
    offset += 3;

    if (offset + certDataLen > MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *LeafCertOut = Message + offset;
    *LeafCertLenOut = certDataLen;
    return STATUS_SUCCESS;
}

//
// Reads a DER TLV header at *Data, advancing *Data past the tag+length
// to point at the value; returns the value's length via *ValueLen.
// Handles short-form and 1/2-byte long-form lengths -- sufficient for
// every field in a leaf certificate signed by any well-known CA (none
// of them are large enough to need a 3+ byte length). Fails closed
// (returns FALSE) on the indefinite-length form, longer encodings, or
// running past End.
//
static BOOLEAN TlsDerReadTlv(const UCHAR** Data, const UCHAR* End, UCHAR ExpectedTag, ULONG* ValueLen)
{
    const UCHAR* p = *Data;
    ULONG len;

    if (p >= End || *p != ExpectedTag)
    {
        return FALSE;
    }

    p++;

    if (p >= End)
    {
        return FALSE;
    }

    if (0 == (*p & 0x80))
    {
        len = *p;
        p++;
    }
    else
    {
        ULONG lenBytes = *p & 0x7F;
        ULONG i;

        p++;

        if (0 == lenBytes || lenBytes > 2 || C_CAST(ULONG, End - p) < lenBytes)
        {
            return FALSE;
        }

        len = 0;

        for (i = 0; i < lenBytes; i++)
        {
            len = (len << 8) | p[i];
        }

        p += lenBytes;
    }

    if (C_CAST(ULONG, End - p) < len)
    {
        return FALSE;
    }

    *ValueLen = len;
    *Data = p;
    return TRUE;
}

//
// Same header parsing as TlsDerReadTlv, but the tag isn't checked (the
// caller doesn't care what field this is, only how many bytes to skip
// past it) and *Data lands after the value rather than at its start.
//
static BOOLEAN TlsDerSkipTlv(const UCHAR** Data, const UCHAR* End)
{
    const UCHAR* p = *Data;
    ULONG len;

    if (p >= End)
    {
        return FALSE;
    }

    p++;

    if (p >= End)
    {
        return FALSE;
    }

    if (0 == (*p & 0x80))
    {
        len = *p;
        p++;
    }
    else
    {
        ULONG lenBytes = *p & 0x7F;
        ULONG i;

        p++;

        if (0 == lenBytes || lenBytes > 2 || C_CAST(ULONG, End - p) < lenBytes)
        {
            return FALSE;
        }

        len = 0;

        for (i = 0; i < lenBytes; i++)
        {
            len = (len << 8) | p[i];
        }

        p += lenBytes;
    }

    if (C_CAST(ULONG, End - p) < len)
    {
        return FALSE;
    }

    *Data = p + len;
    return TRUE;
}

//
// Walks Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm,
// signatureValue } and tbsCertificate ::= SEQUENCE { version?, serialNumber,
// signature, issuer, validity, subject, subjectPublicKeyInfo, ... } to reach
// subjectPublicKeyInfo, skipping every other field by length only (their
// content is never interpreted). Returns the SPKI's full TLV span
// (tag+length+value), not just its value, since
// BlorgTlsDecodeP256SubjectPublicKeyInfo expects to see the leading SEQUENCE tag
// itself.
//
NTSTATUS BlorgTlsExtractSpkiFromCertificate(
    const UCHAR* CertDer, ULONG CertDerLen,
    const UCHAR** SpkiOut, ULONG* SpkiLenOut)
{
    const UCHAR* p = CertDer;
    const UCHAR* end = CertDer + CertDerLen;
    const UCHAR* tbsEnd;
    const UCHAR* spkiStart;
    ULONG len;

    if (!TlsDerReadTlv(&p, end, 0x30, &len))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!TlsDerReadTlv(&p, end, 0x30, &len))
    {
        return STATUS_INVALID_PARAMETER;
    }

    tbsEnd = p + len;

    if (p < tbsEnd && 0xA0 == *p)
    {
        if (!TlsDerSkipTlv(&p, tbsEnd))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (!TlsDerSkipTlv(&p, tbsEnd) ||
        !TlsDerSkipTlv(&p, tbsEnd) ||
        !TlsDerSkipTlv(&p, tbsEnd) ||
        !TlsDerSkipTlv(&p, tbsEnd) ||
        !TlsDerSkipTlv(&p, tbsEnd))
    {
        return STATUS_INVALID_PARAMETER;
    }

    spkiStart = p;

    if (!TlsDerReadTlv(&p, tbsEnd, 0x30, &len))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *SpkiOut = spkiStart;
    *SpkiLenOut = C_CAST(ULONG, (p + len) - spkiStart);
    return STATUS_SUCCESS;
}

//
// Parses a CertificateVerify message (ecdsa_secp256r1_sha256 only) and
// converts its DER SEQUENCE { INTEGER r, INTEGER s } signature to the raw
// r || s (32 + 32 bytes) form BCryptVerifySignature expects. DER INTEGERs
// are variable-length and may carry a leading 0x00 to stay non-negative
// (P-256's field/order both have their top bit sometimes set); that padding
// byte is stripped, and a shorter-than-32-byte value (leading zero bytes
// not encoded in DER) is left-zero-padded back out to 32 the other way.
//
NTSTATUS BlorgTlsParseCertificateVerifyMessage(
    const UCHAR* Message, ULONG MessageLen,
    UCHAR RawSignatureOut[64])
{
    ULONG sigLen;
    const UCHAR* der;
    const UCHAR* p;
    const UCHAR* end;
    const UCHAR* seqEnd;
    ULONG rLen, sLen;
    const UCHAR* rStart;
    const UCHAR* sStart;

    if (MessageLen < 4)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (TLS_SIGALG_ECDSA_SECP256R1_SHA256 != TlsGetUint16(Message))
    {
        return STATUS_INVALID_PARAMETER;
    }

    sigLen = TlsGetUint16(Message + 2);

    if (4 + sigLen != MessageLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    der = Message + 4;
    p = der;
    end = der + sigLen;

    if (!TlsDerReadTlv(&p, end, 0x30, &sigLen))
    {
        return STATUS_INVALID_PARAMETER;
    }

    seqEnd = p + sigLen;

    if (!TlsDerReadTlv(&p, seqEnd, 0x02, &rLen))
    {
        return STATUS_INVALID_PARAMETER;
    }

    rStart = p;
    p += rLen;

    if (!TlsDerReadTlv(&p, seqEnd, 0x02, &sLen))
    {
        return STATUS_INVALID_PARAMETER;
    }

    sStart = p;

    if (33 == rLen && 0x00 == rStart[0])
    {
        rStart++;
        rLen--;
    }

    if (33 == sLen && 0x00 == sStart[0])
    {
        sStart++;
        sLen--;
    }

    if (rLen > 32 || sLen > 32 || 0 == rLen || 0 == sLen)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(RawSignatureOut, 64);
    RtlCopyMemory(RawSignatureOut + (32 - rLen), rStart, rLen);
    RtlCopyMemory(RawSignatureOut + 32 + (32 - sLen), sStart, sLen);

    return STATUS_SUCCESS;
}

//
// Builds the exact byte sequence the server signs for CertificateVerify:
// 64 spaces, the fixed context string "TLS 1.3, server CertificateVerify"
// (33 bytes, stored as raw hex rather than an ASCII string literal), a
// 0x00 separator, then the transcript hash, per RFC 8446 4.4.3. Returns
// failure if the assembled length doesn't match TLS_CERT_VERIFY_CONTENT_LEN
// in Tls.h, which would indicate that constant is wrong.
//
NTSTATUS BlorgTlsBuildServerCertVerifyContent(const UCHAR TranscriptHash[TLS_HASH_LEN], UCHAR ContentOut[TLS_CERT_VERIFY_CONTENT_LEN])
{
    static const UCHAR contextString[] =
    {
        0x54, 0x4c, 0x53, 0x20, 0x31, 0x2e, 0x33, 0x2c, 0x20, 0x73, 0x65, 0x72,
        0x76, 0x65, 0x72, 0x20, 0x43, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69, 0x63,
        0x61, 0x74, 0x65, 0x56, 0x65, 0x72, 0x69, 0x66, 0x79
    };

    ULONG offset = 0;

    RtlFillMemory(ContentOut, 64, 0x20);
    offset += 64;

    RtlCopyMemory(ContentOut + offset, contextString, sizeof(contextString));
    offset += sizeof(contextString);

    ContentOut[offset] = 0x00;
    offset += 1;

    RtlCopyMemory(ContentOut + offset, TranscriptHash, TLS_HASH_LEN);
    offset += TLS_HASH_LEN;

    return (TLS_CERT_VERIFY_CONTENT_LEN == offset) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

//
// Compares two buffers without early-exit branching on the result, so
// comparison time doesn't leak how many leading bytes matched (used for
// MAC/pin comparisons).
//
BOOLEAN BlorgTlsConstantTimeEqual(const UCHAR* A, const UCHAR* B, ULONG Len)
{
    UCHAR diff = 0;
    ULONG i;

    for (i = 0; i < Len; i++)
    {
        diff |= C_CAST(UCHAR, A[i] ^ B[i]);
    }

    return 0 == diff;
}

//
// Strips the TLSInnerPlaintext zero-padding and trailing real content-type
// byte from a decrypted record (RFC 8446 5.2), returning the content type
// and the length of the content preceding it.
//
BOOLEAN BlorgTlsStripInnerPlaintext(const UCHAR* Plaintext, ULONG PlaintextLen, UCHAR* ContentTypeOut, ULONG* ContentLenOut)
{
    ULONG i = PlaintextLen;

    while (i > 0 && 0 == Plaintext[i - 1])
    {
        i--;
    }

    if (0 == i)
    {
        return FALSE;
    }

    *ContentTypeOut = Plaintext[i - 1];
    *ContentLenOut = i - 1;
    return TRUE;
}

//
// Zero-initializes a connection's TLS state and sets it to the not-started
// stage.
//
VOID BlorgTlsInitializeConnectionState(PTLS_CONNECTION_STATE State)
{
    RtlZeroMemory(State, sizeof(TLS_CONNECTION_STATE));
    State->State = TlsHandshakeNotStarted;
}

//
// Releases a connection's AEAD key handles, if any, and zeroes the state.
// BCryptDestroyKey is DISPATCH-safe here because the handle traces back to
// TlsAesGcmProvider (BlorgTlsGlobalInit), which was opened with
// BCRYPT_PROV_DISPATCH -- required because Socket.c calls this from the WSK
// close-completion path, which runs at DISPATCH_LEVEL.
//
VOID BlorgTlsDestroyConnectionState(PTLS_CONNECTION_STATE State)
{
    if (State->WriteKeyHandle)
    {
        BCryptDestroyKey(State->WriteKeyHandle);
    }

    if (State->ReadKeyHandle)
    {
        BCryptDestroyKey(State->ReadKeyHandle);
    }

    RtlZeroMemory(State, sizeof(TLS_CONNECTION_STATE));
}
