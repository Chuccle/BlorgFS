#include "Driver.h"

//
// Ghost cache: replays every fetch against a set-associative table of the
// 64 KB blocks recent fetches covered, and counts the fetches an on-disk
// block cache would have served. See GhostCache.h for why it exists and
// Statistics.h (Ghost*) for what it reports.
//
// The model, and the ways it is deliberately pessimistic
// ---------------------------------------------------------------------
// A block is identified by (path, file size, last-modified time, block
// index), so a file that changes on the backend stops matching, which is
// what a real cache's validator would do. Each entry keeps the global
// block clock at its last fetch; the clock advances once per block
// fetched, so the gap is how many blocks have been fetched since.
//
// That gap stands in for LRU stack distance, which is the number of
// DISTINCT blocks touched since, and it can only be larger. A fetch is
// counted as a hit for a capacity when every block it needs is held and
// the largest gap fits in that capacity, so a hit reported here is a hit
// an LRU cache of that size would have had, and some real hits are
// reported as misses. Two more things push the same direction: a block
// only counts as held once some fetch covered all of it, since a real
// cache admits whole blocks; and an entry pushed out of a full table set
// loses its history, which reads as a miss later.
//
// Two admission policies are modelled side by side, because the choice
// trades hit rate against SSD wear: admit on the first miss (every fetched
// block is written) and admit on the second (a block is written only once
// it has been fetched twice, so a file read once costs no writes at all).
// GhostAdmitBytes is what each would have written.
//
// Concurrency
// ---------------------------------------------------------------------
// None, on purpose. Fetches on different processors race on the table and
// the clock with plain loads and stores, exactly as ReadTrackStream races
// on the stream trackers. The worst outcome is a torn entry or a lost
// clock tick, which misclassifies one fetch; every table index is derived
// from the caller's own key and never from table contents, so no race can
// reach outside the allocation. The clock is the one shared written line,
// and it is written once per 64 KB fetched rather than once per read.
//

#define GHOST_CACHE_TAG          'hGPB'
#define GHOST_CACHE_BLOCK_SHIFT  16
#define GHOST_CACHE_BLOCK_SIZE   (1ULL << GHOST_CACHE_BLOCK_SHIFT)
#define GHOST_CACHE_WAYS         4
#define GHOST_CACHE_TOUCH_MAX    3
#define GHOST_CACHE_MAX_TABLE_MB 256

//
// One gigabyte, in blocks: the first distance bucket's upper bound. Each
// later bucket is four times the one before (Statistics.h).
//
#define GHOST_CACHE_GB_BLOCKS    (1ULL << (30 - GHOST_CACHE_BLOCK_SHIFT))

typedef struct _GHOST_CACHE_ENTRY
{
    ULONG64 Tag;         // Mixed (file key, block index), never zero; zero marks an empty slot
    ULONG   LastTouch;   // GhostCache.Clock when a fetch last included this block, truncated
    USHORT  Touches;     // Fetches that included this block, saturating at GHOST_CACHE_TOUCH_MAX
    BOOLEAN Full;        // Some fetch covered the whole block, so a real cache would hold it
    UCHAR   Reserved[1]; // explicit tail padding
} GHOST_CACHE_ENTRY, * PGHOST_CACHE_ENTRY;

CHECK_PADDING_BETWEEN(GHOST_CACHE_ENTRY, Tag, LastTouch);
CHECK_PADDING_BETWEEN(GHOST_CACHE_ENTRY, LastTouch, Touches);
CHECK_PADDING_BETWEEN(GHOST_CACHE_ENTRY, Touches, Full);
CHECK_PADDING_BETWEEN(GHOST_CACHE_ENTRY, Full, Reserved);
CHECK_PADDING_END(GHOST_CACHE_ENTRY, Reserved);

typedef struct _GHOST_CACHE_STATE
{
    PGHOST_CACHE_ENTRY Entries; // GHOST_CACHE_WAYS entries per set, PagedPool; NULL when the model is off
    ULONG64            SetMask; // Sets minus one; the set count is a power of two
    ULONG64            Clock;   // Blocks fetched since load; plain, see the file header
} GHOST_CACHE_STATE;

static GHOST_CACHE_STATE GhostCache;

//
// splitmix64's finalizer. The table is indexed by low bits of the tag, and
// both halves of the key -- a path hash and a small block index -- have
// most of their entropy elsewhere.
//
static ULONG64 GhostCacheMix(ULONG64 Value)
{
    Value ^= Value >> 30;
    Value *= 0xBF58476D1CE4E5B9ULL;
    Value ^= Value >> 27;
    Value *= 0x94D049BB133111EBULL;
    Value ^= Value >> 31;

    return Value;
}

//
// FNV-1a over the path bytes, folded with the two halves of the validator.
// Computed per fetch rather than stored on the FCB: a fetch is a network
// round trip, and hashing a path is not measurable against one.
//
static ULONG64 GhostCacheFileKey(const UNICODE_STRING* Path, ULONG64 FileSize, ULONG64 LastModifiedTime)
{
    const UCHAR* bytes = C_CAST(const UCHAR*, Path->Buffer);
    ULONG64 hash = 0xCBF29CE484222325ULL;

    for (USHORT i = 0; i < Path->Length; ++i)
    {
        hash = (hash ^ bytes[i]) * 0x100000001B3ULL;
    }

    return GhostCacheMix(hash ^ GhostCacheMix(FileSize ^ GhostCacheMix(LastModifiedTime)));
}

//
// Which capacity bucket a gap falls in: the count of bucket bounds it
// exceeds, so 0 is "within 1 GB" and the last is "beyond 256 GB". Branch
// free, and the same walk the harness does in reverse.
//
static ULONG GhostCacheDistanceBucket(ULONG64 DistanceBlocks)
{
    ULONG bucket = 0;

    for (ULONG i = 0; i < BLORGFS_GHOST_DISTANCE_BUCKETS - 1; ++i)
    {
        bucket += (DistanceBlocks > (GHOST_CACHE_GB_BLOCKS << (2 * i))) ? 1 : 0;
    }

    return bucket;
}

//
// The entry for Tag in its set, or the slot it should take: an empty one
// first, otherwise the one fetched longest ago. Ages are taken modulo
// 2^32 blocks, 256 TB of fetching, so the truncated stamp is exact for
// every distance the counters can report.
//
static PGHOST_CACHE_ENTRY GhostCacheSlot(ULONG64 Tag, ULONG Now, PBOOLEAN Found)
{
    PGHOST_CACHE_ENTRY set = &GhostCache.Entries[((Tag >> 1) & GhostCache.SetMask) * GHOST_CACHE_WAYS];
    ULONG victim = 0;
    ULONG victimAge = 0;

    for (ULONG i = 0; i < GHOST_CACHE_WAYS; ++i)
    {
        if (Tag == set[i].Tag)
        {
            *Found = TRUE;
            return &set[i];
        }

        const ULONG age = (0 == set[i].Tag) ? MAXULONG : (Now - set[i].LastTouch);

        if (age > victimAge)
        {
            victim = i;
            victimAge = age;
        }
    }

    *Found = FALSE;
    return &set[victim];
}

NTSTATUS BlorgGhostCacheInitialize(ULONG TableMb)
{
    if (0 == TableMb)
    {
        return STATUS_SUCCESS;
    }

    ULONG tableMb = (TableMb < GHOST_CACHE_MAX_TABLE_MB) ? TableMb : GHOST_CACHE_MAX_TABLE_MB;

    while (0 != (tableMb & (tableMb - 1)))
    {
        tableMb &= tableMb - 1;
    }

    const SIZE_T tableBytes = C_CAST(SIZE_T, tableMb) << 20;

    GhostCache.Entries = ExAllocatePoolZero(PagedPool, tableBytes, GHOST_CACHE_TAG);

    if (!GhostCache.Entries)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    GhostCache.SetMask = (tableBytes / (sizeof(GHOST_CACHE_ENTRY) * GHOST_CACHE_WAYS)) - 1;
    GhostCache.Clock = 0;

    return STATUS_SUCCESS;
}

VOID BlorgGhostCacheCleanup(VOID)
{
    if (GhostCache.Entries)
    {
        ExFreePool(GhostCache.Entries);
    }

    RtlZeroMemory(&GhostCache, sizeof(GhostCache));
}

//
// One pass over the blocks the fetch spans. For each policy p a block is
// held when some earlier fetch covered all of it and it has been fetched
// more than p times before this one; the fetch hits only if every block is
// held, and lands in the bucket of its stalest block. A fetch with some
// blocks held and not all is a partial: a first version of the real cache
// would send it to the network whole, and the count says whether serving
// the split would be worth building.
//
// A block this fetch is seeing for the p-th time is the one policy p
// would write, which is what GhostAdmitBytes accumulates.
//
VOID BlorgGhostCacheObserve(
    const UNICODE_STRING* Path,
    ULONG64 FileSize,
    ULONG64 LastModifiedTime,
    ULONG64 Offset,
    ULONG Length,
    BOOLEAN Demand)
{
    if (!GhostCache.Entries || 0 == Length)
    {
        return;
    }

    const ULONG64 fileKey = GhostCacheFileKey(Path, FileSize, LastModifiedTime);
    const ULONG64 end = Offset + Length;
    const ULONG64 lastBlock = (end - 1) >> GHOST_CACHE_BLOCK_SHIFT;

    BOOLEAN held[BLORGFS_GHOST_POLICIES] = { TRUE, TRUE };
    BOOLEAN anyHeld[BLORGFS_GHOST_POLICIES] = { FALSE, FALSE };
    ULONG64 admitBytes[BLORGFS_GHOST_POLICIES] = { 0, 0 };
    ULONG64 maxDistance = 0;
    ULONG64 evictions = 0;

    for (ULONG64 block = Offset >> GHOST_CACHE_BLOCK_SHIFT; block <= lastBlock; ++block)
    {
        const ULONG now = C_CAST(ULONG, GhostCache.Clock);
        const ULONG64 tag = GhostCacheMix(fileKey + (block * 0x9E3779B97F4A7C15ULL)) | 1;

        BOOLEAN found = FALSE;
        PGHOST_CACHE_ENTRY entry = GhostCacheSlot(tag, now, &found);

        const ULONG touches = found ? entry->Touches : 0;
        const BOOLEAN full = found && entry->Full;
        const ULONG64 distance = found ? C_CAST(ULONG, now - entry->LastTouch) : MAXULONG64;
        const BOOLEAN covers =
            (block << GHOST_CACHE_BLOCK_SHIFT) >= Offset &&
            ((block + 1) << GHOST_CACHE_BLOCK_SHIFT) <= end;

        for (ULONG p = 0; p < BLORGFS_GHOST_POLICIES; ++p)
        {
            const BOOLEAN blockHeld = full && (touches > p);

            held[p] = held[p] && blockHeld;
            anyHeld[p] = anyHeld[p] || blockHeld;
            admitBytes[p] += (touches == p) ? GHOST_CACHE_BLOCK_SIZE : 0;
        }

        maxDistance = (distance > maxDistance) ? distance : maxDistance;
        evictions += (!found && 0 != entry->Tag) ? 1 : 0;

        entry->Tag = tag;
        entry->LastTouch = now;
        entry->Touches = C_CAST(USHORT, (touches < GHOST_CACHE_TOUCH_MAX) ? touches + 1 : GHOST_CACHE_TOUCH_MAX);
        entry->Full = full || covers;

        GhostCache.Clock += 1;
    }

    const ULONG bucket = GhostCacheDistanceBucket(maxDistance);

    BLORGFS_STAT_INC(GhostFetches);
    BLORGFS_STAT_ADD(GhostFetchBytes, Length);
    BLORGFS_STAT_ADD(GhostDemandFetches, Demand ? 1 : 0);
    BLORGFS_STAT_ADD(GhostEvictions, evictions);

    for (ULONG p = 0; p < BLORGFS_GHOST_POLICIES; ++p)
    {
        BLORGFS_STAT_ADD(GhostAdmitBytes[p], admitBytes[p]);
        BLORGFS_STAT_ADD(GhostPartialFetches[p], (anyHeld[p] && !held[p]) ? 1 : 0);

        if (held[p])
        {
            BLORGFS_STAT_INC(GhostHitFetches[p][bucket]);
            BLORGFS_STAT_ADD(GhostHitBytes[p][bucket], Length);
            BLORGFS_STAT_ADD(GhostHitDemandFetches[p][bucket], Demand ? 1 : 0);
        }
    }
}
