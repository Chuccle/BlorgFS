#include "Driver.h"

//
// FSP worker pool: a cancel-safe IRP queue (IO_CSQ) serviced by a fixed set
// of PASSIVE_LEVEL system threads. Dispatches IRP_MJ_CREATE/READ/
// DIRECTORY_CONTROL to their Blorg* handlers, and provides the post/requeue
// entry points used to hand IRPs from DISPATCH_LEVEL completions back to a
// PASSIVE_LEVEL worker.
//

//
// Most handlers complete asynchronously without blocking a worker. The
// exception is a cached-read miss: BlorgVolumeRead calls
// CcCopyReadEx(Wait=TRUE), which blocks the worker for the duration of the
// underlying async HTTP round trip. Concurrent activity on other files
// (e.g. Create and probe/thumbnail reads on a file being opened while
// another streams) competes for this same pool and can delay the next
// read-ahead chunk on an already-open file. Sized for headroom against
// that, not just steady-state single-stream throughput.
//
// FSP_THREAD_COUNT is the ceiling (and the ThreadHandle array size); the
// pool actually started is min(max(4 x active cores, FSP_THREAD_COUNT_MIN),
// FSP_THREAD_COUNT), computed once in CreateWorkQueue. The pool exists to
// absorb RTT blocking, which does not scale with core count -- a blocked
// worker costs no CPU -- so small machines keep a healthy floor for
// concurrent blocked operations; the core scaling only trims thread
// stacks and wake-burst width on machines that cannot run the full pool
// concurrently anyway.
//
#define FSP_THREAD_COUNT 16
#define FSP_THREAD_COUNT_MIN 8

NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject);
NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp);
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp);

//
// Global state for the FSP worker pool: worker thread handles, the pending
// IRP queue, and its synchronization/cancel-safe queue objects.
//
typedef struct _FSP_QUEUE_STATE
{
    HANDLE           ThreadHandle[FSP_THREAD_COUNT]; // Worker thread handles
    IO_CSQ           Csq;                            // Cancel-safe queue for pending IRPs
    KEVENT           TerminationEvent;               // Signaled to tell workers to exit
    KEVENT           WorkEvent;                      // Signaled when an IRP is queued
    LIST_ENTRY       IrpQueue;                       // Pending IRPs awaiting a worker
    KSPIN_LOCK       IrpQueueSpinLock;               // Protects IrpQueue
    LONG             ThreadsActive;                  // Interlocked idempotency flag - FALSE once teardown has begun
    ULONG            ThreadCount;                    // Threads actually started (core-scaled, <= FSP_THREAD_COUNT)
} FSP_QUEUE_STATE;

static FSP_QUEUE_STATE FspQueue;

// IO_CSQ insert callback: appends Irp to the tail of the pending-IRP queue.
VOID FspCsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&FspQueue.IrpQueue, &Irp->Tail.Overlay.ListEntry);
}

// IO_CSQ remove callback: unlinks Irp from the pending-IRP queue.
VOID FspCsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

//
// IO_CSQ peek callback: returns the next IRP after Irp, or the head if Irp
// is NULL; NULL if the queue is exhausted. Used by IoCsqRemoveNextIrp and by
// cancel processing to walk the queue under the CSQ lock.
//
PIRP FspCsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
{
    UNREFERENCED_PARAMETER(Csq);
    UNREFERENCED_PARAMETER(PeekContext);

    PLIST_ENTRY nextEntry;

    if (!Irp)
    {
        nextEntry = FspQueue.IrpQueue.Flink;
    }
    else
    {
        nextEntry = Irp->Tail.Overlay.ListEntry.Flink;
    }

    if (nextEntry != &FspQueue.IrpQueue)
    {
        return CONTAINING_RECORD(nextEntry, IRP, Tail.Overlay.ListEntry);
    }
    else
    {
        return NULL;
    }
}

_IRQL_raises_(DISPATCH_LEVEL)
VOID FspCsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&FspQueue.IrpQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID FspCsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&FspQueue.IrpQueueSpinLock, Irql);
}

//
// IO_CSQ cancel callback: completes an IRP that was cancelled while still
// queued (the CSQ has already removed it by the time this runs).
//
VOID FspCsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    CompleteRequest(Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}

// System threads disable kernel APCs so no need to explicitly disable APCs here.
VOID FspDispatch(_In_ PVOID StartContext)

/*++

Routine Description:

    This is the main FSP thread routine that is executed to receive
    and dispatch IRP requests.

    Each worker drops its own base priority to 7, one below the system
    process base it inherits. Worker CPU time (cache copies, parsing) sits
    on an RTT-/bandwidth-bound pipeline, so on an otherwise idle machine
    the lower priority costs no throughput, while under CPU contention a
    foreground workload (e.g. a game) wins scheduling ties instead of
    losing them to us. ERESOURCE has no priority inheritance, so a
    preempted worker holding an FCB resource can delay an exclusive
    waiter under sustained contention -- bounded by the balance-set
    manager's starvation boost.

Arguments:

    StartContext - Not currently used, required by the KSTART_ROUTINE signature.

Return Value:

    None - This routine never exits

--*/

{
    UNREFERENCED_PARAMETER(StartContext);

    KeSetBasePriorityThread(KeGetCurrentThread(), -1);

    while (TRUE)
    {

        PVOID waitObjectArray[2] = { &FspQueue.WorkEvent, &FspQueue.TerminationEvent };

        if (STATUS_WAIT_1 == KeWaitForMultipleObjects(2,
            waitObjectArray,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,
            NULL))
        {
            break;
        }

        PIRP irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

        NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
        
        while (irp)
        {
            ULONG_PTR flags = C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[0]);

            SetFlag(flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

            irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);

            PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

            BLORGFS_PRINT("FspDispatch: Irp = %p\n", irp);

            if (FlagOn(C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_RECURSIVE_CALL))
            {
                IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_FSP_TOP_LEVEL_IRP));
            }
            else
            {
                IoSetTopLevelIrp(irp);
            }

            switch (irpSp->MajorFunction)
            {
                case IRP_MJ_CREATE:
                {
                    result = BlorgVolumeCreate(irp, irpSp, irpSp->DeviceObject);
                    break;
                }
                case IRP_MJ_READ:
                {
                    result = BlorgVolumeRead(irp, irpSp);
                    break;
                }
                case IRP_MJ_DIRECTORY_CONTROL:
                {
                    result = BlorgVolumeDirectoryControl(irp, irpSp);
                    break;
                }

                default:
                {
                    break;
                }
            }

            if (STATUS_PENDING != result)
            {
                CompleteRequest(irp, result, IO_DISK_INCREMENT);
            }

            IoSetTopLevelIrp(NULL);

            irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);
        }

    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

//
//  Local support routine. Queues Irp to the FSP workers and wakes one.
//  Every major that gets posted carries a file object, so this asserts
//  rather than silently skipping the insert -- a skipped insert would
//  strand an IRP its poster already reported as STATUS_PENDING.
//
static void AddToWorkqueue(
    IN PIRP Irp
)
{
    NT_ASSERT(NULL != IoGetCurrentIrpStackLocation(Irp)->FileObject);

    IoCsqInsertIrp(&FspQueue.Csq, Irp, NULL);
    KeSetEvent(&FspQueue.WorkEvent, EVENT_INCREMENT, FALSE);
}

NTSTATUS PrePostIrp(
    IN PVOID Context,
    IN PIRP Irp
)
{

    UNREFERENCED_PARAMETER(Context);

    if (!Irp)
    {
        return STATUS_SUCCESS;
    }

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction)
    {
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
        {
            if (!FlagOn(IrpSp->MinorFunction, IRP_MN_MDL))
            {
                return LockUserBuffer(Irp,
                    (IrpSp->MajorFunction == IRP_MJ_READ) ?
                    IoWriteAccess : IoReadAccess,
                    (IrpSp->MajorFunction == IRP_MJ_READ) ?
                    IrpSp->Parameters.Read.Length : IrpSp->Parameters.Write.Length);
            }
            break;
        }
        case IRP_MJ_DIRECTORY_CONTROL:
        {
            if (IRP_MN_QUERY_DIRECTORY == IrpSp->MinorFunction)
            {
                return LockUserBuffer(Irp,
                    IoWriteAccess,
                    IrpSp->Parameters.QueryDirectory.Length);
            }
            break;
        }
        case IRP_MJ_QUERY_EA:
        {
            return LockUserBuffer(Irp,
                IoWriteAccess,
                IrpSp->Parameters.QueryEa.Length);
        }
        case IRP_MJ_SET_EA:
        {
            return LockUserBuffer(Irp,
                IoReadAccess,
                IrpSp->Parameters.SetEa.Length);
        }
		default:
		{
			break;
		}
    }

    return STATUS_SUCCESS;
}

//
//  PostIrpRoutine handed to FsRtlCheckOplock / FsRtlOplockFsctrl. The oplock
//  package calls this, then parks the IRP in its own queue -- making it
//  eligible for asynchronous completion by a break acknowledgement on another
//  CPU -- before it returns STATUS_PENDING. Nothing else marks the IRP pending
//  on that path (it never reaches our CSQ until OplockComplete re-queues it),
//  and marking after FsRtlCheckOplock returns would race that completion, so we
//  must mark here, before the package parks it. The FsdPostRequest path uses
//  plain PrePostIrp instead and lets IoCsqInsertIrp do the marking.
//
//  Unlike FsdPostRequest, a buffer-lock failure cannot be turned into a
//  fail-fast here: by the time the package invokes this routine it has
//  already committed to parking the IRP and returning STATUS_PENDING, so
//  there is no return path to abort on. The lock status is therefore
//  discarded; if the lock failed, MdlAddress stays NULL and the worker-side
//  handler falls back to its SEH-guarded user-buffer path, which faults
//  safely rather than corrupting memory when run in the wrong context.
//

void OplockPrePostIrp(IN PVOID Context, IN PIRP Irp)
{
    PrePostIrp(Context, Irp);

    if (Irp)
    {
        IoMarkIrpPending(Irp);
    }
}

NTSTATUS FsdPostRequest(
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
)

/*++

Routine Description:

    This routine enqueues the request packet specified by IrpContext to the
    FSP threads.  This is a FSD routine.

    The ThreadsActive gate is an advisory read (ReadAcquire, no interlocked
    op): it only rejects posts that arrive after teardown has begun, and the
    driver lifecycle guarantees no post can race StopWorkQueueThreads, so
    the gate needs no atomicity with the queue insert that follows it.

Arguments:

    IrpContext - Pointer to the IrpContext to be queued to the Fsp

    Irp - I/O Request Packet, or NULL if it has already been completed.

    IrpSp - Pointer to the current I/O stack location for the Irp

Return Value:

    STATUS_PENDING


--*/

{
    NT_ASSERT(ARGUMENT_PRESENT(Irp));
    UNREFERENCED_PARAMETER(IrpSp);

    if (!ReadAcquire(&FspQueue.ThreadsActive))
    {
        return STATUS_DEVICE_REMOVED;
    }

    NTSTATUS prePostStatus = PrePostIrp(NULL, Irp);

    if (!NT_SUCCESS(prePostStatus))
    {
        return prePostStatus;
    }

    AddToWorkqueue(Irp);

    return STATUS_PENDING;
}

NTSTATUS FsdRequeueRequest(
    IN PIRP Irp
)

/*++

Routine Description:

    Re-posts an already-pending IRP to the FSP threads for a second worker
    pass. Used by the async-HTTP completion routines (which run at
    DISPATCH_LEVEL) to hand an IRP back to PASSIVE_LEVEL once the network
    result is ready -- the buffer was already locked by the original
    FsdPostRequest, so PrePostIrp is intentionally not repeated here.

    The ThreadsActive gate matches FsdPostRequest's: an advisory ReadAcquire,
    see the note there.

Arguments:

    Irp - the pending I/O Request Packet to re-queue.

Return Value:

    STATUS_PENDING on success; STATUS_DEVICE_REMOVED if the FSP threads are
    being torn down (the caller must then complete the IRP itself).

--*/

{
    NT_ASSERT(ARGUMENT_PRESENT(Irp));

    if (!ReadAcquire(&FspQueue.ThreadsActive))
    {
        return STATUS_DEVICE_REMOVED;
    }

    AddToWorkqueue(Irp);

    return STATUS_PENDING;
}

//
// Oplock-break completion callback: on a granted/acknowledged oplock,
// re-queues the parked IRP to the FSP workers to resume normal dispatch;
// otherwise completes it with the failure status. Mirrors OplockPrePostIrp's
// pending/parking side of the FsRtlCheckOplock contract.
//
void OplockComplete(PVOID Context, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Context);

    if (STATUS_SUCCESS == Irp->IoStatus.Status)
    {
        AddToWorkqueue(Irp);
    }
    else
    {
        CompleteRequest(Irp, Irp->IoStatus.Status, IO_DISK_INCREMENT);
    }
}

//
// Signals termination and reaps the first ThreadCount worker threads:
// waits for each to exit in turn, then closes its handle. One blocking
// wait per thread rather than a single KeWaitForMultipleObjects -- all
// threads must exit either way, so the order is immaterial, and the
// sequential form needs no wait-object/wait-block arrays, leaving this
// teardown with no allocation-failure path that could strand running
// threads. The count is a parameter because CreateWorkQueue's
// partial-failure unwind reaps only the threads it actually started.
//
// NOTE: This is not thread-safe against concurrent calls of 
// CreateWorkQueue and DestroyWorkQueue.
// 
// Designed to follow driver lifecycle so is naturally serialized 
// by the driver load/unload path, but if that changes we internally
// synchronise.
//
static void StopWorkQueueThreads(ULONG ThreadCount)
{
	if (!InterlockedCompareExchange(&FspQueue.ThreadsActive, FALSE, TRUE))
	{
		return;
	}

    KeSetEvent(&FspQueue.TerminationEvent, EVENT_INCREMENT, FALSE);

    for (ULONG i = 0; i < ThreadCount; ++i)
    {
        PVOID thread;

        if (NT_SUCCESS(ObReferenceObjectByHandle(FspQueue.ThreadHandle[i], SYNCHRONIZE, *PsThreadType, KernelMode, &thread, NULL)))
        {
            KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(thread);
        }

        ZwClose(FspQueue.ThreadHandle[i]);
        FspQueue.ThreadHandle[i] = NULL;
    }
}

//
// Initializes the FSP queue state (CSQ, events, spin lock) and spins up
// the core-scaled worker count (see the FSP_THREAD_COUNT note above).
// Called at volume creation. A failure part-way through thread creation
// unwinds the threads already started via StopWorkQueueThreads and fails
// the whole call -- the pool either comes up complete or not at all.
// Without that unwind, a partial pool reported as success would leave
// DestroyWorkQueue trying to reap a NULL handle (early-out, threads
// never terminated) and the survivors running FspDispatch out of an
// unloaded driver image.
//
// NOTE: This is not thread-safe against concurrent calls of 
// CreateWorkQueue and DestroyWorkQueue.
// 
// Designed to follow driver lifecycle so is naturally serialized 
// by the driver load/unload path, but if that changes we internally
// synchronise.
//
NTSTATUS CreateWorkQueue(void)
{
    if (InterlockedCompareExchange(&FspQueue.ThreadsActive, TRUE, FALSE))
    {
		return STATUS_SUCCESS;
    }   

    KeInitializeSpinLock(&FspQueue.IrpQueueSpinLock);
    InitializeListHead(&FspQueue.IrpQueue);

    KeInitializeEvent(&FspQueue.WorkEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&FspQueue.TerminationEvent, NotificationEvent, FALSE);

    NTSTATUS result = IoCsqInitialize(&FspQueue.Csq,
        FspCsqInsertIrp,
        FspCsqRemoveIrp,
        FspCsqPeekNextIrp,
        FspCsqAcquireLock,
        FspCsqReleaseLock,
        FspCsqCompleteCanceledIrp);

    if (!NT_SUCCESS(result))
    {
		InterlockedExchange(&FspQueue.ThreadsActive, FALSE);
        return result;
    }

    ULONG threadCount = 4 * KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    if (threadCount < FSP_THREAD_COUNT_MIN)
    {
        threadCount = FSP_THREAD_COUNT_MIN;
    }

    if (threadCount > FSP_THREAD_COUNT)
    {
        threadCount = FSP_THREAD_COUNT;
    }

    FspQueue.ThreadCount = threadCount;

    for (ULONG i = 0; i < threadCount; ++i)
    {
        result = PsCreateSystemThread(&FspQueue.ThreadHandle[i], DELETE | SYNCHRONIZE, NULL, NULL, NULL, FspDispatch, NULL);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("CreateWorkQueue: PsCreateSystemThread failed for worker %lu: %8lx\n", i, result);
            StopWorkQueueThreads(i);
            return result;
        }
    }

    return STATUS_SUCCESS;
}

//
// Stops and reaps every worker thread, then drains and cancels any IRPs
// still left in the queue. CreateWorkQueue guarantees all-or-nothing
// thread creation, so all FspQueue.ThreadCount handles are valid here.
//
void DestroyWorkQueue(void)
{
    StopWorkQueueThreads(FspQueue.ThreadCount);

    PIRP irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

    while (irp)
    {
        CompleteRequest(irp, STATUS_CANCELLED, IO_NO_INCREMENT);
        irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);
    }
}
