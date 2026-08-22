#include "Driver.h"

//
//  Full-path resolution cache: caches the result of a create-time existence
//  check (exists+metadata, or not-found) keyed by full path, decoupled from
//  node lifetime. Sharded into buckets each guarded by its own EX_PUSH_LOCK
//  (shared for lookups, exclusive for insert/invalidate). Invalidation is
//  layered: per-entry TTL, single-path drop, prefix/subtree drop, and an
//  O(1) generation-bump full flush. Occupancy is capped per bucket with
//  FIFO eviction and opportunistic reaping of stale entries.
//

#define PATH_CACHE_BUCKETS         256u   // power of two
#define PATH_CACHE_MAX_PER_BUCKET  16u
#define PATH_CACHE_MAX_PATH_BYTES  4096u
#define PATH_CACHE_TAG             'CPHT'

// 4 seconds, in 100ns units (KeQueryInterruptTime).
#define PATH_CACHE_TTL_100NS       (4LL * 10LL * 1000LL * 1000LL)

//
// One cached path-lookup result (exists+metadata, or not-found).
// Reserved is explicit tail padding so CHECK_PADDING_END can verify layout.
//
typedef struct _PATH_CACHE_ENTRY
{
    LIST_ENTRY               Link;        // bucket list linkage
    UNICODE_STRING           Path;        // owned copy, PagedPool (all access <= APC_LEVEL under push locks)
    ULONG64                  ExpiryTime;  // KeQueryInterruptTime units
    DIRECTORY_ENTRY_METADATA Meta;        // valid only when Exists
    ULONG                    Generation;  // snapshot of PathCache.Generation at insert
    BOOLEAN                  Exists;      // whether the path resolved
    UCHAR                    Reserved[3]; // explicit tail padding
} PATH_CACHE_ENTRY, * PPATH_CACHE_ENTRY;

CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, Link, Path);
CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, Path, ExpiryTime);
CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, ExpiryTime, Meta);
CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, Meta, Generation);
CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, Generation, Exists);
CHECK_PADDING_BETWEEN(PATH_CACHE_ENTRY, Exists, Reserved);
CHECK_PADDING_END(PATH_CACHE_ENTRY, Reserved);

//
// One shard of the path cache: an independently locked bucket of entries.
// Sized and aligned to exactly one 64-byte cache line so contention on
// one bucket's push lock never falsely shares a line with its neighbours
// (the create path probes this cache on every warm-miss open).
//
typedef struct DECLSPEC_ALIGN(CACHE_LINE_SIZE) _PATH_CACHE_BUCKET
{
    EX_PUSH_LOCK Lock;  // guards List/Count; shared for lookup, exclusive for insert/invalidate
    LIST_ENTRY   List;  // PATH_CACHE_ENTRY list
    ULONG        Count; // entries currently in this bucket
    UCHAR        Reserved[36]; // explicit pad to the 64-byte line
} PATH_CACHE_BUCKET;

//
// One bucket per cache line. An absolute-size claim, so it holds only
// where EX_PUSH_LOCK is the kernel's pointer-sized push lock; a build that
// substitutes a fatter one simply gets larger buckets.
//
C_ASSERT(sizeof(EX_PUSH_LOCK) != sizeof(PVOID) ||
         CACHE_LINE_SIZE == sizeof(PATH_CACHE_BUCKET));

// Global path cache state: all buckets plus generation/count bookkeeping.
typedef struct _PATH_CACHE_STATE
{
    PATH_CACHE_BUCKET Buckets[PATH_CACHE_BUCKETS];
    BOOLEAN           Ready;

    //
    // Bumped by PathCacheInvalidateAll; entries stamped with an older
    // generation are treated as misses. Only ever updated via
    // InterlockedIncrement, so it doesn't need to be volatile.
    //
    LONG     Generation;

    //
    // Live entry count across all buckets, for tracing only. Updated via
    // interlocked ops, so it doesn't need to be volatile.
    //
    LONG     Count;
} PATH_CACHE_STATE;

static PATH_CACHE_STATE PathCache;

//
// Hashes Path (case-insensitive) to a bucket index in [0, PATH_CACHE_BUCKETS).
// Falls back to a manual case-insensitive hash on failure, which is
// belt-and-braces since RtlHashUnicodeString only fails on bad args already
// excluded above.
//
static ULONG PathCacheBucketIndex(const UNICODE_STRING* Path)
{
    ULONG hash = 0;

    if (!NT_SUCCESS(RtlHashUnicodeString(Path, TRUE, HASH_STRING_ALGORITHM_DEFAULT, &hash)))
    {
        for (USHORT i = 0; i < Path->Length / sizeof(WCHAR); i++)
        {
            hash = (hash * 131u) + RtlUpcaseUnicodeChar(Path->Buffer[i]);
        }
    }

    return hash & (PATH_CACHE_BUCKETS - 1u);
}

//
// Frees an entry's owned path buffer and the entry itself. Caller must have
// already unlinked it from its bucket.
//
static VOID PathCacheFreeEntry(PPATH_CACHE_ENTRY Entry)
{
    if (Entry->Path.Buffer)
    {
        ExFreePool(Entry->Path.Buffer);
    }
    ExFreePool(Entry);
}

//
//  Unlink an entry from its bucket, drop the global counter, free it. Caller
//  holds the bucket lock exclusive.
//
static VOID PathCacheRemoveEntry(PATH_CACHE_BUCKET* Bucket, PPATH_CACHE_ENTRY Entry)
{
    RemoveEntryList(&Entry->Link);
    Bucket->Count--;
    InterlockedDecrement(&PathCache.Count);
    PathCacheFreeEntry(Entry);
}

//
//  An entry is honoured only while unexpired AND minted under the current
//  generation. Both a stale TTL and a stale generation make it a miss.
//
static BOOLEAN PathCacheEntryLive(const PATH_CACHE_ENTRY* Entry, ULONG64 Now, LONG Generation)
{
    return (Now < Entry->ExpiryTime) && (Entry->Generation == C_CAST(ULONG, Generation));
}

//
// Initializes all bucket locks/lists and resets generation/count. Called
// once at driver load, before any lookups/inserts can occur.
//
VOID PathCacheInit(VOID)
{
    for (ULONG i = 0; i < PATH_CACHE_BUCKETS; i++)
    {
        ExInitializePushLock(&PathCache.Buckets[i].Lock);
        InitializeListHead(&PathCache.Buckets[i].List);
        PathCache.Buckets[i].Count = 0;
    }

    PathCache.Generation = 0;
    PathCache.Count = 0;
    PathCache.Ready = TRUE;
}

//
// Frees every entry in every bucket and marks the cache not-ready. Called
// only at driver unload, after I/O has drained, so it takes no locks.
//
VOID PathCacheCleanup(VOID)
{
    if (!PathCache.Ready)
    {
        return;
    }

    PathCache.Ready = FALSE;

    for (ULONG i = 0; i < PATH_CACHE_BUCKETS; i++)
    {
        while (!IsListEmpty(&PathCache.Buckets[i].List))
        {
            PLIST_ENTRY e = RemoveHeadList(&PathCache.Buckets[i].List);
            PathCacheFreeEntry(CONTAINING_RECORD(e, PATH_CACHE_ENTRY, Link));
        }
        PathCache.Buckets[i].Count = 0;
    }

    PathCache.Count = 0;
}

//
// Looks up Path's cached existence result under the owning bucket's shared
// lock, copying out Meta on a live "exists" hit. Returns a miss for
// expired/stale-generation entries rather than reclaiming them here --
// reclamation happens under the exclusive lock in PathCacheInsert instead.
//
PATH_CACHE_RESULT PathCacheLookup(const UNICODE_STRING* Path, PDIRECTORY_ENTRY_METADATA Meta)
{
    if (!PathCache.Ready || !Path || 0 == Path->Length || !Path->Buffer)
    {
        return PathCacheMiss;
    }

    PATH_CACHE_BUCKET* bucket = &PathCache.Buckets[PathCacheBucketIndex(Path)];
    ULONG64 now = KeQueryInterruptTime();
    LONG generation = ReadNoFence(&PathCache.Generation);
    PATH_CACHE_RESULT result = PathCacheMiss;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&bucket->Lock);

    for (PLIST_ENTRY e = bucket->List.Flink; e != &bucket->List; e = e->Flink)
    {
        PPATH_CACHE_ENTRY entry = CONTAINING_RECORD(e, PATH_CACHE_ENTRY, Link);

        if (RtlEqualUnicodeString(&entry->Path, Path, TRUE))
        {
            if (PathCacheEntryLive(entry, now, generation))
            {
                if (entry->Exists)
                {
                    if (Meta)
                    {
                        *Meta = entry->Meta;
                    }
                    result = PathCacheExists;
                }
                else
                {
                    result = PathCacheNotFound;
                }
            }
            break;
        }
    }

    ExReleasePushLockShared(&bucket->Lock);
    KeLeaveCriticalRegion();

    BLORGFS_STAT_INC(MetaDataReads);

    if (PathCacheMiss == result)
    {
        BLORGFS_STAT_INC(PathCacheMisses);
    }
    else
    {
        BLORGFS_STAT_INC(PathCacheHits);
    }

    return result;
}

//
// Inserts or refreshes a path's cached result (exists+metadata, or
// not-found). Builds the new entry outside the bucket lock so the exclusive
// section is pointer manipulation only, reclaims expired/stale-generation
// entries while walking the bucket, and evicts the oldest entry (FIFO) if
// the bucket is at capacity. If an existing entry is refreshed in place
// instead, the prebuilt entry's ownership was never transferred to the
// bucket, so it is freed before returning.
//
static VOID PathCacheInsert(const UNICODE_STRING* Path, BOOLEAN Exists, const DIRECTORY_ENTRY_METADATA* Meta)
{
    BOOLEAN hasMeta = Exists && Meta;

    if (!PathCache.Ready || !Path || 0 == Path->Length || !Path->Buffer ||
        Path->Length > PATH_CACHE_MAX_PATH_BYTES)
    {
        return;
    }

    PPATH_CACHE_ENTRY newEntry = ExAllocatePoolZero(PagedPool, sizeof(PATH_CACHE_ENTRY), PATH_CACHE_TAG);

    if (!newEntry)
    {
        return;
    }

    newEntry->Path.Buffer = ExAllocatePoolUninitialized(PagedPool, Path->Length, PATH_CACHE_TAG);

    if (!newEntry->Path.Buffer)
    {
        ExFreePool(newEntry);
        return;
    }

    RtlCopyMemory(newEntry->Path.Buffer, Path->Buffer, Path->Length);
    newEntry->Path.Length = Path->Length;
    newEntry->Path.MaximumLength = Path->Length;
    newEntry->Exists = Exists;

    if (hasMeta)
    {
        newEntry->Meta = *Meta;
    }

    ULONG64 now = KeQueryInterruptTime();
    LONG generation = ReadNoFence(&PathCache.Generation);
    newEntry->Generation = C_CAST(ULONG, generation);
    newEntry->ExpiryTime = now + PATH_CACHE_TTL_100NS;

    PATH_CACHE_BUCKET* bucket = &PathCache.Buckets[PathCacheBucketIndex(Path)];
    BOOLEAN inserted = FALSE;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    PLIST_ENTRY e = bucket->List.Flink;

    while (e != &bucket->List)
    {
        PPATH_CACHE_ENTRY entry = CONTAINING_RECORD(e, PATH_CACHE_ENTRY, Link);
        PLIST_ENTRY next = e->Flink;

        if (RtlEqualUnicodeString(&entry->Path, Path, TRUE))
        {
            entry->Exists = Exists;
            if (hasMeta)
            {
                entry->Meta = *Meta;
            }
            entry->Generation = newEntry->Generation;
            entry->ExpiryTime = newEntry->ExpiryTime;
            inserted = TRUE;
            break;
        }

        if (!PathCacheEntryLive(entry, now, generation))
        {
            PathCacheRemoveEntry(bucket, entry);
        }

        e = next;
    }

    if (!inserted)
    {
        if (bucket->Count >= PATH_CACHE_MAX_PER_BUCKET && !IsListEmpty(&bucket->List))
        {
            PLIST_ENTRY victim = bucket->List.Flink;
            PathCacheRemoveEntry(bucket, CONTAINING_RECORD(victim, PATH_CACHE_ENTRY, Link));
        }

        InsertTailList(&bucket->List, &newEntry->Link);
        bucket->Count++;
        InterlockedIncrement(&PathCache.Count);
        newEntry = NULL;
    }

    ExReleasePushLockExclusive(&bucket->Lock);
    KeLeaveCriticalRegion();

    if (newEntry)
    {
        PathCacheFreeEntry(newEntry);
    }
}

// Caches a successful path resolution with its metadata.
VOID PathCacheInsertExists(const UNICODE_STRING* Path, const DIRECTORY_ENTRY_METADATA* Meta)
{
    PathCacheInsert(Path, TRUE, Meta);
}

// Caches a failed (not-found) path resolution.
VOID PathCacheInsertNotFound(const UNICODE_STRING* Path)
{
    PathCacheInsert(Path, FALSE, NULL);
}

//
//  Drop one exact path. Cheap: hashes straight to the owning bucket and walks
//  only that (short) chain. A no-op if the path is not cached.
//
VOID PathCacheInvalidate(const UNICODE_STRING* Path)
{
    if (!PathCache.Ready || !Path || 0 == Path->Length || !Path->Buffer)
    {
        return;
    }

    PATH_CACHE_BUCKET* bucket = &PathCache.Buckets[PathCacheBucketIndex(Path)];

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    for (PLIST_ENTRY e = bucket->List.Flink; e != &bucket->List; e = e->Flink)
    {
        PPATH_CACHE_ENTRY entry = CONTAINING_RECORD(e, PATH_CACHE_ENTRY, Link);

        if (RtlEqualUnicodeString(&entry->Path, Path, TRUE))
        {
            PathCacheRemoveEntry(bucket, entry);
            break;
        }
    }

    ExReleasePushLockExclusive(&bucket->Lock);
    KeLeaveCriticalRegion();
}

//
//  True when Path is Dir itself or lies beneath it: a boundary-checked prefix
//  test so "\Movie" does not match "\Movies". Case-insensitive.
//
//  The boundary is normally the separator that must follow Dir inside Path,
//  but a Dir that already ends in one has consumed it in the prefix match
//  itself -- the character at that index is then the first character of the
//  child's name, not a separator. The volume root ("\", the root DCB's
//  FullPath per Driver.c, and what BlorgDirComplete passes on a root listing
//  publish) is the only such Dir this driver produces; without the
//  trailing-separator case it matched nothing beneath itself, silently
//  disabling the stale-negative eviction that invalidation exists to perform
//  for every file sitting directly in the volume root.
//
static BOOLEAN PathCacheIsUnder(const UNICODE_STRING* Dir, const UNICODE_STRING* Path)
{
    if (Path->Length < Dir->Length)
    {
        return FALSE;
    }

    if (!RtlPrefixUnicodeString(Dir, Path, TRUE))
    {
        return FALSE;
    }

    BOOLEAN dirEndsWithSeparator =
        (0 < Dir->Length) && (L'\\' == Dir->Buffer[(Dir->Length / sizeof(WCHAR)) - 1]);

    return (Path->Length == Dir->Length) ||
           dirEndsWithSeparator ||
           (L'\\' == Path->Buffer[Dir->Length / sizeof(WCHAR)]);
}

//
//  Drop a directory and its entire subtree. A subtree's paths hash to
//  different buckets, so this sweeps every bucket -- acceptable because
//  invalidation is a rare event (a listing refresh or a directory mutation),
//  unlike the per-probe lookup path. Each bucket is taken and released in
//  turn, so no two locks are ever held together.
//
VOID PathCacheInvalidatePrefix(const UNICODE_STRING* Dir)
{
    if (!PathCache.Ready || !Dir || 0 == Dir->Length || !Dir->Buffer)
    {
        return;
    }

    for (ULONG i = 0; i < PATH_CACHE_BUCKETS; i++)
    {
        PATH_CACHE_BUCKET* bucket = &PathCache.Buckets[i];

        KeEnterCriticalRegion();
        ExAcquirePushLockExclusive(&bucket->Lock);

        PLIST_ENTRY e = bucket->List.Flink;

        while (e != &bucket->List)
        {
            PPATH_CACHE_ENTRY entry = CONTAINING_RECORD(e, PATH_CACHE_ENTRY, Link);
            PLIST_ENTRY next = e->Flink;

            if (PathCacheIsUnder(Dir, &entry->Path))
            {
                PathCacheRemoveEntry(bucket, entry);
            }

            e = next;
        }

        ExReleasePushLockExclusive(&bucket->Lock);
        KeLeaveCriticalRegion();
    }
}

//
//  Wholesale flush in O(1): bump the generation so every existing entry is
//  now stale (a miss on lookup, a reap target on the next insert). Memory is
//  reclaimed lazily rather than eagerly, which is fine -- the bucket cap still
//  bounds it, and inserts sweep the dead entries as they go.
//
VOID PathCacheInvalidateAll(VOID)
{
    InterlockedIncrement(&PathCache.Generation);
}
