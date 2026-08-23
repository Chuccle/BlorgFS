#include "Driver.h"

//
// Implements the per-FCB sequential-read prefetch ring: creating and
// freeing it, pumping fetches to keep it full, serving/parking paging
// reads against it, and its fetch-completion callback.
//
// See Prefetch.h for the design: why the ring exists, the IRQL/issuance
// rules (all fetch issuance at PASSIVE, completions at <= DISPATCH touch
// only the ring), the slot-owns-a-chunk rule that the driver-wide chunk
// pool rests on, and the RefCount/Generation lifetime rules.
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
// Driver-wide chunk budget by machine size, the same coarse
// MmQuerySystemSize tiering fastfat/ntfs use for cache sizing. The tiers
// are the same 8/16/32 MB of NonPagedPoolNx the old ring cap worked out
// to at full depth, but denominated in the thing that actually holds the
// memory, so the ceiling is now a real ceiling rather than a worst case
// that idle rings reserved and never reached.
//
LONG BlorgPrefetchMaxChunks(VOID)
{
    switch (MmQuerySystemSize())
    {
        case MmSmallSystem:
        {
            return 16;
        }
        case MmMediumSystem:
        {
            return 32;
        }
        default:
        {
            return 64;
        }
    }
}

//
// Driver-wide chunk pool: every chunk not currently owned by a slot.
//
// ChunkCount is chunks in existence, owned and free alike, and is what
// the budget caps -- releasing to the pool does not decrement it, since
// a pooled chunk is still committed NonPagedPoolNx. It only falls when
// BlorgPrefetchReleaseChunkPool actually returns memory to the system.
//
// The lock is a leaf below Ring->Lock, taken for a list push or pop and
// nothing else. Release runs from fetch completions at DISPATCH_LEVEL,
// which is why it may not allocate or free: it pushes and returns.
//
static SINGLE_LIST_ENTRY PrefetchChunkFreeList = { NULL };
static KSPIN_LOCK PrefetchChunkLock;
static volatile LONG PrefetchChunkCount;

//
// Takes a chunk for a slot about to reserve: a pooled one if there is
// one, otherwise a fresh allocation if the budget allows, otherwise
// NULL. NULL is an ordinary outcome, not an error -- the caller leaves
// the slot Empty and the ring simply runs shallower this round, which is
// the whole point of budgeting chunks instead of rings.
//
// PASSIVE_LEVEL only: the miss path allocates pool and builds an MDL.
// Every caller is the pump, which is always at PASSIVE.
//
// The budget is claimed by CAS before the allocation so two racing pumps
// cannot both see room and both allocate; a failed allocation gives the
// claim straight back.
//
_IRQL_requires_(PASSIVE_LEVEL)
static PREFETCH_CHUNK_BLOCK* PrefetchChunkAcquire(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&PrefetchChunkLock, &irql);
    PSINGLE_LIST_ENTRY entry = PopEntryList(&PrefetchChunkFreeList);
    KeReleaseSpinLock(&PrefetchChunkLock, irql);

    if (entry)
    {
        return CONTAINING_RECORD(entry, PREFETCH_CHUNK_BLOCK, Link);
    }

    LONG maxChunks = BlorgPrefetchMaxChunks();
    LONG current = ReadNoFence(&PrefetchChunkCount);

    while (current < maxChunks)
    {
        LONG previous = InterlockedCompareExchange(&PrefetchChunkCount, current + 1, current);

        if (previous == current)
        {
            break;
        }

        current = previous;
    }

    if (current >= maxChunks)
    {
        BLORGFS_STAT_INC(PrefetchChunkStarvations);
        return NULL;
    }

    PREFETCH_CHUNK_BLOCK* chunk = ExAllocatePoolZero(NonPagedPoolNx, sizeof(PREFETCH_CHUNK_BLOCK), PREFETCH_TAG);

    if (chunk)
    {
        chunk->Buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, PREFETCH_CHUNK, PREFETCH_TAG);

        if (chunk->Buffer)
        {
            chunk->Mdl = IoAllocateMdl(chunk->Buffer, PREFETCH_CHUNK, FALSE, FALSE, NULL);

            if (chunk->Mdl)
            {
                MmBuildMdlForNonPagedPool(chunk->Mdl);

                return chunk;
            }

            ExFreePool(chunk->Buffer);
        }

        ExFreePool(chunk);
    }

    InterlockedDecrement(&PrefetchChunkCount);
    return NULL;
}

//
// Hands a drained chunk back for the next reservation on any ring. Runs
// at <= DISPATCH_LEVEL (fetch completions release here), so it pushes
// and returns without touching the pool allocator.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID PrefetchChunkRelease(PREFETCH_CHUNK_BLOCK* Chunk)
{
    KIRQL irql;
    KeAcquireSpinLock(&PrefetchChunkLock, &irql);
    PushEntryList(&PrefetchChunkFreeList, &Chunk->Link);
    KeReleaseSpinLock(&PrefetchChunkLock, irql);
}

//
// Returns every pooled chunk to the system.
//
// Called from BlorgPrefetchDrain once the last ring is gone, so by
// definition no slot owns a chunk and the free list holds all of them.
// Safe to call at any other quiescent moment too -- it takes only what is
// on the free list, never what a slot owns -- which is what lets the
// sandbox reclaim the pool between tests instead of reporting recycled
// chunks as leaked.
//
VOID BlorgPrefetchReleaseChunkPool(VOID)
{
    for (;;)
    {
        KIRQL irql;
        KeAcquireSpinLock(&PrefetchChunkLock, &irql);
        PSINGLE_LIST_ENTRY entry = PopEntryList(&PrefetchChunkFreeList);
        KeReleaseSpinLock(&PrefetchChunkLock, irql);

        if (!entry)
        {
            break;
        }

        PREFETCH_CHUNK_BLOCK* chunk = CONTAINING_RECORD(entry, PREFETCH_CHUNK_BLOCK, Link);

        IoFreeMdl(chunk->Mdl);
        ExFreePool(chunk->Buffer);
        ExFreePool(chunk);

        InterlockedDecrement(&PrefetchChunkCount);
    }
}

//
// Chunks in existence, owned and pooled alike -- the driver's prefetch
// transfer footprint, for the statistics snapshot.
//
// Read off the budget counter rather than maintained as a separate gauge.
// The two would be counting the same quantity by two mechanisms, which is
// a thing to keep in sync at every allocation and every free and a thing
// to be wrong about exactly when the number matters.
//
LONG BlorgPrefetchChunksLive(VOID)
{
    return ReadNoFence(&PrefetchChunkCount);
}

//
// Hands back any chunks the ring's slots still own, then frees the ring
// itself and drops its reference. Called once RefCount has dropped to
// zero -- no fetches or FCB attachment remain -- or for a ring that was
// never published (creation failure, CAS loser).
//
// Slots still holding chunks here are the Ready ones: data fetched that
// the reader closed the file before consuming. Those go back to the pool
// for another stream, which is the reclamation the old ring-budget model
// had no way to express.
//
static VOID PrefetchFreeRing(PREFETCH_RING* Ring)
{
    for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
    {
        if (Ring->Chunks[i])
        {
            PrefetchChunkRelease(Ring->Chunks[i]);
            Ring->Chunks[i] = NULL;
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
    BLORGFS_STAT_INC(PrefetchRingsFreed);

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
// Claims a reference for a new ring. This is now purely the unload gate:
// a zero count means BlorgPrefetchDrain has released the standing
// reference and the last ring is gone, and refusing here is what stops a
// late arm from lifting the count back off zero after the drain has been
// signalled.
//
// There is deliberately no cap on rings any more. Capping them capped the
// wrong thing -- an armed ring is a few hundred bytes of struct, and the
// megabytes are in the chunks, which PrefetchChunkAcquire budgets
// directly. Refusing a ring cost a stream its entire pipeline; refusing a
// chunk costs it one slot of depth, so every stream arms and pressure is
// shared out instead of falling entirely on whoever arrived last.
//
// The CAS loop (rather than increment-then-test) is what keeps the count
// from transiently reading as non-zero after the drain: an increment that
// had to be undone would let a concurrent drain observe a count it never
// legitimately had.
//
static BOOLEAN PrefetchAcquireRing(VOID)
{
    LONG current = ReadNoFence(&PrefetchRingCount);

    while (0 != current)
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
    KeInitializeSpinLock(&PrefetchChunkLock);
}

//
// Releases the standing reference and waits for the last ring to go. A
// ring outlives its FCB by however long its in-flight fetches take, so
// without this a dismount-then-unload can race a fetch whose completion
// routine lives in this image. Bounded by the socket watchdogs, so a dead
// peer cannot hang unload indefinitely.
//
// The chunk pool is freed only after that wait returns. Every chunk is
// owned by a slot or sitting on the free list, and the last ring's
// teardown is what releases its slots' chunks, so waiting first is what
// makes "the free list holds all of them" true.
//
VOID BlorgPrefetchDrain(VOID)
{
    PrefetchReleaseRing();

    KeWaitForSingleObject(&PrefetchDrainEvent, Executive, KernelMode, FALSE, NULL);

    BlorgPrefetchReleaseChunkPool();
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
// own Path/FileSize snapshot, never the FCB.
//
// Chunks are taken before the reservation lock, because acquiring one
// may allocate and the reservation runs at DISPATCH under the ring's
// spin lock. The count is sized by a first pass -- empty chunkless slots,
// clipped to the chunks the file has left to fetch, so the tail of a file
// never takes chunks it cannot use -- and anything the second pass does
// not place goes straight back to the pool. Between the two passes
// another pump may have moved NextFetchOffset, which is exactly why the
// reservation re-tests rather than trusting the first pass's count.
//
// Coming up short is normal and is the designed behaviour under memory
// pressure: unplaced slots stay Empty, the ring runs shallower this
// round, and the next pump tries again. Nothing is refused outright.
//
// Once reserved, Hot[i] is safe to read without the lock: an
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
// of the slot would complete it with data from a different range. The
// failed slot's chunk goes back to the pool with it, since an Empty slot
// owns nothing.
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

    ULONG wanted = 0;

    KeAcquireSpinLock(&Ring->Lock, &irql);

    for (ULONG i = 0; i < depthLimit; ++i)
    {
        if (PrefetchSlotEmpty == Ring->Hot[i].State && !Ring->Chunks[i])
        {
            ++wanted;
        }
    }

    ULONG64 unfetched = (Ring->NextFetchOffset < fileSize) ? (fileSize - Ring->NextFetchOffset) : 0;

    KeReleaseSpinLock(&Ring->Lock, irql);

    ULONG64 chunksLeftInFile = (unfetched + PREFETCH_CHUNK - 1) / PREFETCH_CHUNK;

    if (wanted > chunksLeftInFile)
    {
        wanted = C_CAST(ULONG, chunksLeftInFile);
    }

    PREFETCH_CHUNK_BLOCK* taken[PREFETCH_DEPTH];
    ULONG takenCount = 0;

    while (takenCount < wanted)
    {
        PREFETCH_CHUNK_BLOCK* chunk = PrefetchChunkAcquire();

        if (!chunk)
        {
            break;
        }

        taken[takenCount++] = chunk;
    }

    ULONG nextTaken = 0;

    KeAcquireSpinLock(&Ring->Lock, &irql);

    for (ULONG i = 0; i < depthLimit && Ring->NextFetchOffset < fileSize; ++i)
    {
        if (PrefetchSlotEmpty != Ring->Hot[i].State)
        {
            continue;
        }

        if (!Ring->Chunks[i])
        {
            if (nextTaken == takenCount)
            {
                continue;
            }

            Ring->Chunks[i] = taken[nextTaken++];
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

    while (nextTaken < takenCount)
    {
        PrefetchChunkRelease(taken[nextTaken++]);
    }

    for (ULONG r = 0; r < reservedCount; ++r)
    {
        ULONG i = reserved[r];

        BlorgStatisticsGaugeIncrement(
            &BlorgStatisticsGauges.FetchesActive,
            &BlorgStatisticsGauges.FetchesActivePeak);

        NTSTATUS status = BlorgHttpGetFileMdl(
            &Ring->Path,
            Ring->Hot[i].RangeOffset,
            Ring->Hot[i].Length,
            Ring->Chunks[i]->Mdl,
            PrefetchFetchComplete,
            &Ring->FetchCtx[i]);

        BLORGFS_STAT_INC(PrefetchFetchesIssued);

        if (STATUS_PENDING != status)
        {
            BLORGFS_PRINT("PrefetchPump: issue failed: %8lx\n", status);

            BLORGFS_STAT_INC(PrefetchFetchesFailed);

            BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.FetchesActive);

            KeAcquireSpinLock(&Ring->Lock, &irql);
            PIRP waiter = Ring->Waiters[i];
            Ring->Waiters[i] = NULL;
            Ring->WaiterLengths[i] = 0;
            Ring->Hot[i].State = PrefetchSlotEmpty;
            PREFETCH_CHUNK_BLOCK* chunk = Ring->Chunks[i];
            Ring->Chunks[i] = NULL;
            KeReleaseSpinLock(&Ring->Lock, irql);

            if (chunk)
            {
                PrefetchChunkRelease(chunk);
            }

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
// meantime -- the chunk leaves the slot for this copy and then goes back
// to the shared pool, not back to the slot, since the slot is now Empty
// and an Empty slot owns nothing); otherwise marks the slot Ready for a
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
// retry, so a dead server can't drive a fetch-fail-refetch storm. The
// chunk is released before the pump is queued, so the chunk this slot
// just finished with is available to the pump that refills it.
//
static VOID PrefetchFetchComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PREFETCH_FETCH_CTX* ctx = CallerContext;
    PREFETCH_RING* ring = ctx->Ring;
    ULONG i = ctx->SlotIndex;

    ULONG validBytes = (NT_SUCCESS(Status) && FileBuffer) ? C_CAST(ULONG, FileBuffer->BodyBufferSize) : 0;

    BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.FetchesActive);

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

    PREFETCH_CHUNK_BLOCK* chunk = NULL;
    PCHAR buffer = NULL;
    BOOLEAN queuePump = FALSE;

    if (waiter)
    {
        chunk = ring->Chunks[i];
        buffer = chunk ? chunk->Buffer : NULL;
        ring->Chunks[i] = NULL;
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

        chunk = ring->Chunks[i];
        ring->Chunks[i] = NULL;
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

            if (targetVa && buffer)
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
    }

    if (chunk)
    {
        PrefetchChunkRelease(chunk);
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
// The ring's containment test, in the one place both scanners share it:
// TRUE when slot Index holds every byte of [Offset, Offset + Length), so
// the read can be served from SlotOffsetOut bytes into that slot's buffer.
// Callers hold Ring->Lock -- State, RangeOffset and Length are all written
// under it.
//
// Written to be overflow-proof rather than merely correct for sane inputs,
// because this predicate is what stands between a bad offset and a copy out
// of a slot buffer. The obvious spelling,
// (Offset - RangeOffset) + Length > Hot.Length, has two ways to wrap: the
// subtraction when Offset is below RangeOffset, and the addition when the
// difference is large. Both wrap to a small value that passes the test, and
// the hit path then copies from buffer + (ULONG)(Offset - RangeOffset) --
// a kernel out-of-bounds read at an attacker-influenced displacement.
// Ordering the checks so the subtraction only runs after Offset is known
// to be the larger, and comparing against the remaining slot bytes instead
// of summing, leaves no expression that can wrap for any input at all. That
// matters because the offsets reaching here are only as trustworthy as
// whoever built the IRP: Read.c rejects negative offsets at dispatch, but
// the safety of this copy must not depend on that check having run.
//
static BOOLEAN PrefetchSlotCovers(PREFETCH_RING* Ring, ULONG Index, ULONG64 Offset, ULONG Length, PULONG SlotOffsetOut)
{
    if (PrefetchSlotEmpty == Ring->Hot[Index].State || Offset < Ring->Hot[Index].RangeOffset)
    {
        return FALSE;
    }

    ULONG64 slotOffset = Offset - Ring->Hot[Index].RangeOffset;

    if (slotOffset > Ring->Hot[Index].Length || Length > Ring->Hot[Index].Length - slotOffset)
    {
        return FALSE;
    }

    *SlotOffsetOut = C_CAST(ULONG, slotOffset);

    return TRUE;
}

//
// Counts a miss that some slot's range did cover, which since the lookup
// became a containment test no longer means what the name suggests.
//
// It used to mean "a containment test would have served this and the
// exact-offset one did not", and it measured the running cost of exact
// matching. That case cannot occur any more: this scan and the serve scan
// call the same PrefetchSlotCovers, so anything covered here was covered
// there too. What is left is the reasons the serve path can decline a slot
// it matched, and there is essentially one -- the slot is still in flight
// and another reader is already parked on it. One waiter per slot, so the
// second concurrent reader of the same range falls through to a direct
// fetch.
//
// So this now measures the cost of that restriction, which is exactly the
// multi-stream case worth watching: two readers on one file (a video and
// its subtitle track) chasing the same chunk, where the second pays a full
// round trip for bytes already on the wire. A small remainder is race
// rather than contention -- the serve loop drops the lock before this
// runs, so a fetch can land in the gap and make a slot Ready that was not.
//
// Read it against PrefetchParks rather than PrefetchMisses: a high ratio
// says slots are contended, not that coverage is poor.
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
        ULONG slotOffset;

        if (!PrefetchSlotCovers(Ring, i, Offset, Length, &slotOffset))
        {
            continue;
        }

        BLORGFS_STAT_INC(PrefetchNearMisses);
        break;
    }

    //
    // A read that STARTS inside a Ready slot but runs past its end. The
    // containment test can never serve one from a single slot, so it
    // misses and fetches directly -- while the bytes it wanted sit split
    // across two slots the ring already holds, and the tail of the first
    // slot goes unread.
    //
    // Counted separately from PrefetchNearMisses because it is a different
    // defect with a different fix: near-misses are contention, these are
    // coverage the lookup cannot express.
    //
    for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
    {
        if (PrefetchSlotReady != Ring->Hot[i].State)
        {
            continue;
        }

        ULONG64 slotStart = Ring->Hot[i].RangeOffset;
        ULONG64 slotEnd = slotStart + Ring->Hot[i].Length;

        if (Offset >= slotStart && Offset < slotEnd && (Offset + Length) > slotEnd)
        {
            BLORGFS_STAT_INC(PrefetchStraddleMisses);
            break;
        }
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
            ULONG slotOffset;

            if (!PrefetchSlotCovers(ring, i, Offset, Length, &slotOffset))
            {
                continue;
            }

            if (PrefetchSlotReady == ring->Hot[i].State)
            {
                PREFETCH_CHUNK_BLOCK* chunk = ring->Chunks[i];
                ULONG generation = ring->Generation;
                ULONG64 slotBase = ring->Hot[i].RangeOffset;
                ULONG slotLength = ring->Hot[i].Length;

                BOOLEAN exhausted = (slotOffset + Length >= slotLength);

                ring->Chunks[i] = NULL;
                ring->Hot[i].State = PrefetchSlotEmpty;
                ring->LastConsumeClock = serveClock;
                KeReleaseSpinLock(&ring->Lock, irql);

                NTSTATUS result;
                PVOID targetVa = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                if (targetVa && chunk)
                {
                    RtlCopyMemory(targetVa, chunk->Buffer + slotOffset, Length);
                    Irp->IoStatus.Information = Length;
                    result = STATUS_SUCCESS;
                }
                else
                {
                    result = STATUS_INSUFFICIENT_RESOURCES;
                }

                if (chunk)
                {
                    BOOLEAN reinstated = FALSE;

                    if (!exhausted)
                    {
                        KeAcquireSpinLock(&ring->Lock, &irql);

                        if (PrefetchSlotEmpty == ring->Hot[i].State &&
                            !ring->Chunks[i] &&
                            generation == ring->Generation)
                        {
                            ring->Hot[i].RangeOffset = slotBase;
                            ring->Hot[i].Length = slotLength;
                            ring->Hot[i].State = PrefetchSlotReady;
                            ring->Chunks[i] = chunk;
                            reinstated = TRUE;
                        }

                        KeReleaseSpinLock(&ring->Lock, irql);

                        BLORGFS_STAT_INC(PrefetchPartialServes);
                    }

                    if (!reinstated)
                    {
                        PrefetchChunkRelease(chunk);
                    }
                }

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
        PREFETCH_CHUNK_BLOCK* reclaimed[PREFETCH_DEPTH];
        ULONG reclaimedCount = 0;

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

                        if (ring->Chunks[i])
                        {
                            reclaimed[reclaimedCount++] = ring->Chunks[i];
                            ring->Chunks[i] = NULL;
                        }
                    }
                }

                ring->NextFetchOffset = Offset + Length;
                ring->LastConsumeClock = serveClock;
                reaimed = TRUE;
            }
        }

        KeReleaseSpinLock(&ring->Lock, irql);

        for (ULONG r = 0; r < reclaimedCount; ++r)
        {
            PrefetchChunkRelease(reclaimed[r]);
        }

        BLORGFS_STAT_ADD(PrefetchReaimDiscardedChunks, reclaimedCount);

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

    BLORGFS_STAT_INC(PrefetchRingsDetached);

    KIRQL irql;
    KeAcquireSpinLock(&ring->Lock, &irql);
    ring->Detached = TRUE;
    ++ring->Generation;
    KeReleaseSpinLock(&ring->Lock, irql);

    PrefetchReleaseRef(ring);
}
