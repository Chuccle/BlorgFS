#include "Driver.h"
#include "Socket.h"
#include "TlsHandshake.h"

//
// Kernel-specific I/O glue for the TLS handshake -- see TlsHandshake.h.
// All protocol logic (message construction/parsing, key schedule, crypto)
// lives in Tls.c; this file drives that sequence as an async,
// WSK-completion-driven state machine instead of blocking Winsock calls.
//

#define TLS_HS_TAG 'HSLT'

//
// Customer-defined NTSTATUS (bit 29 set) in FACILITY_NTCERT (0x8) for a
// certificate pin mismatch, distinct from generic parse-failure codes.
// Severity=Error(3), Customer=1, Facility=FACILITY_NTCERT(0x8), Code=1.
//
#define STATUS_BLORGFS_CERT_PIN_MISMATCH ((NTSTATUS)0xE0080001L)

//
// A received record is capped at TLS_RECORD_CIPHERTEXT_MAX (Tls.h), not at
// the 2^14 plaintext maximum: RFC 8446 5.1 caps TLSPlaintext.length at
// 2^14, but 5.2 lets TLSCiphertext.length carry that plus the inner
// content type and the AEAD tag. A server fragmenting a large certificate
// chain at the plaintext maximum emits exactly that, so capping at 2^14
// rejected a legal record 17 bytes over and failed the handshake outright.
// The same constant is what Client.c's post-handshake drain and Socket.c's
// accumulator sizing already used, so this is one shared definition rather
// than a second one free to drift out of step with them again.
//
#define TLS_HS_FLIGHT_MAX     32768  // 2x max record: comfortable headroom for
                                      // a flight spanning a couple of records
#define TLS_HS_TRANSCRIPT_MAX  8192  // generous headroom for a realistic cert chain

//
// Configured certificate pin, protected by a single push lock so
// Configured is never TRUE while Value is only half-written.
//
typedef struct _TLS_PIN_STATE
{
    EX_PUSH_LOCK Lock;          // guards Value and Configured together
    UCHAR Value[TLS_HASH_LEN];  // SHA-256 of the pinned certificate's SPKI
    BOOLEAN Configured;         // TRUE once a pin has been set
} TLS_PIN_STATE;

static TLS_PIN_STATE TlsPin;

//
// EX_PUSH_LOCK's zero-initialized state happens to be directly usable on
// real hardware, but every other push lock in this driver (PathCache.c,
// Structs.c) is explicitly initialized before first use, and TlsPin.Lock
// was the one silent exception. Called once from DriverEntry, ahead of the
// registry read that may call BlorgTlsSetPin.
//
VOID BlorgTlsHandshakeGlobalInit(VOID)
{
    ExInitializePushLock(&TlsPin.Lock);
}

//
// Sets the configured certificate pin under exclusive lock so a concurrent
// reader in BlorgTlsCheckPin never observes Configured=TRUE with a partially
// written Value.
//
NTSTATUS BlorgTlsSetPin(const UCHAR Pin[TLS_HASH_LEN])
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&TlsPin.Lock);

    RtlCopyMemory(TlsPin.Value, Pin, TLS_HASH_LEN);
    TlsPin.Configured = TRUE;

    ExReleasePushLockExclusive(&TlsPin.Lock);
    KeLeaveCriticalRegion();

    return STATUS_SUCCESS;
}

//
// Hashes the given SPKI and compares it to the configured pin in constant
// time. Returns FALSE (fail closed) if no pin has been configured yet --
// the explicit Configured check fails closed rather than relying on an
// all-zero Value never matching a real SHA-256 digest.
//
BOOLEAN BlorgTlsCheckPin(const UCHAR* Spki, ULONG SpkiLen)
{
    UCHAR computed[TLS_HASH_LEN];

    if (!NT_SUCCESS(BlorgTlsSha256(Spki, SpkiLen, computed)))
    {
        return FALSE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&TlsPin.Lock);

    BOOLEAN match = TlsPin.Configured && BlorgTlsConstantTimeEqual(computed, TlsPin.Value, TLS_HASH_LEN);

    ExReleasePushLockShared(&TlsPin.Lock);
    KeLeaveCriticalRegion();

    return match;
}

#define TLS_HS_STACK_SAFETY_MARGIN (PAGE_SIZE * 2)
#define TLS_HS_STACK_EXPAND_SIZE   (PAGE_SIZE * 8)

typedef enum _TLS_HS_STAGE
{
    TlsHsWaitingForServerHello,
    TlsHsWaitingForFlightRecord
} TLS_HS_STAGE;

// Per-handshake state carried across the async WSK completion chain.
typedef struct _TLS_HANDSHAKE_CONTEXT
{
    PKSOCKET Socket;                                    // socket the handshake runs over

    //
    // QPC stamp taken as the handshake starts, so both terminal paths can
    // report how long it took (Statistics.h). Handshake latency is
    // per-connection rather than per-request, and lands squarely on
    // stream startup: a fresh stream opens up to PREFETCH_DEPTH
    // connections at once, each paying this before its first byte moves.
    // Captured into a local before the context is zeroed on the way out.
    //
    LONG64 IssueQpc;
    PBLORG_TLS_HANDSHAKE_COMPLETION CompletionRoutine;   // called when the handshake finishes/fails
    PVOID CallerContext;                                 // opaque context passed to CompletionRoutine
    TLS_HS_STAGE Stage;                                  // current wait state in the handshake

    //
    // Preallocated so the PASSIVE bounce in
    // TlsHandshakeOnReceiveRecordPayload can never fail; that is the one
    // completion that leads to every PASSIVE_LEVEL-only CNG call in this
    // handshake (ECDH, ECDSA verify, hash/HKDF), reached transitively
    // from there. PendingBounceStatus carries the Status argument across
    // the bounce (IoQueueWorkItem's worker signature has no room for it).
    //
    PIO_WORKITEM WorkItem;           // preallocated work item for the PASSIVE bounce
    NTSTATUS PendingBounceStatus;    // Status value carried across the bounce

    UCHAR ClientPrivate[TLS_ECC_COORD_LEN];   // our ephemeral ECDHE private scalar
    UCHAR ClientPublic[TLS_ECC_PUBKEY_LEN];   // our ephemeral ECDHE public point
    UCHAR ServerPublic[TLS_ECC_PUBKEY_LEN];   // server's ECDHE public point from ServerHello

    UCHAR Transcript[TLS_HS_TRANSCRIPT_MAX];  // running handshake transcript for hashing
    ULONG TranscriptLen;                      // bytes currently in Transcript

    UCHAR HandshakeSecret[TLS_HASH_LEN];    // HKDF handshake secret
    UCHAR ClientHsTraffic[TLS_HASH_LEN];    // client handshake traffic secret
    UCHAR ServerHsTraffic[TLS_HASH_LEN];    // server handshake traffic secret

    UCHAR ClientHsKey[TLS_KEY_LEN];   // derived client handshake AEAD key
    UCHAR ClientHsIv[TLS_IV_LEN];     // derived client handshake static IV
    UCHAR ServerHsKey[TLS_KEY_LEN];   // derived server handshake AEAD key
    UCHAR ServerHsIv[TLS_IV_LEN];     // derived server handshake static IV
    ULONGLONG ServerSeq;              // server record sequence number during handshake

    UCHAR ServerLongTermKey[TLS_ECC_PUBKEY_LEN];           // server's certificate public key
    UCHAR TranscriptHashThroughCert[TLS_HASH_LEN];         // transcript hash up to Certificate
    UCHAR TranscriptHashThroughCertVerify[TLS_HASH_LEN];   // transcript hash up to CertificateVerify
    BOOLEAN SawEncryptedExtensions;   // EncryptedExtensions message received
    BOOLEAN SawCertificate;           // Certificate message received
    BOOLEAN SawCertVerify;            // CertificateVerify message received

    //
    // Record framing: header first (always exactly 5 bytes), then
    // exactly the declared payload length, both via WSK_FLAG_WAITALL --
    // one completion per piece, no raw-stream reassembly needed, since
    // a TLS record's length is always known from its own fixed-size
    // header.
    //
    UCHAR RecordHeader[5];                        // current record's 5-byte header
    UCHAR RecordPayload[TLS_RECORD_CIPHERTEXT_MAX];  // current record's payload
    ULONG RecordPayloadLen;                        // declared length of RecordPayload

    //
    // Decrypted handshake bytes not yet fully message-parsed (a record's
    // decrypted content may end mid-message if the peer fragments a
    // large Certificate across multiple records).
    //
    UCHAR Flight[TLS_HS_FLIGHT_MAX];   // decrypted, not-yet-parsed handshake bytes
    ULONG FlightLen;                   // bytes currently in Flight

    //
    // On-wire buffers for the two outgoing messages -- must stay valid
    // until their async send completes, so they live in the context, not
    // on a stack frame that may already have unwound.
    //
    UCHAR ClientHelloRecord[5 + TLS_CLIENT_HELLO_MAX_LEN];               // outgoing ClientHello record
    UCHAR ClientFinishedRecord[5 + 4 + TLS_HASH_LEN + 1 + TLS_TAG_LEN];  // outgoing client Finished record
} TLS_HANDSHAKE_CONTEXT, *PTLS_HANDSHAKE_CONTEXT;

static VOID TlsHandshakeFail(PTLS_HANDSHAKE_CONTEXT Ctx, NTSTATUS Status);

static VOID TlsHandshakeOnSendClientHello(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context);

static VOID TlsHandshakeIssueReceiveRecordHeader(PTLS_HANDSHAKE_CONTEXT Ctx);
static VOID TlsHandshakeIssueReceiveRecordHeaderExpandedCallout(PVOID Parameter);
static VOID TlsHandshakeOnBulkReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context);
static VOID TlsHandshakeOnReceiveRecordPayload(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context);
static VOID TlsHandshakeOnReceiveServerHello(PTLS_HANDSHAKE_CONTEXT Ctx);
static NTSTATUS TlsHandshakeProcessFlightMessages(PTLS_HANDSHAKE_CONTEXT Ctx, BOOLEAN* Done);

static VOID TlsHandshakeSendClientFinished(PTLS_HANDSHAKE_CONTEXT Ctx);
static VOID TlsHandshakeOnSendClientFinished(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context);

//
// Starts the TLS handshake state machine on a fresh socket: allocates the
// handshake context and its PASSIVE-bounce work item, generates the
// client's ephemeral ECDHE key pair and ClientHello random, builds and
// sends the ClientHello record, then waits for the server's response.
// The SNI extension carries global.RemoteHostSniAnsi (Driver.h) -- the
// configured hostname without any port suffix -- so a TLS-terminating
// backend that picks its certificate by SNI sees the same name the
// driver resolved. It is NULL (and the extension omitted, which is
// always legal) when the host is an IP literal, which RFC 6066 forbids
// in SNI. Its length is measured with RtlStringCbLengthA against
// BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES (Driver.h), the same bound the
// string was allocated under. The ClientHello record header's type
// byte is 0x16 (handshake); its
// legacy_record_version is fixed at 0x0301, which RFC 8446 requires only
// for the very first record.
//
VOID BlorgTlsStartHandshakeAsync(
    PKSOCKET Socket,
    PBLORG_TLS_HANDSHAKE_COMPLETION CompletionRoutine,
    PVOID CallerContext)
{
    PTLS_HANDSHAKE_CONTEXT ctx = ExAllocatePoolZero(NonPagedPoolNx, sizeof(TLS_HANDSHAKE_CONTEXT), TLS_HS_TAG);

    if (!ctx)
    {
        Socket->Tls.State = TlsHandshakeFailed;
        CompletionRoutine(STATUS_INSUFFICIENT_RESOURCES, CallerContext);
        return;
    }

    ctx->WorkItem = IoAllocateWorkItem(global.FileSystemDeviceObject);

    if (!ctx->WorkItem)
    {
        ExFreePool(ctx);
        Socket->Tls.State = TlsHandshakeFailed;
        CompletionRoutine(STATUS_INSUFFICIENT_RESOURCES, CallerContext);
        return;
    }

    ctx->Socket = Socket;
    ctx->CompletionRoutine = CompletionRoutine;
    ctx->CallerContext = CallerContext;
    ctx->Stage = TlsHsWaitingForServerHello;
    ctx->IssueQpc = BlorgStatisticsNow();

    BLORGFS_STAT_INC(HandshakesStarted);

    Socket->Tls.State = TlsHandshakeInProgress;

    UCHAR clientRandom[TLS_HANDSHAKE_RANDOM_LEN];
    NTSTATUS status = BlorgTlsEcdhGenerateKeyPair(ctx->ClientPrivate, ctx->ClientPublic);

    if (NT_SUCCESS(status))
    {
        status = BCryptGenRandom(NULL, clientRandom, TLS_HANDSHAKE_RANDOM_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    }

    ULONG clientHelloLen = 0;

    if (NT_SUCCESS(status))
    {
        const char* serverName = global.RemoteHostSniAnsi;
        size_t serverNameLenT = 0;

        if (serverName)
        {
            status = RtlStringCbLengthA(serverName, BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES, &serverNameLenT);
        }

        ULONG serverNameLen = C_CAST(ULONG, serverNameLenT);

        if (NT_SUCCESS(status))
        {
            status = BlorgTlsBuildClientHello(
                clientRandom, ctx->ClientPublic, serverName, serverNameLen,
                ctx->ClientHelloRecord + 5, C_CAST(ULONG, sizeof(ctx->ClientHelloRecord) - 5), &clientHelloLen);
        }
    }

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(ctx, status);
        return;
    }

    ctx->ClientHelloRecord[0] = 0x16;
    ctx->ClientHelloRecord[1] = 0x03;
    ctx->ClientHelloRecord[2] = 0x01;
    ctx->ClientHelloRecord[3] = C_CAST(UCHAR, clientHelloLen >> 8);
    ctx->ClientHelloRecord[4] = C_CAST(UCHAR, clientHelloLen & 0xFF);

    RtlCopyMemory(ctx->Transcript, ctx->ClientHelloRecord + 5, clientHelloLen);
    ctx->TranscriptLen = clientHelloLen;

    NTSTATUS sendStatus = BlorgSendWskAsync(
        Socket,
        ctx->ClientHelloRecord,
        5 + clientHelloLen,
        WSK_FLAG_NODELAY,
        TlsHandshakeOnSendClientHello,
        ctx);

    if (STATUS_PENDING != sendStatus)
    {
        TlsHandshakeFail(ctx, sendStatus);
    }
}

//
// Common failure path for the handshake state machine: marks the socket
// failed, tears down the context, and invokes the caller's completion
// routine with the given status. The context is zeroed before the pool
// free -- it holds the ephemeral ECDHE private scalar and every
// handshake secret/traffic key, and freed pool contents are otherwise
// preserved; same policy as BlorgTlsDestroyConnectionState (Tls.c) applies to
// the socket's own key material.
//
static VOID TlsHandshakeFail(PTLS_HANDSHAKE_CONTEXT Ctx, NTSTATUS Status)
{
    BLORGFS_LOG("TlsHandshakeFail: status=0x%08lX\n", Status);

    Ctx->Socket->Tls.State = TlsHandshakeFailed;

    PBLORG_TLS_HANDSHAKE_COMPLETION completionRoutine = Ctx->CompletionRoutine;
    PVOID callerContext = Ctx->CallerContext;
    PIO_WORKITEM workItem = Ctx->WorkItem;

    BLORGFS_STAT_INC(HandshakesFailed);

    RtlSecureZeroMemory(Ctx, sizeof(*Ctx));
    IoFreeWorkItem(workItem);
    ExFreePool(Ctx);

    completionRoutine(Status, callerContext);
}

//
// Completion for the ClientHello send: on success, starts waiting for the
// server's record header.
//
static VOID TlsHandshakeOnSendClientHello(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    UNREFERENCED_PARAMETER(BytesTransferred);

    PTLS_HANDSHAKE_CONTEXT ctx = C_CAST(PTLS_HANDSHAKE_CONTEXT, Context);

    if (!NT_SUCCESS(Status))
    {
        TlsHandshakeFail(ctx, Status);
        return;
    }

    TlsHandshakeIssueReceiveRecordHeader(ctx);
}

//
// Trampoline used as the callout target for KeExpandKernelStackAndCalloutEx,
// re-entering TlsHandshakeIssueReceiveRecordHeader on the expanded stack.
//
static VOID TlsHandshakeIssueReceiveRecordHeaderExpandedCallout(PVOID Parameter)
{
    TlsHandshakeIssueReceiveRecordHeader(C_CAST(PTLS_HANDSHAKE_CONTEXT, Parameter));
}

//
// A server sending many small records back-to-back could chain many
// synchronous completions on this same stack if WSK completes each
// receive inline. Check remaining stack and expand rather than guess a
// fixed chain-length cap.
//
// Framing: this used to post two exact-length WAITALL receives per
// record -- five bytes for the header, then the declared payload -- which
// costs an IRP, an MDL allocate/probe/lock, and a watchdog timer arm and
// cancel, twice, for every record in the server's flight. A TLS 1.3
// server flight is four to six records once the certificate chain is
// counted, so a handshake spent roughly ten to twelve WSK operations
// reading a few kilobytes, and any of them that did not complete inline
// cost a full scheduling round trip. All of that sits on connection
// establishment, which a fresh stream pays up to PREFETCH_DEPTH times at
// once.
//
// Instead the ciphertext is received in bulk into the connection's
// accumulator (Socket.h, shared with the post-handshake record drain in
// Client.c) and whole records are handed to the state machine straight
// out of it, so a typical flight costs one or two receives rather than
// ten. The accumulator is the right home for it rather than a
// context-local buffer for a second reason: whatever the last bulk
// receive pulls in past the server's Finished -- a NewSessionTicket,
// typically -- stays buffered on the connection for the first request to
// drain, instead of being discarded with the handshake context and
// desyncing the record sequence.
//
// The delivery shape is deliberately unchanged: a buffered record is
// copied into Ctx->RecordHeader/RecordPayload and fed to
// TlsHandshakeOnReceiveRecordPayload exactly as a completed WAITALL pair
// used to be, so every parsing, decryption, and PASSIVE-bounce path
// downstream is untouched. Delivery is a direct call, so consecutive
// buffered records recurse rather than iterate -- bounded by the records
// in one flight, and covered by the same stack check as before.
//
// When no complete record is buffered, the cursors are reset if the buffer
// drained exactly, and otherwise the partial tail is compacted to the front
// so the next receive has room for a whole max-size record behind it.
//
static VOID TlsHandshakeIssueReceiveRecordHeader(PTLS_HANDSHAKE_CONTEXT Ctx)
{
    SIZE_T remainingStack = IoGetRemainingStackSize();

    if (remainingStack < TLS_HS_STACK_SAFETY_MARGIN)
    {
        NTSTATUS expandResult = KeExpandKernelStackAndCalloutEx(
            TlsHandshakeIssueReceiveRecordHeaderExpandedCallout,
            Ctx,
            TLS_HS_STACK_EXPAND_SIZE,
            FALSE,
            NULL);

        if (!NT_SUCCESS(expandResult))
        {
            TlsHandshakeFail(Ctx, STATUS_INSUFFICIENT_RESOURCES);
        }

        return;
    }

    PKSOCKET socket = Ctx->Socket;

    NTSTATUS accumulatorStatus = BlorgEnsureTlsRecvBuffer(socket);

    if (!NT_SUCCESS(accumulatorStatus))
    {
        TlsHandshakeFail(Ctx, accumulatorStatus);
        return;
    }

    ULONG buffered = socket->TlsRecvLength - socket->TlsRecvOffset;

    if (buffered >= 5)
    {
        PUCHAR record = socket->TlsRecvBuffer + socket->TlsRecvOffset;
        ULONG declaredLen = (C_CAST(ULONG, record[3]) << 8) | C_CAST(ULONG, record[4]);

        if (declaredLen > TLS_RECORD_CIPHERTEXT_MAX)
        {
            TlsHandshakeFail(Ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        if (buffered >= 5 + declaredLen)
        {
            RtlCopyMemory(Ctx->RecordHeader, record, 5);

            if (declaredLen)
            {
                RtlCopyMemory(Ctx->RecordPayload, record + 5, declaredLen);
            }

            Ctx->RecordPayloadLen = declaredLen;
            socket->TlsRecvOffset += 5 + declaredLen;

            TlsHandshakeOnReceiveRecordPayload(STATUS_SUCCESS, declaredLen, Ctx);
            return;
        }
    }

    if (socket->TlsRecvOffset == socket->TlsRecvLength)
    {
        socket->TlsRecvLength = 0;
        socket->TlsRecvOffset = 0;
    }
    else if (socket->TlsRecvOffset &&
        (SocketTlsRecvCapacity - socket->TlsRecvLength) < (5 + TLS_RECORD_CIPHERTEXT_MAX))
    {
        RtlMoveMemory(
            socket->TlsRecvBuffer,
            socket->TlsRecvBuffer + socket->TlsRecvOffset,
            socket->TlsRecvLength - socket->TlsRecvOffset);

        socket->TlsRecvLength -= socket->TlsRecvOffset;
        socket->TlsRecvOffset = 0;
    }

    BLORGFS_STAT_INC(TlsBulkReceives);

    NTSTATUS result = BlorgReceiveWskAsyncMdl(
        socket,
        socket->TlsRecvMdl,
        socket->TlsRecvLength,
        SocketTlsRecvCapacity - socket->TlsRecvLength,
        0,
        TlsHandshakeOnBulkReceive,
        Ctx);

    if (STATUS_PENDING != result)
    {
        TlsHandshakeFail(Ctx, result);
    }
}

//
// Completion for a bulk ciphertext receive during the handshake: accounts
// the arrived bytes and re-enters the drain, which either delivers a
// now-complete record or posts another receive. A zero-byte completion is
// the peer closing mid-handshake, which is always premature here -- unlike
// the post-handshake path there is no "response already complete" case to
// carve out, since the handshake only ever receives while it still needs
// a record.
//
static VOID TlsHandshakeOnBulkReceive(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    PTLS_HANDSHAKE_CONTEXT ctx = C_CAST(PTLS_HANDSHAKE_CONTEXT, Context);

    if (!NT_SUCCESS(Status))
    {
        TlsHandshakeFail(ctx, Status);
        return;
    }

    if (0 == BytesTransferred)
    {
        TlsHandshakeFail(ctx, STATUS_CONNECTION_DISCONNECTED);
        return;
    }

    ctx->Socket->TlsRecvLength += C_CAST(ULONG, BytesTransferred);

    TlsHandshakeIssueReceiveRecordHeader(ctx);
}


//
// PASSIVE_LEVEL work-item callback that re-enters
// TlsHandshakeOnReceiveRecordPayload with the status stashed by the
// DISPATCH_LEVEL bounce check in that function. Re-entry runs at
// PASSIVE_LEVEL, so that function's bounce check falls through inline
// this time.
//
static VOID TlsHandshakeOnReceiveRecordPayloadWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PTLS_HANDSHAKE_CONTEXT ctx = C_CAST(PTLS_HANDSHAKE_CONTEXT, Context);

    TlsHandshakeOnReceiveRecordPayload(ctx->PendingBounceStatus, 0, ctx);
}

//
// Record-payload completion and dispatch point for the handshake state
// machine: decrypts/parses ServerHello or flight records and drives the
// next step. Also the PASSIVE bounce point for every CNG call reachable
// from the rest of the handshake (see the bounce check below).
//
// Everything reachable from here -- BlorgTlsAeadDecrypt, and (via
// TlsHandshakeOnReceiveServerHello / TlsHandshakeProcessFlightMessages /
// TlsHandshakeSendClientFinished) BlorgTlsEcdhComputeSharedSecret,
// BlorgTlsEcdsaVerify, and every HKDF/HMAC/SHA-256 call in Tls.c -- may call
// PASSIVE_LEVEL-only CNG (BCryptSecretAgreement, BCryptVerifySignature,
// BCryptCreateHash). This completion runs on the WSK completion chain,
// which carries no IRQL guarantee below DISPATCH_LEVEL -- the IRQL check
// at the top of this function bounces to PASSIVE first, unconditionally,
// before touching any of that. IRQL isn't sticky across a completion
// boundary, so every async op needs its own bounce, not just the first
// one.
//
// Once past the ServerHello stage (TlsHsWaitingForFlightRecord), the
// legal incoming record types are: 0x15 alert (fails the handshake),
// 0x14 change_cipher_spec (a middlebox-compat no-op per RFC 8446
// Appendix D.4, not part of the encrypted flight -- just re-arms the
// next record receive), and 0x17 application_data, the only other legal
// type here and the one actually carrying encrypted flight content.
//
static VOID TlsHandshakeOnReceiveRecordPayload(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    UNREFERENCED_PARAMETER(BytesTransferred);

    PTLS_HANDSHAKE_CONTEXT ctx = C_CAST(PTLS_HANDSHAKE_CONTEXT, Context);

    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
    {
        ctx->PendingBounceStatus = Status;
        IoQueueWorkItem(ctx->WorkItem, TlsHandshakeOnReceiveRecordPayloadWorker, DelayedWorkQueue, ctx);
        return;
    }

    if (!NT_SUCCESS(Status))
    {
        TlsHandshakeFail(ctx, Status);
        return;
    }

    UCHAR recordType = ctx->RecordHeader[0];

    if (TlsHsWaitingForServerHello == ctx->Stage)
    {
        if (0x16 != recordType)
        {
            TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
            return;
        }

        TlsHandshakeOnReceiveServerHello(ctx);
        return;
    }

    if (0x15 == recordType)
    {
        TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    if (0x14 == recordType)
    {
        TlsHandshakeIssueReceiveRecordHeader(ctx);
        return;
    }

    if (0x17 != recordType)
    {
        TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    if (ctx->RecordPayloadLen < TLS_TAG_LEN)
    {
        TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    ULONG ciphertextLen = ctx->RecordPayloadLen - TLS_TAG_LEN;

    if (ctx->FlightLen + ciphertextLen > sizeof(ctx->Flight))
    {
        TlsHandshakeFail(ctx, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    NTSTATUS decStatus = BlorgTlsAeadDecrypt(
        ctx->ServerHsKey, ctx->ServerHsIv, ctx->ServerSeq,
        ctx->RecordHeader, 5,
        ctx->RecordPayload, ciphertextLen, ctx->RecordPayload + ciphertextLen,
        ctx->Flight + ctx->FlightLen);

    if (!NT_SUCCESS(decStatus))
    {
        TlsHandshakeFail(ctx, decStatus);
        return;
    }

    ctx->ServerSeq++;

    UCHAR contentType;
    ULONG contentLen;

    if (!BlorgTlsStripInnerPlaintext(ctx->Flight + ctx->FlightLen, ciphertextLen, &contentType, &contentLen))
    {
        TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    if (0x16 != contentType)
    {
        TlsHandshakeFail(ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    ctx->FlightLen += contentLen;

    BOOLEAN done = FALSE;
    NTSTATUS processStatus = TlsHandshakeProcessFlightMessages(ctx, &done);

    if (!NT_SUCCESS(processStatus))
    {
        TlsHandshakeFail(ctx, processStatus);
        return;
    }

    if (done)
    {
        TlsHandshakeSendClientFinished(ctx);
    }
    else
    {
        TlsHandshakeIssueReceiveRecordHeader(ctx);
    }
}

//
// Parses the ServerHello message, appends it to the transcript, and derives
// the handshake secret and traffic keys (RFC 8446 7.1) from the ECDH shared
// secret, transitioning to waiting for the encrypted flight.
//
static VOID TlsHandshakeOnReceiveServerHello(PTLS_HANDSHAKE_CONTEXT Ctx)
{
    if (Ctx->RecordPayloadLen < 4 || 0x02 != Ctx->RecordPayload[0])
    {
        TlsHandshakeFail(Ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    ULONG shBodyLen = (C_CAST(ULONG, Ctx->RecordPayload[1]) << 16) |
                       (C_CAST(ULONG, Ctx->RecordPayload[2]) << 8) |
                       Ctx->RecordPayload[3];

    if (4 + shBodyLen != Ctx->RecordPayloadLen)
    {
        TlsHandshakeFail(Ctx, STATUS_INVALID_PARAMETER);
        return;
    }

    if (Ctx->TranscriptLen + Ctx->RecordPayloadLen > sizeof(Ctx->Transcript))
    {
        TlsHandshakeFail(Ctx, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    RtlCopyMemory(Ctx->Transcript + Ctx->TranscriptLen, Ctx->RecordPayload, Ctx->RecordPayloadLen);
    Ctx->TranscriptLen += Ctx->RecordPayloadLen;

    UCHAR serverRandom[TLS_HANDSHAKE_RANDOM_LEN];
    NTSTATUS status = BlorgTlsParseServerHello(Ctx->RecordPayload + 4, shBodyLen, serverRandom, Ctx->ServerPublic);

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(Ctx, status);
        return;
    }

    UCHAR zero32[32] = { 0 };
    UCHAR sharedSecret[TLS_ECC_COORD_LEN];
    UCHAR transcriptHashChSh[TLS_HASH_LEN];
    UCHAR emptyHash[TLS_HASH_LEN];
    UCHAR derivedForHandshake[TLS_HASH_LEN];

    status = BlorgTlsEcdhComputeSharedSecret(Ctx->ClientPrivate, Ctx->ClientPublic, Ctx->ServerPublic, sharedSecret);
    if (NT_SUCCESS(status)) status = BlorgTlsSha256(Ctx->Transcript, Ctx->TranscriptLen, transcriptHashChSh);
    if (NT_SUCCESS(status)) status = BlorgTlsSha256(NULL, 0, emptyHash);

    UCHAR earlySecret[TLS_HASH_LEN];
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExtract(zero32, 32, zero32, 32, earlySecret);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(earlySecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForHandshake);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExtract(derivedForHandshake, TLS_HASH_LEN, sharedSecret, TLS_ECC_COORD_LEN, Ctx->HandshakeSecret);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->HandshakeSecret, TLS_HASH_LEN, "c hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, Ctx->ClientHsTraffic);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->HandshakeSecret, TLS_HASH_LEN, "s hs traffic", transcriptHashChSh, TLS_HASH_LEN, TLS_HASH_LEN, Ctx->ServerHsTraffic);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->ServerHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, Ctx->ServerHsKey);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->ServerHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, Ctx->ServerHsIv);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->ClientHsTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, Ctx->ClientHsKey);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->ClientHsTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, Ctx->ClientHsIv);

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(Ctx, status);
        return;
    }

    Ctx->Stage = TlsHsWaitingForFlightRecord;
    TlsHandshakeIssueReceiveRecordHeader(Ctx);
}

//
// Drains as many complete handshake messages as are available from the
// front of Ctx->Flight, processing each as it's identified. Leftover
// partial-message bytes are shifted to the front for the next record.
// Message type codes handled: 0x08 EncryptedExtensions (nothing needed
// from it, must follow directly after none seen yet and before
// Certificate -- any other order or a duplicate fails the handshake),
// 0x0B Certificate, 0x0F CertificateVerify, 0x14 Finished; each of the
// latter three requires the prior one(s) already seen and rejects a
// duplicate or out-of-order arrival. Any other message type fails the
// handshake. A pin mismatch on the Certificate message (BlorgTlsCheckPin
// failing on an otherwise cryptographically well-formed certificate, or
// no pin configured at all) returns STATUS_BLORGFS_CERT_PIN_MISMATCH, a
// status distinct from the generic STATUS_INVALID_PARAMETER every other
// parse failure here uses.
//
// SECURITY NOTE: this verifies the server's CertificateVerify signature
// (proof it holds the private key for the certificate it presented), the
// Finished MAC (proof of a correctly-completed key exchange), and that
// the leaf certificate's SPKI matches the configured pin (BlorgTlsCheckPin) --
// proof it's the *right* key, not just *some* valid key. A handshake
// completed by this code is authenticated exactly as strongly as
// whatever pin is currently configured (BlorgTlsSetPin) -- if none ever was,
// BlorgTlsCheckPin fails closed and no Certificate message can pass this check.
//
static NTSTATUS TlsHandshakeProcessFlightMessages(PTLS_HANDSHAKE_CONTEXT Ctx, BOOLEAN* Done)
{
    ULONG msgOffset = 0;
    *Done = FALSE;

    while (msgOffset + 4 <= Ctx->FlightLen)
    {
        UCHAR msgType = Ctx->Flight[msgOffset];
        ULONG msgBodyLen = (C_CAST(ULONG, Ctx->Flight[msgOffset + 1]) << 16) |
                            (C_CAST(ULONG, Ctx->Flight[msgOffset + 2]) << 8) |
                            Ctx->Flight[msgOffset + 3];
        ULONG msgTotalLen = 4 + msgBodyLen;

        if (msgOffset + msgTotalLen > Ctx->FlightLen)
        {
            break;
        }

        const UCHAR* msgBody = Ctx->Flight + msgOffset + 4;
        NTSTATUS status = STATUS_SUCCESS;

        if (Ctx->TranscriptLen + msgTotalLen > sizeof(Ctx->Transcript))
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(Ctx->Transcript + Ctx->TranscriptLen, Ctx->Flight + msgOffset, msgTotalLen);
        Ctx->TranscriptLen += msgTotalLen;

        if (0x08 == msgType)
        {
            if (Ctx->SawEncryptedExtensions || Ctx->SawCertificate)
            {
                return STATUS_INVALID_PARAMETER;
            }

            Ctx->SawEncryptedExtensions = TRUE;
        }
        else if (0x0B == msgType)
        {
            const UCHAR* leafCert = NULL;
            ULONG leafCertLen = 0;
            const UCHAR* spki = NULL;
            ULONG spkiLen = 0;

            if (!Ctx->SawEncryptedExtensions || Ctx->SawCertificate)
            {
                return STATUS_INVALID_PARAMETER;
            }

            status = BlorgTlsParseCertificateMessage(msgBody, msgBodyLen, &leafCert, &leafCertLen);
            if (NT_SUCCESS(status)) status = BlorgTlsExtractSpkiFromCertificate(leafCert, leafCertLen, &spki, &spkiLen);

            if (NT_SUCCESS(status) && !BlorgTlsCheckPin(spki, spkiLen))
            {
                BLORGFS_PRINT("TlsHandshakeProcessFlightMessages: certificate pin mismatch, rejecting\n");
                status = STATUS_BLORGFS_CERT_PIN_MISMATCH;
            }

            if (NT_SUCCESS(status)) status = BlorgTlsDecodeP256SubjectPublicKeyInfo(spki, spkiLen, Ctx->ServerLongTermKey);
            if (NT_SUCCESS(status)) status = BlorgTlsSha256(Ctx->Transcript, Ctx->TranscriptLen, Ctx->TranscriptHashThroughCert);

            if (NT_SUCCESS(status))
            {
                Ctx->SawCertificate = TRUE;
            }
        }
        else if (0x0F == msgType)
        {
            UCHAR rawSignature[64];
            UCHAR verifyContent[TLS_CERT_VERIFY_CONTENT_LEN];
            UCHAR verifyDigest[TLS_HASH_LEN];

            if (!Ctx->SawCertificate || Ctx->SawCertVerify)
            {
                return STATUS_INVALID_PARAMETER;
            }

            status = BlorgTlsParseCertificateVerifyMessage(msgBody, msgBodyLen, rawSignature);
            if (NT_SUCCESS(status)) status = BlorgTlsBuildServerCertVerifyContent(Ctx->TranscriptHashThroughCert, verifyContent);
            if (NT_SUCCESS(status)) status = BlorgTlsSha256(verifyContent, TLS_CERT_VERIFY_CONTENT_LEN, verifyDigest);
            if (NT_SUCCESS(status)) status = BlorgTlsEcdsaVerify(Ctx->ServerLongTermKey, verifyDigest, TLS_HASH_LEN, rawSignature);
            if (NT_SUCCESS(status)) status = BlorgTlsSha256(Ctx->Transcript, Ctx->TranscriptLen, Ctx->TranscriptHashThroughCertVerify);

            if (NT_SUCCESS(status))
            {
                Ctx->SawCertVerify = TRUE;
            }
        }
        else if (0x14 == msgType)
        {
            UCHAR serverFinishedKey[TLS_HASH_LEN];
            UCHAR expectedFinishedMac[TLS_HASH_LEN];

            if (!Ctx->SawCertVerify || 32 != msgBodyLen)
            {
                return STATUS_INVALID_PARAMETER;
            }

            status = BlorgTlsHkdfExpandLabel(Ctx->ServerHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, serverFinishedKey);
            if (NT_SUCCESS(status)) status = BlorgTlsHmacSha256(serverFinishedKey, TLS_HASH_LEN, Ctx->TranscriptHashThroughCertVerify, TLS_HASH_LEN, expectedFinishedMac);

            if (NT_SUCCESS(status) && !BlorgTlsConstantTimeEqual(expectedFinishedMac, msgBody, TLS_HASH_LEN))
            {
                status = STATUS_INVALID_PARAMETER;
            }

            if (NT_SUCCESS(status))
            {
                *Done = TRUE;
            }
        }
        else
        {
            status = STATUS_INVALID_PARAMETER;
        }

        if (!NT_SUCCESS(status))
        {
            return status;
        }

        msgOffset += msgTotalLen;

        if (*Done)
        {
            break;
        }
    }

    RtlMoveMemory(Ctx->Flight, Ctx->Flight + msgOffset, Ctx->FlightLen - msgOffset);
    Ctx->FlightLen -= msgOffset;

    return STATUS_SUCCESS;
}

//
// Derives the master secret and application traffic keys, installs them on
// the socket (and imports dispatch-usable AEAD key handles), then encrypts
// and sends the client Finished message under the still-current handshake
// keys.
//
// Application traffic keys become the socket's *current* keys (Write/Read
// Key/Iv/Seq) as soon as they're derived -- used for real HTTP traffic
// once this handshake completes. Sequence numbers start fresh at 0 for
// this new key (RFC 8446 7.2's key schedule transition). It's safe to
// update Socket->Tls before the Finished send below: that send explicitly
// uses Ctx->ClientHsKey/ClientHsIv, not Socket->Tls's already-swapped
// fields -- the client Finished is the last message sent under handshake
// keys, per RFC 8446 4.4.4.
//
// DISPATCH-usable key handles (WriteKeyHandle/ReadKeyHandle) are cached
// alongside the raw key bytes right after -- this is what lets the
// record-layer hot path (ordinary HTTP read/write once this handshake
// completes) call BlorgTlsAeadEncryptKeyed/BlorgTlsAeadDecryptKeyed without ever
// needing a PASSIVE bounce. Safe to do here: this whole function only
// runs within the handshake's own PASSIVE-bounced completion chain (see
// TlsHandshakeOnReceiveRecordPayload), and BlorgTlsImportKeyHandle has no IRQL
// restriction of its own.
//
// The client Finished plaintext's trailing byte is the inner content
// type (0x16, handshake), per RFC 8446 5.2's TLSInnerPlaintext.
//
static VOID TlsHandshakeSendClientFinished(PTLS_HANDSHAKE_CONTEXT Ctx)
{
    UCHAR emptyHash[TLS_HASH_LEN];
    UCHAR derivedForMaster[TLS_HASH_LEN];
    UCHAR masterSecret[TLS_HASH_LEN];
    UCHAR transcriptHashThroughServerFinished[TLS_HASH_LEN];
    UCHAR zero32[32] = { 0 };
    UCHAR clientAppTraffic[TLS_HASH_LEN];
    UCHAR serverAppTraffic[TLS_HASH_LEN];
    UCHAR clientAppKey[TLS_KEY_LEN];
    UCHAR clientAppIv[TLS_IV_LEN];
    UCHAR serverAppKey[TLS_KEY_LEN];
    UCHAR serverAppIv[TLS_IV_LEN];
    UCHAR clientFinishedKey[TLS_HASH_LEN];
    UCHAR clientFinishedMac[TLS_HASH_LEN];
    NTSTATUS status;

    status = BlorgTlsSha256(NULL, 0, emptyHash);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->HandshakeSecret, TLS_HASH_LEN, "derived", emptyHash, TLS_HASH_LEN, TLS_HASH_LEN, derivedForMaster);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExtract(derivedForMaster, TLS_HASH_LEN, zero32, 32, masterSecret);
    if (NT_SUCCESS(status)) status = BlorgTlsSha256(Ctx->Transcript, Ctx->TranscriptLen, transcriptHashThroughServerFinished);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(masterSecret, TLS_HASH_LEN, "c ap traffic", transcriptHashThroughServerFinished, TLS_HASH_LEN, TLS_HASH_LEN, clientAppTraffic);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(masterSecret, TLS_HASH_LEN, "s ap traffic", transcriptHashThroughServerFinished, TLS_HASH_LEN, TLS_HASH_LEN, serverAppTraffic);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(clientAppTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, clientAppKey);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(clientAppTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, clientAppIv);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(serverAppTraffic, TLS_HASH_LEN, "key", NULL, 0, TLS_KEY_LEN, serverAppKey);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(serverAppTraffic, TLS_HASH_LEN, "iv", NULL, 0, TLS_IV_LEN, serverAppIv);
    if (NT_SUCCESS(status)) status = BlorgTlsHkdfExpandLabel(Ctx->ClientHsTraffic, TLS_HASH_LEN, "finished", NULL, 0, TLS_HASH_LEN, clientFinishedKey);
    if (NT_SUCCESS(status)) status = BlorgTlsHmacSha256(clientFinishedKey, TLS_HASH_LEN, transcriptHashThroughServerFinished, TLS_HASH_LEN, clientFinishedMac);

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(Ctx, status);
        return;
    }

    RtlCopyMemory(Ctx->Socket->Tls.WriteKey, clientAppKey, TLS_KEY_LEN);
    RtlCopyMemory(Ctx->Socket->Tls.WriteIv, clientAppIv, TLS_IV_LEN);
    Ctx->Socket->Tls.WriteSeq = 0;

    RtlCopyMemory(Ctx->Socket->Tls.ReadKey, serverAppKey, TLS_KEY_LEN);
    RtlCopyMemory(Ctx->Socket->Tls.ReadIv, serverAppIv, TLS_IV_LEN);
    Ctx->Socket->Tls.ReadSeq = 0;

    status = BlorgTlsImportKeyHandle(clientAppKey, &Ctx->Socket->Tls.WriteKeyHandle);
    if (NT_SUCCESS(status)) status = BlorgTlsImportKeyHandle(serverAppKey, &Ctx->Socket->Tls.ReadKeyHandle);

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(Ctx, status);
        return;
    }

    UCHAR plaintext[4 + TLS_HASH_LEN + 1];
    plaintext[0] = 0x14;
    plaintext[1] = 0x00;
    plaintext[2] = 0x00;
    plaintext[3] = TLS_HASH_LEN;
    RtlCopyMemory(plaintext + 4, clientFinishedMac, TLS_HASH_LEN);
    plaintext[4 + TLS_HASH_LEN] = 0x16;

    ULONG recordLen = C_CAST(ULONG, sizeof(plaintext)) + TLS_TAG_LEN;
    Ctx->ClientFinishedRecord[0] = 0x17;
    Ctx->ClientFinishedRecord[1] = 0x03;
    Ctx->ClientFinishedRecord[2] = 0x03;
    Ctx->ClientFinishedRecord[3] = C_CAST(UCHAR, recordLen >> 8);
    Ctx->ClientFinishedRecord[4] = C_CAST(UCHAR, recordLen & 0xFF);

    status = BlorgTlsAeadEncrypt(
        Ctx->ClientHsKey, Ctx->ClientHsIv, 0,
        Ctx->ClientFinishedRecord, 5,
        plaintext, C_CAST(ULONG, sizeof(plaintext)),
        Ctx->ClientFinishedRecord + 5, Ctx->ClientFinishedRecord + 5 + sizeof(plaintext));

    if (!NT_SUCCESS(status))
    {
        TlsHandshakeFail(Ctx, status);
        return;
    }

    NTSTATUS sendStatus = BlorgSendWskAsync(
        Ctx->Socket,
        Ctx->ClientFinishedRecord,
        C_CAST(ULONG, 5 + sizeof(plaintext) + TLS_TAG_LEN),
        WSK_FLAG_NODELAY,
        TlsHandshakeOnSendClientFinished,
        Ctx);

    if (STATUS_PENDING != sendStatus)
    {
        TlsHandshakeFail(Ctx, sendStatus);
    }
}

//
// Completion for the client Finished send: on success, marks the socket's
// TLS state complete and invokes the caller's completion routine. The
// context is zeroed before the pool free for the same reason as in
// TlsHandshakeFail -- the handshake secrets it holds must not survive
// into recycled pool.
//
static VOID TlsHandshakeOnSendClientFinished(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    UNREFERENCED_PARAMETER(BytesTransferred);

    PTLS_HANDSHAKE_CONTEXT ctx = C_CAST(PTLS_HANDSHAKE_CONTEXT, Context);

    if (!NT_SUCCESS(Status))
    {
        TlsHandshakeFail(ctx, Status);
        return;
    }

    ctx->Socket->Tls.State = TlsHandshakeComplete;

    PBLORG_TLS_HANDSHAKE_COMPLETION completionRoutine = ctx->CompletionRoutine;
    PVOID callerContext = ctx->CallerContext;
    PIO_WORKITEM workItem = ctx->WorkItem;

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (statsBlock)
    {
        statsBlock->HandshakesCompleted++;

        BlorgStatisticsRecordLatency(
            &statsBlock->HandshakeLatencySumUs,
            &statsBlock->HandshakeLatencyMaxUs,
            NULL,
            BlorgStatisticsNow() - ctx->IssueQpc);
    }

    RtlSecureZeroMemory(ctx, sizeof(*ctx));
    IoFreeWorkItem(workItem);
    ExFreePool(ctx);

    completionRoutine(STATUS_SUCCESS, callerContext);
}
