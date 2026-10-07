#include "Driver.h"

//
// The disk cache's index: which slot of the cache file holds which block,
// which slot to give up for the next one, and which blocks of a partly held
// read to fetch rather than serve. No I/O; DiskCache.c does that, and the
// sandbox drives this directly.
//
// A slot is found by hashing its key into a chain. Every state change
// happens under the one spin lock, held for a chain walk or a short clock
// sweep at most, because reads pin from the paging path and fills are
// admitted from fetch completions at DISPATCH_LEVEL.
//
// Replacement is a clock over the slots: the hand passes over a slot that
// was served since it last came by, clearing the mark, and takes the first
// that was not. A pinned slot is never taken, which is what makes it safe
// to read from or write to the cache file without holding the lock: the
// bytes behind a pinned slot are its key's until the pin is dropped. A
// newly filled slot starts unmarked, so a block read once more after it was
// written survives a pass and one never read again does not. The hand looks
// at DISK_CACHE_CLOCK_REACH slots at most for one victim, so the time the
// lock is held does not grow with the cache; when every slot in that reach
// was marked, it takes the first unpinned one, marked or not.
//
// Admission waits for a block's second miss. The first only records the
// block's tag in a small set-associative ghost table, newest first, oldest
// falling out; the block is written when it is fetched again while its tag
// is still there. A file read once, which is most of what a copy or a scan
// touches, then costs the disk nothing, and it cannot push out what is read
// repeatedly.
//

#define DISK_CACHE_INDEX_TAG 'iDPB'
#define DISK_CACHE_GOLDEN    0x9E3779B97F4A7C15ull

//
// Mixes every field of Key, so the high half picks a chain and the low half
// a ghost set independently of it.
//
static ULONG64 DiskCacheIndexMix(const DISK_CACHE_KEY* Key)
{
    ULONG64 hash = Key->File[0];

    hash = (hash ^ Key->File[1]) * DISK_CACHE_GOLDEN;
    hash = (hash ^ Key->Size) * DISK_CACHE_GOLDEN;
    hash = (hash ^ Key->ModifiedTime) * DISK_CACHE_GOLDEN;
    hash = (hash ^ Key->Block) * DISK_CACHE_GOLDEN;

    return hash ^ (hash >> 29);
}

static PULONG DiskCacheIndexHead(PDISK_CACHE_INDEX Index, ULONG64 Mix)
{
    return &Index->Heads[C_CAST(ULONG, Mix >> 32) & Index->HashMask];
}

// The slot holding or filling Key, or DISK_CACHE_NO_SLOT. Under Lock.
static ULONG DiskCacheIndexFind(PDISK_CACHE_INDEX Index, const DISK_CACHE_KEY* Key, ULONG64 Mix)
{
    for (ULONG slot = *DiskCacheIndexHead(Index, Mix); DISK_CACHE_NO_SLOT != slot; slot = Index->Slots[slot].Next)
    {
        if (RtlEqualMemory(&Index->Slots[slot].Key, Key, sizeof(DISK_CACHE_KEY)))
        {
            return slot;
        }
    }

    return DISK_CACHE_NO_SLOT;
}

// Takes Slot out of its key's chain. Under Lock, and only for a slot in one.
static VOID DiskCacheIndexUnlink(PDISK_CACHE_INDEX Index, ULONG Slot)
{
    PULONG link = DiskCacheIndexHead(Index, DiskCacheIndexMix(&Index->Slots[Slot].Key));

    while (Slot != *link)
    {
        NT_ASSERT(DISK_CACHE_NO_SLOT != *link);
        link = &Index->Slots[*link].Next;
    }

    *link = Index->Slots[Slot].Next;
    Index->Slots[Slot].Next = DISK_CACHE_NO_SLOT;
}

//
// The next slot the clock gives up: the first unpinned slot that is free
// or was not served since the hand last passed it, within
// DISK_CACHE_CLOCK_REACH slots of the hand, or else the first unpinned slot
// the hand passed on the way, its mark already cleared. Fails only when
// every slot in reach is pinned. Under Lock.
//
static ULONG DiskCacheIndexVictim(PDISK_CACHE_INDEX Index)
{
    ULONG fallback = DISK_CACHE_NO_SLOT;
    const ULONG reach = min(Index->SlotCount, DISK_CACHE_CLOCK_REACH);

    for (ULONG looked = 0; looked < reach; ++looked)
    {
        const ULONG slot = Index->Hand;
        PDISK_CACHE_SLOT entry = &Index->Slots[slot];

        Index->Hand = (slot + 1 == Index->SlotCount) ? 0 : slot + 1;

        if (0 != entry->Pins)
        {
            continue;
        }

        if (DiskCacheSlotFree != entry->State && entry->Referenced)
        {
            entry->Referenced = FALSE;
            fallback = (DISK_CACHE_NO_SLOT == fallback) ? slot : fallback;
            continue;
        }

        return slot;
    }

    return fallback;
}

NTSTATUS BlorgDiskCacheIndexInitialize(PDISK_CACHE_INDEX Index, ULONG SlotCount)
{
    RtlZeroMemory(Index, sizeof(DISK_CACHE_INDEX));
    KeInitializeSpinLock(&Index->Lock);

    if (0 == SlotCount || SlotCount > (DISK_CACHE_MAX_MB << (20 - DISK_CACHE_BLOCK_SHIFT)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG heads = 1;

    while (heads < SlotCount)
    {
        heads <<= 1;
    }

    Index->Slots = ExAllocatePoolZero(NonPagedPoolNx, C_CAST(SIZE_T, SlotCount) * sizeof(DISK_CACHE_SLOT), DISK_CACHE_INDEX_TAG);
    Index->Heads = ExAllocatePoolZero(NonPagedPoolNx, C_CAST(SIZE_T, heads) * sizeof(ULONG), DISK_CACHE_INDEX_TAG);
    const ULONG ghostSets = (heads > 1) ? heads / 2 : 1;

    Index->Ghosts = ExAllocatePoolZero(NonPagedPoolNx, C_CAST(SIZE_T, ghostSets) * DISK_CACHE_GHOST_WAYS * sizeof(ULONG64), DISK_CACHE_INDEX_TAG);

    if (!Index->Slots || !Index->Heads || !Index->Ghosts)
    {
        BlorgDiskCacheIndexCleanup(Index);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < SlotCount; ++i)
    {
        Index->Slots[i].Next = DISK_CACHE_NO_SLOT;
    }

    for (ULONG i = 0; i < heads; ++i)
    {
        Index->Heads[i] = DISK_CACHE_NO_SLOT;
    }

    Index->SlotCount = SlotCount;
    Index->HashMask = heads - 1;
    Index->GhostMask = ghostSets - 1;

    return STATUS_SUCCESS;
}

VOID BlorgDiskCacheIndexCleanup(PDISK_CACHE_INDEX Index)
{
    if (Index->Slots)
    {
        ExFreePool(Index->Slots);
    }

    if (Index->Heads)
    {
        ExFreePool(Index->Heads);
    }

    if (Index->Ghosts)
    {
        ExFreePool(Index->Ghosts);
    }

    Index->Slots = NULL;
    Index->Heads = NULL;
    Index->Ghosts = NULL;
    Index->SlotCount = 0;
}

ULONG BlorgDiskCacheIndexPinHeld(PDISK_CACHE_INDEX Index, const DISK_CACHE_KEY* Key, ULONG64 LastBlock, PULONG Slots)
{
    DISK_CACHE_KEY key = *Key;
    ULONG pinned = 0;

    KIRQL oldIrql;
    KeAcquireSpinLock(&Index->Lock, &oldIrql);

    for (ULONG i = 0; key.Block <= LastBlock; ++key.Block, ++i)
    {
        const ULONG slot = DiskCacheIndexFind(Index, &key, DiskCacheIndexMix(&key));

        if (DISK_CACHE_NO_SLOT == slot || DiskCacheSlotValid != Index->Slots[slot].State)
        {
            Slots[i] = DISK_CACHE_NO_SLOT;
            continue;
        }

        Index->Slots[slot].Pins++;
        Slots[i] = slot;
        pinned++;
    }

    KeReleaseSpinLock(&Index->Lock, oldIrql);

    return pinned;
}

VOID BlorgDiskCacheIndexUnpin(PDISK_CACHE_INDEX Index, ULONG Slot, BOOLEAN Served)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Index->Lock, &oldIrql);

    NT_ASSERT(0 < Index->Slots[Slot].Pins);
    Index->Slots[Slot].Pins--;

    if (Served)
    {
        Index->Slots[Slot].Referenced = TRUE;
    }

    KeReleaseSpinLock(&Index->Lock, oldIrql);
}

ULONG BlorgDiskCacheIndexPlanRead(PDISK_CACHE_INDEX Index, PULONG Slots, ULONG Count, ULONG Held)
{
    for (;;)
    {
        ULONG runs = 0;
        ULONG gapStart = 0;
        ULONG gapLength = MAXULONG;

        for (ULONG i = 0; i < Count; ++i)
        {
            if (DISK_CACHE_NO_SLOT != Slots[i] || (0 != i && DISK_CACHE_NO_SLOT == Slots[i - 1]))
            {
                continue;
            }

            if (0 != runs)
            {
                ULONG start = i;

                while (0 < start && DISK_CACHE_NO_SLOT != Slots[start - 1])
                {
                    start--;
                }

                if (i - start < gapLength)
                {
                    gapStart = start;
                    gapLength = i - start;
                }
            }

            runs++;
        }

        if (runs <= DISK_CACHE_MAX_READ_FETCHES)
        {
            return Held;
        }

        for (ULONG i = gapStart; i < gapStart + gapLength; ++i)
        {
            BlorgDiskCacheIndexUnpin(Index, Slots[i], FALSE);
            Slots[i] = DISK_CACHE_NO_SLOT;
            Held--;
        }
    }
}

DISK_CACHE_ADMIT BlorgDiskCacheIndexReserve(PDISK_CACHE_INDEX Index, const DISK_CACHE_KEY* Key, PULONG Slot)
{
    const ULONG64 mix = DiskCacheIndexMix(Key);
    const ULONG64 tag = mix | 1;
    DISK_CACHE_ADMIT admit;

    KIRQL oldIrql;
    KeAcquireSpinLock(&Index->Lock, &oldIrql);

    PULONG64 ghosts = &Index->Ghosts[C_CAST(SIZE_T, C_CAST(ULONG, mix) & Index->GhostMask) * DISK_CACHE_GHOST_WAYS];
    ULONG way = 0;

    while (way < DISK_CACHE_GHOST_WAYS && tag != ghosts[way])
    {
        way++;
    }

    if (DISK_CACHE_NO_SLOT != DiskCacheIndexFind(Index, Key, mix))
    {
        admit = DiskCacheHeld;
    }
    else if (DISK_CACHE_GHOST_WAYS == way)
    {
        RtlMoveMemory(&ghosts[1], &ghosts[0], (DISK_CACHE_GHOST_WAYS - 1) * sizeof(ULONG64));
        ghosts[0] = tag;
        admit = DiskCacheFirstMiss;
    }
    else
    {
        const ULONG slot = DiskCacheIndexVictim(Index);

        if (DISK_CACHE_NO_SLOT == slot)
        {
            admit = DiskCacheNoVictim;
        }
        else
        {
            PDISK_CACHE_SLOT entry = &Index->Slots[slot];
            PULONG head = DiskCacheIndexHead(Index, mix);

            if (DiskCacheSlotFree != entry->State)
            {
                DiskCacheIndexUnlink(Index, slot);
            }

            entry->Key = *Key;
            entry->State = DiskCacheSlotFilling;
            entry->Referenced = FALSE;
            entry->Pins = 1;
            entry->Next = *head;
            *head = slot;

            ghosts[way] = 0;
            *Slot = slot;
            admit = DiskCacheReserved;
        }
    }

    KeReleaseSpinLock(&Index->Lock, oldIrql);

    return admit;
}

VOID BlorgDiskCacheIndexCommit(PDISK_CACHE_INDEX Index, ULONG Slot, BOOLEAN Written)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Index->Lock, &oldIrql);

    PDISK_CACHE_SLOT entry = &Index->Slots[Slot];

    NT_ASSERT(DiskCacheSlotFilling == entry->State);
    NT_ASSERT(0 < entry->Pins);

    if (Written)
    {
        entry->State = DiskCacheSlotValid;
    }
    else
    {
        DiskCacheIndexUnlink(Index, Slot);
        entry->State = DiskCacheSlotFree;
    }

    entry->Pins--;

    KeReleaseSpinLock(&Index->Lock, oldIrql);
}
