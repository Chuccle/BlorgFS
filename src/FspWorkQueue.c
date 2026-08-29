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
// FSP_THREAD_COUNT), computed once in BlorgCreateWorkQueue. The pool exists to
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
VOID BlorgFspCsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&FspQueue.IrpQueue, &Irp->Tail.Overlay.ListEntry);
}

// IO_CSQ remove callback: unlinks Irp from the pending-IRP queue.
VOID BlorgFspCsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

//
// IO_CSQ peek callback: returns the next IRP after Irp, or the head if Irp
// is NULL; NULL if the queue is exhausted. Used by IoCsqRemoveNextIrp and by
// cancel processing to walk the queue under the CSQ lock.
//
PIRP BlorgFspCsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
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
VOID BlorgFspCsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&FspQueue.IrpQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID BlorgFspCsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&FspQueue.IrpQueueSpinLock, Irql);
}

//
// Releases the per-pass payload a queued IRP may be carrying, for the two
// places that complete a posted IRP WITHOUT running its handler:
// cancellation, and the teardown drain. Normally the handler consumes it
// at the top of its next pass (BlorgVolumeCreate), so it is only ever
// orphaned when that pass never happens.
//
// IRP_CONTEXT_FLAG_NET_DONE is the marker that an async completion stashed
// something; only the create path attaches one (DirectoryControl sets the
// same flag with nothing in DriverContext[1], which the NULL test covers).
// The flag is cleared alongside the free so this stays single-shot, exactly
// as consumption in BlorgVolumeCreate does.
//
static VOID FspDiscardPendingIrpContext(PIRP Irp)
{
    if (!FlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_NET_DONE))
    {
        return;
    }

    BlorgClearIrpContextFlag(Irp, IRP_CONTEXT_FLAG_NET_DONE);

    PVOID stash = Irp->Tail.Overlay.DriverContext[1];

    if (stash)
    {
        ExFreePool(stash);
        Irp->Tail.Overlay.DriverContext[1] = NULL;
    }
}

//
// IO_CSQ cancel callback: completes an IRP that was cancelled while still
// queued (the CSQ has already removed it by the time this runs).
//
VOID BlorgFspCsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    FspDiscardPendingIrpContext(Irp);

    BlorgCompleteRequest(Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}

// PsCreateSystemThread creates threads inside a critical region with kernel
// APCs disabled (see wdm.h PsCreateSystemThread Remarks); the region
// persists for the thread's lifetime.  No explicit KeEnterCriticalRegion
// needed here.
VOID BlorgFspDispatch(_In_ PVOID StartContext)

/*++

Routine Description:

    This is the main FSP thread routine that is executed to receive
    and dispatch IRP requests.

    The dispatch result is scoped to one IRP, not to one wake: a major
    function with no case here must fall through to its own
    STATUS_INVALID_DEVICE_REQUEST and be completed. Hoisting the
    declaration out of the drain loop instead let an unhandled major
    inherit the previous IRP's status, and a previous STATUS_PENDING then
    skipped completion entirely -- stranding an IRP already removed from
    the CSQ, where not even BlorgDestroyWorkQueue's drain can reach it. Only
    CREATE/READ/DIRECTORY_CONTROL are posted today, but BlorgPrePostIrp already
    prepares WRITE and the EA majors.

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

        while (irp)
        {
            NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

            ULONG_PTR flags = C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[0]);

            SetFlag(flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

            irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);

            PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

            BLORGFS_PRINT("BlorgFspDispatch: Irp = %p\n", irp);

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
                BlorgCompleteRequest(irp, result, IO_DISK_INCREMENT);
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
static VOID AddToWorkqueue(
    PIRP Irp
)
{
    NT_ASSERT(NULL != IoGetCurrentIrpStackLocation(Irp)->FileObject);

    IoCsqInsertIrp(&FspQueue.Csq, Irp, NULL);
    KeSetEvent(&FspQueue.WorkEvent, EVENT_INCREMENT, FALSE);
}

NTSTATUS BlorgPrePostIrp(
    PVOID Context,
    PIRP Irp
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
                return BlorgLockUserBuffer(Irp,
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
                return BlorgLockUserBuffer(Irp,
                    IoWriteAccess,
                    IrpSp->Parameters.QueryDirectory.Length);
            }
            break;
        }
        case IRP_MJ_QUERY_EA:
        {
            return BlorgLockUserBuffer(Irp,
                IoWriteAccess,
                IrpSp->Parameters.QueryEa.Length);
        }
        case IRP_MJ_SET_EA:
        {
            return BlorgLockUserBuffer(Irp,
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
//  on that path (it never reaches our CSQ until BlorgOplockComplete re-queues it),
//  and marking after FsRtlCheckOplock returns would race that completion, so we
//  must mark here, before the package parks it. The BlorgFsdPostRequest path uses
//  plain BlorgPrePostIrp instead and lets IoCsqInsertIrp do the marking.
//
//  Unlike BlorgFsdPostRequest, a buffer-lock failure cannot be turned into a
//  fail-fast here: by the time the package invokes this routine it has
//  already committed to parking the IRP and returning STATUS_PENDING, so
//  there is no return path to abort on. The lock status is therefore
//  discarded; if the lock failed, MdlAddress stays NULL and the worker-side
//  handler falls back to its SEH-guarded user-buffer path, which faults
//  safely rather than corrupting memory when run in the wrong context.
//

VOID BlorgOplockPrePostIrp(PVOID Context, PIRP Irp)
{
    BlorgPrePostIrp(Context, Irp);

    if (Irp)
    {
        IoMarkIrpPending(Irp);
    }
}

NTSTATUS BlorgFsdPostRequest(
    PIRP Irp,
    PIO_STACK_LOCATION IrpSp
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

    NTSTATUS prePostStatus = BlorgPrePostIrp(NULL, Irp);

    if (!NT_SUCCESS(prePostStatus))
    {
        return prePostStatus;
    }

    AddToWorkqueue(Irp);

    return STATUS_PENDING;
}

NTSTATUS BlorgFsdRequeueRequest(
    PIRP Irp
)

/*++

Routine Description:

    Re-posts an already-pending IRP to the FSP threads for a second worker
    pass. Used by the async-HTTP completion routines (which run at
    DISPATCH_LEVEL) to hand an IRP back to PASSIVE_LEVEL once the network
    result is ready -- the buffer was already locked by the original
    BlorgFsdPostRequest, so BlorgPrePostIrp is intentionally not repeated here.

    The ThreadsActive gate matches BlorgFsdPostRequest's: an advisory ReadAcquire,
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
// otherwise completes it with the failure status. Mirrors BlorgOplockPrePostIrp's
// pending/parking side of the FsRtlCheckOplock contract.
//
// The ThreadsActive gate is the same one BlorgFsdPostRequest and
// BlorgFsdRequeueRequest consult, and here it is not the advisory
// race-narrowing it is there -- it is load-bearing. Volume teardown calls
// BlorgDestroyWorkQueue, which stops the workers and drains the queue, and
// only then frees the node tree; freeing a node runs
// FsRtlUninitializeOplock, which completes every IRP the oplock package
// still holds through this routine. Queueing those would deposit them in a
// queue with no workers left to dispatch them and no drain left to cancel
// them -- an IRP the caller waits on forever. The other two gate readers can
// return STATUS_DEVICE_REMOVED and let their caller complete the IRP; a
// callback has no such return path, so it completes the IRP itself, with the
// status its siblings hand back for exactly this condition.
//
VOID BlorgOplockComplete(PVOID Context, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Context);

    if (STATUS_SUCCESS == Irp->IoStatus.Status && ReadAcquire(&FspQueue.ThreadsActive))
    {
        AddToWorkqueue(Irp);
        return;
    }

    NTSTATUS result = NT_SUCCESS(Irp->IoStatus.Status) ? STATUS_DEVICE_REMOVED : Irp->IoStatus.Status;

    BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
}

//
// Signals termination and reaps the first ThreadCount worker threads:
// waits for each to exit in turn, then closes its handle. One blocking
// wait per thread rather than a single KeWaitForMultipleObjects -- all
// threads must exit either way, so the order is immaterial, and the
// sequential form needs no wait-object/wait-block arrays, leaving this
// teardown with no allocation-failure path that could strand running
// threads. The count is a parameter because BlorgCreateWorkQueue's
// partial-failure unwind reaps only the threads it actually started.
//
// NOTE: This is not thread-safe against concurrent calls of 
// BlorgCreateWorkQueue and BlorgDestroyWorkQueue.
// 
// Designed to follow driver lifecycle so is naturally serialized 
// by the driver load/unload path, but if that changes we internally
// synchronise.
//
static VOID StopWorkQueueThreads(ULONG ThreadCount)
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
// BlorgDestroyWorkQueue trying to reap a NULL handle (early-out, threads
// never terminated) and the survivors running BlorgFspDispatch out of an
// unloaded driver image.
//
// NOTE: This is not thread-safe against concurrent calls of 
// BlorgCreateWorkQueue and BlorgDestroyWorkQueue.
// 
// Designed to follow driver lifecycle so is naturally serialized 
// by the driver load/unload path, but if that changes we internally
// synchronise.
//
NTSTATUS BlorgCreateWorkQueue(VOID)
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
        BlorgFspCsqInsertIrp,
        BlorgFspCsqRemoveIrp,
        BlorgFspCsqPeekNextIrp,
        BlorgFspCsqAcquireLock,
        BlorgFspCsqReleaseLock,
        BlorgFspCsqCompleteCanceledIrp);

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
        result = PsCreateSystemThread(&FspQueue.ThreadHandle[i], DELETE | SYNCHRONIZE, NULL, NULL, NULL, BlorgFspDispatch, NULL);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("BlorgCreateWorkQueue: PsCreateSystemThread failed for worker %lu: %8lx\n", i, result);
            StopWorkQueueThreads(i);
            return result;
        }
    }

    return STATUS_SUCCESS;
}

//
// Stops and reaps every worker thread, then drains and cancels any IRPs
// still left in the queue. BlorgCreateWorkQueue guarantees all-or-nothing
// thread creation, so all FspQueue.ThreadCount handles are valid here.
//
// Each drained IRP goes through FspDiscardPendingIrpContext first: an IRP
// re-queued by an async completion carries that completion's stashed
// result, and cancelling it here is the one path where no handler pass
// will ever consume it.
//
VOID BlorgDestroyWorkQueue(VOID)
{
    StopWorkQueueThreads(FspQueue.ThreadCount);

    PIRP irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

    while (irp)
    {
        FspDiscardPendingIrpContext(irp);

        BlorgCompleteRequest(irp, STATUS_CANCELLED, IO_NO_INCREMENT);
        irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);
    }
}
