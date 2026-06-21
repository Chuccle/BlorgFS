#include "Driver.h"

//
//  This file implements the volume read dispatch path: cached reads
//  (via Cc) and non-cached/paging reads (direct async HTTP fetch or
//  served from the sequential prefetcher), plus their completions and
//  the end-of-file trim shared by both paths.
//

//
//  READ_AHEAD_GRANULARITY (Driver.h) is Cc's read-ahead granularity for
//  cached reads. Cc's default heuristics are tuned for local disk seek
//  costs, not for a backend where every miss is an HTTP round trip; left
//  unset, Cc under-fetches relative to what one Range GET can profitably
//  return. 256 KB matches HTTP_FILE_INITIAL_RECV_CAPACITY (Client.c) so a
//  read-ahead-sized fetch doesn't immediately trigger a buffer regrow on
//  the first response. Larger granularities make the serialized miss
//  bigger and hurt streaming latency, so this is not increased.
//  Also the basis for PREFETCH_CHUNK (Prefetch.h) -- see there for why
//  it's derived rather than a second hardcoded constant.
//

#ifdef DBG
//
//  Read-pattern characterization: tracks read size/sequentiality to
//  observe actual workload behavior. Read-only with respect to
//  prefetcher state (Sequential is derived the same way
//  BlorgPrefetchServeRead derives it internally -- Offset continues one
//  of the FCB's stream trackers, sampled here before that call updates
//  them -- but this file never writes to the ring). Printed as a rolling
//  window (resets every STATS_WINDOW reads) rather than a session-long
//  average so the numbers reflect current behavior.
//
//  Reads/Sequential/SumLength/ActiveFetches are kept coherent via
//  Interlocked ops (see below); MinLength/MaxLength/PeakActiveFetches
//  are plain racy updates -- acceptable for a characterization trace
//  (worst case, a printed window under-reports a peak by missing one
//  racing update), not a correctness path. None of these are volatile:
//  Interlocked* is a full barrier regardless of the variable's
//  qualifiers.
//
#define STATS_WINDOW 256

typedef struct _READ_PATTERN_STATS
{
    LONG Reads;
    LONG Sequential;
    LONG64 SumLength;
    LONG MinLength;
    LONG MaxLength;
    LONG ActiveFetches;
    LONG PeakActiveFetches;
} READ_PATTERN_STATS;

static READ_PATTERN_STATS Stats = { .MinLength = MAXLONG };

//
// Whether a read at Offset continues any tracked stream -- the same
// contiguity test BlorgPrefetchServeRead applies internally, evaluated
// against the tracker array before the serve call updates it. Unrolled
// by the compiler over one cache line; the OR-accumulate keeps it
// branch-free.
//
static BOOLEAN StatsReadIsSequential(const FCB* Fcb, ULONG64 Offset)
{
    BOOLEAN sequential = FALSE;

    for (ULONG i = 0; i < READ_STREAM_TRACKER_COUNT; ++i)
    {
        sequential |= (Offset == Fcb->Streams[i].End);
    }

    return sequential;
}

//
// Folds one paging read into the rolling read-pattern window, printing and
// resetting the window every STATS_WINDOW reads. Debug-only instrumentation:
// most fields use Interlocked ops for coherency, but Min/Max/PeakActiveFetches
// are deliberately racy since this is a characterization trace, not a
// correctness path. On a window reset, PeakActiveFetches carries the current
// in-flight level forward as next window's starting peak rather than
// dropping it to 0, since otherwise a sustained-but-unchanging concurrency
// level would never show.
//
static VOID StatsRecordRead(BOOLEAN Sequential, ULONG Length)
{
    InterlockedExchangeAdd64(&Stats.SumLength, Length);

    if (Sequential)
    {
        InterlockedIncrement(&Stats.Sequential);
    }

    if (C_CAST(LONG, Length) < Stats.MinLength)
    {
        Stats.MinLength = C_CAST(LONG, Length);
    }

    if (C_CAST(LONG, Length) > Stats.MaxLength)
    {
        Stats.MaxLength = C_CAST(LONG, Length);
    }

    if (STATS_WINDOW == InterlockedIncrement(&Stats.Reads))
    {
        BLORGFS_PRINT("read-pattern window: reads=%lu seq=%lu%% avgLen=%llx minLen=%lx maxLen=%lx peakFetches=%lu\n",
            C_CAST(ULONG, STATS_WINDOW),
            C_CAST(ULONG, (100 * C_CAST(ULONG64, Stats.Sequential)) / STATS_WINDOW),
            C_CAST(ULONG64, Stats.SumLength) / STATS_WINDOW,
            C_CAST(ULONG, Stats.MinLength),
            C_CAST(ULONG, Stats.MaxLength),
            C_CAST(ULONG, Stats.PeakActiveFetches));

        Stats.Reads = 0;
        Stats.Sequential = 0;
        Stats.SumLength = 0;
        Stats.MinLength = MAXLONG;
        Stats.MaxLength = 0;
        Stats.PeakActiveFetches = Stats.ActiveFetches;
    }
}
#endif

//
//  Completion for an async non-cached read. Invoked from the WSK
//  completion path at <= DISPATCH_LEVEL, so everything it touches must be
//  legal there: the source body lives in the NonPagedPoolNx HTTP receive
//  buffer, and the destination is the user buffer already locked into
//  Irp->MdlAddress by PrePostIrp when the IRP was posted to the FSP queue.
//  CallerContext is the PIRP.
//
//  This is a zero-copy read (BlorgHttpGetFileMdl): the body was received
//  directly into Irp->MdlAddress by the client, so there is nothing to
//  map, copy, or free here -- FileBuffer carries only the byte count
//  (the client validated it against the requested range length, so it
//  never exceeds the locked user buffer).
//
//  In DBG builds this pairs with the ActiveFetches increment at the
//  direct-fetch issue site in BlorgVolumeRead -- every BlorgHttpGetFileMdl
//  call from there has exactly one completion here. The per-chunk fetch
//  latency trace reads DriverContext[2], the issue-time QPC stamp set in
//  BlorgVolumeRead under the same log-level gate (and only for READ IRPs;
//  [1] belongs to the CREATE path's stash), and formats it as an integer
//  only -- this runs at <= DISPATCH on the WSK completion chain, where
//  %wZ/%Z would touch paged code and bugcheck.
//
//  For non-paging reads, this mirrors the post-read bookkeeping the
//  synchronous path used to do: advance the file position for
//  synchronous file objects and note that a fast-IO read happened.
//
static VOID BlorgReadComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PIRP irp = CallerContext;

#ifdef DBG
    if (global.LogLevel >= 1)
    {
        InterlockedDecrement(&Stats.ActiveFetches);
    }
#endif

    if (!NT_SUCCESS(Status))
    {
        BLORGFS_PRINT("BlorgReadComplete: http read failed: %8lx\n", Status);
        CompleteRequest(irp, Status, IO_DISK_INCREMENT);
        return;
    }

    irp->IoStatus.Information = FileBuffer->BodyBufferSize;

#ifdef DBG
    if (global.LogLevel >= 1 && irp->Tail.Overlay.DriverContext[2])
    {
        LARGE_INTEGER frequency;
        LONGLONG endStamp = KeQueryPerformanceCounter(&frequency).QuadPart;
        LONGLONG startStamp = C_CAST(LONGLONG, C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[2]));

        BLORGFS_PRINT("chunk off=%llx len=%llx lat=%llu us\n",
            IoGetCurrentIrpStackLocation(irp)->Parameters.Read.ByteOffset.QuadPart,
            C_CAST(ULONG64, FileBuffer->BodyBufferSize),
            C_CAST(ULONG64, ((endStamp - startStamp) * 1000000) / frequency.QuadPart));
    }
#endif

    if (!BooleanFlagOn(irp->Flags, IRP_PAGING_IO))
    {
        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

        if (BooleanFlagOn(irpSp->FileObject->Flags, FO_SYNCHRONOUS_IO))
        {
            irpSp->FileObject->CurrentByteOffset.QuadPart =
                irpSp->Parameters.Read.ByteOffset.QuadPart + irp->IoStatus.Information;
        }

        SetFlag(irpSp->FileObject->Flags, FO_FILE_FAST_IO_READ);
    }

    CompleteRequest(irp, STATUS_SUCCESS, IO_DISK_INCREMENT);
}

//
// Shared by both the non-cached and cached read paths below, which used
// to each carry an identical copy of this check: trims BytesLength down
// to what's actually available before EOF, or signals STATUS_END_OF_FILE
// (with Irp->IoStatus.Information already zeroed) for a read starting
// at or past it -- expected/handled by callers like a media player
// seeking near the tail of a file, not a hard error.
//
static NTSTATUS BlorgTrimReadToFileSize(PFCB Fcb, LARGE_INTEGER StartingByte, ULONG BytesLength, PIRP Irp, PULONG RealLengthOut)
{
    if (StartingByte.QuadPart >= Fcb->Header.FileSize.QuadPart)
    {
        BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
            Fcb->Header.AllocationSize.QuadPart,
            StartingByte.QuadPart,
            BytesLength);

        Irp->IoStatus.Information = 0;
        return STATUS_END_OF_FILE;
    }

    if (StartingByte.QuadPart + BytesLength > Fcb->Header.FileSize.QuadPart)
    {
        BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
            Fcb->Header.AllocationSize.QuadPart,
            StartingByte.QuadPart,
            BytesLength);

        ULONG trimLength = C_CAST(ULONG, (StartingByte.QuadPart + BytesLength) - Fcb->Header.FileSize.QuadPart);
        *RealLengthOut = BytesLength - trimLength;
    }
    else
    {
        *RealLengthOut = BytesLength;
    }

    return STATUS_SUCCESS;
}

//
// Volume IRP_MJ_READ handler: validates the node/request, then dispatches to
// either the non-cached path (paging reads served inline, possibly via the
// sequential prefetcher, else a direct async HTTP fetch) or the cached path
// (CcCopyReadEx / CcMdlRead, initializing the cache map on first use). Both
// paths share end-of-file trimming and post-read bookkeeping (file position,
// fast-IO flag) for non-paging requests.
//
// For non-paging requests, FsRtlCheckOplock's result must be returned as-is
// without completing the IRP when it is cancelled or pending. A non-error
// result means the oplock check may have broken an existing conflicting
// oplock, so IsFastIoPossible is refreshed from FsRtlOplockIsFastIoPossible
// before the read-access lock check.
//
// Paging reads bypass the FSP queue and issue the async request inline --
// this is load-bearing, not just an optimisation. A paging read is
// frequently issued by an FSP worker already blocked in CcCopyReadEx
// satisfying an asynchronous cached read-ahead miss; posting that paging
// read back to the same FSP queue would make the blocked worker depend on
// another worker to drain it, and with enough concurrent read-ahead the
// whole pool blocks waiting on paging reads queued behind it -- deadlock,
// independent of FSP_THREAD_COUNT. Issuing inline lets the WSK completion
// (a DPC, not a worker) satisfy the paging read, so the blocked worker's
// own read completes without needing a second worker; FSP_THREAD_COUNT
// becomes a pure throughput knob. Non-paging non-cached reads (e.g.
// FILE_FLAG_NO_BUFFERING) still post: their user buffer must be locked
// (PrePostIrp) and they need a guaranteed PASSIVE_LEVEL worker context.
// Inline issuance happens when either already on a worker (IN_FSP -- the
// original post locked the buffer) or this is a paging read at
// PASSIVE_LEVEL (MM already supplied the MDL, nothing to lock); a paging
// read at raised IRQL -- rare, but possible -- falls through to the post
// path, safe because LockUserBuffer no-ops when Irp->MdlAddress is already
// set (always true for paging I/O).
//
// The sequential prefetcher (Prefetch.h) serves paging reads of a detected
// sequential stream from (or parks them on) chunks already fetched ahead
// of the reader, hiding the per-chunk HTTP RTT that otherwise bounds
// streaming throughput. STATUS_NOT_FOUND means no coverage -- fall through
// to the direct fetch below, which also keeps the prefetch pipeline topped
// up on misses. This applies to paging reads only: they carry none of the
// post-read bookkeeping (file-position advance, fast-IO flag) that
// non-paging reads get in BlorgReadComplete. The DBG-only StatsRecordRead
// call there samples sequentiality before BlorgPrefetchServeRead updates
// the stream trackers, using the same contiguity test the prefetcher uses
// internally -- read characterization, not prefetcher behavior.
//
// The direct async HTTP read returns STATUS_PENDING on success; the client
// receives the body straight into the locked user MDL (zero-copy -- both
// arrival paths have one: MM supplies it for paging I/O, PrePostIrp locked
// one for posted non-paging reads) and BlorgReadComplete completes the IRP
// from the WSK completion path, so this function neither blocks nor copies
// nor completes the IRP itself. If issuing the request fails synchronously,
// the callback never runs and the returned error completes the IRP
// normally. In DBG builds, DriverContext[2] is stamped with the issue-time
// QPC value for BlorgReadComplete's per-chunk latency trace (READ IRPs do
// not otherwise use DriverContext[2]), and this is the one direct-fetch
// issue site for every non-cached read (paging misses and posted
// non-paging reads alike) that increments Stats.ActiveFetches, paired with
// the decrement in BlorgReadComplete -- read-pattern characterization
// only; the prefetcher's own in-flight fetches (PrefetchPump) are not
// counted here on purpose.
//
// The cached path delays CcInitializeCacheMap until the first read, in
// case the caller never does any I/O to the file (FileObject->
// PrivateCacheMap stays NULL until then). The cache manager does not
// tolerate reads beyond file size, hence the same BlorgTrimReadToFileSize
// trim used by the non-cached path. Non-paging requests then mirror the
// synchronous-path bookkeeping: advance CurrentByteOffset for
// FO_SYNCHRONOUS_IO file objects, and set FO_FILE_FAST_IO_READ so the
// dirent's last-access time is updated on close.
//
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BLORGFS_PRINT("BlorgVolumeRead...\n");
    BLORGFS_PRINT(" Irp                    = %p\n", Irp);
    BLORGFS_PRINT(" ->ByteCount           = %8lx\n", IrpSp->Parameters.Read.Length);
    BLORGFS_PRINT(" ->ByteOffset.Quadpart = %llx\n", IrpSp->Parameters.Read.ByteOffset.QuadPart);

    ULONG bytesLength = IrpSp->Parameters.Read.Length;
    ULONG realLength = bytesLength;
    LARGE_INTEGER startingByte = IrpSp->Parameters.Read.ByteOffset;

    if (0 == bytesLength)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    PFCB fcb = IrpSp->FileObject->FsContext;

    switch GET_NODE_TYPE(fcb)
    {
        case BLORGFS_FCB_SIGNATURE:
        {
            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            return STATUS_INVALID_PARAMETER;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeRead: Invalid node type\n");
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) && 
        BooleanFlagOn(Irp->Flags, IRP_NOCACHE) &&
        IrpSp->FileObject->SectionObjectPointer->DataSectionObject)
    {
        IO_STATUS_BLOCK ioStatus = { 0 };

        CcFlushCache(IrpSp->FileObject->SectionObjectPointer,
            &startingByte,
            bytesLength,
            &ioStatus);

        if (!NT_SUCCESS(ioStatus.Status))
        {
            return ioStatus.Status;
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        result = FsRtlCheckOplock(&fcb->Header.Oplock,
            Irp,
            NULL,
            OplockComplete,
            OplockPrePostIrp);

        if (STATUS_SUCCESS != result)
        {
            return result;
        }

        fcb->Header.IsFastIoPossible = FsRtlOplockIsFastIoPossible(&fcb->Header.Oplock);

        if (!FsRtlCheckLockForReadAccess(&fcb->FileLock, Irp))
        {
            return STATUS_FILE_LOCK_CONFLICT;
        }
    }

    if (BooleanFlagOn(Irp->Flags, IRP_NOCACHE))
    {
        BLORGFS_PRINT("Non cached read.\n");

        NTSTATUS trimStatus = BlorgTrimReadToFileSize(fcb, startingByte, bytesLength, Irp, &realLength);

        if (STATUS_END_OF_FILE == trimStatus)
        {
            return STATUS_END_OF_FILE;
        }

        BOOLEAN canIssueInline =
            BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_IN_FSP) ||
            (BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) && PASSIVE_LEVEL == KeGetCurrentIrql());

        if (!canIssueInline)
        {
            BLORGFS_PRINT("BlorgVolumeRead: Enqueue to Fsp\n");
            return FsdPostRequest(Irp, IrpSp);
        }

        if (BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
        {
#ifdef DBG
            if (global.LogLevel >= 1)
            {
                StatsRecordRead(
                    StatsReadIsSequential(fcb, C_CAST(ULONG64, startingByte.QuadPart)),
                    realLength);
            }
#endif

            NTSTATUS prefetchResult = BlorgPrefetchServeRead(
                fcb,
                Irp,
                C_CAST(ULONG64, startingByte.QuadPart),
                realLength);

            if (STATUS_NOT_FOUND != prefetchResult)
            {
                return prefetchResult;
            }
        }

#ifdef DBG
        if (global.LogLevel >= 1)
        {
            Irp->Tail.Overlay.DriverContext[2] =
                C_CAST(PVOID, C_CAST(ULONG_PTR, KeQueryPerformanceCounter(NULL).QuadPart));

            LONG active = InterlockedIncrement(&Stats.ActiveFetches);

            if (active > Stats.PeakActiveFetches)
            {
                Stats.PeakActiveFetches = active;
            }
        }
#endif

        return BlorgHttpGetFileMdl(&fcb->FullPath, startingByte.QuadPart, realLength, Irp->MdlAddress, BlorgReadComplete, Irp);
    }

    else
    {
        if (!IrpSp->FileObject->PrivateCacheMap)
        {
            BLORGFS_PRINT("Initialize cache mapping.\n");

            CcInitializeCacheMap(IrpSp->FileObject, C_CAST(PCC_FILE_SIZES, &fcb->Header.AllocationSize), FALSE, &global.CacheManagerCallbacks, fcb);

            CcSetReadAheadGranularity(IrpSp->FileObject, READ_AHEAD_GRANULARITY);
        }

        NTSTATUS trimStatus = BlorgTrimReadToFileSize(fcb, startingByte, bytesLength, Irp, &realLength);

        if (STATUS_END_OF_FILE == trimStatus)
        {
            return STATUS_END_OF_FILE;
        }

        BLORGFS_PRINT("Cached read.\n");

        if (!FlagOn(IrpSp->MinorFunction, IRP_MN_MDL))
        {
            PVOID systemBuffer = (!Irp->MdlAddress) ? Irp->UserBuffer : MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

            if (!systemBuffer)
            {
               return STATUS_INSUFFICIENT_RESOURCES;
            }

            __try
            {
                if (!CcCopyReadEx(IrpSp->FileObject,
                    &startingByte,
                    realLength,
                    BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_WAIT),
                    systemBuffer,
                    &Irp->IoStatus,
                    Irp->Tail.Overlay.Thread))
                {
                    BLORGFS_PRINT("Cached Read could not wait\n");
                    return FsdPostRequest(Irp, IrpSp);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());
                return GetExceptionCode();
            }
            
            result = Irp->IoStatus.Status;

            NT_ASSERT(NT_SUCCESS(result));
        }
        else
        {
            BLORGFS_PRINT("MDL read.\n");

            __try
            {
                CcMdlRead(IrpSp->FileObject,
                    &startingByte,
                    realLength,
                    &Irp->MdlAddress,
                    &Irp->IoStatus);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                BLORGFS_PRINT("Cached Read exception: %8lx\n", GetExceptionCode());
                return GetExceptionCode();
            }

            result = Irp->IoStatus.Status;
            NT_ASSERT(NT_SUCCESS(result));
        }
    }

    if (!BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        if (BooleanFlagOn(IrpSp->FileObject->Flags, FO_SYNCHRONOUS_IO))
        {
            IrpSp->FileObject->CurrentByteOffset.QuadPart = startingByte.QuadPart + Irp->IoStatus.Information;
        }

        if (NT_SUCCESS(result))
        {
            SetFlag(IrpSp->FileObject->Flags, FO_FILE_FAST_IO_READ);
        }
    }

    return result;
}

//
// IRP_MJ_READ dispatch entry point: sets up the IRP context and file-system
// entry/exit bracketing, then routes to BlorgVolumeRead for the volume
// device object (disk/FS-control device objects have no read support yet).
//
NTSTATUS BlorgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = IsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            BlorgSetupIrpContext(Irp, IoIsOperationSynchronous(Irp));
            result = BlorgVolumeRead(Irp, irpSp);
            if (STATUS_PENDING != result)
            {
                CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
    }
    FsRtlExitFileSystem();

    if (topLevel)
    {
        IoSetTopLevelIrp(NULL);
    }
    return result;
}
