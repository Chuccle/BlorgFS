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

//
//  Whether a read at Offset continues any tracked stream -- the same
//  contiguity test BlorgPrefetchServeRead applies internally, evaluated
//  against the tracker array before the serve call updates it. Read-only
//  with respect to prefetcher state. Unrolled by the compiler over one
//  cache line; the OR-accumulate keeps it branch-free.
//
//  This is all that remains of the old DBG-only read-pattern block. The
//  window it used to print every 256 reads -- sequential share, mean/min/max
//  length, peak in-flight fetches -- is now carried by the always-on
//  counters in Statistics.h, which report the same shape without a checked
//  build and without a DbgPrint on the read path perturbing the very
//  timings being characterized.
//
static BOOLEAN BlorgReadIsSequential(const FCB* Fcb, ULONG64 Offset)
{
    BOOLEAN sequential = FALSE;

    for (ULONG i = 0; i < READ_STREAM_TRACKER_COUNT; ++i)
    {
        sequential |= (Offset == Fcb->Streams[i].End);
    }

    return sequential;
}

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

    BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.FetchesActive);

    LONG64 issueQpc = C_CAST(LONG64, C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[2]));

    if (!NT_SUCCESS(Status))
    {
        BLORGFS_PRINT("BlorgReadComplete: http read failed: %8lx\n", Status);
        BLORGFS_STAT_INC(FetchesFailed);
        CompleteRequest(irp, Status, IO_DISK_INCREMENT);
        return;
    }

    irp->IoStatus.Information = FileBuffer->BodyBufferSize;

    BLORGFS_STAT_INC(FetchesCompleted);
    BLORGFS_STAT_ADD(FetchBytes, FileBuffer->BodyBufferSize);

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (statsBlock && issueQpc)
    {
        BlorgStatisticsRecordLatency(
            &statsBlock->FetchLatencySumUs,
            &statsBlock->FetchLatencyMaxUs,
            statsBlock->FetchLatencyBuckets,
            BlorgStatisticsNow() - issueQpc);
    }

    if (!BooleanFlagOn(irp->Flags, IRP_PAGING_IO))
    {
        BLORGFS_STAT_INC(UserFileReads);
        BLORGFS_STAT_ADD(UserFileReadBytes, irp->IoStatus.Information);

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
        BLORGFS_STAT_INC(ReadsEndOfFile);
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
// non-paging reads get in BlorgReadComplete. The BlorgReadIsSequential
// sample there is taken before BlorgPrefetchServeRead updates the stream
// trackers, using the same contiguity test the prefetcher uses
// internally -- read characterization, not prefetcher behavior.
//
// Both async outcomes of this path -- the prefetcher parking the read and
// the direct fetch below -- return STATUS_PENDING out of a dispatch
// routine that nothing else has marked pending: a paging read bypasses
// the FSP queue (so it never reaches IoCsqInsertIrp, which does the
// marking for posted requests) and skips the oplock package (so
// OplockPrePostIrp never runs either). The prefetcher marks its own
// parked IRP, since it must do so before the waiter is published;
// the direct fetch is marked here, before the issue rather than after,
// because a synchronously-completing issue may already have freed the
// IRP by the time the call returns. Marking an IRP whose issue then
// fails synchronously is harmless -- BlorgRead completes it with that
// error, and a set PendingReturned on a completed IRP costs nothing;
// the damaging direction is returning STATUS_PENDING unmarked, which
// silently breaks pending propagation in any filter layered above.
//
// The direct async HTTP read returns STATUS_PENDING on success; the client
// receives the body straight into the locked user MDL (zero-copy -- both
// arrival paths have one: MM supplies it for paging I/O, PrePostIrp locked
// one for posted non-paging reads) and BlorgReadComplete completes the IRP
// from the WSK completion path, so this function neither blocks nor copies
// nor completes the IRP itself. If issuing the request fails synchronously,
// the callback never runs and the returned error completes the IRP
// normally. DriverContext[2] is stamped with the issue-time QPC value
// that BlorgReadComplete turns into the chunk-latency histogram (READ
// IRPs do not otherwise use DriverContext[2]), and this is the one
// direct-fetch issue site for every non-cached read (paging misses and
// posted non-paging reads alike), so it is where FetchesIssued and the
// in-flight gauge are raised, paired with the completion accounting in
// BlorgReadComplete. The prefetcher's own in-flight fetches are counted
// separately as PrefetchFetchesIssued (Prefetch.c) and deliberately do
// not land in these two, so the direct-fetch rate stays readable as
// "what the prefetcher failed to cover".
//
// Both counters are raised before the issue, because a synchronously
// completing issue runs BlorgReadComplete -- and its matching decrement --
// before the call returns. That ordering makes the synchronous FAILURE
// case this path's own to settle: the client's contract is that a
// non-STATUS_PENDING return means the callback never ran (see
// HttpGetFileCommon), so nothing else will ever terminate the fetch just
// counted. Left unsettled, FetchesIssued outruns
// FetchesCompleted + FetchesFailed -- which Compare-BlorgMetrics.ps1
// reports only as a note about fetches "in flight at sample time" -- and,
// worse, the FetchesActive gauge ratchets up permanently, taking
// FetchesActivePeak with it, since a gauge has nothing to reset it.
// PrefetchPump settles its own equivalent failure the same way.
//
// The NonCachedReads/NonCachedReadBytes pair is counted only on an IRP's
// first pass through here, gated on IRP_CONTEXT_FLAG_IN_FSP. A read that
// cannot issue inline is posted to the FSP, whose worker re-enters this
// same function on the same IRP -- so counting unconditionally scored
// every posted read twice, and since in practice essentially every
// non-cached read takes the post path, both counters simply read 2x
// reality. That matters beyond this driver's own telemetry:
// NonCachedReads feeds the standard FAT_STATISTICS surface that
// fsutil reports.
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

        BOOLEAN alreadyInFsp =
            BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_IN_FSP);

        BOOLEAN canIssueInline =
            alreadyInFsp ||
            (BooleanFlagOn(Irp->Flags, IRP_PAGING_IO) && PASSIVE_LEVEL == KeGetCurrentIrql());

        if (!alreadyInFsp)
        {
            BLORGFS_STAT_INC(NonCachedReads);
            BLORGFS_STAT_ADD(NonCachedReadBytes, realLength);
        }

        if (!canIssueInline)
        {
            BLORGFS_PRINT("BlorgVolumeRead: Enqueue to Fsp\n");
            BLORGFS_STAT_INC(ReadsPosted);
            return FsdPostRequest(Irp, IrpSp);
        }

        if (BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
        {
            BLORGFS_STAT_INC(ReadsPagingInline);

            if (BlorgReadIsSequential(fcb, C_CAST(ULONG64, startingByte.QuadPart)))
            {
                BLORGFS_STAT_INC(ReadsSequential);
            }

            NTSTATUS prefetchResult = BlorgPrefetchServeRead(
                fcb,
                Irp,
                C_CAST(ULONG64, startingByte.QuadPart),
                realLength);

            if (STATUS_NOT_FOUND != prefetchResult)
            {
                return prefetchResult;
            }

            BLORGFS_STAT_INC(PrefetchMisses);
        }

        Irp->Tail.Overlay.DriverContext[2] =
            C_CAST(PVOID, C_CAST(ULONG_PTR, BlorgStatisticsNow()));

        BLORGFS_STAT_INC(FetchesIssued);

        BlorgStatisticsGaugeIncrement(
            &BlorgStatisticsGauges.FetchesActive,
            &BlorgStatisticsGauges.FetchesActivePeak);

        IoMarkIrpPending(Irp);

        NTSTATUS fetchStatus = BlorgHttpGetFileMdl(
            &fcb->FullPath,
            startingByte.QuadPart,
            realLength,
            Irp->MdlAddress,
            BlorgReadComplete,
            Irp);

        if (STATUS_PENDING != fetchStatus)
        {
            BLORGFS_STAT_INC(FetchesFailed);
            BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.FetchesActive);
        }

        return fetchStatus;
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

        BLORGFS_STAT_INC(ReadsCached);

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
        BLORGFS_STAT_INC(UserFileReads);
        BLORGFS_STAT_ADD(UserFileReadBytes, Irp->IoStatus.Information);

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
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            BlorgSetupIrpContext(Irp, IoIsOperationSynchronous(Irp));
            result = BlorgVolumeRead(Irp, irpSp);
            if (STATUS_PENDING != result)
            {
                CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BlorgDeviceDisk:
        {
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
        case BlorgDeviceFileSystem:
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
