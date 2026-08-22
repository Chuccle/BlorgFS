//
// Scriptable peer, standing in for the WSK transport. See
// SandboxSocket.h for why the script -- not the stub -- is the point.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "SandboxSocket.h"

ULONG SocketTlsRecvCapacity = 16 * (5 + TLS_RECORD_CIPHERTEXT_MAX);

static const SANDBOX_STEP* ScriptSteps = NULL;
static SIZE_T ScriptStepCount = 0;

static ULONG SocketsCreated = 0;
static ULONG SocketsClosed = 0;
static ULONG SocketsPooled = 0;
static ULONG SocketsLive = 0;
static ULONG AcquireFailuresPending = 0;

static SANDBOX_PEER LastPeer;

//
// The keep-alive pool, modelled at exactly the fidelity the client's
// retry logic can observe: one slot, holding at most one idle socket.
// That is enough to produce both cases the client distinguishes -- an
// acquire that reuses a connection (Reused == TRUE, failures retryable)
// and one that connects fresh -- without pretending to reproduce the
// driver's real LIFO pool, which is not the code under test here.
//
static PKSOCKET PooledSocket = NULL;

//
// Per-socket sandbox bookkeeping, kept beside the socket rather than
// inside it. KSOCKET is the driver's own struct now, so anything the model
// wants to remember about a socket has to live here -- which is the point:
// adding a field to the real KSOCKET can no longer be silently shadowed by
// a sandbox copy that has different ones.
//
typedef struct _SANDBOX_SOCKET_STATE
{
    PKSOCKET Socket;
    SANDBOX_PEER* Peer;
    BOOLEAN Closed;
    BOOLEAN Pooled;
    struct _SANDBOX_SOCKET_STATE* Next;
} SANDBOX_SOCKET_STATE;

static SANDBOX_SOCKET_STATE* SocketStates = NULL;

static SANDBOX_SOCKET_STATE* SocketState(PKSOCKET Socket)
{
    for (SANDBOX_SOCKET_STATE* state = SocketStates; state; state = state->Next)
    {
        if (state->Socket == Socket)
        {
            return state;
        }
    }

    SANDBOX_SOCKET_STATE* state = (SANDBOX_SOCKET_STATE*)calloc(1, sizeof(SANDBOX_SOCKET_STATE));

    if (state)
    {
        state->Socket = Socket;
        state->Next = SocketStates;
        SocketStates = state;
    }

    return state;
}

static VOID SocketStateForget(PKSOCKET Socket)
{
    SANDBOX_SOCKET_STATE** link = &SocketStates;

    while (*link)
    {
        if ((*link)->Socket == Socket)
        {
            SANDBOX_SOCKET_STATE* dead = *link;
            *link = dead->Next;
            free(dead);
            return;
        }

        link = &(*link)->Next;
    }
}

//
// Deferred completions. A scripted step marked non-inline lands here and
// runs when the scenario drains, which is what lets a test interleave
// "the response arrives" with anything else it wants to assert first.
//
typedef struct _SANDBOX_DEFERRED
{
    PKSOCKET_COMPLETION_ROUTINE Routine;
    PVOID Context;
    NTSTATUS Status;
    SIZE_T Bytes;
    struct _SANDBOX_DEFERRED* Next;
} SANDBOX_DEFERRED;

static SANDBOX_DEFERRED* DeferredHead = NULL;
static SANDBOX_DEFERRED* DeferredTail = NULL;

typedef struct _SANDBOX_ACQUIRE_DEFERRED
{
    PKSOCKET_ACQUIRE_COMPLETION_ROUTINE Routine;
    PVOID Context;
    NTSTATUS Status;
    PKSOCKET Socket;
    BOOLEAN Reused;
    struct _SANDBOX_ACQUIRE_DEFERRED* Next;
} SANDBOX_ACQUIRE_DEFERRED;

static SANDBOX_ACQUIRE_DEFERRED* AcquireDeferredHead = NULL;

VOID SandboxSetPeerScript(const SANDBOX_STEP* Steps, SIZE_T StepCount)
{
    ScriptSteps = Steps;
    ScriptStepCount = StepCount;
}

VOID SandboxFailNextAcquires(ULONG Count)
{
    AcquireFailuresPending = Count;
}

ULONG SandboxSocketsCreated(VOID) { return SocketsCreated; }
ULONG SandboxSocketsClosed(VOID) { return SocketsClosed; }
ULONG SandboxSocketsPooled(VOID) { return SocketsPooled; }
ULONG SandboxSocketsLive(VOID) { return SocketsLive; }

const unsigned char* SandboxLastRequest(SIZE_T* Length)
{
    if (Length)
    {
        *Length = LastPeer.SentLength;
    }

    return LastPeer.SentBuffer;
}

VOID SandboxSocketsReset(VOID)
{
    SocketsCreated = 0;
    SocketsClosed = 0;
    SocketsPooled = 0;
    SocketsLive = 0;
    AcquireFailuresPending = 0;

    memset(&LastPeer, 0, sizeof(LastPeer));

    while (DeferredHead)
    {
        SANDBOX_DEFERRED* next = DeferredHead->Next;
        free(DeferredHead);
        DeferredHead = next;
    }

    DeferredTail = NULL;

    while (AcquireDeferredHead)
    {
        SANDBOX_ACQUIRE_DEFERRED* next = AcquireDeferredHead->Next;
        free(AcquireDeferredHead);
        AcquireDeferredHead = next;
    }

    if (PooledSocket)
    {
        free(SocketState(PooledSocket)->Peer);
        SocketStateForget(PooledSocket);
        free(PooledSocket);
        PooledSocket = NULL;
    }
}

static VOID QueueDeferred(PKSOCKET_COMPLETION_ROUTINE Routine, PVOID Context, NTSTATUS Status, SIZE_T Bytes)
{
    SANDBOX_DEFERRED* node = (SANDBOX_DEFERRED*)calloc(1, sizeof(SANDBOX_DEFERRED));

    node->Routine = Routine;
    node->Context = Context;
    node->Status = Status;
    node->Bytes = Bytes;

    if (DeferredTail)
    {
        DeferredTail->Next = node;
    }
    else
    {
        DeferredHead = node;
    }

    DeferredTail = node;
}

//
// Completions run at DISPATCH_LEVEL, because that is where WSK delivers
// them and the whole point of the driver's bounce logic is to cope with
// it. Running them at PASSIVE would silently excuse exactly the bugs
// this sandbox exists to catch.
//
VOID SandboxDrainCompletions(VOID)
{
    for (;;)
    {
        while (AcquireDeferredHead)
        {
            SANDBOX_ACQUIRE_DEFERRED* node = AcquireDeferredHead;
            AcquireDeferredHead = node->Next;

            KIRQL saved = KmGetIrql();
            KmSetIrql(DISPATCH_LEVEL);

            node->Routine(node->Status, node->Socket, node->Reused, node->Context);

            KmSetIrql(saved);
            free(node);
        }

        if (!DeferredHead)
        {
            return;
        }

        SANDBOX_DEFERRED* node = DeferredHead;
        DeferredHead = node->Next;

        if (!DeferredHead)
        {
            DeferredTail = NULL;
        }

        KIRQL saved = KmGetIrql();
        KmSetIrql(DISPATCH_LEVEL);

        node->Routine(node->Status, (ULONG_PTR)node->Bytes, node->Context);

        KmSetIrql(saved);
        free(node);
    }
}

///////////////////////////////////////////////////////////////////////////
// Accumulator
///////////////////////////////////////////////////////////////////////////

NTSTATUS EnsureTlsRecvBuffer(PKSOCKET Socket)
{
    if (Socket->TlsRecvMdl)
    {
        return STATUS_SUCCESS;
    }

    if (!Socket->TlsRecvBuffer)
    {
        Socket->TlsRecvBuffer = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx, SocketTlsRecvCapacity, 'TSKS');

        if (!Socket->TlsRecvBuffer)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (!Socket->TlsPlaintextScratch)
    {
        Socket->TlsPlaintextScratch = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx, TLS_RECORD_CIPHERTEXT_MAX, 'TSKS');

        if (!Socket->TlsPlaintextScratch)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Socket->TlsRecvMdl = ShimCreateMdl(Socket->TlsRecvBuffer, SocketTlsRecvCapacity);

    if (!Socket->TlsRecvMdl)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Send / receive
///////////////////////////////////////////////////////////////////////////

NTSTATUS SendWskAsync(PKSOCKET Socket, PVOID Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext)
{
    (void)Flags;

    SANDBOX_PEER* peer = SocketState(Socket)->Peer;

    SIZE_T room = sizeof(peer->SentBuffer) - peer->SentLength;
    SIZE_T copy = (Length < room) ? Length : room;

    memcpy(peer->SentBuffer + peer->SentLength, Buffer, copy);
    peer->SentLength += copy;

    memcpy(&LastPeer, peer, sizeof(LastPeer));

    //
    // Sends always succeed inline. A send failure is not an interesting
    // shape to script: the client treats it exactly like a receive
    // failure, and the receive side can produce that case with far more
    // control over where in the response it happens.
    //
    CompletionRoutine(STATUS_SUCCESS, Length, CompletionContext);

    return STATUS_PENDING;
}

//
// Pulls up to Length bytes out of the script into Destination. Returns
// the step outcome; a Deliver that satisfies only part of the request is
// still a success, which is what makes a short read testable.
//
static NTSTATUS SandboxConsume(
    SANDBOX_PEER* Peer,
    unsigned char* Destination,
    ULONG Length,
    BOOLEAN WaitAll,
    SIZE_T* BytesOut,
    BOOLEAN* InlineOut)
{
    SIZE_T produced = 0;

    *InlineOut = TRUE;

    while (produced < Length)
    {
        if (Peer->StepIndex >= Peer->StepCount)
        {
            //
            // Script exhausted. Treat as a peer that closed: a scenario
            // that under-specifies its script gets a clean, diagnosable
            // end rather than a hang.
            //
            break;
        }

        const SANDBOX_STEP* step = &Peer->Steps[Peer->StepIndex];

        if (SandboxStepDeliver != step->Kind)
        {
            *InlineOut = step->Inline;

            if (produced > 0)
            {
                //
                // Deliver what we have; the terminal step is seen by the
                // next receive. This is how "half a response, then a
                // reset" is expressed.
                //
                break;
            }

            Peer->StepIndex++;

            if (SandboxStepClose == step->Kind)
            {
                *BytesOut = 0;
                return STATUS_SUCCESS;
            }

            if (SandboxStepStall == step->Kind)
            {
                Peer->Stalled = TRUE;
                *BytesOut = 0;
                return STATUS_PENDING;
            }

            *BytesOut = 0;
            return step->Status;
        }

        SIZE_T available = step->Length - Peer->StepOffset;
        SIZE_T want = Length - produced;
        SIZE_T take = (available < want) ? available : want;

        memcpy(Destination + produced, step->Data + Peer->StepOffset, take);

        produced += take;
        Peer->StepOffset += take;
        *InlineOut = step->Inline;

        if (Peer->StepOffset >= step->Length)
        {
            Peer->StepIndex++;
            Peer->StepOffset = 0;
        }

        //
        // Without WAITALL a receive completes on whatever has arrived, so
        // one Deliver step satisfies it and the rest waits for the next
        // receive. With WAITALL the loop keeps pulling until the caller's
        // buffer is full, matching the flag's contract.
        //
        if (!WaitAll)
        {
            break;
        }
    }

    *BytesOut = produced;

    return STATUS_SUCCESS;
}

static NTSTATUS SandboxReceiveCommon(
    PKSOCKET Socket,
    unsigned char* Destination,
    ULONG Length,
    ULONG Flags,
    PKSOCKET_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext)
{
    SIZE_T bytes = 0;
    BOOLEAN completeInline = TRUE;

    NTSTATUS status = SandboxConsume(
        SocketState(Socket)->Peer,
        Destination,
        Length,
        (Flags & WSK_FLAG_WAITALL) ? TRUE : FALSE,
        &bytes,
        &completeInline);

    if (STATUS_PENDING == status)
    {
        // Stalled: nothing completes, the request stays parked.
        return STATUS_PENDING;
    }

    if (completeInline)
    {
        KIRQL saved = KmGetIrql();
        KmSetIrql(DISPATCH_LEVEL);

        CompletionRoutine(status, (ULONG_PTR)bytes, CompletionContext);

        KmSetIrql(saved);
    }
    else
    {
        QueueDeferred(CompletionRoutine, CompletionContext, status, bytes);
    }

    return STATUS_PENDING;
}

NTSTATUS ReceiveWskAsync(PKSOCKET Socket, PVOID Buffer, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext)
{
    return SandboxReceiveCommon(Socket, (unsigned char*)Buffer, Length, Flags, CompletionRoutine, CompletionContext);
}

NTSTATUS ReceiveWskAsyncMdl(PKSOCKET Socket, PMDL Mdl, ULONG Offset, ULONG Length, ULONG Flags, PKSOCKET_COMPLETION_ROUTINE CompletionRoutine, PVOID CompletionContext)
{
    if (Offset + Length > Mdl->Length)
    {
        fprintf(stderr, "[sandbox] MDL receive window %lu+%lu exceeds MDL length %zu\n",
            Offset, Length, Mdl->Length);
        abort();
    }

    return SandboxReceiveCommon(
        Socket,
        ((unsigned char*)Mdl->Base) + Offset,
        Length,
        Flags,
        CompletionRoutine,
        CompletionContext);
}

///////////////////////////////////////////////////////////////////////////
// Connection lifecycle
///////////////////////////////////////////////////////////////////////////

static PKSOCKET SandboxCreateSocket(void)
{
    PKSOCKET socket = (PKSOCKET)calloc(1, sizeof(KSOCKET));

    if (!socket)
    {
        return NULL;
    }

    SocketState(socket)->Peer = (SANDBOX_PEER*)calloc(1, sizeof(SANDBOX_PEER));

    if (!SocketState(socket)->Peer)
    {
        free(socket);
        return NULL;
    }

    SocketState(socket)->Peer->Steps = ScriptSteps;
    SocketState(socket)->Peer->StepCount = ScriptStepCount;

    TlsInitializeConnectionState(&socket->Tls);

    SocketsCreated++;
    SocketsLive++;

    return socket;
}

static VOID SandboxDestroySocket(PKSOCKET Socket)
{
    if (Socket->TlsRecvMdl)
    {
        ShimFreeMdl(Socket->TlsRecvMdl);
    }

    if (Socket->TlsRecvBuffer)
    {
        ExFreePool(Socket->TlsRecvBuffer);
    }

    if (Socket->TlsPlaintextScratch)
    {
        ExFreePool(Socket->TlsPlaintextScratch);
    }

    TlsDestroyConnectionState(&Socket->Tls);

    free(SocketState(Socket)->Peer);
    SocketStateForget(Socket);
    free(Socket);

    SocketsLive--;
}

NTSTATUS AcquireReusableWskSocketAsync(
    PSOCKADDR RemoteAddress,
    BOOLEAN ForceFresh,
    PKSOCKET_ACQUIRE_COMPLETION_ROUTINE CompletionRoutine,
    PVOID CompletionContext)
{
    (void)RemoteAddress;

    if (AcquireFailuresPending > 0)
    {
        AcquireFailuresPending--;
        CompletionRoutine(STATUS_CONNECTION_DISCONNECTED, NULL, FALSE, CompletionContext);
        return STATUS_PENDING;
    }

    if (!ForceFresh && PooledSocket)
    {
        PKSOCKET reused = PooledSocket;
        PooledSocket = NULL;
        SocketState(reused)->Pooled = FALSE;

        CompletionRoutine(STATUS_SUCCESS, reused, TRUE, CompletionContext);
        return STATUS_PENDING;
    }

    PKSOCKET fresh = SandboxCreateSocket();

    if (!fresh)
    {
        CompletionRoutine(STATUS_INSUFFICIENT_RESOURCES, NULL, FALSE, CompletionContext);
        return STATUS_PENDING;
    }

    CompletionRoutine(STATUS_SUCCESS, fresh, FALSE, CompletionContext);
    return STATUS_PENDING;
}

NTSTATUS ReleaseReusableWskSocket(PKSOCKET Socket)
{
    if (!Socket)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SocketsPooled++;

    if (PooledSocket)
    {
        SandboxDestroySocket(Socket);
        SocketsClosed++;
        return STATUS_SUCCESS;
    }

    SocketState(Socket)->Pooled = TRUE;
    PooledSocket = Socket;

    return STATUS_SUCCESS;
}

NTSTATUS CloseWskSocketAsync(PKSOCKET Socket)
{
    if (!Socket)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (SocketState(Socket)->Closed)
    {
        fprintf(stderr, "[sandbox] socket closed twice\n");
        abort();
    }

    SocketState(Socket)->Closed = TRUE;
    SocketsClosed++;

    SandboxDestroySocket(Socket);

    return STATUS_PENDING;
}

NTSTATUS InitialiseWskClient(void)
{
    return STATUS_SUCCESS;
}

void CleanupWskClient(void)
{
    if (PooledSocket)
    {
        SandboxDestroySocket(PooledSocket);
        PooledSocket = NULL;
    }
}


//
// Address resolution is DNS, not driver logic, and the sandbox hands the
// client a fixed loopback address at init instead.
//
NTSTATUS GetWskAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo)
{
    (void)NodeName;
    (void)ServiceName;
    (void)Hints;

    *RemoteAddrInfo = global.RemoteAddressInfo;
    return STATUS_SUCCESS;
}

void FreeWskAddrInfo(PADDRINFOEXW AddrInfo)
{
    (void)AddrInfo;
}

///////////////////////////////////////////////////////////////////////////
// Scenario lifecycle
///////////////////////////////////////////////////////////////////////////

//
// Points the client at a scripted loopback backend and resets everything a
// scenario can perturb. This lives with the peer model rather than in the
// NT shim because it configures the subject of the test -- which host the
// client dials, whether TLS is on -- not the kernel underneath it.
//
static SOCKADDR_IN SandboxRemoteAddress;
static ADDRINFOEXW SandboxAddrInfo;
static char SandboxHostAnsi[] = "sandbox.blorg.lan";

VOID SandboxInitialize(VOID)
{
    ShimReset();

    memset(&SandboxRemoteAddress, 0, sizeof(SandboxRemoteAddress));
    SandboxRemoteAddress.sin_family = AF_INET;
    SandboxRemoteAddress.sin_port = htons(80);
    SandboxRemoteAddress.sin_addr.s_addr = htonl(0x7F000001);

    memset(&SandboxAddrInfo, 0, sizeof(SandboxAddrInfo));
    SandboxAddrInfo.ai_family = AF_INET;
    SandboxAddrInfo.ai_addrlen = sizeof(SOCKADDR_IN);
    SandboxAddrInfo.ai_addr = (struct sockaddr*)&SandboxRemoteAddress;

    global.RemoteHostAnsi = SandboxHostAnsi;
    global.RemoteHostSniAnsi = SandboxHostAnsi;
    global.TlsEnabled = FALSE;
    global.RemoteAddressInfo = &SandboxAddrInfo;
    global.FileSystemDeviceObject = NULL;

    ShimSetRemainingStack(64 * 1024);

    SandboxSocketsReset();
}

VOID SandboxCleanup(VOID)
{
    ShimDrainWorkItems();
}
