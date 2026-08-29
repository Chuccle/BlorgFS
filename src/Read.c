#include "Driver.h"

//
//  This file implements the volume read dispatch path: cached reads
//  (via Cc) and non-cached/paging reads (direct async HTTP fetch), plus
//  their completions and the end-of-file trim shared by both paths.
//

//
//  READ_AHEAD_GRANULARITY (Driver.h) is Cc's read-ahead granularity for
//  cached reads. Cc's default heuristics are tuned for local disk seek
//  costs, not for a backend where every miss is an HTTP round trip; left
//  unset, Cc under-fetches relative to what one Range GET can profitably
//  return. 256 KB matches HTTP_FILE_INITIAL_RECV_CAPACITY (Client.c) so a
//  read-ahead-sized fetch doesn't immediately trigger a buffer regrow on
//  the first response. See Driver.h for why it now sits at 512 KB and why
//  larger values were measured and rejected.
//

//
//  Whether a read at Offset continues any tracked stream, evaluated
//  against the tracker array before ReadTrackStream advances it. Unrolled
//  by the compiler over one cache line; the OR-accumulate keeps it
//  branch-free.
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
// Finds the tracker whose last read ended exactly at Offset -- this
// reader's own trail -- or claims the coldest tracker for a new or seeked
// stream. One unconditional pass over the FCB's single-cache-line tracker
// array: the match test and the coldest scan share the loop, with no early
// exit to mispredict.
//
static READ_STREAM_TRACKER* ReadClaimStream(FCB* Fcb, ULONG64 Offset)
{
    READ_STREAM_TRACKER* match = NULL;
    READ_STREAM_TRACKER* coldest = &Fcb->Streams[0];

    for (ULONG i = 0; i < READ_STREAM_TRACKER_COUNT; ++i)
    {
        READ_STREAM_TRACKER* tracker = &Fcb->Streams[i];

        match = (Offset == tracker->End) ? tracker : match;
        coldest = (tracker->Streak < coldest->Streak) ? tracker : coldest;
    }

    return match ? match : coldest;
}

//
// Advances this reader's tracker past the read being dispatched. Moved
// here when the prefetch ring was removed: the ring was the original
// consumer of the streak, but ReadsSequential is measured from these
// trackers and an on-disk hot cache will want the same signal to decide
// what to admit.
//
// PASSIVE_LEVEL only (the FCB is paged), and deliberately unlocked --
// concurrent readers of one file can lose an update here, which costs
// detection accuracy and nothing else.
//
static VOID ReadTrackStream(FCB* Fcb, ULONG64 Offset, ULONG Length)
{
    READ_STREAM_TRACKER* stream = ReadClaimStream(Fcb, Offset);

    stream->Streak = (Offset == stream->End) ? stream->Streak + 1 : 1;
    stream->End = Offset + Length;
}

// Files the application-visible latency of one non-paging read.
//
// A zero stamp means a paging read, or an IRP completed by a path that
// never took one; either way nobody was waiting on it and there is nothing
// to record.
//
static VOID BlorgReadRecordUserLatency(LONG64 ArrivedQpc)
{
    if (0 == ArrivedQpc)
    {
        return;
    }

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (!statsBlock)
    {
        return;
    }

    statsBlock->UserReadSamples++;

    BlorgStatisticsRecordLatency(
        &statsBlock->UserReadLatencySumUs,
        &statsBlock->UserReadLatencyMaxUs,
        statsBlock->UserReadLatencyBuckets,
        BlorgStatisticsNow() - ArrivedQpc);
}

//
// The fast-I/O read path, timed.
//
// FsRtlCopyRead does the whole read inline: it takes the FCB's resource,
// calls CcCopyRead, and returns TRUE having copied the bytes. When the
// requested range is resident that costs microseconds; when it is not, the
// call blocks inside CcCopyRead until read-ahead or a demand fetch
// delivers it, and the caller waits for exactly that long. This wrapper
// exists so that wait is a number.
//
// Recorded only when fast I/O actually handled the read. A FALSE return
// means the I/O manager will reissue it as an IRP, which BlorgRead times
// on its own; counting both would double every fallback.
//
BOOLEAN BlorgFastIoRead(
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject)
{
    const LONG64 arrivedQpc = BlorgStatisticsNow();

    const BOOLEAN handled = FsRtlCopyRead(
        FileObject, FileOffset, Length, Wait, LockKey, Buffer, IoStatus, DeviceObject);

    if (handled)
    {
        BlorgReadRecordUserLatency(arrivedQpc);
    }

    return handled;
}

//
//  Completion for an async non-cached read. Invoked from the WSK
//  completion path at <= DISPATCH_LEVEL, so everything it touches must be
//  legal there: the source body lives in the NonPagedPoolNx HTTP receive
//  buffer, and the destination is the user buffer already locked into
//  Irp->MdlAddress by BlorgPrePostIrp when the IRP was posted to the FSP queue.
//  CallerContext is the PIRP.
//
//  This is a zero-copy read (BlorgHttpGetFileMdl): the body was received
//  directly into Irp->MdlAddress by the client, so there is nothing to
//  map, copy, or free here -- FileBuffer carries only the byte count
//  (the client validated it against the requested range length, so it
//  never exceeds the locked user buffer).
//
//  Two spans are closed here, and they are not the same span.
//  DriverContext[2] carries the fetch issue stamp set at the direct-fetch
//  site in BlorgVolumeRead and measures what the driver waited on the
//  network; DriverContext[3] carries the arrival stamp set in BlorgRead
//  and measures what the application waited on the driver. Only READ IRPs
//  use these two slots -- [1] belongs to the CREATE path's stash. Nothing
//  here formats a %wZ/%Z: this runs at <= DISPATCH on the WSK completion
//  chain, where that would touch paged code and bugcheck.
//
//  For non-paging reads, this mirrors the post-read bookkeeping the
//  synchronous path used to do: advance the file position for
//  synchronous file objects and note that a fast-IO read happened.
//
static VOID BlorgReadComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PIRP irp = CallerContext;

    const LONG64 arrivedQpc = C_CAST(LONG64, C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[3]));

    LONG64 issueQpc = C_CAST(LONG64, C_CAST(ULONG_PTR, irp->Tail.Overlay.DriverContext[2]));

    if (!NT_SUCCESS(Status))
    {
        BLORGFS_PRINT("BlorgReadComplete: http read failed: %8lx\n", Status);
        BLORGFS_STAT_INC(FetchesFailed);
        BlorgReadRecordUserLatency(arrivedQpc);
        BlorgCompleteRequest(irp, Status, IO_DISK_INCREMENT);
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

    BlorgReadRecordUserLatency(arrivedQpc);

    BlorgCompleteRequest(irp, STATUS_SUCCESS, IO_DISK_INCREMENT);
}

//
// Shared by both the non-cached and cached read paths below, which used
// to each carry an identical copy of this check: trims BytesLength down
// to what's actually available before EOF, or signals STATUS_END_OF_FILE
// (with Irp->IoStatus.Information already zeroed) for a read starting
// at or past it -- expected/handled by callers like a media player
// seeking near the tail of a file, not a hard error.
//
// The second bound is the one that is easy to miss. A backend-declared size
// near the top of the range, and a hand-built kernel IRP whose offset sits
// just under it, make the addition wrap signed: the sum goes negative, both
// it and the end-of-file comparison go false, and an untrimmed request
// reaches the fetch with a nonsensical range. The negative-offset check at
// the top of BlorgVolumeRead catches only half of that shape. END_OF_FILE
// rather than an error, because the read genuinely starts inside the
// declared size but cannot be bounded by it, which is the same
// caller-visible answer as any other tail past EOF.
//
static NTSTATUS BlorgTrimReadToFileSize(PFCB Fcb, LARGE_INTEGER StartingByte, ULONG BytesLength, PIRP Irp, PULONG RealLengthOut)
{
    if (StartingByte.QuadPart >= Fcb->Header.FileSize.QuadPart)
    {
        BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
            Fcb->Header.FileSize.QuadPart,
            StartingByte.QuadPart,
            BytesLength);

        Irp->IoStatus.Information = 0;
        BLORGFS_STAT_INC(ReadsEndOfFile);
        return STATUS_END_OF_FILE;
    }

    if (StartingByte.QuadPart > MAXLONGLONG - C_CAST(LONGLONG, BytesLength))
    {
        BLORGFS_PRINT("Read end unrepresentable - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
            Fcb->Header.FileSize.QuadPart,
            StartingByte.QuadPart,
            BytesLength);

        Irp->IoStatus.Information = 0;
        BLORGFS_STAT_INC(ReadsEndOfFile);
        return STATUS_END_OF_FILE;
    }

    if (StartingByte.QuadPart + BytesLength > Fcb->Header.FileSize.QuadPart)
    {
        BLORGFS_PRINT("Read beyond file size - file size = %llu, requested starting byte = %llu, requested length = %lu\n",
            Fcb->Header.FileSize.QuadPart,
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
// either the non-cached path (paging reads served inline by a direct async
// HTTP fetch) or the cached path
// (CcCopyReadEx / CcMdlRead, initializing the cache map on first use). Both
// paths share end-of-file trimming and post-read bookkeeping (file position,
// fast-IO flag) for non-paging requests.
//
// A negative ByteOffset is rejected outright, before anything else reads
// it. The I/O manager screens negative offsets out of NtReadFile (measured:
// STATUS_INVALID_PARAMETER), so usermode cannot reach this, but a kernel
// caller that builds its own IRP -- IoAllocateIrp plus a hand-filled
// Parameters.Read.ByteOffset, which is exactly what a filter layered above
// a filesystem does -- is validated by nobody. Nothing downstream would
// catch it either: BlorgTrimReadToFileSize's two comparisons are both
// false for a negative offset (it is neither >= FileSize nor does adding
// the length exceed it), so the read passes through untrimmed and is then
// widened to ULONG64, turning -4096 into 2^64-4096. Nothing downstream
// treats that as a memory-safety boundary any more -- the prefetch ring
// whose containment test used to backstop it is gone -- so this check is
// now the only thing standing between a negative offset and a range GET
// for a nonsensical part of the file. The read is meaningless regardless,
// and failing it here makes the answer a clean error instead of a short
// read from wherever the arithmetic landed.
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
// (BlorgPrePostIrp) and they need a guaranteed PASSIVE_LEVEL worker context.
// Inline issuance happens when either already on a worker (IN_FSP -- the
// original post locked the buffer) or this is a paging read at
// PASSIVE_LEVEL (MM already supplied the MDL, nothing to lock); a paging
// read at raised IRQL -- rare, but possible -- falls through to the post
// path, safe because BlorgLockUserBuffer no-ops when Irp->MdlAddress is already
// set (always true for paging I/O).
//
// Paging reads advance this reader's stream tracker and then go straight
// to a direct fetch. Lookahead is Cc's alone: it reads ahead of the
// application, issues demand-driven so it paces itself against the link,
// and lands in the paging IRP's MDL with no copy. Tracking applies to
// paging reads only: they carry none of the
// post-read bookkeeping (file-position advance, fast-IO flag) that
// non-paging reads get in BlorgReadComplete. The BlorgReadIsSequential
// sample is taken before ReadTrackStream advances the trackers, so it
// characterizes the read against the stream's prior position.
//
// The direct fetch below returns STATUS_PENDING out of a dispatch
// routine that nothing else has marked pending: a paging read bypasses
// the FSP queue (so it never reaches IoCsqInsertIrp, which does the
// marking for posted requests) and skips the oplock package (so
// BlorgOplockPrePostIrp never runs either). The fetch is marked here,
// before the issue rather than after,
// because a synchronously-completing issue may already have freed the
// IRP by the time the call returns. Marking an IRP whose issue then
// fails synchronously is harmless -- BlorgRead completes it with that
// error, and a set PendingReturned on a completed IRP costs nothing;
// the damaging direction is returning STATUS_PENDING unmarked, which
// silently breaks pending propagation in any filter layered above.
//
// The direct async HTTP read returns STATUS_PENDING on success; the client
// receives the body straight into the locked user MDL (zero-copy -- both
// arrival paths have one: MM supplies it for paging I/O, BlorgPrePostIrp locked
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
// BlorgReadComplete. With the prefetch ring gone these count every fetch
// the driver makes, so FetchesIssued is now simply the request rate against
// the backend.
//
// Both counters are raised before the issue, because a synchronously
// completing issue runs BlorgReadComplete -- and its matching decrement --
// before the call returns. That ordering makes the synchronous FAILURE
// case this path's own to settle: the client's contract is that a
// non-STATUS_PENDING return means the callback never ran (see
// HttpGetFileCommon), so nothing else will ever terminate the fetch just
// counted. Left unsettled, FetchesIssued outruns
// FetchesCompleted + FetchesFailed permanently, and since in-flight is now
// derived from exactly that difference rather than tracked in a gauge, the
// reported depth would never return to zero. Compare-BlorgMetrics.ps1
// reports the same difference as a note about fetches "in flight at sample
// time".
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
// UserDiskReads is raised alongside the driver's own FetchesIssued at the
// direct-fetch site because it is the same event named by the standard:
// FILESYSTEM_STATISTICS means by it a read that had to leave the cache to
// be answered, and UserFileReadBytes over it is the average size the cache
// manager asked this filesystem for. That is what makes
// `fsutil fsinfo statistics` on this volume comparable with the same
// command on NTFS; reporting zero made the standard surface useless and
// sent the only comparison that needed it through the vendor IOCTL.
//
// The read-ahead granularity override is applied at cache-map time, where
// zero means leave Cc's own default in place -- the one setting no override
// value can express.
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

    if (startingByte.QuadPart < 0)
    {
        BLORGFS_PRINT("BlorgVolumeRead: negative byte offset %lld\n", startingByte.QuadPart);

        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
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
            BlorgOplockComplete,
            BlorgOplockPrePostIrp);

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
            return BlorgFsdPostRequest(Irp, IrpSp);
        }

        if (BooleanFlagOn(Irp->Flags, IRP_PAGING_IO))
        {
            BLORGFS_STAT_INC(ReadsPagingInline);

            if (BlorgReadIsSequential(fcb, C_CAST(ULONG64, startingByte.QuadPart)))
            {
                BLORGFS_STAT_INC(ReadsSequential);
            }

            ReadTrackStream(fcb, C_CAST(ULONG64, startingByte.QuadPart), realLength);
        }

        Irp->Tail.Overlay.DriverContext[2] =
            C_CAST(PVOID, C_CAST(ULONG_PTR, BlorgStatisticsNow()));

        BLORGFS_STAT_INC(FetchesIssued);

        BLORGFS_STAT_INC(UserDiskReads);

        if (BooleanFlagOn(Irp->Flags, IRP_NOCACHE))
        {
            BLORGFS_STAT_INC(NonCachedDiskReads);
        }

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
        }

        return fetchStatus;
    }

    else
    {
        if (!IrpSp->FileObject->PrivateCacheMap)
        {
            BLORGFS_PRINT("Initialize cache mapping.\n");

            CcInitializeCacheMap(IrpSp->FileObject, C_CAST(PCC_FILE_SIZES, &fcb->Header.AllocationSize), FALSE, &global.CacheManagerCallbacks, fcb);

            if (0 != global.ReadAheadGranularity)
            {
                CcSetReadAheadGranularity(IrpSp->FileObject, global.ReadAheadGranularity);
            }
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
                    return BlorgFsdPostRequest(Irp, IrpSp);
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
// A non-paging read is stamped on arrival into DriverContext[3], which
// READ IRPs otherwise leave unused. DriverContext[2] already carries the
// fetch issue stamp and is a different span: that one starts when the
// driver asks the network, this one when the application asks the driver.
//
// The synchronous return is where that span is closed, rather than in
// BlorgCompleteRequest, because it is the interesting case: a cached read
// that misses blocks inside CcCopyReadEx and arrives back here having
// waited, which is precisely the stall a viewer feels and the only place
// it is visible.
//
// One switch body covers everything that is not the volume, unknown kinds
// included: each completes with the initialised-invalid status. An unknown
// kind cannot reach this entry point through the I/O manager today -- only
// this driver's three device objects carry its major table -- but a
// dispatcher that completes inside its cases rather than after the switch
// would strand the IRP on any future fourth kind, so the completion is
// unconditional.
//
NTSTATUS BlorgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = BlorgIsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            BlorgSetupIrpContext(Irp, IoIsOperationSynchronous(Irp));

            const BOOLEAN userRead = !BooleanFlagOn(Irp->Flags, IRP_PAGING_IO);
            const LONG64 arrivedQpc = userRead ? BlorgStatisticsNow() : 0;

            if (userRead)
            {
                Irp->Tail.Overlay.DriverContext[3] = C_CAST(PVOID, C_CAST(ULONG_PTR, arrivedQpc));
            }

            result = BlorgVolumeRead(Irp, irpSp);

            if (STATUS_PENDING != result)
            {
                BlorgReadRecordUserLatency(arrivedQpc);
                BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }

            break;
        }

        case BlorgDeviceDisk:
        case BlorgDeviceFileSystem:
        default:
        {
            BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
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
