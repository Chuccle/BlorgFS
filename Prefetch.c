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
// Driver-wide count of live rings, incremented by PrefetchCreateRing and
// decremented by PrefetchFreeRing (so the CAS-loser free and every
// failure path stay symmetric with their create). Gates ring creation
// against PrefetchMaxRings -- Prefetch.h's arm-time-only budget.
//
static LONG PrefetchRingCount;

static VOID PrefetchFetchComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext);

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

    ExFreePool(Ring);

    InterlockedDecrement(&PrefetchRingCount);
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
static PREFETCH_RING* PrefetchCreateRing(VOID)
{
    if (PrefetchMaxRings() < InterlockedIncrement(&PrefetchRingCount))
    {
        InterlockedDecrement(&PrefetchRingCount);
        return NULL;
    }

    PREFETCH_RING* ring = ExAllocatePoolZero(NonPagedPoolNx, sizeof(PREFETCH_RING), PREFETCH_TAG);

    if (!ring)
    {
        InterlockedDecrement(&PrefetchRingCount);
        return NULL;
    }

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

    KeInitializeSpinLock(&ring->Lock);
    ring->DepthLimit = PREFETCH_MIN_DEPTH;
    ring->RefCount = 1;

    return ring;
}

//
// Top the pipeline back up: realize any pending depth growth, reserve
// every empty slot under the lock, then issue the fetches outside it
// (HttpBuildRequest touches paged code, so this must run at
// PASSIVE_LEVEL -- which it does, being called only from the paging-read
// path; never from a fetch completion). Depth growth first: slots
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
// (never the last ref -- the caller's FCB attachment is still held). A
// paging read may have parked on the reserved slot between the
// reservation and the failed issue, so the failure path drains
// Waiters[i] under the lock and completes the parked IRP with the issue
// status -- leaving it would strand the read forever, and a later reuse
// of the slot would complete it with data from a different range.
//
static VOID PrefetchPump(FCB* Fcb, PREFETCH_RING* Ring)
{
    ULONG64 fileSize = C_CAST(ULONG64, Fcb->Header.FileSize.QuadPart);
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

        InterlockedIncrement(&Ring->RefCount);
        Ring->NextFetchOffset += fetchLength;
        reserved[reservedCount++] = i;
    }

    KeReleaseSpinLock(&Ring->Lock, irql);

    for (ULONG r = 0; r < reservedCount; ++r)
    {
        ULONG i = reserved[r];

        NTSTATUS status = BlorgHttpGetFileMdl(
            &Fcb->FullPath,
            Ring->Hot[i].RangeOffset,
            Ring->Hot[i].Length,
            Ring->BufferMdls[i],
            PrefetchFetchComplete,
            &Ring->FetchCtx[i]);

        if (STATUS_PENDING != status)
        {
            BLORGFS_PRINT("PrefetchPump: issue failed: %8lx\n", status);

            KeAcquireSpinLock(&Ring->Lock, &irql);
            PIRP waiter = Ring->Waiters[i];
            Ring->Waiters[i] = NULL;
            Ring->Hot[i].State = PrefetchSlotEmpty;
            KeReleaseSpinLock(&Ring->Lock, irql);

            if (waiter)
            {
                CompleteRequest(waiter, status, IO_DISK_INCREMENT);
            }

            PrefetchReleaseRef(Ring);
        }
    }
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
static VOID PrefetchFetchComplete(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PREFETCH_FETCH_CTX* ctx = CallerContext;
    PREFETCH_RING* ring = ctx->Ring;
    ULONG i = ctx->SlotIndex;

    ULONG validBytes = (NT_SUCCESS(Status) && FileBuffer) ? C_CAST(ULONG, FileBuffer->BodyBufferSize) : 0;

    KIRQL irql;
    KeAcquireSpinLock(&ring->Lock, &irql);

    PIRP waiter = ring->Waiters[i];
    ring->Waiters[i] = NULL;

    PCHAR buffer = NULL;

    if (waiter)
    {
        buffer = ring->Buffers[i];
        ring->Buffers[i] = NULL;
        ring->Hot[i].State = PrefetchSlotEmpty;
    }
    else if (NT_SUCCESS(Status) && ctx->Generation == ring->Generation)
    {
        ring->Hot[i].State = PrefetchSlotReady;
        ring->Hot[i].Length = validBytes;
    }
    else
    {
        ring->Hot[i].State = PrefetchSlotEmpty;
    }

    KeReleaseSpinLock(&ring->Lock, irql);

    BLORGFS_PRINT("prefetch fill slot=%lu st=%8lx bytes=%lx waiter=%d\n",
        i, Status, validBytes, waiter ? 1 : 0);

    if (waiter)
    {
        NTSTATUS waiterStatus = Status;

        if (NT_SUCCESS(Status))
        {
            PIO_STACK_LOCATION waiterSp = IoGetCurrentIrpStackLocation(waiter);
            ULONG copyLength = waiterSp->Parameters.Read.Length;

            if (copyLength > validBytes)
            {
                copyLength = validBytes;
            }

            PVOID targetVa = MmGetSystemAddressForMdlSafe(waiter->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

            if (targetVa)
            {
                RtlCopyMemory(targetVa, buffer, copyLength);
                waiter->IoStatus.Information = copyLength;
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

        CompleteRequest(waiter, waiterStatus, IO_DISK_INCREMENT);

        KeAcquireSpinLock(&ring->Lock, &irql);
        ring->Buffers[i] = buffer;
        KeReleaseSpinLock(&ring->Lock, irql);
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

        ring = PrefetchCreateRing();

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

        PrefetchPump(Fcb, ring);

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
            if (ring->Hot[i].RangeOffset != Offset ||
                PrefetchSlotEmpty == ring->Hot[i].State ||
                Length > ring->Hot[i].Length)
            {
                continue;
            }

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
                    RtlCopyMemory(targetVa, buffer, Length);
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

                PrefetchPump(Fcb, ring);
                return result;
            }

            if (!ring->Waiters[i])
            {
                if (pumped)
                {
                    if (ring->DepthLimit < PREFETCH_DEPTH)
                    {
                        ++ring->DepthLimit;
                    }

                    ring->Waiters[i] = Irp;
                    ring->LastConsumeClock = serveClock;
                    KeReleaseSpinLock(&ring->Lock, irql);

                    BLORGFS_PRINT("prefetch park off=%llx len=%lx\n", Offset, Length);

                    return STATUS_PENDING;
                }

                rescan = TRUE;
            }

            break;
        }

        KeReleaseSpinLock(&ring->Lock, irql);

        if (rescan)
        {
            PrefetchPump(Fcb, ring);
            pumped = TRUE;
        }
    }
    while (rescan);

    if (streak >= PREFETCH_ARM_STREAK)
    {
        BOOLEAN reaimed = FALSE;

        KeAcquireSpinLock(&ring->Lock, &irql);

        if (serveClock - ring->LastConsumeClock > PREFETCH_REAIM_IDLE_SERVES)
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

        KeReleaseSpinLock(&ring->Lock, irql);

        if (reaimed)
        {
            BLORGFS_PRINT("prefetch miss+rearm off=%llx streak=%llu\n", Offset, streak);

            PrefetchPump(Fcb, ring);
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
    ++ring->Generation;
    KeReleaseSpinLock(&ring->Lock, irql);

    PrefetchReleaseRef(ring);
}
