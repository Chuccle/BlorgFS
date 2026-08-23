//
// WSK socket layer: connection/address-resolution setup, a keep-alive
// connection pool, and synchronous and async (callback-driven) send/
// receive/connect/close paths with per-operation timeout watchdogs.
//

#include "Driver.h"
#include "Socket.h"

#define SOCKET_TAG 'HTTP'

//
// The WSK client registration and the captured provider dispatch, both
// owned entirely by this file: acquired in BlorgInitialiseWskClient,
// released in BlorgCleanupWskClient, and read by every
// send/receive/connect/close below.
//
// static because nothing outside Socket.c refers to them, and the
// convention is that a name at file scope with external linkage is either
// prefixed and declared in a header or belongs in `global` (Driver.h).
// These are neither -- they are this module's private state, and leaving
// them external published two symbols nobody was allowed to use.
//
static WSK_REGISTRATION WskRegistration;
static WSK_PROVIDER_NPI WskProviderNpi;

typedef struct _SOCKET_POOL_STATE
{
    KSPIN_LOCK Lock;
    LIST_ENTRY List;
    ULONG      Count;
} SOCKET_POOL_STATE;

static SOCKET_POOL_STATE SocketPool;

//
// Keep-alive pool depth: how many warm connections the driver is willing
// to hold idle. Reusing one costs a list pop; replacing one costs a TCP
// handshake and, with TLS enabled, a full ECDH handshake.
//
// This must be at least the driver's PEAK CONCURRENT request count, not
// its average, and that is a stronger requirement than it sounds. The cap
// bites on release: once more sockets exist than the cap allows, the
// surplus is closed rather than pooled, so the pool can never accumulate
// past it. A workload that regularly runs N requests in flight with a cap
// below N therefore never builds N warm connections -- it re-establishes
// the difference forever.
//
// It was a flat 32, on a comment reasoning about "the prefetch pipeline
// depth", which was written when one stream at PREFETCH_DEPTH was the
// whole picture. Sixteen streams put 51-62 fetches in flight, and the
// measured result was 35-47 fresh connects per 25 s run averaging 473-660
// ms each, with a 2.5 s worst case -- SYN loss under a connection burst.
// Those stalls dominated the latency tail (p99 635-704 ms) and collapsed
// fairness to 0.23.
//
// So it tracks the prefetch chunk budget, which is what actually bounds
// concurrent fetches, plus headroom for the direct-fetch and metadata
// traffic that shares the pool. Peer idle-close of pooled connections is
// handled by the reused-connection retry in the HTTP client.
//
#define SOCKET_POOL_HEADROOM 32

ULONG SocketMaxPoolSize = 64 + SOCKET_POOL_HEADROOM;

//
// TLS ciphertext accumulator sizing (see Socket.h). Ciphertext is
// received in bulk and complete records are drained out of this buffer
// without touching the wire again, so this bounds how many on-wire
// records one WSK receive can pick up, not any per-response total. It
// must exceed one max-size record or a drain loop could never make
// progress on a full buffer.
//
// Tiered by machine size, the same coarse MmQuerySystemSize tiering the
// prefetch chunk budget uses: the worst case is this times
// SocketMaxPoolSize of NonPagedPoolNx, and a small machine has no
// business reserving that to save receive IRPs. Only sockets that
// actually carry TLS ever allocate one (BlorgEnsureTlsRecvBuffer), so a
// plaintext deployment pays none of it.
//
#define SOCKET_TLS_RECV_RECORDS_LARGE  16
#define SOCKET_TLS_RECV_RECORDS_MEDIUM 8
#define SOCKET_TLS_RECV_RECORDS_SMALL  4

#define SOCKET_TLS_RECORD_MAX_BYTES (5 + TLS_RECORD_CIPHERTEXT_MAX)

ULONG SocketTlsRecvCapacity = SOCKET_TLS_RECV_RECORDS_LARGE * SOCKET_TLS_RECORD_MAX_BYTES;

//
// Every async send/receive needs a KSOCKET_ASYNC_CONTEXT; the TLS record
// receive path in particular issues them at a high rate. A lookaside
// keeps that per-op allocation off the general pool; entries are handed
// out with every field explicitly initialized (AllocateAsyncSocketContext
// / ArmSocketTimeout), so no zeroing is needed on reuse.
//
static NPAGED_LOOKASIDE_LIST AsyncContextLookaside;

static IO_COMPLETION_ROUTINE SocketContextCompletionRoutine;
static IO_COMPLETION_ROUTINE SocketAsyncCompletionRoutine;
static KDEFERRED_ROUTINE SocketAsyncTimeoutDpc;

// CloseWskSocket is defined below but referenced earlier (BlorgCleanupWskSocketPool).
static NTSTATUS CloseWskSocket(PKSOCKET Socket);

//
// See Socket.h. The MDL is both the last thing built and the guard, so a
// caller that failed partway through leaves nothing a later caller can
// mistake for a finished accumulator. NonPagedPoolNx because the drain
// loops read this at <= DISPATCH_LEVEL, and MmBuildMdlForNonPagedPool
// because that lets every bulk receive skip a fresh IoAllocateMdl plus
// MmProbeAndLockPages.
//
NTSTATUS BlorgEnsureTlsRecvBuffer(PKSOCKET Socket)
{
    if (Socket->TlsRecvMdl)
    {
        return STATUS_SUCCESS;
    }

    if (!Socket->TlsRecvBuffer)
    {
        Socket->TlsRecvBuffer = ExAllocatePoolUninitialized(NonPagedPoolNx, SocketTlsRecvCapacity, SOCKET_TAG);

        if (!Socket->TlsRecvBuffer)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (!Socket->TlsPlaintextScratch)
    {
        Socket->TlsPlaintextScratch = ExAllocatePoolUninitialized(NonPagedPoolNx, TLS_RECORD_CIPHERTEXT_MAX, SOCKET_TAG);

        if (!Socket->TlsPlaintextScratch)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Socket->TlsRecvMdl = IoAllocateMdl(Socket->TlsRecvBuffer, SocketTlsRecvCapacity, FALSE, FALSE, NULL);

    if (!Socket->TlsRecvMdl)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MmBuildMdlForNonPagedPool(Socket->TlsRecvMdl);

    return STATUS_SUCCESS;
}

//
// Single free point for a KSOCKET and everything riding on it: the TLS
// connection state and the TLS receive accumulator (allocated by the HTTP
// client, freed here because every socket free funnels through this
// file). Safe at <= DISPATCH_LEVEL: the buffers are NonPagedPoolNx and
// the key handles are dispatch-safe (BCRYPT_PROV_DISPATCH).
//
static VOID FreeKSocket(PKSOCKET Socket)
{
    BlorgTlsDestroyConnectionState(&Socket->Tls);

    if (Socket->TlsRecvMdl)
    {
        IoFreeMdl(Socket->TlsRecvMdl);
    }

    if (Socket->TlsRecvBuffer)
    {
        ExFreePool(Socket->TlsRecvBuffer);
    }

    if (Socket->TlsPlaintextScratch)
    {
        ExFreePool(Socket->TlsPlaintextScratch);
    }

    ExFreePool(Socket);
}

//
// Per-operation timeouts. Generous but bounded -- the point is to fail a
// stuck request in seconds rather than hang forever, not to police latency.
// connect/send rarely block on a healthy LAN peer; receive is given more
// room because the server may legitimately take longer to produce a chunk.
//
#define SOCKET_CONNECT_TIMEOUT_MS  15000
#define SOCKET_SEND_TIMEOUT_MS     15000
#define SOCKET_RECEIVE_TIMEOUT_MS  30000

//
// Arm a per-operation watchdog. Must be called *before* the WSK op is
// issued, so the operation's completion can never observe an un-armed
// timer. Initializes every SOCKET_OP_TIMEOUT field itself (no caller
// zeroing precondition -- contexts come from a lookaside and may carry a
// prior op's state); caller must not have started the op yet.
//
static VOID ArmSocketTimeout(
    PSOCKET_OP_TIMEOUT Timeout,
    PIRP Irp,
    PKDEFERRED_ROUTINE DpcRoutine,
    PVOID DpcContext,
    ULONG TimeoutMs
)
{
    Timeout->Irp = Irp;
    Timeout->RefCount = 2;
    Timeout->TimedOut = 0;

    KeInitializeTimer(&Timeout->Timer);
    KeInitializeDpc(&Timeout->Dpc, DpcRoutine, DpcContext);

    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -C_CAST(LONGLONG, TimeoutMs) * 10 * 1000;

    KeSetTimer(&Timeout->Timer, dueTime, &Timeout->Dpc);
}

//
// Called once at the top of a completion routine. Cancels the watchdog and,
// if the timer was still pending (so the DPC will never run), releases the
// reference reserved for the DPC. Returns the status the caller should
// report, translating a timeout-induced cancel into STATUS_IO_TIMEOUT.
//
static NTSTATUS DisarmSocketTimeout(PSOCKET_OP_TIMEOUT Timeout, NTSTATUS Status)
{
    if (KeCancelTimer(&Timeout->Timer))
    {
        InterlockedDecrement(&Timeout->RefCount);
    }

    if (ReadNoFence(&Timeout->TimedOut) && (STATUS_CANCELLED == Status))
    {
        return STATUS_IO_TIMEOUT;
    }

    return Status;
}

//
// Drop one reference. Returns TRUE to the caller that drives the count to
// zero -- that caller, and only that caller, frees the enclosing context.
//
static BOOLEAN ReleaseSocketTimeoutRef(PSOCKET_OP_TIMEOUT Timeout)
{
    return 0 == InterlockedDecrement(&Timeout->RefCount);
}

const WSK_CLIENT_DISPATCH WskAppDispatch =
{
    MAKE_WSK_VERSION(1,0), // Use WSK version 1.0
    0,    // Reserved
    NULL  // WskClientEvent callback not required for WSK version 1.0
};

//
// --- Synchronous (event-wait) IRP path -----------------------------------
//
// One KSOCKET_CONTEXT (and its IRP) is allocated per call and freed
// before the call returns -- no reuse across calls.
//

static NTSTATUS SocketContextCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (!Context)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeSetEvent(C_CAST(PKEVENT, Context), EVENT_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Allocates the IRP for a synchronous WSK op and wires its completion
// routine to signal SocketContext->CompletionEvent.
//
static NTSTATUS InitialiseSocketContext(PKSOCKET_CONTEXT SocketContext)
{
    KeInitializeEvent(
        &SocketContext->CompletionEvent,
        NotificationEvent,
        FALSE
    );

    SocketContext->Irp = IoAllocateIrp(1, FALSE);

    if (!SocketContext->Irp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoSetCompletionRoutine(
        SocketContext->Irp,
        &SocketContextCompletionRoutine,
        &SocketContext->CompletionEvent,
        TRUE,
        TRUE,
        TRUE
    );

    return STATUS_SUCCESS;
}

// Releases the IRP allocated by InitialiseSocketContext.
static void FreeSocketContext(PKSOCKET_CONTEXT SocketContext)
{
    IoFreeIrp(SocketContext->Irp);
}

//
// If the op is still pending, blocks on CompletionEvent and replaces
// *Status with the IRP's final status. No-op if already completed.
//
static void WaitForCompletionSocketContext(PKSOCKET_CONTEXT SocketContext, PNTSTATUS Status)
{
    if (*Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(
            &SocketContext->CompletionEvent,
            Executive,
            KernelMode,
            FALSE,
            NULL
        );

        *Status = SocketContext->Irp->IoStatus.Status;
    }
}

//
// --- Asynchronous (callback) IRP path -------------------------------------
//
// Same per-call ownership model as the sync path: every async
// send/receive draws its own KSOCKET_ASYNC_CONTEXT from the lookaside
// (plus an IRP), and both are released unconditionally in
// SocketAsyncCompletionRoutine once the caller's callback returns. Never
// reused across concurrent ops -- the timeout DPC may still hold a
// reference after the completion routine has run, so a context only goes
// back to the lookaside when the refcount hits zero.
//

static PKSOCKET_ASYNC_CONTEXT AllocateAsyncSocketContext(
    PKSOCKET_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext
)
{
    PKSOCKET_ASYNC_CONTEXT asyncContext = ExAllocateFromNPagedLookasideList(&AsyncContextLookaside);

    if (!asyncContext)
    {
        return NULL;
    }

    asyncContext->Irp = IoAllocateIrp(1, FALSE);

    if (!asyncContext->Irp)
    {
        ExFreeToNPagedLookasideList(&AsyncContextLookaside, asyncContext);
        return NULL;
    }

    asyncContext->CompletionRoutine = CompletionRoutine;
    asyncContext->CompletionContext = CompletionContext;
    asyncContext->Mdl = NULL;

    IoSetCompletionRoutine(
        asyncContext->Irp,
        &SocketAsyncCompletionRoutine,
        asyncContext,
        TRUE,
        TRUE,
        TRUE
    );

    return asyncContext;
}

//
// Watchdog DPC for an async send/receive. Cancels the in-flight IRP (which
// drives SocketAsyncCompletionRoutine to run with STATUS_CANCELLED) and
// drops the timeout's reference -- freeing the context here only if the
// completion routine has already run and released its own reference.
//
static VOID SocketAsyncTimeoutDpc(PKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    PKSOCKET_ASYNC_CONTEXT asyncContext = Context;

	if (!asyncContext)
	{
		return;
	}

    BLORGFS_STAT_INC(SocketTimeouts);

    InterlockedExchange(&asyncContext->Timeout.TimedOut, 1);
    IoCancelIrp(asyncContext->Timeout.Irp);

    if (ReleaseSocketTimeoutRef(&asyncContext->Timeout))
    {
        IoFreeIrp(asyncContext->Irp);
        ExFreeToNPagedLookasideList(&AsyncContextLookaside, asyncContext);
    }
}

//
// Completion routine for async send/receive: disarms the timeout,
// unlocks/frees an owned MDL, invokes the caller's callback, then frees
// the IRP and context once the timeout ref is also released. The callback
// runs at <= DISPATCH_LEVEL and must not block; if it wants to issue
// another async op it must use a fresh AllocateAsyncSocketContext call (or
// a queued work item), never a reuse of this context/IRP.
//
static NTSTATUS SocketAsyncCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PKSOCKET_ASYNC_CONTEXT asyncContext = Context;

    if (!asyncContext)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = DisarmSocketTimeout(&asyncContext->Timeout, Irp->IoStatus.Status);
    ULONG_PTR bytesTransferred = Irp->IoStatus.Information;

    if (asyncContext->Mdl)
    {
        MmUnlockPages(asyncContext->Mdl);
        IoFreeMdl(asyncContext->Mdl);
        asyncContext->Mdl = NULL;
    }

    if (asyncContext->CompletionRoutine)
    {
        asyncContext->CompletionRoutine(status, bytesTransferred, asyncContext->CompletionContext);
    }

    if (ReleaseSocketTimeoutRef(&asyncContext->Timeout))
    {
        IoFreeIrp(asyncContext->Irp);
        ExFreeToNPagedLookasideList(&AsyncContextLookaside, asyncContext);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

///////////////////////////////////////////////////////////////////////////////////////////////

NTSTATUS BlorgInitialiseWskClient(void)
{
    WSK_CLIENT_NPI wskClientNpi =
    {
        .ClientContext = NULL,
        .Dispatch = &WskAppDispatch
    };

    NTSTATUS result = WskRegister(&wskClientNpi, &WskRegistration);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("WSK Registration Failed: 0x%X\n", result);
        return result;
    }

    result = WskCaptureProviderNPI(&WskRegistration, WSK_INFINITE_WAIT, &WskProviderNpi);

    if (!NT_SUCCESS(result))
    {
        WskDeregister(&WskRegistration);
        BLORGFS_PRINT("WSK Provider Capture Failed: 0x%X\n", result);
        return result;
    }
    KeInitializeSpinLock(&SocketPool.Lock);
    InitializeListHead(&SocketPool.List);
    SocketPool.Count = 0;

    ExInitializeNPagedLookasideList(&AsyncContextLookaside, NULL, NULL, POOL_NX_ALLOCATION, sizeof(KSOCKET_ASYNC_CONTEXT), SOCKET_TAG, 0);

    ULONG tlsRecvRecords;

    switch (MmQuerySystemSize())
    {
        case MmSmallSystem:
        {
            tlsRecvRecords = SOCKET_TLS_RECV_RECORDS_SMALL;
            break;
        }
        case MmMediumSystem:
        {
            tlsRecvRecords = SOCKET_TLS_RECV_RECORDS_MEDIUM;
            break;
        }
        default:
        {
            tlsRecvRecords = SOCKET_TLS_RECV_RECORDS_LARGE;
            break;
        }
    }

    SocketTlsRecvCapacity = tlsRecvRecords * SOCKET_TLS_RECORD_MAX_BYTES;

    SocketMaxPoolSize = C_CAST(ULONG, BlorgPrefetchMaxChunks()) + SOCKET_POOL_HEADROOM;

    return STATUS_SUCCESS;
}

//
// Tears down the WSK client: drains the pooled sockets, releases the
// provider NPI, then deregisters. Order matters -- sockets must be
// closed before the provider they were obtained from is released.
//
void BlorgCleanupWskClient(void)
{
    BlorgCleanupWskSocketPool();
    WskReleaseProviderNPI(&WskRegistration);
    WskDeregister(&WskRegistration);
    ExDeleteNPagedLookasideList(&AsyncContextLookaside);
}

//
// Drains and synchronously closes every pooled socket. Releases the pool
// lock around each CloseWskSocket call since that call waits on an IRP
// and must not hold a spinlock across a blocking wait.
//
VOID BlorgCleanupWskSocketPool(void)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&SocketPool.Lock, &oldIrql);

    while (!IsListEmpty(&SocketPool.List))
    {
        PLIST_ENTRY listEntry = RemoveHeadList(&SocketPool.List);
        SocketPool.Count--;
        KeReleaseSpinLock(&SocketPool.Lock, oldIrql);

        PKSOCKET socket = CONTAINING_RECORD(listEntry, KSOCKET, PoolEntry);
        CloseWskSocket(socket);

        KeAcquireSpinLock(&SocketPool.Lock, &oldIrql);
    }

    KeReleaseSpinLock(&SocketPool.Lock, oldIrql);
}

//
// Synchronous DNS/address resolution via WskGetAddressInfo, using the
// per-call IRP/event pattern to block until the lookup completes.
//
NTSTATUS BlorgGetWskAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo)
{
    KSOCKET_CONTEXT socketContext;

    NTSTATUS result = InitialiseSocketContext(&socketContext);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("Failed InitialiseSocketContext(): 0x%X\n", result);
        return result;
    }

    result = WskProviderNpi.Dispatch->WskGetAddressInfo(
        WskProviderNpi.Client,
        C_CAST(PUNICODE_STRING, NodeName),
        C_CAST(PUNICODE_STRING, ServiceName),
        0,
        NULL,
        C_CAST(PADDRINFOEXW, Hints),
        RemoteAddrInfo,
        NULL,
        NULL,
        socketContext.Irp
    );

    WaitForCompletionSocketContext(&socketContext, &result);

    FreeSocketContext(&socketContext);

    return result;
}

// Frees an ADDRINFOEXW chain returned by BlorgGetWskAddrInfo.
void BlorgFreeWskAddrInfo(PADDRINFOEXW AddrInfo)
{
    WskProviderNpi.Dispatch->WskFreeAddressInfo(
        WskProviderNpi.Client,
        AddrInfo
    );
}

//
// Synchronously closes a socket and frees its KSOCKET, including TLS
// connection state. PASSIVE_LEVEL only (blocks waiting for the close IRP);
// use BlorgCloseWskSocketAsync from the DISPATCH_LEVEL completion chain instead.
// If the IRP allocation fails, the KSOCKET is still freed -- there is no
// path that lets the caller retry a close, and leaking the struct on an
// already-rare allocation failure is worse than leaking the (already
// broken) underlying socket.
//
static NTSTATUS CloseWskSocket(PKSOCKET Socket)
{
    KSOCKET_CONTEXT socketContext;

    NTSTATUS result = InitialiseSocketContext(&socketContext);

    if (!NT_SUCCESS(result))
    {
        FreeKSocket(Socket);
        return result;
    }

    result = Socket->WskConnectionDispatch->WskCloseSocket(
        Socket->WskSocket,
        socketContext.Irp
    );

    WaitForCompletionSocketContext(&socketContext, &result);

    FreeSocketContext(&socketContext);

    FreeKSocket(Socket);

    return result;
}

//
// Fire-and-forget close. Unlike CloseWskSocket, this never waits, so it is
// safe to call from the WSK completion routines (<= DISPATCH_LEVEL) that
// drive the async HTTP pipeline. The IRP, the KSOCKET, and this context are
// all owned by SocketCloseAsyncCompletionRoutine once WskCloseSocket is
// issued, and freed there. Use the synchronous CloseWskSocket only for
// PASSIVE_LEVEL teardown (BlorgCleanupWskSocketPool).
//

// Context for a single async socket close, owned by its completion routine.
typedef struct _KSOCKET_CLOSE_ASYNC_CONTEXT
{
    PIRP Irp;
    PKSOCKET Socket;
} KSOCKET_CLOSE_ASYNC_CONTEXT, * PKSOCKET_CLOSE_ASYNC_CONTEXT;

static IO_COMPLETION_ROUTINE SocketCloseAsyncCompletionRoutine;

//
// Completion for an async socket close: frees the IRP, destroys TLS
// connection state, and frees the KSOCKET and close context. Safe at
// DISPATCH_LEVEL because the TLS key handles were opened with
// BCRYPT_PROV_DISPATCH (TlsAesGcmProvider / BlorgTlsGlobalInit), so
// BlorgTlsDestroyConnectionState's BCryptDestroyKey calls are guaranteed
// dispatch-safe here. Returns STATUS_MORE_PROCESSING_REQUIRED since this
// IRP was allocated by IoAllocateIrp and we own its completion.
//
static NTSTATUS SocketCloseAsyncCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    PKSOCKET_CLOSE_ASYNC_CONTEXT closeCtx = Context;

    if (!closeCtx)
    {
        return STATUS_INVALID_PARAMETER;
    }

    IoFreeIrp(closeCtx->Irp);

    FreeKSocket(closeCtx->Socket);
    ExFreePool(closeCtx);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Issues WskCloseSocket without waiting; ownership of the socket, IRP,
// and close context transfers to SocketCloseAsyncCompletionRoutine as
// soon as the close is issued. Safe to call at DISPATCH_LEVEL. If the
// close context can't be allocated, the socket is deliberately leaked
// rather than falling back to a blocking close -- under pool exhaustion
// there is no context left to drive an async close, and a
// KeWaitForSingleObject here would be a fatal IRQL violation on this
// rare path.
//
NTSTATUS BlorgCloseWskSocketAsync(PKSOCKET Socket)
{
    if (!Socket)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PKSOCKET_CLOSE_ASYNC_CONTEXT closeCtx = ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(KSOCKET_CLOSE_ASYNC_CONTEXT),
        SOCKET_TAG
    );

    if (!closeCtx)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    closeCtx->Irp = IoAllocateIrp(1, FALSE);

    if (!closeCtx->Irp)
    {
        ExFreePool(closeCtx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    closeCtx->Socket = Socket;

    IoSetCompletionRoutine(
        closeCtx->Irp,
        &SocketCloseAsyncCompletionRoutine,
        closeCtx,
        TRUE,
        TRUE,
        TRUE
    );

    Socket->WskConnectionDispatch->WskCloseSocket(
        Socket->WskSocket,
        closeCtx->Irp
    );

    return STATUS_PENDING;
}

//
// restrict: the one call site always passes two distinct SOCKADDR_STORAGE
// objects (a pooled socket's own RemoteAddress vs. the caller's), never
// the same memory -- this is a kernel-only C TU (no C++ consumers, unlike
// Tls.h), so plain `restrict` applies directly, no portability macro needed.
//
static BOOLEAN SockAddrEqual(const SOCKADDR* restrict A, const SOCKADDR* restrict B)
{
    if (A->sa_family != B->sa_family)
    {
        return FALSE;
    }

    if (A->sa_family == AF_INET)
    {
        PSOCKADDR_IN a4 = C_CAST(PSOCKADDR_IN, A);
        PSOCKADDR_IN b4 = C_CAST(PSOCKADDR_IN, B);
        return (a4->sin_port == b4->sin_port) &&
            (a4->sin_addr.s_addr == b4->sin_addr.s_addr);
    }

    if (A->sa_family == AF_INET6)
    {
        PSOCKADDR_IN6 a6 = C_CAST(PSOCKADDR_IN6, A);
        PSOCKADDR_IN6 b6 = C_CAST(PSOCKADDR_IN6, B);
        return (a6->sin6_port == b6->sin6_port) &&
            (RtlCompareMemory(&a6->sin6_addr, &b6->sin6_addr, sizeof(IN6_ADDR)) == sizeof(IN6_ADDR));
    }

    return FALSE;
}

//
// Pool ownership model: a KSOCKET handed out by BlorgAcquireReusableWskSocketAsync
// belongs exclusively to that caller until it is passed back to
// BlorgReleaseReusableWskSocket (or closed on failure). It should not be used
// concurrently from more than one thread/operation at a time -- each
// send/receive/close allocates its own IRP per call, so concurrent
// use on one socket is a correctness issue (interleaved writes/reads on
// the wire) rather than a kernel-memory hazard, but it's still not a
// supported usage pattern. The pool itself does not need its own
// busy-tracking beyond list membership: a socket is either "in the list"
// (idle, owned by the pool) or "out" (owned by exactly one caller), and
// the spinlock only ever protects list membership transitions, never an
// in-flight I/O operation.
// 
// LIFO: the most recently used connection goes back on the head,
// where BlorgAcquireReusableWskSocketAsync's RemoveHeadList will hand it
// out next. A FIFO here cycles through all pooled connections,
// maximizing each one's idle time (and with it the peer idle-close
// races the retry path exists for); LIFO keeps a hot working set
// sized by actual concurrency and lets the tail go cold.
//
// Called from the async HTTP pipeline at DISPATCH_LEVEL, so a socket that
// doesn't fit in the pool is closed via the non-blocking BlorgCloseWskSocketAsync
// rather than the synchronous CloseWskSocket.
//
NTSTATUS BlorgReleaseReusableWskSocket(PKSOCKET Socket)
{
    if (!Socket)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&SocketPool.Lock, &oldIrql);

    if (SocketPool.Count >= SocketMaxPoolSize)
    {
        KeReleaseSpinLock(&SocketPool.Lock, oldIrql);
        BLORGFS_STAT_INC(ConnectionsClosedPoolFull);
        return BlorgCloseWskSocketAsync(Socket);
    }

    InsertHeadList(&SocketPool.List, &Socket->PoolEntry);
    SocketPool.Count++;
    KeReleaseSpinLock(&SocketPool.Lock, oldIrql);

    BLORGFS_STAT_INC(ConnectionsReleasedToPool);

    return STATUS_SUCCESS;
}

//
// Caller contract: on a failed send/receive, do not call
// BlorgReleaseReusableWskSocket -- call CloseWskSocket (or just drop the
// socket and let it be closed) instead, so a connection the peer may
// have already torn down never goes back into the pool to be handed to
// a different caller.
//

//
// Async path: one IRP + one KSOCKET_ASYNC_CONTEXT per call, freed in
// SocketAsyncCompletionRoutine once CompletionRoutine returns. No
// BytesWritten out-param -- the result is only known once the operation
// completes, which is after this function has already returned, so the
// byte count is delivered to CompletionRoutine instead (its
// BytesTransferred parameter).
//
// Socket ownership: per the pool's "one socket per caller" contract, the
// caller must not issue a second async op on the same KSOCKET until the
// CompletionRoutine for the first has run. Nothing here enforces that --
// it's the same single-owner discipline the pool already depends on,
// just without a wait to make it automatic.
//

NTSTATUS BlorgSendWskAsync(PKSOCKET Socket, const void* Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext)
{
    return BlorgSendRecvWskAsync(Socket, C_CAST(PVOID, Buffer), Length, Flags, TRUE, CompletionRoutine, CompletionContext);
}

//
// Async receive into a plain (non-MDL) buffer; thin wrapper over
// BlorgSendRecvWskAsync with Send = FALSE.
//
NTSTATUS BlorgReceiveWskAsync(PKSOCKET Socket, PVOID Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext)
{
    return BlorgSendRecvWskAsync(Socket, Buffer, Length, Flags, FALSE, CompletionRoutine, CompletionContext);
}

//
// Async receive directly into a caller-owned MDL. The MDL is borrowed,
// not owned: asyncContext->Mdl stays NULL (set by
// AllocateAsyncSocketContext), so SocketAsyncCompletionRoutine's
// unlock/free of an owned MDL is naturally skipped -- no mode flag needed.
// Same ownership handoff as BlorgSendRecvWskAsync: the watchdog is armed before
// the op is issued, and asyncContext must not be touched again after the
// WskReceive call. Same return contract too: STATUS_PENDING once WskReceive
// is issued (CompletionRoutine owns the outcome), a hard error only for the
// pre-issue context-allocation failure (caller must complete the request).
//
NTSTATUS BlorgReceiveWskAsyncMdl(
    PKSOCKET Socket,
    PMDL Mdl,
    ULONG Offset,
    ULONG Length,
    ULONG Flags,
    PKSOCKET_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext
)
{
    PKSOCKET_ASYNC_CONTEXT asyncContext = AllocateAsyncSocketContext(CompletionRoutine, CompletionContext);

    if (!asyncContext)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WSK_BUF wskBuffer =
    {
        .Offset = Offset,
        .Length = Length,
        .Mdl = Mdl
    };

    ArmSocketTimeout(
        &asyncContext->Timeout,
        asyncContext->Irp,
        SocketAsyncTimeoutDpc,
        asyncContext,
        SOCKET_RECEIVE_TIMEOUT_MS);

    Socket->WskConnectionDispatch->WskReceive(
        Socket->WskSocket,
        &wskBuffer,
        Flags,
        asyncContext->Irp
    );

    return STATUS_PENDING;
}

//
// Allocates and probes/locks an MDL over Buffer, arms the watchdog, then
// issues WskSend or WskReceive. The watchdog is armed before the op is
// issued -- once WskSend/WskReceive is called the completion can run on
// another CPU immediately and must never see an un-armed timer. From that
// point on asyncContext (and its IRP/MDL) is owned jointly by the
// completion routine and the timeout DPC (refcount), and is never touched
// directly here again.
//
// Return contract: STATUS_PENDING once the WSK op has been issued -- from
// then on the CompletionRoutine is guaranteed to run (WSK always completes
// the IRP) and owns the outcome, so callers must NOT complete the request
// themselves. A hard error (STATUS_INSUFFICIENT_RESOURCES, a probe fault
// code) is returned ONLY for the pre-issue setup failures below, where the
// CompletionRoutine never runs and no watchdog is armed -- on those the
// caller must fail/complete the request itself. So the single caller rule
// is: if the return is not STATUS_PENDING, fail the request with it.
//
NTSTATUS BlorgSendRecvWskAsync(
    PKSOCKET Socket,
    PVOID Buffer,
    ULONG Length,
    ULONG Flags,
    BOOLEAN Send,
    PKSOCKET_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext
)
{
    PKSOCKET_ASYNC_CONTEXT asyncContext = AllocateAsyncSocketContext(CompletionRoutine, CompletionContext);

    if (!asyncContext)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    asyncContext->Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);

    if (!asyncContext->Mdl)
    {
        IoFreeIrp(asyncContext->Irp);
        ExFreeToNPagedLookasideList(&AsyncContextLookaside, asyncContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try
    {
        MmProbeAndLockPages(asyncContext->Mdl, KernelMode, Send ? IoReadAccess : IoWriteAccess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        NTSTATUS exceptionCode = GetExceptionCode();
        IoFreeMdl(asyncContext->Mdl);
        IoFreeIrp(asyncContext->Irp);
        ExFreeToNPagedLookasideList(&AsyncContextLookaside, asyncContext);
        return exceptionCode;
    }

    WSK_BUF wskBuffer =
    {
        .Offset = 0,
        .Length = Length,
        .Mdl = asyncContext->Mdl
    };

    ArmSocketTimeout(
        &asyncContext->Timeout,
        asyncContext->Irp,
        SocketAsyncTimeoutDpc,
        asyncContext,
        Send ? SOCKET_SEND_TIMEOUT_MS : SOCKET_RECEIVE_TIMEOUT_MS);

    if (Send)
    {
        Socket->WskConnectionDispatch->WskSend(
            Socket->WskSocket,
            &wskBuffer,
            Flags,
            asyncContext->Irp
        );
    }
    else
    {
        Socket->WskConnectionDispatch->WskReceive(
            Socket->WskSocket,
            &wskBuffer,
            Flags,
            asyncContext->Irp
        );
    }

    return STATUS_PENDING;
}

// Context for a single async connect, owned by its completion routine.
typedef struct _KSOCKET_CONNECT_ASYNC_CONTEXT
{
    PIRP Irp;
    PKSOCKET NewSocket;
    SOCKADDR_STORAGE RemoteAddress; // copy: caller's RemoteAddress may not outlive this call
    PKSOCKET_ACQUIRE_COMPLETION_ROUTINE CompletionRoutine;
    PVOID CompletionContext;
    SOCKET_OP_TIMEOUT Timeout;
} KSOCKET_CONNECT_ASYNC_CONTEXT, * PKSOCKET_CONNECT_ASYNC_CONTEXT;

static IO_COMPLETION_ROUTINE SocketConnectAsyncCompletionRoutine;
static KDEFERRED_ROUTINE SocketConnectTimeoutDpc;

//
// Watchdog DPC for an async connect. Same handshake as the send/receive
// DPC: cancel the IRP (forcing the completion to run with STATUS_CANCELLED)
// and free the context only if the completion has already released its ref.
//
static VOID SocketConnectTimeoutDpc(PKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    PKSOCKET_CONNECT_ASYNC_CONTEXT connectCtx = Context;

	if (!connectCtx)
	{
		return;
	}

    BLORGFS_STAT_INC(SocketTimeouts);

    InterlockedExchange(&connectCtx->Timeout.TimedOut, 1);
    IoCancelIrp(connectCtx->Timeout.Irp);

    if (ReleaseSocketTimeoutRef(&connectCtx->Timeout))
    {
        IoFreeIrp(connectCtx->Irp);
        ExFreePool(connectCtx);
    }
}

//
// Completion for an async connect: on success, populates the new
// KSOCKET's dispatch table and remote address and hands it to the
// caller's callback; on failure, frees the half-built socket instead.
// Returns STATUS_MORE_PROCESSING_REQUIRED since this IRP was allocated via
// IoAllocateIrp and we own its completion; the actual free is gated on the
// timeout refcount so the watchdog DPC cannot pull the IRP/context out
// from under a completion running concurrently.
//
static NTSTATUS SocketConnectAsyncCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PKSOCKET_CONNECT_ASYNC_CONTEXT connectCtx = Context;

    if (!connectCtx)
    {
		return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = DisarmSocketTimeout(&connectCtx->Timeout, Irp->IoStatus.Status);

    if (NT_SUCCESS(status))
    {
        connectCtx->NewSocket->WskSocket = C_CAST(PWSK_SOCKET, Irp->IoStatus.Information);
        connectCtx->NewSocket->WskDispatch = C_CAST(PVOID, connectCtx->NewSocket->WskSocket->Dispatch);

        RtlCopyMemory(
            &connectCtx->NewSocket->RemoteAddress,
            &connectCtx->RemoteAddress,
            (connectCtx->RemoteAddress.ss_family == AF_INET6) ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN)
        );

        BLORGFS_STAT_INC(ConnectionsFresh);
        connectCtx->CompletionRoutine(STATUS_SUCCESS, connectCtx->NewSocket, FALSE, connectCtx->CompletionContext);
    }
    else
    {
        BLORGFS_STAT_INC(ConnectionsFailed);
        ExFreePool(connectCtx->NewSocket);
        connectCtx->CompletionRoutine(status, NULL, FALSE, connectCtx->CompletionContext);
    }

    if (ReleaseSocketTimeoutRef(&connectCtx->Timeout))
    {
        IoFreeIrp(connectCtx->Irp);
        ExFreePool(connectCtx);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Fast path: a pooled socket is checked out and handed back synchronously
// (list manipulation under a spinlock, no I/O), which is fine because it's
// bounded, unlike the WSK connect path -- this matches how every other
// "instant" success is handled elsewhere (e.g. cache hits don't get
// bounced through a trampoline either). ForceFresh skips the pool
// entirely: the caller is retrying after a pooled connection turned out
// to be dead, and must not be handed another possibly-stale one. A pooled
// socket connected to the wrong target is closed asynchronously (this
// path is reachable at DISPATCH_LEVEL, so the close must not block) and
// falls through to the slow path below; this is rare, hit only when the
// configured remote address changes.
//
// Slow path: connects a brand-new socket, fully async. The connect
// watchdog is armed before WskSocketConnect is issued -- a dead/unreachable
// peer is exactly the case where the connect would otherwise never
// complete -- after which connectCtx is jointly owned by the completion
// routine and the timeout DPC (refcount) and is never touched here again.
// WskSocketConnect normally returns STATUS_PENDING; the completion routine
// set above handles every outcome, including one that comes back
// already-completed (WSK/IoCompletion still guarantees it runs).
//
// The local address is what fixes the socket's address family -- WSK takes
// it from there, not from the remote -- so it is built to match
// RemoteAddress rather than being a fixed AF_INET wildcard. It used to be
// the latter, which made every IPv6 backend unreachable no matter what the
// resolver returned: DriverEntry resolves AF_UNSPEC and everything from the
// pool comparison down handles AF_INET6, and then the connect asked for an
// IPv4 socket. A zeroed SOCKADDR_STORAGE carrying only the family is the
// wildcard for either one -- INADDR_ANY and in6addr_any are both all-zero,
// as are port, flowinfo and scope id -- so there is no per-family branch to
// keep in step.
//
NTSTATUS BlorgAcquireReusableWskSocketAsync(
    const SOCKADDR* RemoteAddress,
    BOOLEAN ForceFresh,
    PKSOCKET_ACQUIRE_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext
)
{
    if (!RemoteAddress || !CompletionRoutine)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!ForceFresh)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&SocketPool.Lock, &oldIrql);

        if (!IsListEmpty(&SocketPool.List))
        {
            PLIST_ENTRY listEntry = RemoveHeadList(&SocketPool.List);
            SocketPool.Count--;
            KeReleaseSpinLock(&SocketPool.Lock, oldIrql);

            PKSOCKET pooledSocket = CONTAINING_RECORD(listEntry, KSOCKET, PoolEntry);
            RtlZeroMemory(&pooledSocket->PoolEntry, sizeof(pooledSocket->PoolEntry));

            if (SockAddrEqual(C_CAST(const SOCKADDR*, &pooledSocket->RemoteAddress), RemoteAddress))
            {
                BLORGFS_STAT_INC(ConnectionsPooled);
                CompletionRoutine(STATUS_SUCCESS, pooledSocket, TRUE, CompletionContext);
                return STATUS_PENDING;
            }

            BlorgCloseWskSocketAsync(pooledSocket);
        }
        else
        {
            KeReleaseSpinLock(&SocketPool.Lock, oldIrql);
        }
    }

    PKSOCKET newSocket = ExAllocatePoolZero(NonPagedPoolNx, sizeof(KSOCKET), SOCKET_TAG);

    if (!newSocket)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    BlorgTlsInitializeConnectionState(&newSocket->Tls);

    PKSOCKET_CONNECT_ASYNC_CONTEXT connectCtx = ExAllocatePoolZero(
        NonPagedPoolNx,
        sizeof(KSOCKET_CONNECT_ASYNC_CONTEXT),
        SOCKET_TAG
    );

    if (!connectCtx)
    {
        ExFreePool(newSocket);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    connectCtx->Irp = IoAllocateIrp(1, FALSE);

    if (!connectCtx->Irp)
    {
        ExFreePool(connectCtx);
        ExFreePool(newSocket);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    connectCtx->NewSocket = newSocket;
    connectCtx->CompletionRoutine = CompletionRoutine;
    connectCtx->CompletionContext = CompletionContext;

    RtlCopyMemory(
        &connectCtx->RemoteAddress,
        RemoteAddress,
        (RemoteAddress->sa_family == AF_INET6) ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN)
    );

    IoSetCompletionRoutine(
        connectCtx->Irp,
        &SocketConnectAsyncCompletionRoutine,
        connectCtx,
        TRUE,
        TRUE,
        TRUE
    );

    SOCKADDR_STORAGE localAddress = { 0 };

    localAddress.ss_family = RemoteAddress->sa_family;

    ArmSocketTimeout(
        &connectCtx->Timeout,
        connectCtx->Irp,
        SocketConnectTimeoutDpc,
        connectCtx,
        SOCKET_CONNECT_TIMEOUT_MS);

    WskProviderNpi.Dispatch->WskSocketConnect(
        WskProviderNpi.Client,
        SOCK_STREAM,
        IPPROTO_TCP,
        C_CAST(PSOCKADDR, &localAddress),
        C_CAST(PSOCKADDR, RemoteAddress),
        0,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        connectCtx->Irp
    );

    return STATUS_PENDING;
}
