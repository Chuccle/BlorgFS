//
// Tests for GhostCache.c, the memory-only model of an on-disk block cache.
//
// The model's output is a hit rate that decides whether the real cache is
// worth building, so the defects worth defending against are the ones that
// would inflate it: counting a block as cached before a real cache could
// hold it, keeping a block across a change to the file, or binning a hit
// into a smaller capacity than it needed. Each of those still produces
// plausible counters, which is why these assert on exact values rather
// than on "some hits happened".
//
// Driven through BlorgGhostCacheObserve directly rather than through a
// read IRP. Read.c's only job is to call it once per fetch with the FCB's
// path and validator; everything that can be wrong about the numbers is
// in here, and the dispatch tests already cover which reads fetch.
//
// Links the real Statistics.c, whose process-lifetime table
// StatisticsTest.cpp's environment sets up, and reads the counters back
// through BlorgStatisticsQuery the way the harness does.
//

#include <gtest/gtest.h>

#include <cwchar>

extern "C" {
#include "..\..\src\Driver.h"
}

namespace
{

constexpr ULONG64 Kb = 1024;
constexpr ULONG64 Gb = 1024ull * 1024ull * 1024ull;

UNICODE_STRING MakePath(const wchar_t* Text)
{
    UNICODE_STRING path;
    path.Buffer = const_cast<PWCH>(Text);
    path.Length = static_cast<USHORT>(wcslen(Text) * sizeof(WCHAR));
    path.MaximumLength = path.Length;
    return path;
}

class GhostCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        BlorgStatisticsReset();
        ASSERT_EQ(STATUS_SUCCESS, BlorgGhostCacheInitialize(4));
    }

    void TearDown() override
    {
        BlorgGhostCacheCleanup();
    }

    static void Fetch(const wchar_t* File, ULONG64 Offset, ULONG Length, BOOLEAN Demand = TRUE, ULONG64 Modified = 1)
    {
        UNICODE_STRING path = MakePath(File);
        BlorgGhostCacheObserve(&path, 8 * Gb, Modified, Offset, Length, Demand);
    }

    static BLORGFS_STATISTICS Totals()
    {
        BLORGFS_STATISTICS_RESPONSE response;
        BlorgStatisticsQuery(&response);
        return response.Totals;
    }

    static ULONG64 Hits(const BLORGFS_STATISTICS& T, int Policy)
    {
        ULONG64 sum = 0;

        for (int i = 0; i < BLORGFS_GHOST_DISTANCE_BUCKETS; ++i)
        {
            sum += T.GhostHitFetches[Policy][i];
        }

        return sum;
    }
};

//
// Off is the shipped default, and off has to mean the read path pays
// nothing and reports nothing -- a zero GhostFetches is how the harness
// tells "not measured" from "no hits".
//
TEST(GhostCacheOffTest, ObserveIsANoOpUntilATableIsAllocated)
{
    BlorgStatisticsReset();
    ASSERT_EQ(STATUS_SUCCESS, BlorgGhostCacheInitialize(0));

    UNICODE_STRING path = MakePath(L"\\a.mkv");
    BlorgGhostCacheObserve(&path, Gb, 1, 0, 128 * Kb, TRUE);

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);
    EXPECT_EQ(0u, response.Totals.GhostFetches);
}

TEST(GhostCacheOffTest, AFailedAllocationLeavesTheModelOff)
{
    BlorgStatisticsReset();

    ShimPoolFailAt(0);
    NTSTATUS status = BlorgGhostCacheInitialize(4);
    ShimPoolFailAt(-1);

    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, status);

    UNICODE_STRING path = MakePath(L"\\a.mkv");
    BlorgGhostCacheObserve(&path, Gb, 1, 0, 128 * Kb, TRUE);

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);
    EXPECT_EQ(0u, response.Totals.GhostFetches);

    BlorgGhostCacheCleanup();
}

//
// The two admission policies on the simplest history there is: the same
// 128 KB fetched three times. First-miss admission writes on the first
// fetch and hits from the second; second-miss admission writes on the
// second and hits only on the third. Getting the policies' off-by-one
// wrong is exactly the error that would make second-miss admission look
// free.
//
TEST_F(GhostCacheTest, FirstMissAdmissionHitsOnTheSecondFetchAndSecondMissOnTheThird)
{
    Fetch(L"\\film.mkv", 0, 128 * Kb);

    BLORGFS_STATISTICS t = Totals();
    EXPECT_EQ(0u, Hits(t, 0));
    EXPECT_EQ(0u, Hits(t, 1));
    EXPECT_EQ(128 * Kb, t.GhostAdmitBytes[0]);
    EXPECT_EQ(0u, t.GhostAdmitBytes[1]);

    Fetch(L"\\film.mkv", 0, 128 * Kb);

    t = Totals();
    EXPECT_EQ(1u, t.GhostHitFetches[0][0]);
    EXPECT_EQ(128 * Kb, t.GhostHitBytes[0][0]);
    EXPECT_EQ(0u, Hits(t, 1));
    EXPECT_EQ(128 * Kb, t.GhostAdmitBytes[0]);
    EXPECT_EQ(128 * Kb, t.GhostAdmitBytes[1]);

    Fetch(L"\\film.mkv", 0, 128 * Kb);

    t = Totals();
    EXPECT_EQ(2u, t.GhostHitFetches[0][0]);
    EXPECT_EQ(1u, t.GhostHitFetches[1][0]);
    EXPECT_EQ(3u, t.GhostFetches);
    EXPECT_EQ(3 * 128 * Kb, t.GhostFetchBytes);
}

//
// A changed file must not hit. Without the validator in the key the model
// would credit the cache with serving yesterday's bytes, which a real
// cache must never do and so must never be counted as a win.
//
TEST_F(GhostCacheTest, AChangedModifiedTimeIsADifferentFile)
{
    Fetch(L"\\film.mkv", 0, 128 * Kb, TRUE, 1);
    Fetch(L"\\film.mkv", 0, 128 * Kb, TRUE, 2);

    EXPECT_EQ(0u, Hits(Totals(), 0));
}

//
// A real cache admits whole blocks, so a fetch that only covered part of
// one leaves nothing servable. A model that counted the block as held on
// any touch would report a 4 KB demand fault followed by a different 4 KB
// in the same 64 KB as a hit that no cache could have delivered.
//
TEST_F(GhostCacheTest, ABlockOnlyPartlyFetchedIsNotHeld)
{
    Fetch(L"\\film.mkv", 0, 4 * Kb);
    Fetch(L"\\film.mkv", 8 * Kb, 4 * Kb);

    BLORGFS_STATISTICS t = Totals();
    EXPECT_EQ(0u, Hits(t, 0));
    EXPECT_EQ(0u, t.GhostPartialFetches[0]);

    Fetch(L"\\film.mkv", 0, 64 * Kb);
    Fetch(L"\\film.mkv", 8 * Kb, 4 * Kb);

    EXPECT_EQ(1u, Hits(Totals(), 0));
}

//
// A fetch needing two blocks when only one is held is a partial, not a
// hit: the first version of the real cache sends it to the network whole.
//
TEST_F(GhostCacheTest, AFetchWithSomeBlocksHeldIsAPartialNotAHit)
{
    Fetch(L"\\film.mkv", 0, 64 * Kb);
    Fetch(L"\\film.mkv", 0, 128 * Kb);

    BLORGFS_STATISTICS t = Totals();
    EXPECT_EQ(0u, Hits(t, 0));
    EXPECT_EQ(1u, t.GhostPartialFetches[0]);
    EXPECT_EQ(0u, t.GhostPartialFetches[1]);
}

//
// Only fetches somebody was blocked on count as demand hits -- those are
// the stalls a cache removes, and the number that says whether a user
// would feel it. A speculative hit only saves bandwidth.
//
TEST_F(GhostCacheTest, SpeculativeHitsAreNotCountedAsDemandHits)
{
    Fetch(L"\\film.mkv", 0, 128 * Kb, FALSE);
    Fetch(L"\\film.mkv", 0, 128 * Kb, FALSE);
    Fetch(L"\\film.mkv", 0, 128 * Kb, TRUE);

    BLORGFS_STATISTICS t = Totals();
    EXPECT_EQ(2u, t.GhostHitFetches[0][0]);
    EXPECT_EQ(1u, t.GhostHitDemandFetches[0][0]);
    EXPECT_EQ(1u, t.GhostDemandFetches);
}

//
// The capacity a hit is binned under is how much was fetched in between.
// Two gigabytes of another file between the two fetches of this one puts
// the hit in the 1-4 GB bucket: a 1 GB cache would have evicted it. A
// model that binned by the block's own age in fetches, or by the distance
// of its freshest block rather than its stalest, lands it in bucket 0 and
// overstates what a small cache buys.
//
TEST_F(GhostCacheTest, AHitIsBinnedByEverythingFetchedSince)
{
    Fetch(L"\\film.mkv", 0, 128 * Kb);
    Fetch(L"\\other.mkv", 0, static_cast<ULONG>(2 * Gb - 64 * Kb));
    Fetch(L"\\film.mkv", 0, 128 * Kb);

    BLORGFS_STATISTICS t = Totals();
    EXPECT_EQ(0u, t.GhostHitFetches[0][0]);
    EXPECT_EQ(1u, t.GhostHitFetches[0][1]);
}

} // namespace
