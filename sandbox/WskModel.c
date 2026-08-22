//
// The WSK provider model. See WskModel.h for why the completion timing is
// under test control rather than fixed.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\Driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _WSK_MODEL_CONNECTION
{
    WSK_SOCKET Socket;
    BOOLEAN Closed;
} WSK_MODEL_CONNECTION;

//
// One outstanding operation. Held so a deferred completion, or a
// cancellation the driver initiates, can be finished later.
//
typedef struct _WSK_MODEL_PENDING
{
    PIRP Irp;
    PMDL Mdl;
    ULONG Offset;
    SIZE_T Length;
    WSK_MODEL_BEHAVIOUR Behaviour;
    BOOLEAN IsReceive;
    struct _WSK_MODEL_PENDING* Next;
} WSK_MODEL_PENDING;

static WSK_MODEL_PENDING* PendingHead = NULL;
static CRITICAL_SECTION PendingCs;
static long PendingCsInit = 0;

static WSK_MODEL_BEHAVIOUR SendBehaviour;
static WSK_MODEL_BEHAVIOUR ReceiveBehaviour;
static WSK_MODEL_BEHAVIOUR ConnectBehaviour;

static volatile LONG Connects = 0;
static volatile LONG Sends = 0;
static volatile LONG Receives = 0;
static volatile LONG Closes = 0;
static volatile LONG Cancelled = 0;

//
// See WskModelLastSendBytes (WskModel.h) for why this is captured at
// WskModelSend time rather than read back out of the completed IRP.
//
#define WSK_MODEL_LAST_SEND_CAPACITY 4096
static unsigned char LastSendBuffer[WSK_MODEL_LAST_SEND_CAPACITY];
static SIZE_T LastSendLength = 0;

//
// The provider hands this back as the client handle; nothing
// dereferences it, so an address is all it needs to be.
//
static int ModelClientStorage;
#define ModelClient (*(PWSK_CLIENT)&ModelClientStorage)

static void EnsurePendingCs(void)
{
    if (0 == InterlockedCompareExchange(&PendingCsInit, 1, 0))
    {
        InitializeCriticalSection(&PendingCs);
        InterlockedExchange(&PendingCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&PendingCsInit, 2, 2))
    {
        Sleep(0);
    }
}

///////////////////////////////////////////////////////////////////////////
// Completion
///////////////////////////////////////////////////////////////////////////

//
// Delivers one operation's result. Receives copy their payload into the
// caller's MDL window first, because the driver reads that buffer inside
// its completion routine -- filling it afterwards would let a bug that
// reads uninitialised data pass.
//
static void WskModelComplete(WSK_MODEL_PENDING* Pending, NTSTATUS Status, SIZE_T Bytes)
{
    if (Pending->IsReceive && NT_SUCCESS(Status) && Bytes > 0 && Pending->Mdl)
    {
        SIZE_T copy = Bytes;

        if (copy > Pending->Length)
        {
            copy = Pending->Length;
        }

        if (Pending->Behaviour.Payload && copy > Pending->Behaviour.PayloadLength)
        {
            copy = Pending->Behaviour.PayloadLength;
        }

        if (Pending->Behaviour.Payload)
        {
            memcpy(((unsigned char*)Pending->Mdl->Base) + Pending->Offset,
                Pending->Behaviour.Payload, copy);
        }

        Bytes = copy;
    }

    Pending->Irp->IoStatus.Status = Status;
    Pending->Irp->IoStatus.Information = (ULONG_PTR)Bytes;

    IoCompleteRequest(Pending->Irp, IO_NO_INCREMENT);
}

//
// Runs a completion at DISPATCH_LEVEL, which is where WSK delivers them.
// Running at the caller's level would silently excuse the bugs the model
// exists to catch.
//
static void WskModelCompleteAtDispatch(WSK_MODEL_PENDING* Pending, NTSTATUS Status, SIZE_T Bytes)
{
    unsigned char saved = KmGetIrql();
    KmSetIrql(DISPATCH_LEVEL);

    WskModelComplete(Pending, Status, Bytes);

    KmSetIrql(saved);
}

static void WskModelQueuePending(WSK_MODEL_PENDING* Pending)
{
    EnsurePendingCs();
    EnterCriticalSection(&PendingCs);

    Pending->Next = PendingHead;
    PendingHead = Pending;

    LeaveCriticalSection(&PendingCs);
}

static WSK_MODEL_PENDING* WskModelTakePending(void)
{
    EnsurePendingCs();
    EnterCriticalSection(&PendingCs);

    WSK_MODEL_PENDING* pending = PendingHead;

    if (pending)
    {
        PendingHead = pending->Next;
    }

    LeaveCriticalSection(&PendingCs);

    return pending;
}

int WskModelReleaseDeferred(VOID)
{
    int released = 0;

    for (;;)
    {
        WSK_MODEL_PENDING* pending = WskModelTakePending();

        if (!pending)
        {
            return released;
        }

        if (WskModelNever == pending->Behaviour.Completion)
        {
            //
            // Never-completing operations stay outstanding. Putting them
            // back rather than dropping them is what lets the watchdog be
            // the only thing that ends them.
            //
            WskModelQueuePending(pending);
            return released;
        }

        WskModelCompleteAtDispatch(pending, pending->Behaviour.Status, pending->Behaviour.Bytes);

        free(pending);
        released++;
    }
}

int WskModelDeferredCount(VOID)
{
    int count = 0;

    EnsurePendingCs();
    EnterCriticalSection(&PendingCs);

    for (WSK_MODEL_PENDING* p = PendingHead; p; p = p->Next)
    {
        count++;
    }

    LeaveCriticalSection(&PendingCs);

    return count;
}

//
// The transport noticing a cancel. In the kernel this is asynchronous, so
// the test drives it explicitly -- which also lets a test choose to pump
// cancellations BEFORE or AFTER a racing real completion, and so exercise
// both orderings of the driver's refcount protocol.
//
int WskModelPumpCancellations(VOID)
{
    int completed = 0;

    for (;;)
    {
        EnsurePendingCs();
        EnterCriticalSection(&PendingCs);

        WSK_MODEL_PENDING** link = &PendingHead;
        WSK_MODEL_PENDING* found = NULL;

        while (*link)
        {
            if ((*link)->Irp->Cancel && !(*link)->Irp->Completed)
            {
                found = *link;
                *link = found->Next;
                break;
            }

            link = &(*link)->Next;
        }

        LeaveCriticalSection(&PendingCs);

        if (!found)
        {
            return completed;
        }

        InterlockedIncrement(&Cancelled);

        WskModelCompleteAtDispatch(found, STATUS_CANCELLED, 0);

        free(found);
        completed++;
    }
}

///////////////////////////////////////////////////////////////////////////
// Provider entry points
///////////////////////////////////////////////////////////////////////////

static NTSTATUS WskModelIssue(
    PIRP Irp,
    PMDL Mdl,
    ULONG Offset,
    SIZE_T Length,
    const WSK_MODEL_BEHAVIOUR* Behaviour,
    BOOLEAN IsReceive)
{
    WSK_MODEL_PENDING* pending = (WSK_MODEL_PENDING*)calloc(1, sizeof(WSK_MODEL_PENDING));

    pending->Irp = Irp;
    pending->Mdl = Mdl;
    pending->Offset = Offset;
    pending->Length = Length;
    pending->Behaviour = *Behaviour;
    pending->IsReceive = IsReceive;

    Irp->Outstanding = TRUE;

    if (WskModelInline == Behaviour->Completion)
    {
        WskModelCompleteAtDispatch(pending, Behaviour->Status, Behaviour->Bytes);
        free(pending);
        return STATUS_SUCCESS;
    }

    WskModelQueuePending(pending);

    return STATUS_PENDING;
}

static NTSTATUS WskModelSend(PWSK_SOCKET Socket, PWSK_BUF Buffer, ULONG Flags, PIRP Irp)
{
    (void)Socket;
    (void)Flags;

    InterlockedIncrement(&Sends);

    LastSendLength = Buffer->Length;
    if (LastSendLength > WSK_MODEL_LAST_SEND_CAPACITY)
    {
        LastSendLength = WSK_MODEL_LAST_SEND_CAPACITY;
    }
    memcpy(LastSendBuffer, ((unsigned char*)Buffer->Mdl->Base) + Buffer->Offset, LastSendLength);

    WSK_MODEL_BEHAVIOUR behaviour = SendBehaviour;

    if (0 == behaviour.Bytes && NT_SUCCESS(behaviour.Status))
    {
        behaviour.Bytes = Buffer->Length;
    }

    return WskModelIssue(Irp, Buffer->Mdl, Buffer->Offset, Buffer->Length, &behaviour, FALSE);
}

static NTSTATUS WskModelReceive(PWSK_SOCKET Socket, PWSK_BUF Buffer, ULONG Flags, PIRP Irp)
{
    (void)Socket;
    (void)Flags;

    InterlockedIncrement(&Receives);

    return WskModelIssue(Irp, Buffer->Mdl, Buffer->Offset, Buffer->Length, &ReceiveBehaviour, TRUE);
}

static NTSTATUS WskModelCloseSocket(PWSK_SOCKET Socket, PIRP Irp)
{
    InterlockedIncrement(&Closes);

    WSK_MODEL_CONNECTION* connection = Socket->Connection;

    if (connection)
    {
        if (connection->Closed)
        {
            KmReportViolation(KmViolationLifetime, "WskCloseSocket on an already-closed socket");
        }

        connection->Closed = TRUE;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    unsigned char saved = KmGetIrql();
    KmSetIrql(DISPATCH_LEVEL);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    KmSetIrql(saved);

    KmObjectDestroyed(KmObjectSocket);
    free(connection);

    return STATUS_SUCCESS;
}

static const WSK_PROVIDER_CONNECTION_DISPATCH ConnectionDispatch =
{
    WskModelCloseSocket,
    WskModelSend,
    WskModelReceive
};

static NTSTATUS WskModelSocketConnect(
    PWSK_CLIENT Client, USHORT SocketType, ULONG Protocol,
    PSOCKADDR LocalAddress, PSOCKADDR RemoteAddress,
    ULONG Flags, PVOID SocketContext, const VOID* Dispatch,
    PVOID OwningProcess, PVOID OwningThread, PVOID SecurityDescriptor, PIRP Irp)
{
    (void)Client; (void)SocketType; (void)Protocol; (void)LocalAddress;
    (void)RemoteAddress; (void)Flags; (void)SocketContext; (void)Dispatch;
    (void)OwningProcess; (void)OwningThread; (void)SecurityDescriptor;

    InterlockedIncrement(&Connects);

    if (!NT_SUCCESS(ConnectBehaviour.Status))
    {
        Irp->IoStatus.Status = ConnectBehaviour.Status;
        Irp->IoStatus.Information = 0;
        Irp->Outstanding = TRUE;

        unsigned char saved = KmGetIrql();
        KmSetIrql(DISPATCH_LEVEL);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        KmSetIrql(saved);

        return STATUS_SUCCESS;
    }

    WSK_MODEL_CONNECTION* connection = (WSK_MODEL_CONNECTION*)calloc(1, sizeof(WSK_MODEL_CONNECTION));

    connection->Socket.Dispatch = &ConnectionDispatch;
    connection->Socket.Connection = connection;

    KmObjectCreated(KmObjectSocket);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = (ULONG_PTR)&connection->Socket;
    Irp->Outstanding = TRUE;

    if (WskModelDeferred == ConnectBehaviour.Completion)
    {
        WSK_MODEL_PENDING* pending = (WSK_MODEL_PENDING*)calloc(1, sizeof(WSK_MODEL_PENDING));
        pending->Irp = Irp;
        pending->Behaviour = ConnectBehaviour;
        pending->Behaviour.Bytes = (SIZE_T)&connection->Socket;
        WskModelQueuePending(pending);
        return STATUS_PENDING;
    }

    unsigned char saved = KmGetIrql();
    KmSetIrql(DISPATCH_LEVEL);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    KmSetIrql(saved);

    return STATUS_SUCCESS;
}

static NTSTATUS WskModelGetAddressInfo(
    PWSK_CLIENT Client, PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName,
    ULONG NameSpace, void* Provider, PADDRINFOEXW Hints, PADDRINFOEXW* Result,
    PVOID OwningProcess, PVOID OwningThread, PIRP Irp)
{
    (void)Client; (void)NodeName; (void)ServiceName; (void)NameSpace;
    (void)Provider; (void)Hints; (void)OwningProcess; (void)OwningThread;

    *Result = NULL;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

static VOID WskModelFreeAddressInfo(PWSK_CLIENT Client, PADDRINFOEXW AddrInfo)
{
    (void)Client;
    (void)AddrInfo;
}

static const WSK_PROVIDER_DISPATCH ProviderDispatch =
{
    WskModelSocketConnect,
    WskModelGetAddressInfo,
    WskModelFreeAddressInfo
};

NTSTATUS WskRegister(PWSK_CLIENT_NPI ClientNpi, PWSK_REGISTRATION Registration)
{
    (void)ClientNpi;
    Registration->Reserved = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS WskCaptureProviderNPI(PWSK_REGISTRATION Registration, ULONG WaitTimeout, PWSK_PROVIDER_NPI ProviderNpi)
{
    (void)Registration;
    (void)WaitTimeout;

    ProviderNpi->Client = &ModelClient;
    ProviderNpi->Dispatch = &ProviderDispatch;

    return STATUS_SUCCESS;
}

VOID WskReleaseProviderNPI(PWSK_REGISTRATION Registration) { (void)Registration; }
VOID WskDeregister(PWSK_REGISTRATION Registration) { (void)Registration; }

///////////////////////////////////////////////////////////////////////////
// Test control
///////////////////////////////////////////////////////////////////////////

VOID WskModelSetSendBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour) { SendBehaviour = *Behaviour; }
VOID WskModelSetReceiveBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour) { ReceiveBehaviour = *Behaviour; }
VOID WskModelSetConnectBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour) { ConnectBehaviour = *Behaviour; }

const unsigned char* WskModelLastSendBytes(SIZE_T* LengthOut)
{
    *LengthOut = LastSendLength;
    return LastSendBuffer;
}

ULONG WskModelConnects(VOID) { return (ULONG)Connects; }
ULONG WskModelSends(VOID) { return (ULONG)Sends; }
ULONG WskModelReceives(VOID) { return (ULONG)Receives; }
ULONG WskModelCloses(VOID) { return (ULONG)Closes; }
ULONG WskModelCancelled(VOID) { return (ULONG)Cancelled; }

VOID WskModelReset(VOID)
{
    EnsurePendingCs();
    EnterCriticalSection(&PendingCs);

    while (PendingHead)
    {
        WSK_MODEL_PENDING* next = PendingHead->Next;
        free(PendingHead);
        PendingHead = next;
    }

    LeaveCriticalSection(&PendingCs);

    Connects = 0;
    Sends = 0;
    Receives = 0;
    Closes = 0;
    Cancelled = 0;
    LastSendLength = 0;

    memset(&SendBehaviour, 0, sizeof(SendBehaviour));
    SendBehaviour.Completion = WskModelInline;
    SendBehaviour.Status = STATUS_SUCCESS;

    memset(&ReceiveBehaviour, 0, sizeof(ReceiveBehaviour));
    ReceiveBehaviour.Completion = WskModelInline;
    ReceiveBehaviour.Status = STATUS_SUCCESS;

    memset(&ConnectBehaviour, 0, sizeof(ConnectBehaviour));
    ConnectBehaviour.Completion = WskModelInline;
    ConnectBehaviour.Status = STATUS_SUCCESS;
}
