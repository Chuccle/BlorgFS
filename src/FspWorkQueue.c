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
// Measured at last. A metadata storm from a cold path cache posts 29
// requests over eight passes and 117 over thirty -- 2250 creates -- against
// a pool of eight workers, and the queue is empty again by the end of each.
// A sequential read posts nothing at all.
//
// The first measurement of this said 11 and 30, and was taken on a warm
// path cache: 87% to 100% hits, so almost nothing reached the network or
// this queue. Cold, the hit rate is 3% to 12% and the posts quadruple. A
// metadata number measured after anything else has run says little.
//
// No high-water mark is kept. One was written here and removed: maintaining
// it needs an interlocked global, which the statistics block refuses, and
// FspPosts against FspDispatches bounds the work without writing anything
// shared on the path. What that buys is a bound rather than a peak -- 117
// posts spread across a run cannot have queued eight workers deep for long,
// but the exact worst instant is not recorded, deliberately.
//
// Left as it is rather than trimmed. Being oversized costs thread stacks
// and nothing else, and the failure direction of an undersized pool is a
// stall on the create path that no read benchmark would show.
//
#define FSP_THREAD_COUNT 16
#define FSP_THREAD_COUNT_MIN 8

NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject);
NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp);
NTSTATUS BlorgFspRead(PIRP Irp, PIO_STACK_LOCATION IrpSp);

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
static VOID FspCsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&FspQueue.IrpQueue, &Irp->Tail.Overlay.ListEntry);
}

// IO_CSQ remove callback: unlinks Irp from the pending-IRP queue.
static VOID FspCsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

//
// IO_CSQ peek callback: returns the next IRP after Irp, or the head if Irp
// is NULL; NULL if the queue is exhausted. Used by IoCsqRemoveNextIrp and by
// cancel processing to walk the queue under the CSQ lock.
//
static PIRP FspCsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
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
static VOID FspCsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&FspQueue.IrpQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
static VOID FspCsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
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
// Completes an IRP that was posted, counting it first if it is a create:
// a create that pends is finished here or by CreateComplete, never by
// BlorgCreate, which counts only those finished in the FSD pass
// (BlorgCountCreate).
//
static VOID FspCompleteRequest(PIRP Irp, NTSTATUS Status, CCHAR PriorityBoost)
{
    if (IRP_MJ_CREATE == IoGetCurrentIrpStackLocation(Irp)->MajorFunction)
    {
        BlorgCountCreate(Status);
    }

    BlorgCompleteRequest(Irp, Status, PriorityBoost);
}

//
// IO_CSQ cancel callback: completes an IRP that was cancelled while still
// queued (the CSQ has already removed it by the time this runs).
//
static VOID FspCsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    FspDiscardPendingIrpContext(Irp);

    FspCompleteRequest(Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}

//
// The FSP worker thread: waits for posted IRPs and dispatches each one
// until the queue is torn down. StartContext is unused.
//
// The dispatch result is scoped to one IRP, not to one wake: a major
// function with no case here must fall through to its own
// STATUS_INVALID_DEVICE_REQUEST and be completed. Hoisting the declaration
// out of the drain loop instead let an unhandled major inherit the
// previous IRP's status, and a previous STATUS_PENDING then skipped
// completion entirely -- stranding an IRP already removed from the CSQ,
// where not even BlorgDestroyWorkQueue's drain can reach it. Only
// CREATE/READ/DIRECTORY_CONTROL are posted today, but BlorgPrePostIrp
// already prepares WRITE and the EA majors.
//
// Each worker drops its own base priority to 7, one below the system
// process base it inherits. Worker CPU time (cache copies, parsing) sits
// on an RTT-/bandwidth-bound pipeline, so on an otherwise idle machine the
// lower priority costs no throughput, while under CPU contention a
// foreground workload (e.g. a game) wins scheduling ties instead of losing
// them to us. ERESOURCE has no priority inheritance, so a preempted worker
// holding an FCB resource can delay an exclusive waiter under sustained
// contention -- bounded by the balance-set manager's starvation boost.
//
// PsCreateSystemThread creates threads inside a critical region with
// kernel APCs disabled (see wdm.h PsCreateSystemThread Remarks), and the
// region persists for the thread's lifetime, so no KeEnterCriticalRegion
// is needed here.
//
VOID BlorgFspDispatch(_In_ PVOID StartContext)
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

        if (irp)
        {
            BLORGFS_STAT_INC(FspDispatches);
        }

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
                    irp->Tail.Overlay.DriverContext[3] = irp->Tail.Overlay.DriverContext[1];
                    irp->Tail.Overlay.DriverContext[1] = NULL;

                    result = BlorgFspRead(irp, irpSp);
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
                FspCompleteRequest(irp, result, IO_DISK_INCREMENT);
            }

            IoSetTopLevelIrp(NULL);

            irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

            if (irp)
            {
                BLORGFS_STAT_INC(FspDispatches);
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

//
// The queue owns DriverContext[3] while an IRP is in it: the CSQ keeps its
// own pointer there and clears it on removal. A read carries its arrival
// stamp there (BlorgRead), so across the queue the stamp rides in
// DriverContext[1], which a read does not use until it is issued, and the
// worker puts it back.
//
static VOID FspAddToWorkQueue(
    PIRP Irp
)
{
    NT_ASSERT(NULL != IoGetCurrentIrpStackLocation(Irp)->FileObject);

    if (IRP_MJ_READ == IoGetCurrentIrpStackLocation(Irp)->MajorFunction)
    {
        Irp->Tail.Overlay.DriverContext[1] = Irp->Tail.Overlay.DriverContext[3];
    }

    IoCsqInsertIrp(&FspQueue.Csq, Irp, NULL);
    KeSetEvent(&FspQueue.WorkEvent, EVENT_INCREMENT, FALSE);

    BLORGFS_STAT_INC(FspPosts);
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

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (irpSp->MajorFunction)
    {
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
        {
            if (!FlagOn(irpSp->MinorFunction, IRP_MN_MDL))
            {
                return BlorgLockUserBuffer(Irp,
                    (IRP_MJ_READ == irpSp->MajorFunction) ?
                    IoWriteAccess : IoReadAccess,
                    (IRP_MJ_READ == irpSp->MajorFunction) ?
                    irpSp->Parameters.Read.Length : irpSp->Parameters.Write.Length);
            }
            break;
        }
        case IRP_MJ_DIRECTORY_CONTROL:
        {
            if (IRP_MN_QUERY_DIRECTORY == irpSp->MinorFunction)
            {
                return BlorgLockUserBuffer(Irp,
                    IoWriteAccess,
                    irpSp->Parameters.QueryDirectory.Length);
            }
            break;
        }
        case IRP_MJ_QUERY_EA:
        {
            return BlorgLockUserBuffer(Irp,
                IoWriteAccess,
                irpSp->Parameters.QueryEa.Length);
        }
        case IRP_MJ_SET_EA:
        {
            return BlorgLockUserBuffer(Irp,
                IoReadAccess,
                irpSp->Parameters.SetEa.Length);
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

//
// Queues Irp to the FSP workers, locking its user buffer first
// (BlorgPrePostIrp), and returns STATUS_PENDING. A buffer that cannot be
// locked fails the request with the lock's status instead, and a post that
// arrives once teardown has begun fails with STATUS_DEVICE_REMOVED; the
// caller completes the IRP on either.
//
// The ThreadsActive gate is an advisory read (ReadAcquire, no interlocked
// op): it only rejects posts that arrive after teardown has begun, and the
// driver lifecycle guarantees no post can race FspStopWorkQueueThreads, so
// the gate needs no atomicity with the queue insert that follows it.
//
NTSTATUS BlorgFsdPostRequest(
    PIRP Irp,
    PIO_STACK_LOCATION IrpSp
)
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

    FspAddToWorkQueue(Irp);

    return STATUS_PENDING;
}

//
// Re-posts an already-pending IRP to the FSP workers for a second pass.
// Used by the async-HTTP completion routines (which run at DISPATCH_LEVEL)
// to hand an IRP back to PASSIVE_LEVEL once the network result is ready.
// The buffer was already locked by the original BlorgFsdPostRequest, so
// BlorgPrePostIrp is intentionally not repeated here.
//
// Returns STATUS_PENDING, or STATUS_DEVICE_REMOVED if the workers are
// being torn down, in which case the caller must complete the IRP itself.
// The ThreadsActive gate is BlorgFsdPostRequest's advisory ReadAcquire.
//
NTSTATUS BlorgFsdRequeueRequest(
    PIRP Irp
)
{
    NT_ASSERT(ARGUMENT_PRESENT(Irp));

    if (!ReadAcquire(&FspQueue.ThreadsActive))
    {
        return STATUS_DEVICE_REMOVED;
    }

    FspAddToWorkQueue(Irp);

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
        FspAddToWorkQueue(Irp);
        return;
    }

    NTSTATUS result = NT_SUCCESS(Irp->IoStatus.Status) ? STATUS_DEVICE_REMOVED : Irp->IoStatus.Status;

    FspCompleteRequest(Irp, result, IO_DISK_INCREMENT);
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
static VOID FspStopWorkQueueThreads(ULONG ThreadCount)
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
// unwinds the threads already started via FspStopWorkQueueThreads and fails
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

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    for (ULONG i = 0; i < threadCount; ++i)
    {
        result = PsCreateSystemThread(&FspQueue.ThreadHandle[i], DELETE | SYNCHRONIZE, &attributes, NULL, NULL, BlorgFspDispatch, NULL);

        if (!NT_SUCCESS(result))
        {
            BLORGFS_PRINT("BlorgCreateWorkQueue: PsCreateSystemThread failed for worker %lu: %8lx\n", i, result);
            FspStopWorkQueueThreads(i);
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
    FspStopWorkQueueThreads(FspQueue.ThreadCount);

    PIRP irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

    if (irp)
    {
        BLORGFS_STAT_INC(FspDispatches);
    }

    while (irp)
    {
        FspDiscardPendingIrpContext(irp);

        FspCompleteRequest(irp, STATUS_CANCELLED, IO_NO_INCREMENT);
        irp = IoCsqRemoveNextIrp(&FspQueue.Csq, NULL);

        if (irp)
        {
            BLORGFS_STAT_INC(FspDispatches);
        }
    }
}
