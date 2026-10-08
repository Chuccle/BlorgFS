//
// Exhaustive interleaving proof of PathCache.c's cross-shard concurrency,
// on the real PathCache.c.
//
// Each bucket is guarded by its own EX_PUSH_LOCK, so within one bucket the
// lock already serializes everything -- that is an ordinary instance of
// the push-lock protocol this project has proven correct elsewhere. The
// risk specific to sharding is different: PathCache.Count is one counter
// shared by every bucket, updated via a plain Interlocked op from inside
// whichever bucket's lock happens to be held, so two threads working
// entirely different buckets -- different locks, no shared critical
// section -- must still never lose an update to it.
//
// This drives three threads across the real public API -- an insert, a
// targeted invalidate of an unrelated path, and a racing lookup -- through
// every interleaving the scheduler can construct, rather than the one or
// two orderings a hand-written test would think to try.
//
// The invalidate is the long thread: it takes its path bucket and then the
// listing buckets of the path and of its parent, three lock pairs to the
// others' one each. Every acquire and release is a scheduling point, so the
// space is at most 13!/(3! 3! 7!) = 34320 schedules -- past the 1680 it was
// before invalidation dropped listings, and past a cap sized for that.
//
// The proofs after it cover the two places the listing cache depends on an
// ordering: the invalidation ticket, and the one-shot refresh claim.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "Scheduler.h"
}

#include "ListingBuilder.h"

namespace
{

struct PathCacheProof
{
    UNICODE_STRING Inserted;
    wchar_t InsertedBuffer[32];

    UNICODE_STRING Invalidated;
    wchar_t InvalidatedBuffer[32];

    DIRECTORY_ENTRY_METADATA Meta;

    volatile long InsertRan;
    volatile long InvalidateRan;
    volatile long LookupRan;
};

void InsertThread(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    BlorgPathCacheInsertExists(&proof->Inserted, &proof->Meta, nullptr);

    InterlockedIncrement(&proof->InsertRan);
}

void InvalidateThread(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    BlorgPathCacheInvalidate(&proof->Invalidated);

    InterlockedIncrement(&proof->InvalidateRan);
}

//
// Races a lookup of the path the other thread is concurrently inserting.
// The outcome (hit or miss) is not asserted -- it legitimately depends on
// the interleaving -- only that taking the bucket lock shared while
// another thread holds it (or is about to take it exclusive) never
// corrupts anything, which the model's own push-lock protocol checks
// enforce by aborting the run.
//
void LookupThread(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    DIRECTORY_ENTRY_METADATA out = {};
    BlorgPathCacheLookup(&proof->Inserted, &out);

    InterlockedIncrement(&proof->LookupRan);
}

void PathCacheProofSetup(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    ShimReset();
    BlorgPathCacheInit();

    proof->InsertRan = 0;
    proof->InvalidateRan = 0;
    proof->LookupRan = 0;

    wcscpy_s(proof->InsertedBuffer, L"\\media\\shard-a\\reel.mkv");
    proof->Inserted.Buffer = proof->InsertedBuffer;
    proof->Inserted.Length = (USHORT)(wcslen(proof->InsertedBuffer) * sizeof(wchar_t));
    proof->Inserted.MaximumLength = proof->Inserted.Length;

    // A different directory, and a different string length, to bias this
    // toward a different bucket than Inserted rather than colliding on it
    // by construction -- the cross-shard case is the one under proof.
    wcscpy_s(proof->InvalidatedBuffer, L"\\media\\shard-b\\other-reel.mkv");
    proof->Invalidated.Buffer = proof->InvalidatedBuffer;
    proof->Invalidated.Length = (USHORT)(wcslen(proof->InvalidatedBuffer) * sizeof(wchar_t));
    proof->Invalidated.MaximumLength = proof->Invalidated.Length;

    proof->Meta = {};
    proof->Meta.Size = 4096;

    // Pre-populate the path the invalidate thread targets, so the race is
    // "invalidate races a concurrent unrelated insert", not "invalidate a
    // path that was never cached to begin with".
    BlorgPathCacheInsertExists(&proof->Invalidated, &proof->Meta, nullptr);

    KmSchedSpawn(InsertThread, proof);
    KmSchedSpawn(InvalidateThread, proof);
    KmSchedSpawn(LookupThread, proof);
}

void PathCacheProofTeardown(void* Parameter)
{
    (void)Parameter;

    BlorgPathCacheCleanup();
}

class PathCacheSchedTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("PathCacheSchedTest teardown");
    }
};

TEST_F(PathCacheSchedTest, NoInterleavingOfCrossShardOpsCorruptsState)
{
    static PathCacheProof proof;

    proof = {};

    KM_SCHED_RESULT result =
        KmExploreInterleavings(PathCacheProofSetup, PathCacheProofTeardown, &proof, 100000);

    //
    // ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
    // assertion after this one would run against corrupted state.
    //
    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 100000)
        << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_GT(proof.InsertRan, 0) << "no schedule ever ran the insert";
    EXPECT_GT(proof.InvalidateRan, 0) << "no schedule ever ran the invalidate";
    EXPECT_GT(proof.LookupRan, 0) << "no schedule ever ran the lookup";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

//
// The ticket protocol (BlorgPathCacheTakeTicket), across every interleaving:
// a reader takes its ticket, reads, and inserts what it read, while another
// thread invalidates the same directory. Whatever the order, a result whose
// ticket predates the invalidation must not be in the cache once both
// threads finish -- the cache would otherwise serve, for a full TTL, what
// the server said before the change it was just told about. Both the path
// cache insert and the listing publish carry the ticket, and both are
// checked: the listing is the one a re-list serves, the path entry the one
// an open does.
//
// Checked after the threads finish, against a ticket taken then: an entry
// may be present only if the reader's ticket is as new as the final
// sequence. A single-threaded test can only show the refusal for one fixed
// order; the window that matters is the invalidation landing between the
// ticket and the bucket lock, which only the scheduler reaches reliably.
//
struct TicketProof
{
    UNICODE_STRING Dir;
    wchar_t DirBuffer[32];

    UNICODE_STRING Child;
    wchar_t ChildBuffer[48];

    DIRECTORY_ENTRY_METADATA Meta;
    PDIRECTORY_INFO Listing;
    PATH_CACHE_TICKET Ticket;

    volatile long ReaderRan;
    volatile long InvalidateRan;
    long Violations;
};

void TicketReaderThread(void* Parameter)
{
    TicketProof* proof = (TicketProof*)Parameter;

    BlorgPathCacheTakeTicket(&proof->Ticket);
    BlorgPathCacheInsertExists(&proof->Child, &proof->Meta, &proof->Ticket);
    BlorgPathCachePublishListing(&proof->Dir, proof->Listing, &proof->Ticket);
    BlorgReleaseDirectoryInfo(proof->Listing);

    InterlockedIncrement(&proof->ReaderRan);
}

void TicketInvalidateThread(void* Parameter)
{
    TicketProof* proof = (TicketProof*)Parameter;

    BlorgPathCacheInvalidate(&proof->Child);

    InterlockedIncrement(&proof->InvalidateRan);
}

void TicketProofSetup(void* Parameter)
{
    TicketProof* proof = (TicketProof*)Parameter;

    ShimReset();
    BlorgPathCacheInit();

    wcscpy_s(proof->DirBuffer, L"\\media\\tickets");
    proof->Dir.Buffer = proof->DirBuffer;
    proof->Dir.Length = (USHORT)(wcslen(proof->DirBuffer) * sizeof(wchar_t));
    proof->Dir.MaximumLength = proof->Dir.Length;

    wcscpy_s(proof->ChildBuffer, L"\\media\\tickets\\file0.bin");
    proof->Child.Buffer = proof->ChildBuffer;
    proof->Child.Length = (USHORT)(wcslen(proof->ChildBuffer) * sizeof(wchar_t));
    proof->Child.MaximumLength = proof->Child.Length;

    proof->Meta = {};
    proof->Meta.Size = 1000;
    proof->Listing = BuildSyntheticListing(1, 0);

    KmSchedSpawn(TicketReaderThread, proof);
    KmSchedSpawn(TicketInvalidateThread, proof);
}

void TicketProofTeardown(void* Parameter)
{
    TicketProof* proof = (TicketProof*)Parameter;

    PATH_CACHE_TICKET current;
    BlorgPathCacheTakeTicket(&current);

    const bool readerCurrent = (proof->Ticket.Sequence == current.Sequence);

    PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&proof->Dir, TRUE, nullptr, nullptr, nullptr);

    if (listing && !readerCurrent)
    {
        proof->Violations++;
    }

    BlorgReleaseDirectoryInfo(listing);

    if ((PathCacheMiss != BlorgPathCacheLookup(&proof->Child, nullptr)) && !readerCurrent)
    {
        proof->Violations++;
    }

    BlorgPathCacheCleanup();
}

TEST_F(PathCacheSchedTest, NoInterleavingKeepsAResultReadBeforeAnInvalidation)
{
    static TicketProof proof;

    proof = {};

    KM_SCHED_RESULT result =
        KmExploreInterleavings(TicketProofSetup, TicketProofTeardown, &proof, 20000);

    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, result.Truncated);
    EXPECT_LT(result.Schedules, 20000) << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_EQ(0, proof.Violations)
        << "a result read before an invalidation was still cached after it";
    EXPECT_GT(proof.ReaderRan, 0);
    EXPECT_GT(proof.InvalidateRan, 0);

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

//
// An open the path cache misses resolves from its parent's cached listing
// and inserts what it found (Create.c), while the feed reports a change to
// that child. The invalidation advances the sequence, sweeps the child's
// bucket, then drops the parent's listing, each under its own lock; an open
// that reads the listing in between reads what preceded the change. Taking
// a ticket at the lookup would date that read after the invalidation, and
// the insert would outlive it. Every result the open could insert here
// came from the one listing, which predates the change, so once both
// threads finish the child must not be cached.
//
struct ListingProvenanceProof
{
    UNICODE_STRING Dir;
    wchar_t DirBuffer[32];

    UNICODE_STRING Child;
    wchar_t ChildBuffer[48];

    volatile long OpenRan;
    volatile long InvalidateRan;
    volatile long ResolvedFromListing;
    long Violations;
};

void ListingProvenanceOpenThread(void* Parameter)
{
    ListingProvenanceProof* proof = (ListingProvenanceProof*)Parameter;

    PATH_CACHE_TICKET ticket;
    BlorgPathCacheTakeTicket(&ticket);

    PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&proof->Dir, FALSE, nullptr, nullptr, &ticket);

    if (listing)
    {
        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = BlorgGetFileEntry(listing, 0)->Size;
        BlorgReleaseDirectoryInfo(listing);

        BlorgPathCacheInsertExists(&proof->Child, &meta, &ticket);
        InterlockedIncrement(&proof->ResolvedFromListing);
    }

    InterlockedIncrement(&proof->OpenRan);
}

void ListingProvenanceInvalidateThread(void* Parameter)
{
    ListingProvenanceProof* proof = (ListingProvenanceProof*)Parameter;

    BlorgPathCacheInvalidate(&proof->Child);

    InterlockedIncrement(&proof->InvalidateRan);
}

void ListingProvenanceSetup(void* Parameter)
{
    ListingProvenanceProof* proof = (ListingProvenanceProof*)Parameter;

    ShimReset();
    BlorgPathCacheInit();

    wcscpy_s(proof->DirBuffer, L"\\media\\provenance");
    proof->Dir.Buffer = proof->DirBuffer;
    proof->Dir.Length = (USHORT)(wcslen(proof->DirBuffer) * sizeof(wchar_t));
    proof->Dir.MaximumLength = proof->Dir.Length;

    wcscpy_s(proof->ChildBuffer, L"\\media\\provenance\\file0.bin");
    proof->Child.Buffer = proof->ChildBuffer;
    proof->Child.Length = (USHORT)(wcslen(proof->ChildBuffer) * sizeof(wchar_t));
    proof->Child.MaximumLength = proof->Child.Length;

    PATH_CACHE_TICKET fetched;
    BlorgPathCacheTakeTicket(&fetched);

    PDIRECTORY_INFO listing = BuildSyntheticListing(1, 0);
    BlorgPathCachePublishListing(&proof->Dir, listing, &fetched);
    BlorgReleaseDirectoryInfo(listing);

    KmSchedSpawn(ListingProvenanceOpenThread, proof);
    KmSchedSpawn(ListingProvenanceInvalidateThread, proof);
}

void ListingProvenanceTeardown(void* Parameter)
{
    ListingProvenanceProof* proof = (ListingProvenanceProof*)Parameter;

    if (PathCacheMiss != BlorgPathCacheLookup(&proof->Child, nullptr))
    {
        proof->Violations++;
    }

    BlorgPathCacheCleanup();
}

TEST_F(PathCacheSchedTest, NoInterleavingCachesAChildFromAListingAnInvalidationOvertook)
{
    static ListingProvenanceProof proof;

    proof = {};

    KM_SCHED_RESULT result =
        KmExploreInterleavings(ListingProvenanceSetup, ListingProvenanceTeardown, &proof, 20000);

    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, result.Truncated);
    EXPECT_LT(result.Schedules, 20000) << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_EQ(0, proof.Violations)
        << "a child read from a listing that predates an invalidation was cached after it";
    EXPECT_GT(proof.ResolvedFromListing, 0) << "no schedule resolved the open from the listing";
    EXPECT_GT(proof.OpenRan, 0);
    EXPECT_GT(proof.InvalidateRan, 0);

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

//
// Every query that finds a listing stale races to claim its one refresh
// under the shared bucket lock, so the claim itself has to be the arbiter:
// two lookups holding the lock shared together must not both come away
// owing it (two requests where one was meant) or both come away not owing
// it (a stale listing nobody refreshes until it ages out).
//
struct RefreshClaimProof
{
    UNICODE_STRING Dir;
    wchar_t DirBuffer[32];

    volatile long Owed;
    volatile long LookupsRan;
    long Violations;
};

void RefreshClaimThread(void* Parameter)
{
    RefreshClaimProof* proof = (RefreshClaimProof*)Parameter;

    BOOLEAN stale = FALSE;
    BOOLEAN owed = FALSE;
    PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&proof->Dir, TRUE, &stale, &owed, nullptr);

    if (owed)
    {
        InterlockedIncrement(&proof->Owed);
    }

    BlorgReleaseDirectoryInfo(listing);

    InterlockedIncrement(&proof->LookupsRan);
}

void RefreshClaimSetup(void* Parameter)
{
    RefreshClaimProof* proof = (RefreshClaimProof*)Parameter;

    ShimReset();
    BlorgPathCacheInit();

    proof->Owed = 0;

    wcscpy_s(proof->DirBuffer, L"\\media\\stale");
    proof->Dir.Buffer = proof->DirBuffer;
    proof->Dir.Length = (USHORT)(wcslen(proof->DirBuffer) * sizeof(wchar_t));
    proof->Dir.MaximumLength = proof->Dir.Length;

    PDIRECTORY_INFO listing = BuildSyntheticListing(1, 0);
    BlorgPathCachePublishListing(&proof->Dir, listing, nullptr);
    BlorgReleaseDirectoryInfo(listing);

    // Past the 4-second fresh window, inside the 30-second stale one.
    ShimAdvanceInterruptTime(5ULL * 10ULL * 1000ULL * 1000ULL);

    KmSchedSpawn(RefreshClaimThread, proof);
    KmSchedSpawn(RefreshClaimThread, proof);
}

void RefreshClaimTeardown(void* Parameter)
{
    RefreshClaimProof* proof = (RefreshClaimProof*)Parameter;

    if (1 != proof->Owed)
    {
        proof->Violations++;
    }

    BlorgPathCacheCleanup();
}

TEST_F(PathCacheSchedTest, ConcurrentStaleLookupsOweExactlyOneRefresh)
{
    static RefreshClaimProof proof;

    proof = {};

    KM_SCHED_RESULT result =
        KmExploreInterleavings(RefreshClaimSetup, RefreshClaimTeardown, &proof, 20000);

    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, result.Truncated);
    EXPECT_LT(result.Schedules, 20000) << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_EQ(0, proof.Violations) << "a stale listing's refresh was owed zero or two times";
    EXPECT_GT(proof.LookupsRan, 0);

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}



} // namespace
