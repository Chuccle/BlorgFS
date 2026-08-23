#include "Driver.h"

//
// Implements the per-FCB sequential-read prefetch ring: creating and
// freeing it, pumping fetches to keep it full, serving/parking paging
// reads against it, and its fetch-completion callback.
//
// See Prefetch.h for the design: why the ring exists, the IRQL/issuance
// rules (all fetch issuance at PASSIVE, completions at <= DISPATCH touch
// only the ring), the buffer-detach convention for outside-the-lock
// copies, and the RefCount/Generation lifetime rules.
//

#define PREFETCH_TAG 'FRPB'

//
// Driver-wide live-ring count, doubling as the unload drain gate. It
// starts at 1: that standing reference belongs to the driver itself and
// is released by BlorgPrefetchDrain, which is what lets the count reach
// zero exactly once and lets an acquire refuse to lift it back off zero.
// A live ring holds one count for its own existence, so "count is zero"
// means no ring exists, and therefore no fetch is in flight and no pump
// work item is queued or running -- all three are ring references.
//
// Incremented by PrefetchAcquireRing and decremented by PrefetchFreeRing,
// so the CAS-loser free and every creation failure path stay symmetric
// with their acquire.
//
static volatile LONG PrefetchRingCount = 1;
static KEVENT PrefetchDrainEvent;

static VOID PrefetchFetchComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext);
static VOID PrefetchReleaseRing(VOID);

//
// Driver-wide ring budget by machine size, the same coarse
// MmQuerySystemSize tiering fastfat/ntfs use for cache sizing. Worst
// case ring memory at full depth is the tier count x PREFETCH_DEPTH x
// PREFETCH_CHUNK of NonPagedPoolNx (2/4/8 rings = 8/16/32 MB); rings a
// stream never deepens stay far below that. Called at PASSIVE from ring
// creation only.
//
static LONG PrefetchMaxRings(VOID)
{
    switch (MmQuerySystemSize())
    {
        case MmSmallSystem:
        {
            return 2;
        }
        case MmMediumSystem:
        {
            return 4;
        }
        default:
        {
            return 8;
        }
    }
}

//
// Frees a ring's per-slot buffers and MDLs, then the ring itself, and
// releases its slot in the driver-wide ring budget. Called once RefCount
// has dropped to zero -- no fetches or FCB attachment remain -- or for a
// ring that was never published (creation failure, CAS loser).
//
static VOID PrefetchFreeRing(PREFETCH_RING* Ring)
{
    for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
    {
        if (Ring->BufferMdls[i])
        {
            IoFreeMdl(Ring->BufferMdls[i]);
        }

        if (Ring->Buffers[i])
        {
            ExFreePool(Ring->Buffers[i]);
        }
    }

    if (Ring->PumpWorkItem)
    {
        IoFreeWorkItem(Ring->PumpWorkItem);
    }

    if (Ring->Path.Buffer)
    {
        ExFreePool(Ring->Path.Buffer);
    }

    ExFreePool(Ring);

    BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.PrefetchRingsLive);

    PrefetchReleaseRing();
}

//
// Drop one reference (FCB attachment or a completed fetch); the last one
// frees. Runs at <= DISPATCH_LEVEL -- everything freed is non-paged.
//
static VOID PrefetchReleaseRef(PREFETCH_RING* Ring)
{
    if (0 == InterlockedDecrement(&Ring->RefCount))
    {
        PrefetchFreeRing(Ring);
    }
}

//
// Allocates and initializes a prefetch ring: the first PREFETCH_MIN_DEPTH
// slots' non-paged buffers and their MDLs (built once, reused for every
// fetch so steady-state streaming avoids repeated allocation; deeper
// slots are allocated lazily by the pump as parks grow DepthLimit), plus
// the ring's lock and initial RefCount for its FCB attachment. Non-paged
// since fetch completions read the buffers at DISPATCH_LEVEL. Gated on
// the driver-wide ring budget: the count is claimed first and released
// by PrefetchFreeRing on every failure path, so budget accounting is
// symmetric however creation ends. Returns NULL at the budget cap or on
// any allocation failure -- the stream then runs on direct fetches.
//
//
// Claims a slot for a new ring, subject to both the driver-wide budget
// and the unload gate. One CAS loop serves both, which also fixes what
// the previous increment-then-decrement did: that briefly pushed the
// count past the cap, so a concurrent create could see the inflated value
// and refuse a slot that was in fact free. The CAS never publishes a
// count above the cap.
//
// The standing reference is why the budget compares against count - 1.
//
static BOOLEAN PrefetchAcquireRing(VOID)
{
    LONG maxRings = PrefetchMaxRings();
    LONG current = ReadNoFence(&PrefetchRingCount);

    while (0 != current && (current - 1) < maxRings)
    {
        LONG previous = InterlockedCompareExchange(&PrefetchRingCount, current + 1, current);

        if (previous == current)
        {
            return TRUE;
        }

        current = previous;
    }

    return FALSE;
}

//
// Drops a ring's count; whoever drops the last one signals the drain.
// Runs at <= DISPATCH_LEVEL, where KeSetEvent with Wait = FALSE is legal.
//
static VOID PrefetchReleaseRing(VOID)
{
    if (0 == InterlockedDecrement(&PrefetchRingCount))
    {
        KeSetEvent(&PrefetchDrainEvent, IO_NO_INCREMENT, FALSE);
    }
}

VOID BlorgPrefetchInitialize(VOID)
{
    KeInitializeEvent(&PrefetchDrainEvent, NotificationEvent, FALSE);
}

//
// Releases the standing reference and waits for the last ring to go. A
// ring outlives its FCB by however long its in-flight fetches take, so
// without this a dismount-then-unload can race a fetch whose completion
// routine lives in this image. Bounded by the socket watchdogs, so a dead
// peer cannot hang unload indefinitely.
//
VOID BlorgPrefetchDrain(VOID)
{
    PrefetchReleaseRing();

    KeWaitForSingleObject(&PrefetchDrainEvent, Executive, KernelMode, FALSE, NULL);
}

//
// The file identity the pump needs is snapshotted into the ring here (see
// the field comment in Prefetch.h): an owned non-paged copy of the path
// plus the current file size, so no pump ever has to reach back into the
// paged, teardown-prone FCB.
//
// PrefetchRingsLive is incremented right after the ring struct itself is
// allocated, not once construction fully succeeds: every failure path
// from there on frees the ring through PrefetchFreeRing, whose decrement
// is unconditional, so the increment must be in place before the first
// possible free or the gauge goes negative on a mid-construction
// allocation failure -- exactly the pool pressure that makes it worth
// trusting.
//
static PREFETCH_RING* PrefetchCreateRing(FCB* Fcb)
{
    if (!PrefetchAcquireRing())
    {
        BLORGFS_STAT_INC(PrefetchRingsRefused);
        return NULL;
    }

    PREFETCH_RING* ring = ExAllocatePoolZero(NonPagedPoolNx, sizeof(PREFETCH_RING), PREFETCH_TAG);

    if (!ring)
    {
        PrefetchReleaseRing();
        return NULL;
    }

    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.PrefetchRingsLive, NULL);

    for (ULONG i = 0; i < PREFETCH_MIN_DEPTH; ++i)
    {
        ring->Buffers[i] = ExAllocatePoolUninitialized(NonPagedPoolNx, PREFETCH_CHUNK, PREFETCH_TAG);

        if (!ring->Buffers[i])
        {
            PrefetchFreeRing(ring);
            return NULL;
        }

        ring->BufferMdls[i] = IoAllocateMdl(ring->Buffers[i], PREFETCH_CHUNK, FALSE, FALSE, NULL);

        if (!ring->BufferMdls[i])
        {
            PrefetchFreeRing(ring);
            return NULL;
        }

        MmBuildMdlForNonPagedPool(ring->BufferMdls[i]);
    }

    ring->PumpWorkItem = IoAllocateWorkItem(global.FileSystemDeviceObject);

    if (!ring->PumpWorkItem)
    {
        PrefetchFreeRing(ring);
        return NULL;
    }

    ring->Path.Buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, Fcb->FullPath.Length, PREFETCH_TAG);

    if (!ring->Path.Buffer)
    {
        PrefetchFreeRing(ring);
        return NULL;
    }

    RtlCopyMemory(ring->Path.Buffer, Fcb->FullPath.Buffer, Fcb->FullPath.Length);
    ring->Path.Length = Fcb->FullPath.Length;
    ring->Path.MaximumLength = Fcb->FullPath.Length;
    ring->FileSize = C_CAST(ULONG64, Fcb->Header.FileSize.QuadPart);

    KeInitializeSpinLock(&ring->Lock);
    ring->DepthLimit = PREFETCH_MIN_DEPTH;
    ring->RefCount = 1;

    BLORGFS_STAT_INC(PrefetchRingsArmed);

    return ring;
}

//
// Top the pipeline back up: realize any pending depth growth, reserve
// every empty slot under the lock, then issue the fetches outside it
// (HttpBuildRequest touches paged code, so this must run at
// PASSIVE_LEVEL -- which it does, being called only from the paging-read
// path and the PASSIVE pump work item; never directly from a fetch
// completion). Everything it needs from the file comes from the ring's
// own Path/FileSize snapshot, never the FCB. Depth growth first: slots
// admitted by a park's DepthLimit bump get their buffer and MDL
// allocated here, at PASSIVE, before the reservation pass can see them.
// NULL BufferMdls[i] is what distinguishes a never-allocated slot from
// one whose buffer is merely detached for an outside-the-lock copy
// (detach nulls only Buffers[i]); the unlocked BufferMdls read is
// advisory -- concurrent pumps on one ring can race it -- and the
// publish re-checks under the lock, with the loser freeing its copy. An
// allocation failure just leaves the ring shallower this round; the next
// pump retries. A NULL slot buffer in the reservation pass means the
// slot is detached for a copy elsewhere, so such slots are skipped this
// round. Once reserved, Hot[i] is safe to read without the lock: an
// InFlight slot is owned by its fetch, and only this issuing thread and
// the completion (which cannot run before issue) touch it. On a
// synchronous issue failure the completion callback never ran and never
// will, so the slot and the fetch's ref are released here directly
// (never the last ref -- every caller holds one: the serve path via the
// FCB attachment, the pump worker via the work item's reference). A
// paging read may have parked on the reserved slot between the
// reservation and the failed issue, so the failure path drains
// Waiters[i] under the lock and completes the parked IRP with the issue
// status -- leaving it would strand the read forever, and a later reuse
// of the slot would complete it with data from a different range.
//
static VOID PrefetchPump(PREFETCH_RING* Ring)
{
    ULONG64 fileSize = Ring->FileSize;
    ULONG reserved[PREFETCH_DEPTH];
    ULONG reservedCount = 0;

    KIRQL irql;
    KeAcquireSpinLock(&Ring->Lock, &irql);
    ULONG depthLimit = Ring->DepthLimit;
    KeReleaseSpinLock(&Ring->Lock, irql);

    for (ULONG i = PREFETCH_MIN_DEPTH; i < depthLimit; ++i)
    {
        if (Ring->BufferMdls[i])
        {
            continue;
        }

        PCHAR buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, PREFETCH_CHUNK, PREFETCH_TAG);

        if (!buffer)
        {
            break;
        }

        PMDL mdl = IoAllocateMdl(buffer, PREFETCH_CHUNK, FALSE, FALSE, NULL);

        if (!mdl)
        {
            ExFreePool(buffer);
            break;
        }

        MmBuildMdlForNonPagedPool(mdl);

        KeAcquireSpinLock(&Ring->Lock, &irql);

        if (Ring->BufferMdls[i])
        {
            KeReleaseSpinLock(&Ring->Lock, irql);
            IoFreeMdl(mdl);
            ExFreePool(buffer);
            continue;
        }

        Ring->Buffers[i] = buffer;
        Ring->BufferMdls[i] = mdl;
        KeReleaseSpinLock(&Ring->Lock, irql);
    }

    KeAcquireSpinLock(&Ring->Lock, &irql);

    for (ULONG i = 0; i < depthLimit && Ring->NextFetchOffset < fileSize; ++i)
    {
        if (PrefetchSlotEmpty != Ring->Hot[i].State || !Ring->Buffers[i])
        {
            continue;
        }

        ULONG64 remaining = fileSize - Ring->NextFetchOffset;
        ULONG fetchLength = (remaining < PREFETCH_CHUNK) ? C_CAST(ULONG, remaining) : PREFETCH_CHUNK;

        Ring->Hot[i].RangeOffset = Ring->NextFetchOffset;
        Ring->Hot[i].Length = fetchLength;
        Ring->Hot[i].State = PrefetchSlotInFlight;

        Ring->FetchCtx[i].Ring = Ring;
        Ring->FetchCtx[i].SlotIndex = i;
        Ring->FetchCtx[i].Generation = Ring->Generation;
        Ring->FetchCtx[i].IssueQpc = BlorgStatisticsNow();

        InterlockedIncrement(&Ring->RefCount);
        Ring->NextFetchOffset += fetchLength;
        reserved[reservedCount++] = i;
    }

    KeReleaseSpinLock(&Ring->Lock, irql);

    for (ULONG r = 0; r < reservedCount; ++r)
    {
        ULONG i = reserved[r];

        NTSTATUS status = BlorgHttpGetFileMdl(
            &Ring->Path,
            Ring->Hot[i].RangeOffset,
            Ring->Hot[i].Length,
            Ring->BufferMdls[i],
            PrefetchFetchComplete,
            &Ring->FetchCtx[i]);

        BLORGFS_STAT_INC(PrefetchFetchesIssued);

        if (STATUS_PENDING != status)
        {
            BLORGFS_PRINT("PrefetchPump: issue failed: %8lx\n", status);

            BLORGFS_STAT_INC(PrefetchFetchesFailed);

            KeAcquireSpinLock(&Ring->Lock, &irql);
            PIRP waiter = Ring->Waiters[i];
            Ring->Waiters[i] = NULL;
            Ring->WaiterLengths[i] = 0;
            Ring->Hot[i].State = PrefetchSlotEmpty;
            KeReleaseSpinLock(&Ring->Lock, irql);

            if (waiter)
            {
                BlorgCompleteRequest(waiter, status, IO_DISK_INCREMENT);
            }

            PrefetchReleaseRef(Ring);
        }
    }
}

//
// PASSIVE-level work-item target for the completion-driven pump: re-tops
// the pipeline without waiting for the next paging read. Touches only
// ring-owned state (the pump runs off the ring's Path/FileSize
// snapshot), so it needs no synchronization with FCB teardown at all;
// the Detached check is an economy measure that stops a late worker
// from fetching ahead for a closed file, and racing it is harmless (see
// the field comment in Prefetch.h). PumpQueued is cleared before the
// Detached check, so a completion that lands mid-pump re-queues rather
// than being lost. The ring reference taken by PrefetchQueuePump keeps
// the ring alive throughout and is released last.
//
static IO_WORKITEM_ROUTINE PrefetchPumpWorker;

static VOID PrefetchPumpWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PREFETCH_RING* ring = Context;

    if (!ring)
    {
        return;
    }

    InterlockedExchange(&ring->PumpQueued, 0);

    KIRQL irql;
    KeAcquireSpinLock(&ring->Lock, &irql);
    BOOLEAN detached = ring->Detached;
    KeReleaseSpinLock(&ring->Lock, irql);

    if (!detached)
    {
        PrefetchPump(ring);
    }

    PrefetchReleaseRef(ring);
}

//
// Queues the ring's pump work item, deduplicated: at most one queued
// instance at a time (PumpQueued), re-armable the moment the worker
// starts. Callable at <= DISPATCH_LEVEL; the caller must itself hold a
// ring reference across this call (the queued work item then holds its
// own until the worker releases it).
//
static VOID PrefetchQueuePump(PREFETCH_RING* Ring)
{
    if (InterlockedCompareExchange(&Ring->PumpQueued, 1, 0))
    {
        return;
    }

    InterlockedIncrement(&Ring->RefCount);
    IoQueueWorkItem(Ring->PumpWorkItem, PrefetchPumpWorker, DelayedWorkQueue, Ring);
}

//
// Fetch-completion callback for a prefetch-issued HTTP range request, run at
// <= DISPATCH_LEVEL. If a paging read parked on this slot, copies the fetched
// bytes straight into its MDL and completes it (delivered regardless of
// generation, since the waiter parked on this exact range, so the data is
// right for it even if a concurrent seek re-aimed the ring in the
// meantime -- the buffer is detached from the slot for this copy, then
// donated back afterward for reuse); otherwise marks the slot Ready for a
// later hit, or discards it (fetch failed, or its data predates a seek --
// the next paging read either re-fetches the range directly or has
// re-aimed the pipeline already).
//
// A successful completion that leaves its slot Empty (waiter delivered,
// or a stale-generation discard) queues the PASSIVE pump work item so
// the freed slot is refilled now rather than on the next paging read --
// in the RTT-bound regime that keeps the pipeline genuinely full instead
// of always one slot short, and takes the top-up issue cost off the
// read path. Failed fetches never queue it: the next read's pump is the
// retry, so a dead server can't drive a fetch-fail-refetch storm. For
// the waiter case the queue happens only after the detached buffer has
// been donated back, so the pump can actually use the slot.
//
static VOID PrefetchFetchComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PREFETCH_FETCH_CTX* ctx = CallerContext;
    PREFETCH_RING* ring = ctx->Ring;
    ULONG i = ctx->SlotIndex;

    ULONG validBytes = (NT_SUCCESS(Status) && FileBuffer) ? C_CAST(ULONG, FileBuffer->BodyBufferSize) : 0;

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (statsBlock)
    {
        if (NT_SUCCESS(Status))
        {
            statsBlock->FetchBytes += validBytes;
            statsBlock->FetchesCompleted++;

            BlorgStatisticsRecordLatency(
                &statsBlock->FetchLatencySumUs,
                &statsBlock->FetchLatencyMaxUs,
                statsBlock->FetchLatencyBuckets,
                BlorgStatisticsNow() - ctx->IssueQpc);
        }
        else
        {
            statsBlock->PrefetchFetchesFailed++;
        }
    }

    KIRQL irql;
    KeAcquireSpinLock(&ring->Lock, &irql);

    PIRP waiter = ring->Waiters[i];
    ULONG waiterLength = ring->WaiterLengths[i];
    ULONG waiterSlotOffset = ring->WaiterSlotOffsets[i];
    ring->Waiters[i] = NULL;

    PCHAR buffer = NULL;
    BOOLEAN queuePump = FALSE;

    if (waiter)
    {
        buffer = ring->Buffers[i];
        ring->Buffers[i] = NULL;
        ring->Hot[i].State = PrefetchSlotEmpty;
        queuePump = NT_SUCCESS(Status) && !ring->Detached;
    }
    else if (NT_SUCCESS(Status) && ctx->Generation == ring->Generation)
    {
        ring->Hot[i].State = PrefetchSlotReady;
        ring->Hot[i].Length = validBytes;
    }
    else
    {
        BOOLEAN succeeded = NT_SUCCESS(Status);

        ring->Hot[i].State = PrefetchSlotEmpty;
        queuePump = succeeded && !ring->Detached;

        if (succeeded)
        {
            BLORGFS_STAT_INC(PrefetchStaleDiscards);
        }
    }

    KeReleaseSpinLock(&ring->Lock, irql);

    BLORGFS_PRINT("prefetch fill slot=%lu st=%8lx bytes=%lx waiter=%d\n",
        i, Status, validBytes, waiter ? 1 : 0);

    if (waiter)
    {
        NTSTATUS waiterStatus = Status;

        if (NT_SUCCESS(Status))
        {
            ULONG availableFromSlotOffset =
                (validBytes > waiterSlotOffset) ? (validBytes - waiterSlotOffset) : 0;

            ULONG copyLength = waiterLength;

            if (copyLength > availableFromSlotOffset)
            {
                copyLength = availableFromSlotOffset;
            }

            PVOID targetVa = MmGetSystemAddressForMdlSafe(waiter->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

            if (targetVa)
            {
                RtlCopyMemory(targetVa, buffer + waiterSlotOffset, copyLength);
                waiter->IoStatus.Information = copyLength;

                BLORGFS_STAT_ADD(PrefetchBytesServed, copyLength);
            }
            else
            {
                waiterStatus = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        else
        {
            BLORGFS_PRINT("PrefetchFetchComplete: fetch failed with parked read: %8lx\n", Status);
        }

        BlorgCompleteRequest(waiter, waiterStatus, IO_DISK_INCREMENT);

        KeAcquireSpinLock(&ring->Lock, &irql);
        ring->Buffers[i] = buffer;
        KeReleaseSpinLock(&ring->Lock, irql);
    }

    if (queuePump)
    {
        PrefetchQueuePump(ring);
    }

    PrefetchReleaseRef(ring);
}

//
// Finds the tracker whose last read ended exactly at Offset (this reader's
// own trail), or claims the coldest tracker for a new/seeked stream. One
// unconditional pass over the FCB's single-cache-line tracker array -- the
// match test and the coldest scan share the loop, no early exit.
//
static READ_STREAM_TRACKER* PrefetchClaimStream(FCB* Fcb, ULONG64 Offset)
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
// Counts a miss that a containment test would have served: some slot's
// range covers [Offset, Offset + Length) but the lookup in
// BlorgPrefetchServeRead rejected it, because that lookup demands
// Offset == RangeOffset exactly rather than mere coverage. Every near miss
// is a full HTTP round trip spent re-fetching bytes the ring already held
// or already had in flight, so this is the running cost of the exact match.
//
// Must run before the re-aim below, which drops Ready slots and would erase
// the evidence. Takes Ring->Lock itself rather than extending the caller's
// hold: this path is already committed to a network fetch, so an eight-slot
// scan under a leaf spinlock costs nothing by comparison.
//
static VOID PrefetchCountNearMiss(PREFETCH_RING* Ring, ULONG64 Offset, ULONG Length)
{
    KIRQL irql;
    KeAcquireSpinLock(&Ring->Lock, &irql);

    for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
    {
        if (PrefetchSlotEmpty == Ring->Hot[i].State ||
            Offset < Ring->Hot[i].RangeOffset ||
            (Offset - Ring->Hot[i].RangeOffset) + Length > Ring->Hot[i].Length)
        {
            continue;
        }

        BLORGFS_STAT_INC(PrefetchNearMisses);
        break;
    }

    KeReleaseSpinLock(&Ring->Lock, irql);
}

//
// Entry point for the sequential-read prefetcher on a paging read: updates
// this stream's tracker (READ_STREAM_TRACKER, Structs.h), arms a new ring
// once the stream's streak is long enough, serves/parks against an
// existing ring's hit/in-flight slots, and re-aims (bumps Generation,
// drops ready data) when a streaked stream misses a ring nothing else is
// consuming. Returns STATUS_NOT_FOUND when the caller must fall back to a
// direct fetch (no ring yet, no coverage for this range, or a second
// concurrent reader of an already-parked slot).
//
// Sequential detection is a one-line tracker scan on plain FCB fields,
// PASSIVE_LEVEL only (paged FCB); concurrent readers of one file can race
// these harmlessly -- a lost update only costs detection accuracy, never
// correctness. The FCB is paged, so the serve clock is captured into a
// local up front: everything examined under Ring->Lock (DISPATCH) is
// ring-resident or on the stack. A NULL ring on a failed allocation just
// means no prefetch for this stream -- reads degrade to direct fetches. A
// freshly created ring is published with a CAS: two concurrent readers of
// the same file can both see NULL and both build a ring, but exactly one
// attaches and the loser frees its unpublished copy (nothing else has
// seen it, so a plain free -- no refcount dance -- is correct). The chunk
// already in hand still goes out as a direct fetch regardless.
//
// On a hit, the buffer is detached so the chunk-sized copy runs outside
// the lock without the pump reissuing into it, then donated back and the
// pipeline topped back up. On an in-flight slot, parking means the fetch
// completion copies into the IRP's MDL and completes it at <= DISPATCH
// (same no-IoMarkIrpPending discipline as the existing inline async
// paging path); one waiter per slot, so a second concurrent read of the
// same range falls through to a direct fetch. A park also bumps
// DepthLimit (up to PREFETCH_DEPTH): the reader catching an in-flight
// fetch is the signal the pipeline is too shallow for this stream, and
// the next pump allocates the admitted slot -- a stream whose fetches
// always complete ahead of it (hits only) stays at PREFETCH_MIN_DEPTH
// and never spends the memory. Parking pumps BEFORE
// publishing the waiter, then re-takes the lock and re-checks the slot
// (served as a hit if the fetch completed in the gap, direct fetch if
// it was discarded). The order is a lifetime invariant, not a
// keep-full nicety: the instant the waiter is visible, the fetch
// completion can copy and complete the IRP on another CPU, after which
// MM can finish the in-page, the last handle can close, and the reap
// worker can free the FCB -- taking the ring's attachment reference
// (and possibly the last in-flight-fetch reference, hence the ring
// itself) with it. So nothing may touch Fcb or Ring after the park;
// while the pump runs, the un-parked IRP is what keeps both alive.
// IoMarkIrpPending is called under the lock immediately before the
// waiter is published, for the same reason and in the same window: this
// path returns STATUS_PENDING to a dispatch routine that never posted
// the IRP to the CSQ (paging reads bypass the FSP queue entirely, see
// BlorgVolumeRead) and never ran it through the oplock package, so
// nothing else marks it, and once the waiter is visible the IRP may
// already be completed and gone.
// Both consuming outcomes stamp LastConsumeClock, which is what gates
// the re-aim below. A miss from a streaked stream re-aims the pipeline only
// if the ring has gone idle per that clock (Prefetch.h's re-aim policy);
// a ring another stream is actively consuming is left alone and this
// stream's reads stay direct fetches. In-flight slots keep their buffers
// across a re-aim until their fetches complete (WSK is still writing
// them); bumping Generation makes those completions discard on arrival.
// Ready data from the old region is dropped immediately.
//
NTSTATUS BlorgPrefetchServeRead(FCB* Fcb, PIRP Irp, ULONG64 Offset, ULONG Length)
{
    READ_STREAM_TRACKER* stream = PrefetchClaimStream(Fcb, Offset);

    stream->Streak = (Offset == stream->End) ? stream->Streak + 1 : 1;
    stream->End = Offset + Length;

    ULONG64 serveClock = ++Fcb->StreamClock;
    ULONG64 streak = stream->Streak;

    PREFETCH_RING* ring = ReadPointerAcquire(C_CAST(PVOID volatile*, &Fcb->PrefetchRing));

    if (!ring)
    {
        if (streak < PREFETCH_ARM_STREAK)
        {
            return STATUS_NOT_FOUND;
        }

        ring = PrefetchCreateRing(Fcb);

        if (!ring)
        {
            return STATUS_NOT_FOUND;
        }

        ring->NextFetchOffset = Offset + Length;
        ring->LastConsumeClock = serveClock;

        if (NULL != InterlockedCompareExchangePointer(
                C_CAST(PVOID volatile*, &Fcb->PrefetchRing),
                ring,
                NULL))
        {
            PrefetchFreeRing(ring);
            return STATUS_NOT_FOUND;
        }

        PrefetchPump(ring);

        return STATUS_NOT_FOUND;
    }

    KIRQL irql;
    BOOLEAN pumped = FALSE;
    BOOLEAN rescan;

    do
    {
        rescan = FALSE;

        KeAcquireSpinLock(&ring->Lock, &irql);

        for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
        {
            if (PrefetchSlotEmpty == ring->Hot[i].State ||
                Offset < ring->Hot[i].RangeOffset ||
                (Offset - ring->Hot[i].RangeOffset) + Length > ring->Hot[i].Length)
            {
                continue;
            }

            ULONG slotOffset = C_CAST(ULONG, Offset - ring->Hot[i].RangeOffset);

            if (PrefetchSlotReady == ring->Hot[i].State)
            {
                PCHAR buffer = ring->Buffers[i];
                ring->Buffers[i] = NULL;
                ring->Hot[i].State = PrefetchSlotEmpty;
                ring->LastConsumeClock = serveClock;
                KeReleaseSpinLock(&ring->Lock, irql);

                NTSTATUS result;
                PVOID targetVa = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                if (targetVa)
                {
                    RtlCopyMemory(targetVa, buffer + slotOffset, Length);
                    Irp->IoStatus.Information = Length;
                    result = STATUS_SUCCESS;
                }
                else
                {
                    result = STATUS_INSUFFICIENT_RESOURCES;
                }

                KeAcquireSpinLock(&ring->Lock, &irql);
                ring->Buffers[i] = buffer;
                KeReleaseSpinLock(&ring->Lock, irql);

                BLORGFS_PRINT("prefetch hit off=%llx len=%lx\n", Offset, Length);

                BLORGFS_STAT_INC(PrefetchHits);
                BLORGFS_STAT_ADD(PrefetchBytesServed, (STATUS_SUCCESS == result) ? Length : 0);

                PrefetchPump(ring);
                return result;
            }

            if (!ring->Waiters[i])
            {
                if (pumped)
                {
                    if (ring->DepthLimit < PREFETCH_DEPTH)
                    {
                        ++ring->DepthLimit;
                        BLORGFS_STAT_INC(PrefetchDepthGrowths);
                    }

                    ring->WaiterLengths[i] = Length;
                    ring->WaiterSlotOffsets[i] = slotOffset;
                    IoMarkIrpPending(Irp);
                    ring->Waiters[i] = Irp;
                    ring->LastConsumeClock = serveClock;
                    KeReleaseSpinLock(&ring->Lock, irql);

                    BLORGFS_PRINT("prefetch park off=%llx len=%lx\n", Offset, Length);

                    BLORGFS_STAT_INC(PrefetchParks);

                    return STATUS_PENDING;
                }

                rescan = TRUE;
            }

            break;
        }

        KeReleaseSpinLock(&ring->Lock, irql);

        if (rescan)
        {
            PrefetchPump(ring);
            pumped = TRUE;
        }
    }
    while (rescan);

    PrefetchCountNearMiss(ring, Offset, Length);

    if (streak >= PREFETCH_ARM_STREAK)
    {
        BOOLEAN reaimed = FALSE;
        BOOLEAN suppressed = FALSE;

        KeAcquireSpinLock(&ring->Lock, &irql);

        ULONG64 windowBytes = C_CAST(ULONG64, ring->DepthLimit) * PREFETCH_CHUNK;
        ULONG64 windowStart = (ring->NextFetchOffset > windowBytes) ? (ring->NextFetchOffset - windowBytes) : 0;

        BOOLEAN readerWithinPipeline =
            (Offset + Length > windowStart) && (Offset < ring->NextFetchOffset);

        if (serveClock - ring->LastConsumeClock > PREFETCH_REAIM_IDLE_SERVES)
        {
            suppressed = readerWithinPipeline;

            if (!suppressed)
            {
                ++ring->Generation;

                for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
                {
                    if (PrefetchSlotReady == ring->Hot[i].State)
                    {
                        ring->Hot[i].State = PrefetchSlotEmpty;
                    }
                }

                ring->NextFetchOffset = Offset + Length;
                ring->LastConsumeClock = serveClock;
                reaimed = TRUE;
            }
        }

        KeReleaseSpinLock(&ring->Lock, irql);

        if (suppressed)
        {
            BLORGFS_STAT_INC(PrefetchReaimsSuppressed);

            PrefetchPump(ring);
        }

        if (reaimed)
        {
            BLORGFS_PRINT("prefetch miss+rearm off=%llx streak=%llu\n", Offset, streak);

            BLORGFS_STAT_INC(PrefetchReaims);

            PrefetchPump(ring);
        }
    }

    return STATUS_NOT_FOUND;
}

//
// Detaches and releases the FCB's prefetch ring at file-close/teardown time.
// Bumps Generation so any still-in-flight fetches discard their results on
// arrival instead of touching the (possibly freed) FCB; no waiters can exist
// since a parked waiter implies an in-flight paging read still holding the
// FCB open.
//
// Detached stops any later pump work item from fetching ahead for a file
// that no longer exists; it is not needed for lifetime safety -- the
// pump runs entirely off ring-owned state (Path/FileSize snapshot), and
// a queued work item holds its own ring reference, so nothing here has
// to wait for it.
//
VOID BlorgPrefetchDetach(FCB* Fcb)
{
    PREFETCH_RING* ring = Fcb->PrefetchRing;

    if (!ring)
    {
        return;
    }

    Fcb->PrefetchRing = NULL;

    KIRQL irql;
    KeAcquireSpinLock(&ring->Lock, &irql);
    ring->Detached = TRUE;
    ++ring->Generation;
    KeReleaseSpinLock(&ring->Lock, irql);

    PrefetchReleaseRef(ring);
}
