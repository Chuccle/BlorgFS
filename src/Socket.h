#pragma once

//
// WSK socket wrapper: connection pooling, async send/receive/connect/close,
// per-operation timeouts, and the KSOCKET type carrying WSK dispatch table,
// remote address, and TLS state.
//

// Synchronous (event-wait) IRP context for a single WSK call.
typedef struct _KSOCKET_CONTEXT
{
    PIRP Irp;
    KEVENT CompletionEvent;
} KSOCKET_CONTEXT, * PKSOCKET_CONTEXT;

//
// Invoked from SocketAsyncCompletionRoutine once an async send/receive
// completes. Runs at <= DISPATCH_LEVEL -- must not block, must not touch
// paged memory. BytesTransferred is only meaningful when Status is a
// success code. Returns nothing: the completion routine drives the next
// stage itself, and SocketAsyncCompletionRoutine ignores any result.
//
typedef VOID(*PKSOCKET_COMPLETION_ROUTINE)(
    NTSTATUS Status,
    ULONG_PTR BytesTransferred,
    PVOID Context
    );

//
// Per-operation watchdog. A WSK send/receive/connect that never completes
// (dead peer, half-open connection, server that accepts but never answers)
// would otherwise leave the issuing IRP -- and the user request behind it,
// up to and including an Explorer thread -- pending forever. The timer
// fires IoCancelIrp on the operation's IRP; WSK then completes it with
// STATUS_CANCELLED, which the completion routine reports as
// STATUS_IO_TIMEOUT.
//
// RefCount coordinates the race between a real completion and the timeout
// firing: starts at 2 (completion routine + timeout DPC, each releasing one
// reference at most once). Whichever side drives it to zero owns the free.
// RefCount/TimedOut need no volatile qualifier -- all writes go through
// Interlocked* ops, which are already full memory barriers.
//
typedef struct _SOCKET_OP_TIMEOUT
{
    KTIMER Timer;
    KDPC Dpc;
    PIRP Irp;         // IRP cancelled when the timer fires
    LONG RefCount;    // 2 = completion routine + timeout DPC
    LONG TimedOut;    // set by the DPC before it cancels the IRP
} SOCKET_OP_TIMEOUT, * PSOCKET_OP_TIMEOUT;

//
// One of these is allocated per async send/receive call and freed by
// SocketAsyncCompletionRoutine (or the timeout DPC -- whichever finishes
// last) once CompletionRoutine has returned. Never reused across calls --
// see Socket.c for why.
//
typedef struct _KSOCKET_ASYNC_CONTEXT
{
    PIRP Irp;
    PMDL Mdl;
    PKSOCKET_COMPLETION_ROUTINE CompletionRoutine;
    PVOID CompletionContext;
    SOCKET_OP_TIMEOUT Timeout;
} KSOCKET_ASYNC_CONTEXT, * PKSOCKET_ASYNC_CONTEXT;

//
// A pooled/connected WSK socket plus its dispatch table, remote address,
// and per-connection TLS state.
//
typedef struct _KSOCKET
{
    PWSK_SOCKET	WskSocket;

    union
    {
        PVOID WskDispatch;

        PWSK_PROVIDER_CONNECTION_DISPATCH WskConnectionDispatch;
        PWSK_PROVIDER_LISTEN_DISPATCH WskListenDispatch;
        PWSK_PROVIDER_DATAGRAM_DISPATCH WskDatagramDispatch;
#if (NTDDI_VERSION >= NTDDI_WIN10_RS2)
        PWSK_PROVIDER_STREAM_DISPATCH WskStreamDispatch;
#endif
    };

    //
    // Address this socket is connected to. Set once at creation time and
    // never changed; used by BlorgAcquireReusableWskSocketAsync to refuse handing
    // out a pooled socket connected to a stale/different target.
    //
    SOCKADDR_STORAGE RemoteAddress;

    LIST_ENTRY PoolEntry;

    //
    // Per-connection TLS state (Tls.h) -- lives for exactly as long as
    // this socket's underlying TCP connection, surviving pooled reuse.
    // Zero-cost when unused: State == TlsHandshakeNotStarted (the
    // zero value) is what a plain socket has, and the entire plaintext
    // HTTP client never touches this field.
    //
    TLS_CONNECTION_STATE Tls;

    //
    // TLS record-layer receive accumulator (HttpIssueTlsReceive,
    // Client.c). Per-connection rather than per-request so kept-alive
    // reuse pays the ~256 KB allocation once per connection instead of
    // once per request, and so ciphertext received past one response's
    // boundary (e.g. a late NewSessionTicket) is preserved for the next
    // request's drain instead of being discarded with the request
    // context and desyncing the connection's read sequence. Lazily
    // allocated by the HTTP client on the first TLS receive
    // (NonPagedPoolNx -- the drain loop reads it at <= DISPATCH_LEVEL);
    // freed with the socket (FreeKSocket, Socket.c). Zeroed at socket
    // creation, so a plain (non-TLS) connection never allocates any of
    // it. TlsRecvLength counts buffered ciphertext bytes; TlsRecvOffset
    // is the first unconsumed byte within them.
    //
    // TlsRecvMdl describes the whole accumulator and is built exactly
    // once (MmBuildMdlForNonPagedPool -- no probe/lock, nothing to
    // unlock at completion) so every bulk ciphertext receive goes
    // through BlorgReceiveWskAsyncMdl instead of paying a fresh
    // IoAllocateMdl + MmProbeAndLockPages per receive.
    //
    PUCHAR TlsRecvBuffer;
    PUCHAR TlsPlaintextScratch;
    PMDL   TlsRecvMdl;
    ULONG  TlsRecvLength;
    ULONG  TlsRecvOffset;
} KSOCKET, * PKSOCKET;

//
// Size of each connection's TLS ciphertext accumulator, chosen once at
// init from MmQuerySystemSize. Read by both users of the buffer -- the
// handshake's record drain (TlsHandshake.c) and the HTTP record drain
// (Client.c) -- so the sizing decision lives here, with the field it
// sizes, rather than in one of the two consumers.
//
extern ULONG SocketTlsRecvCapacity;

//
// Lazily allocates a connection's TLS ciphertext accumulator (buffer,
// plaintext scratch, and the prebuilt MDL every bulk receive posts
// through) if it does not have one yet. Idempotent, and safe to call
// again after a partial failure: the guard is the MDL, which is built
// last, so a run that allocated the buffer but failed on the scratch
// retries cleanly instead of leaving a half-built accumulator that a
// later caller would mistake for a complete one.
//
NTSTATUS BlorgEnsureTlsRecvBuffer(PKSOCKET Socket);

NTSTATUS BlorgInitialiseWskClient(void);
void BlorgCleanupWskClient(void);

NTSTATUS BlorgGetWskAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo);
void BlorgFreeWskAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS BlorgReleaseReusableWskSocket(PKSOCKET Socket);

void BlorgCleanupWskSocketPool(void);

//
// Fire-and-forget socket close. Never waits, so it is callable from the
// async completion routines (<= DISPATCH_LEVEL). Returns STATUS_PENDING
// once the close is issued. Use the synchronous close only for
// PASSIVE_LEVEL teardown.
//
NTSTATUS BlorgCloseWskSocketAsync(PKSOCKET Socket);

//
// Asynchronous send/receive. Returns once the operation is issued (not
// completed) -- result/status of STATUS_PENDING means CompletionRoutine
// will be invoked later from SocketAsyncCompletionRoutine at
// <= DISPATCH_LEVEL. No BytesWritten out-param: the transfer count is
// only known at completion time, and is delivered as CompletionRoutine's
// BytesTransferred argument instead. Caller must not issue a second
// async op on the same KSOCKET until CompletionRoutine for the first has
// run.
//
NTSTATUS BlorgSendWskAsync(PKSOCKET Socket, const void* Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext);
NTSTATUS BlorgReceiveWskAsync(PKSOCKET Socket, PVOID Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext);
NTSTATUS BlorgSendRecvWskAsync(PKSOCKET Socket, PVOID Buffer, ULONG Length, ULONG Flags, BOOLEAN Send, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext);

//
// Receive directly into a caller-supplied MDL whose pages are already
// locked (a paging-IO MDL from MM, or one locked via BlorgLockUserBuffer /
// MmProbeAndLockPages). Offset/Length select the target window within the
// memory the MDL describes. Unlike BlorgReceiveWskAsync there is no
// allocate/probe/unlock cycle here at all -- the MDL is borrowed for the
// duration of the operation and untouched at completion; it must stay
// valid (and locked) until CompletionRoutine runs. Same single-op-per-
// socket discipline as above.
//
NTSTATUS BlorgReceiveWskAsyncMdl(PKSOCKET Socket, PMDL Mdl, ULONG Offset, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext);

//
// Reused == TRUE means the socket came from the keep-alive pool (an
// existing connection). Such a connection may have been idle-closed by
// the peer since it was pooled, so a caller that sees a send/receive
// failure on a reused socket -- before any response byte has arrived --
// should treat it as the expected keep-alive race and re-issue once on a
// fresh connection rather than surfacing it as a hard error. Reused ==
// FALSE means a brand-new connect (a failure on it is a real error).
//
typedef VOID(*PKSOCKET_ACQUIRE_COMPLETION_ROUTINE)(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID CompletionContext);

//
// ForceFresh == TRUE bypasses the pool and always connects a new socket.
// Used by the retry path so a retry cannot land on a second stale pooled
// connection.
//
NTSTATUS BlorgAcquireReusableWskSocketAsync(
    const SOCKADDR* RemoteAddress,
    BOOLEAN ForceFresh,
    PKSOCKET_ACQUIRE_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext
);
