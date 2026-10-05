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
//  Beside it, keyed the same way, the listing cache: one immutable
//  directory-listing snapshot per directory, so a listing outlives the DCB
//  and handle that fetched it. Before this a listing died with its last
//  handle, and every `dir` of the same directory paid a dirinfo GET (2.7-3.3
//  ms on the reference link; a repeated `dir /s` of 121 directories paid all
//  121). Its own sharding, sized for far fewer and far larger entries, and a
//  global byte budget, since a 300-entry listing is ~170 KB.
//
//  Both caches share one invalidation sequence. Every result read from
//  elsewhere is inserted with the ticket taken before the read, and refused
//  if an invalidation ran in between; see BlorgPathCacheTakeTicket.
//

#define PATH_CACHE_BUCKET_BITS     8u
#define PATH_CACHE_BUCKETS         (1u << PATH_CACHE_BUCKET_BITS)
#define PATH_CACHE_MAX_PER_BUCKET  16u
#define PATH_CACHE_MAX_PATH_BYTES  4096u
#define PATH_CACHE_TAG             'CPHT'

// 4 seconds, in 100ns units (KeQueryInterruptTime).
#define PATH_CACHE_TTL_100NS       (4LL * 10LL * 1000LL * 1000LL)

// Most entries one listing may seed: a quarter of the cache's capacity (see
// BlorgPathCacheSeedListing).
#define PATH_CACHE_SEED_MAX        ((PATH_CACHE_BUCKETS * PATH_CACHE_MAX_PER_BUCKET) / 4u)

#define LISTING_CACHE_BUCKET_BITS  8u
#define LISTING_CACHE_BUCKETS      (1u << LISTING_CACHE_BUCKET_BITS)
#define LISTING_CACHE_MAX_PER_BUCKET 8u
#define LISTING_CACHE_MAX_BYTES    (32LL * 1024LL * 1024LL)
#define LISTING_CACHE_TAG          'CLHT'

//
// A listing answers without a request for the path cache's own TTL, the
// staleness the driver already accepts for every open. Past that, a
// directory query is still answered from it at once, and one background
// refetch replaces it (BlorgPathCacheLookupListing's RefreshOwed): a re-list
// never waits on the wire, and what it shows is at most one refresh behind.
// The stale window is bounded so a directory nobody has listed for a while
// is fetched in the foreground rather than shown from long ago.
//
#define LISTING_FRESH_100NS        PATH_CACHE_TTL_100NS
#define LISTING_STALE_MAX_100NS    (30LL * 10LL * 1000LL * 1000LL)

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
// One cached directory listing. The entry owns one reference to Listing;
// every reader takes its own under the bucket lock before using it, so
// eviction never frees a snapshot someone is enumerating.
//
typedef struct _LISTING_CACHE_ENTRY
{
    LIST_ENTRY      Link;           // bucket list linkage
    UNICODE_STRING  Path;           // owned copy, PagedPool
    PDIRECTORY_INFO Listing;        // one reference, owned by this entry
    LONG64          Bytes;          // Listing's size, charged to PathCache.ListingBytes
    ULONG64         IssueTime;      // when the fetch that produced Listing was issued
    ULONG           Generation;     // snapshot of PathCache.Generation at insert
    LONG            RefreshClaimed; // Interlocked: concurrent stale lookups race to owe the one refetch
} LISTING_CACHE_ENTRY, * PLISTING_CACHE_ENTRY;

CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, Link, Path);
CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, Path, Listing);
CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, Listing, Bytes);
CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, Bytes, IssueTime);
CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, IssueTime, Generation);
CHECK_PADDING_BETWEEN(LISTING_CACHE_ENTRY, Generation, RefreshClaimed);
CHECK_PADDING_END(LISTING_CACHE_ENTRY, RefreshClaimed);

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
    PATH_CACHE_BUCKET ListingBuckets[LISTING_CACHE_BUCKETS];
    BOOLEAN           Ready;

    //
    // Bumped by BlorgPathCacheInvalidateAll; entries stamped with an older
    // generation are treated as misses. Only ever updated via
    // InterlockedIncrement, so it doesn't need to be volatile.
    //
    LONG     Generation;

    //
    // Live entry count across all buckets, for tracing only. Updated via
    // interlocked ops, so it doesn't need to be volatile.
    //
    LONG     Count;

    //
    // Advanced by every invalidation before it sweeps. Interlocked because
    // invalidations on different buckets' paths run concurrently and none
    // may be lost: a lost increment is an insert ticketed before an
    // invalidation being accepted after it.
    //
    LONG64   Sequence;

    //
    // Bytes of listing held across all listing buckets, against
    // LISTING_CACHE_MAX_BYTES. Interlocked because publishes and evictions
    // in different buckets update it under different locks; it changes once
    // per listing fetch, never per read.
    //
    LONG64   ListingBytes;
} PATH_CACHE_STATE;

static PATH_CACHE_STATE PathCache;

static PATH_CACHE_BUCKET* PathCacheBucket(const UNICODE_STRING* Path)
{
    return &PathCache.Buckets[BlorgHashPath(Path) >> (32u - PATH_CACHE_BUCKET_BITS)];
}

static PATH_CACHE_BUCKET* ListingCacheBucket(const UNICODE_STRING* Path)
{
    return &PathCache.ListingBuckets[BlorgHashPath(Path) >> (32u - LISTING_CACHE_BUCKET_BITS)];
}

//
// A ticket is honoured only if no invalidation has run since it was taken.
// Called under the bucket lock the insert is about to modify: an
// invalidation advances the sequence before it takes any bucket lock, so
// either this read sees the advance, or this insert lands first and the
// invalidation's sweep removes it. NULL is an unconditional insert.
//
static BOOLEAN PathCacheTicketHonoured(_In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    return !Ticket || (Ticket->Sequence == ReadNoFence64(&PathCache.Sequence));
}

//
// Every invalidation calls this before it sweeps anything; see
// PathCacheTicketHonoured for why the order matters.
//
static VOID PathCacheAdvanceSequence(VOID)
{
    InterlockedIncrement64(&PathCache.Sequence);
}

//
// The bytes a listing occupies, as HttpDeserializeDirectoryInfo sized it.
//
static LONG64 ListingCacheSizeOf(const DIRECTORY_INFO* Listing)
{
    return C_CAST(LONG64, sizeof(DIRECTORY_INFO) +
        (Listing->FileCount * sizeof(DIRECTORY_FILE_METADATA)) +
        (Listing->SubDirCount * sizeof(DIRECTORY_SUBDIR_METADATA)));
}

//
// Unlinks a listing entry, uncharges its bytes, and drops the entry's
// reference; the snapshot itself survives while any handle still holds one.
// Caller holds the bucket lock exclusive.
//
static VOID ListingCacheRemoveEntry(PATH_CACHE_BUCKET* Bucket, PLISTING_CACHE_ENTRY Entry)
{
    RemoveEntryList(&Entry->Link);
    Bucket->Count--;
    InterlockedExchangeAdd64(&PathCache.ListingBytes, -Entry->Bytes);
    BlorgReleaseDirectoryInfo(Entry->Listing);
    ExFreePool(Entry->Path.Buffer);
    ExFreePool(Entry);
}

//
// How long ago the fetch behind Entry was issued. Now is read before the
// bucket lock, so an entry published meanwhile by a later-issued fetch can be
// younger than Now; that counts as age zero, not as an unsigned wrap that
// would make the newest listing look the oldest.
//
static ULONG64 ListingCacheAge(const LISTING_CACHE_ENTRY* Entry, ULONG64 Now)
{
    return (Now > Entry->IssueTime) ? (Now - Entry->IssueTime) : 0;
}

//
// Live means minted under the current generation and young enough to be
// served at all, stale or not.
//
static BOOLEAN ListingCacheEntryLive(const LISTING_CACHE_ENTRY* Entry, ULONG64 Now, LONG Generation)
{
    return (Entry->Generation == C_CAST(ULONG, Generation)) &&
           (ListingCacheAge(Entry, Now) < C_CAST(ULONG64, LISTING_STALE_MAX_100NS));
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
VOID BlorgPathCacheInit(VOID)
{
    for (ULONG i = 0; i < PATH_CACHE_BUCKETS; i++)
    {
        ExInitializePushLock(&PathCache.Buckets[i].Lock);
        InitializeListHead(&PathCache.Buckets[i].List);
        PathCache.Buckets[i].Count = 0;
    }

    for (ULONG i = 0; i < LISTING_CACHE_BUCKETS; i++)
    {
        ExInitializePushLock(&PathCache.ListingBuckets[i].Lock);
        InitializeListHead(&PathCache.ListingBuckets[i].List);
        PathCache.ListingBuckets[i].Count = 0;
    }

    PathCache.Generation = 0;
    PathCache.Count = 0;
    PathCache.Sequence = 0;
    PathCache.ListingBytes = 0;
    PathCache.Ready = TRUE;
}

//
// Frees every entry in every bucket and marks the cache not-ready. Called
// only at driver unload, after I/O has drained, so it takes no locks.
//
VOID BlorgPathCacheCleanup(VOID)
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

    for (ULONG i = 0; i < LISTING_CACHE_BUCKETS; i++)
    {
        PATH_CACHE_BUCKET* bucket = &PathCache.ListingBuckets[i];

        while (!IsListEmpty(&bucket->List))
        {
            ListingCacheRemoveEntry(bucket, CONTAINING_RECORD(bucket->List.Flink, LISTING_CACHE_ENTRY, Link));
        }
    }

    PathCache.Count = 0;
}

//
//  Takes the ticket a reader must hold before it reads anything it will
//  insert: before issuing the request, or before reading a cached listing.
//
//  The protocol, in full: an invalidation advances PathCache.Sequence and
//  then sweeps, each bucket under its own lock. An insert compares its
//  ticket's sequence with the current one under the lock of the bucket it is
//  inserting into, and is refused on any difference. For one bucket, either
//  the insert's critical section comes first -- and the sweep, which comes
//  after the advance, removes what it inserted -- or the sweep's comes first,
//  and the insert, ordered after it by the lock, sees the advance. So no
//  result read before an invalidation survives it. The cost is that an
//  invalidation of one path also refuses unrelated inserts in flight across
//  it; invalidations are rare beside reads, and a refused insert only costs
//  the next reader a request.
//
VOID BlorgPathCacheTakeTicket(PPATH_CACHE_TICKET Ticket)
{
    Ticket->Sequence = ReadNoFence64(&PathCache.Sequence);
    Ticket->IssueTime = KeQueryInterruptTime();
}

//
// Counts every resolution in MetaDataReads, not every miss: the number
// means "how many create-time lookups ran", alongside PathCacheHits and
// PathCacheMisses which split it, and MetaDataDiskReads (Client.c) which
// counts only the network fetches a miss can lead to. It is deliberately
// not "metadata I/O" -- a pure cache hit moves no bytes and must not
// inflate an I/O-shaped number.
//
// Looks up Path's cached existence result under the owning bucket's shared
// lock, copying out Meta on a live "exists" hit. Returns a miss for
// expired/stale-generation entries rather than reclaiming them here --
// reclamation happens under the exclusive lock in PathCacheInsert instead.
//
PATH_CACHE_RESULT BlorgPathCacheLookup(const UNICODE_STRING* Path, PDIRECTORY_ENTRY_METADATA Meta)
{
    if (!PathCache.Ready || !Path || 0 == Path->Length || !Path->Buffer)
    {
        return PathCacheMiss;
    }

    PATH_CACHE_BUCKET* bucket = PathCacheBucket(Path);
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
// bucket, so it is freed before returning. A ticket an invalidation has
// overtaken inserts nothing (see BlorgPathCacheTakeTicket).
//
static VOID PathCacheInsert(const UNICODE_STRING* Path, BOOLEAN Exists, const DIRECTORY_ENTRY_METADATA* Meta, _In_opt_ const PATH_CACHE_TICKET* Ticket)
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

    PATH_CACHE_BUCKET* bucket = PathCacheBucket(Path);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    BOOLEAN honoured = PathCacheTicketHonoured(Ticket);
    BOOLEAN inserted = !honoured;
    PLIST_ENTRY e = honoured ? bucket->List.Flink : &bucket->List;

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
VOID BlorgPathCacheInsertExists(const UNICODE_STRING* Path, const DIRECTORY_ENTRY_METADATA* Meta, _In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    PathCacheInsert(Path, TRUE, Meta, Ticket);
}

// Caches a failed (not-found) path resolution.
VOID BlorgPathCacheInsertNotFound(const UNICODE_STRING* Path, _In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    PathCacheInsert(Path, FALSE, NULL, Ticket);
}

//
//  The directory a path sits in: everything before its last separator, or
//  the root ("\") for a path directly beneath it. FALSE for the root
//  itself, which has no parent. Parent aliases Path's buffer.
//
static BOOLEAN PathCacheParent(const UNICODE_STRING* Path, PUNICODE_STRING Parent)
{
    USHORT chars = Path->Length / sizeof(WCHAR);

    while (chars > 0 && L'\\' != Path->Buffer[chars - 1])
    {
        chars--;
    }

    if (0 == chars || C_CAST(SIZE_T, chars) * sizeof(WCHAR) == C_CAST(SIZE_T, Path->Length))
    {
        return FALSE;
    }

    Parent->Buffer = Path->Buffer;
    Parent->Length = C_CAST(USHORT, ((1 == chars) ? 1u : chars - 1u) * sizeof(WCHAR));
    Parent->MaximumLength = Parent->Length;
    return TRUE;
}

//
//  Drops the cached listing of exactly Dir, if there is one.
//
static VOID ListingCacheDrop(const UNICODE_STRING* Dir)
{
    PATH_CACHE_BUCKET* bucket = ListingCacheBucket(Dir);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    for (PLIST_ENTRY e = bucket->List.Flink; e != &bucket->List; e = e->Flink)
    {
        PLISTING_CACHE_ENTRY entry = CONTAINING_RECORD(e, LISTING_CACHE_ENTRY, Link);

        if (RtlEqualUnicodeString(&entry->Path, Dir, TRUE))
        {
            ListingCacheRemoveEntry(bucket, entry);
            break;
        }
    }

    ExReleasePushLockExclusive(&bucket->Lock);
    KeLeaveCriticalRegion();
}

//
//  Drops the listing of the directory Path sits in: a change to Path is a
//  change to that listing.
//
static VOID ListingCacheDropParent(const UNICODE_STRING* Path)
{
    UNICODE_STRING parent;

    if (PathCacheParent(Path, &parent))
    {
        ListingCacheDrop(&parent);
    }
}

//
//  Drop one exact path, its listing if it is a directory, and its parent's
//  listing. Cheap: hashes straight to the owning buckets and walks only
//  those (short) chains. A no-op for whatever is not cached.
//
VOID BlorgPathCacheInvalidate(const UNICODE_STRING* Path)
{
    if (!PathCache.Ready || !Path || 0 == Path->Length || !Path->Buffer)
    {
        return;
    }

    PathCacheAdvanceSequence();

    PATH_CACHE_BUCKET* bucket = PathCacheBucket(Path);

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

    ListingCacheDrop(Path);
    ListingCacheDropParent(Path);
}

//
//  True when Path is Dir itself or lies beneath it: a boundary-checked prefix
//  test so "\Movie" does not match "\Movies". Case-insensitive.
//
//  The boundary is normally the separator that must follow Dir inside Path,
//  but a Dir that already ends in one has consumed it in the prefix match
//  itself -- the character at that index is then the first character of the
//  child's name, not a separator. The volume root ("\", the root DCB's
//  FullPath per Driver.c, and what DirCtrlComplete passes on a root listing
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
//  Drop everything beneath a directory, and the directory's own entry too
//  unless KeepDir. A subtree's paths hash to different buckets, so this
//  sweeps every bucket -- once per listing publish (BlorgPathCacheSeedListing),
//  which is a network round trip's worth of time apart at the very least,
//  so 256 uncontended push-lock acquisitions are noise beside it. Each
//  bucket is taken and released in turn, so no two locks are ever held
//  together.
//
static VOID PathCacheInvalidateUnder(const UNICODE_STRING* Dir, BOOLEAN KeepDir)
{
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

            if (PathCacheIsUnder(Dir, &entry->Path) &&
                !(KeepDir && entry->Path.Length == Dir->Length))
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
//  Drops every cached listing of Dir or anything beneath it. Sweeps every
//  listing bucket, one lock at a time.
//
static VOID ListingCacheDropUnder(_In_opt_ const UNICODE_STRING* Dir)
{
    for (ULONG i = 0; i < LISTING_CACHE_BUCKETS; i++)
    {
        PATH_CACHE_BUCKET* bucket = &PathCache.ListingBuckets[i];

        KeEnterCriticalRegion();
        ExAcquirePushLockExclusive(&bucket->Lock);

        PLIST_ENTRY e = bucket->List.Flink;

        while (e != &bucket->List)
        {
            PLISTING_CACHE_ENTRY entry = CONTAINING_RECORD(e, LISTING_CACHE_ENTRY, Link);
            PLIST_ENTRY next = e->Flink;

            if (!Dir || PathCacheIsUnder(Dir, &entry->Path))
            {
                ListingCacheRemoveEntry(bucket, entry);
            }

            e = next;
        }

        ExReleasePushLockExclusive(&bucket->Lock);
        KeLeaveCriticalRegion();
    }
}

//
//  Drop a directory and its entire subtree, the directory itself included,
//  with every listing in it and the listing of the directory it sits in.
//
VOID BlorgPathCacheInvalidatePrefix(const UNICODE_STRING* Dir)
{
    if (!PathCache.Ready || !Dir || 0 == Dir->Length || !Dir->Buffer)
    {
        return;
    }

    PathCacheAdvanceSequence();
    PathCacheInvalidateUnder(Dir, FALSE);
    ListingCacheDropUnder(Dir);
    ListingCacheDropParent(Dir);
}

//
//  Appends one listing entry's name to the directory path already in
//  Scratch, inserts it with Meta, and trims Scratch back to the directory.
//  Names the listing could not have produced -- empty, or longer than its
//  own MAX_NAME_LEN field -- and paths past the cache's own limit are
//  skipped rather than truncated, since a truncated path would cache a
//  result for a different file.
//
static VOID PathCacheSeedEntry(PUNICODE_STRING Scratch, USHORT DirLength, const WCHAR* Name, SIZE_T NameLength, const DIRECTORY_ENTRY_METADATA* Meta, _In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    if (0 == NameLength || NameLength > MAX_NAME_LEN ||
        DirLength + (NameLength * sizeof(WCHAR)) > Scratch->MaximumLength)
    {
        return;
    }

    RtlCopyMemory(C_CAST(PUCHAR, Scratch->Buffer) + DirLength, Name, NameLength * sizeof(WCHAR));
    Scratch->Length = C_CAST(USHORT, DirLength + (NameLength * sizeof(WCHAR)));

    PathCacheInsert(Scratch, TRUE, Meta, Ticket);

    Scratch->Length = DirLength;
}

//
//  Makes a freshly published listing of Dir the path cache's answer for
//  everything directly in it.
//
//  This replaced a plain BlorgPathCacheInvalidatePrefix on publish, which
//  was correct and cost a round trip per file: a DCB's listing lives only
//  as long as the DCB, so after `dir` closed its handle every child open
//  missed the node table, the path cache (just emptied) and the parent's
//  listing (just freed), and went to the network for metadata the driver
//  had held a moment earlier, and a repeated listing paid a fileinfo GET for
//  the directory's own (evicted) entry before its dirinfo GET. Measured in
//  the CI guest on the reference link: 100 opens straight after a listing
//  went from 101 fileinfo GETs and 5.9 ms per open to 1 GET and 3.2 ms, and
//  a repeated listing from 5.5 ms to 3.1 ms.
//
//  What the listing is authoritative for, and so what this does:
//   - Every direct child is inserted as existing, with the listing's
//     metadata. That also replaces a stale not-found for a child that has
//     since appeared, which was the reason the publish invalidated at all.
//   - Everything deeper is still dropped: the listing says nothing about
//     grandchildren, and a child directory may be gone.
//   - The directory's own entry is kept. A listing that arrived is proof the
//     directory exists, and evicting it is what made a repeated `dir` pay a
//     fileinfo GET before its dirinfo GET.
//
//  Two details keep it equivalent to the per-open listing scan in Create.c
//  (FindEntryByName), which it stands in for once the DCB is gone:
//   - Names are matched case-insensitively there, first match winning, files
//     before subdirectories; on a case-sensitive backend two entries can
//     collide. Insertion refreshes an existing entry in place, so entries go
//     in subdirectories-last-first then files-last-first, and the entry
//     FindEntryByName would have returned is the one inserted last.
//   - At most PATH_CACHE_SEED_MAX entries, the first ones in listing order.
//     The cache holds PATH_CACHE_BUCKETS * PATH_CACHE_MAX_PER_BUCKET entries
//     and evicts FIFO per bucket, so seeding a very large directory in full
//     would flush everything else for entries mostly never opened; the rest
//     resolve the way they always did.
//
//  Every entry is inserted with the ticket the listing's fetch was issued
//  under, so a seed that loses a race with an invalidation inserts nothing
//  the invalidation would have removed.
//
//  Runs from DirCtrlComplete at PASSIVE_LEVEL, as every path-cache entry
//  point does (PagedPool, push locks). One scratch path buffer for the whole
//  listing, from pool rather than the stack.
//
VOID BlorgPathCacheSeedListing(const UNICODE_STRING* Dir, PDIRECTORY_INFO Listing, _In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    if (!PathCache.Ready || !Dir || 0 == Dir->Length || !Dir->Buffer)
    {
        return;
    }

    PathCacheInvalidateUnder(Dir, TRUE);

    if (!Listing || Dir->Length + sizeof(WCHAR) > PATH_CACHE_MAX_PATH_BYTES)
    {
        return;
    }

    UNICODE_STRING scratch;
    scratch.Buffer = C_CAST(PWCH, ExAllocatePoolUninitialized(PagedPool, PATH_CACHE_MAX_PATH_BYTES, PATH_CACHE_TAG));

    if (!scratch.Buffer)
    {
        return;
    }

    scratch.MaximumLength = PATH_CACHE_MAX_PATH_BYTES;
    RtlCopyMemory(scratch.Buffer, Dir->Buffer, Dir->Length);

    USHORT dirLength = Dir->Length;

    if (L'\\' != Dir->Buffer[(Dir->Length / sizeof(WCHAR)) - 1])
    {
        scratch.Buffer[dirLength / sizeof(WCHAR)] = L'\\';
        dirLength = C_CAST(USHORT, dirLength + sizeof(WCHAR));
    }

    scratch.Length = dirLength;

    const SIZE_T files = (Listing->FileCount < PATH_CACHE_SEED_MAX) ? Listing->FileCount : PATH_CACHE_SEED_MAX;
    const SIZE_T subDirs = (Listing->SubDirCount < PATH_CACHE_SEED_MAX - files) ? Listing->SubDirCount : PATH_CACHE_SEED_MAX - files;

    DIRECTORY_ENTRY_METADATA meta;
    RtlZeroMemory(&meta, sizeof(meta));
    meta.IsDirectory = TRUE;

    for (SIZE_T i = subDirs; i > 0; i--)
    {
        PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(Listing, i - 1);

        meta.CreationTime = sub->CreationTime;
        meta.LastAccessedTime = sub->LastAccessedTime;
        meta.LastModifiedTime = sub->LastModifiedTime;

        PathCacheSeedEntry(&scratch, dirLength, sub->Name, sub->NameLength, &meta, Ticket);
    }

    meta.IsDirectory = FALSE;

    for (SIZE_T i = files; i > 0; i--)
    {
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(Listing, i - 1);

        meta.Size = file->Size;
        meta.CreationTime = file->CreationTime;
        meta.LastAccessedTime = file->LastAccessedTime;
        meta.LastModifiedTime = file->LastModifiedTime;

        PathCacheSeedEntry(&scratch, dirLength, file->Name, file->NameLength, &meta, Ticket);
    }

    ExFreePool(scratch.Buffer);
}

//
//  Returns a referenced snapshot of Dir's listing, or NULL. Fresh within the
//  path cache's TTL; with AllowStale, also within LISTING_STALE_MAX_100NS,
//  reported through *Stale, and the first lookup to see a given snapshot
//  stale is told through *RefreshOwed that it owes the one background
//  refetch. That claim is an interlocked flag on the entry rather than an
//  exclusive acquire, so stale lookups stay concurrent; the flag is never
//  reset on the entry -- a successful refetch replaces the whole entry, and a
//  failed one leaves this snapshot unrefreshed until it ages out and the
//  next query fetches in the foreground.
//
//  Create.c passes AllowStale = FALSE: a listing it reads answers "not
//  found" outright, and a snapshot older than the TTL must not hide a file
//  the server has since gained.
//
PDIRECTORY_INFO BlorgPathCacheLookupListing(const UNICODE_STRING* Dir, BOOLEAN AllowStale, _Out_opt_ PBOOLEAN Stale, _Out_opt_ PBOOLEAN RefreshOwed)
{
    BOOLEAN stale = FALSE;
    BOOLEAN owed = FALSE;
    PDIRECTORY_INFO listing = NULL;

    if (PathCache.Ready && Dir && 0 < Dir->Length && Dir->Buffer)
    {
        PATH_CACHE_BUCKET* bucket = ListingCacheBucket(Dir);
        ULONG64 now = KeQueryInterruptTime();
        LONG generation = ReadNoFence(&PathCache.Generation);

        KeEnterCriticalRegion();
        ExAcquirePushLockShared(&bucket->Lock);

        for (PLIST_ENTRY e = bucket->List.Flink; e != &bucket->List; e = e->Flink)
        {
            PLISTING_CACHE_ENTRY entry = CONTAINING_RECORD(e, LISTING_CACHE_ENTRY, Link);

            if (!RtlEqualUnicodeString(&entry->Path, Dir, TRUE))
            {
                continue;
            }

            if (ListingCacheEntryLive(entry, now, generation))
            {
                stale = (ListingCacheAge(entry, now) >= C_CAST(ULONG64, LISTING_FRESH_100NS));

                if (!stale || AllowStale)
                {
                    listing = entry->Listing;
                    BlorgReferenceDirectoryInfo(listing);
                    owed = stale && (0 == InterlockedCompareExchange(&entry->RefreshClaimed, 1, 0));
                }
            }

            break;
        }

        ExReleasePushLockShared(&bucket->Lock);
        KeLeaveCriticalRegion();
    }

    if (Stale)
    {
        *Stale = stale && (NULL != listing);
    }

    if (RefreshOwed)
    {
        *RefreshOwed = owed;
    }

    return listing;
}

//
//  Offers a freshly fetched listing to the cache. Refused outright when its
//  ticket has been overtaken by an invalidation. Otherwise it replaces the
//  cached snapshot of the same directory unless that one came from a fetch
//  issued later -- two fetches of one directory can complete in either
//  order, and the later-issued one is the newer truth. The returned
//  BOOLEAN is that verdict: TRUE means the listing is the current truth for
//  Dir, which is what licenses the caller to seed the path cache from it,
//  whether or not the byte budget let the cache keep it.
//
//  Retention: dead entries are reaped from the bucket on the way through,
//  the bucket is capped FIFO, and the oldest entries in this bucket are
//  evicted while the global byte budget would be exceeded; a listing that
//  still does not fit is simply not kept (its caller still owns its own
//  reference). The budget is soft across buckets -- it never evicts from a
//  bucket it does not hold -- which keeps every publish to one lock.
//
//  The entry is built outside the lock, as PathCacheInsert's is.
//
BOOLEAN BlorgPathCachePublishListing(const UNICODE_STRING* Dir, PDIRECTORY_INFO Listing, _In_opt_ const PATH_CACHE_TICKET* Ticket)
{
    if (!PathCache.Ready || !Dir || 0 == Dir->Length || !Dir->Buffer ||
        Dir->Length > PATH_CACHE_MAX_PATH_BYTES || !Listing)
    {
        return FALSE;
    }

    PLISTING_CACHE_ENTRY newEntry = ExAllocatePoolZero(PagedPool, sizeof(LISTING_CACHE_ENTRY), LISTING_CACHE_TAG);

    if (newEntry)
    {
        newEntry->Path.Buffer = ExAllocatePoolUninitialized(PagedPool, Dir->Length, LISTING_CACHE_TAG);

        if (!newEntry->Path.Buffer)
        {
            ExFreePool(newEntry);
            newEntry = NULL;
        }
    }

    ULONG64 now = KeQueryInterruptTime();
    ULONG64 issueTime = Ticket ? Ticket->IssueTime : now;
    LONG generation = ReadNoFence(&PathCache.Generation);
    LONG64 bytes = ListingCacheSizeOf(Listing);

    if (newEntry)
    {
        RtlCopyMemory(newEntry->Path.Buffer, Dir->Buffer, Dir->Length);
        newEntry->Path.Length = Dir->Length;
        newEntry->Path.MaximumLength = Dir->Length;
        newEntry->Listing = Listing;
        newEntry->Bytes = bytes;
        newEntry->IssueTime = issueTime;
        newEntry->Generation = C_CAST(ULONG, generation);
    }

    PATH_CACHE_BUCKET* bucket = ListingCacheBucket(Dir);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    BOOLEAN current = PathCacheTicketHonoured(Ticket);
    PLISTING_CACHE_ENTRY existing = NULL;
    PLIST_ENTRY e = current ? bucket->List.Flink : &bucket->List;

    while (e != &bucket->List)
    {
        PLISTING_CACHE_ENTRY entry = CONTAINING_RECORD(e, LISTING_CACHE_ENTRY, Link);
        PLIST_ENTRY next = e->Flink;

        if (!ListingCacheEntryLive(entry, now, generation))
        {
            ListingCacheRemoveEntry(bucket, entry);
        }
        else if (RtlEqualUnicodeString(&entry->Path, Dir, TRUE))
        {
            existing = entry;
        }

        e = next;
    }

    if (existing && existing->IssueTime > issueTime)
    {
        current = FALSE;
    }
    else if (existing)
    {
        ListingCacheRemoveEntry(bucket, existing);
    }

    if (current && newEntry)
    {
        if (bucket->Count >= LISTING_CACHE_MAX_PER_BUCKET && !IsListEmpty(&bucket->List))
        {
            ListingCacheRemoveEntry(bucket, CONTAINING_RECORD(bucket->List.Flink, LISTING_CACHE_ENTRY, Link));
        }

        while (ReadNoFence64(&PathCache.ListingBytes) + bytes > LISTING_CACHE_MAX_BYTES &&
               !IsListEmpty(&bucket->List))
        {
            ListingCacheRemoveEntry(bucket, CONTAINING_RECORD(bucket->List.Flink, LISTING_CACHE_ENTRY, Link));
        }

        if (ReadNoFence64(&PathCache.ListingBytes) + bytes <= LISTING_CACHE_MAX_BYTES)
        {
            BlorgReferenceDirectoryInfo(Listing);
            InterlockedExchangeAdd64(&PathCache.ListingBytes, bytes);
            InsertTailList(&bucket->List, &newEntry->Link);
            bucket->Count++;
            newEntry = NULL;
        }
    }

    ExReleasePushLockExclusive(&bucket->Lock);
    KeLeaveCriticalRegion();

    if (newEntry)
    {
        ExFreePool(newEntry->Path.Buffer);
        ExFreePool(newEntry);
    }

    return current;
}

//
//  Wholesale flush in O(1) for the path cache: bump the generation so every
//  existing entry is now stale (a miss on lookup, a reap target on the next
//  insert). Memory is reclaimed lazily rather than eagerly, which is fine --
//  the bucket cap still bounds it, and inserts sweep the dead entries as they
//  go. Listings are swept eagerly instead: they are charged against one
//  global byte budget, and a dead listing left charged in a quiet bucket
//  would hold budget every other bucket needs.
//
VOID BlorgPathCacheInvalidateAll(VOID)
{
    PathCacheAdvanceSequence();
    InterlockedIncrement(&PathCache.Generation);

    if (PathCache.Ready)
    {
        ListingCacheDropUnder(NULL);
    }
}
