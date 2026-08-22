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

#include <gtest/gtest.h>

extern "C" {
#include "..\Driver.h"
#include "Scheduler.h"
}

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

    PathCacheInsertExists(&proof->Inserted, &proof->Meta);

    InterlockedIncrement(&proof->InsertRan);
}

void InvalidateThread(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    PathCacheInvalidate(&proof->Invalidated);

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
    PathCacheLookup(&proof->Inserted, &out);

    InterlockedIncrement(&proof->LookupRan);
}

void PathCacheProofSetup(void* Parameter)
{
    PathCacheProof* proof = (PathCacheProof*)Parameter;

    ShimReset();
    PathCacheInit();

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
    PathCacheInsertExists(&proof->Invalidated, &proof->Meta);

    KmSchedSpawn(InsertThread, proof);
    KmSchedSpawn(InvalidateThread, proof);
    KmSchedSpawn(LookupThread, proof);
}

void PathCacheProofTeardown(void* Parameter)
{
    (void)Parameter;

    PathCacheCleanup();
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
        KmExploreInterleavings(PathCacheProofSetup, PathCacheProofTeardown, &proof, 20000);

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 20000)
        << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_GT(proof.InsertRan, 0) << "no schedule ever ran the insert";
    EXPECT_GT(proof.InvalidateRan, 0) << "no schedule ever ran the invalidate";
    EXPECT_GT(proof.LookupRan, 0) << "no schedule ever ran the lookup";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

} // namespace
