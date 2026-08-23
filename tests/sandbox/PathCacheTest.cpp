//
// Functional tests for PathCache.c: the sharded full-path resolution
// cache wired into the create path (via BlorgPathCacheLookup/InsertExists/
// InsertNotFound) and into BlorgDirComplete (via BlorgPathCacheInvalidatePrefix
// on a listing refresh). Exercised here through its own public API rather
// than through IRP dispatch -- the cache's state machine (TTL, targeted/
// prefix invalidation, per-bucket FIFO eviction) is what OpenCppCoverage
// showed as never exercised at all, compile-only.
//

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "..\..\src\Driver.h"
}

namespace
{

//
// BlorgPathCacheInit/Cleanup re-initialize all 256 bucket push locks, which is
// only cheap under KmExploreInterleavings' explicit "recycle lock ids for
// the duration of the exploration" allowance (Scheduler.h). Outside that,
// recycling is off by default once any sched-test in this binary has run
// (Scheduler.c leaves it off after KmExploreInterleavings returns, so a
// later reuse doesn't mask a real double-init), so calling Init/Cleanup
// once per test here would mint 256 fresh ids every time and exhaust the
// model's fixed-size lock table. Bracketing the whole process with one
// Initialize/Cleanup pair -- the same pattern StatisticsTest.cpp uses for
// its own process-lifetime table -- keeps this to a one-time cost.
//
class PathCacheEnvironment : public ::testing::Environment
{
public:
    void SetUp() override { BlorgPathCacheInit(); }
    void TearDown() override { BlorgPathCacheCleanup(); }
};

::testing::Environment* const g_pathCacheEnvironment =
    ::testing::AddGlobalTestEnvironment(new PathCacheEnvironment());

class PathCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Logically clears every prior test's entries in O(1) -- a
        // generation bump rather than a re-init -- so tests stay isolated
        // without touching lock identities. Each test also uses paths no
        // other test uses, so this is a belt-and-braces reset rather than
        // a load-bearing one.
        BlorgPathCacheInvalidateAll();
    }
};

DIRECTORY_ENTRY_METADATA MakeMeta(ULONG64 Size, BOOLEAN IsDirectory)
{
    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = Size;
    meta.CreationTime = 100;
    meta.LastAccessedTime = 200;
    meta.LastModifiedTime = 300;
    meta.IsDirectory = IsDirectory;
    return meta;
}

TEST_F(PathCacheTest, InsertExistsThenLookupHitsWithMetadataWithinTtl)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\alpha.mkv");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(123456789ULL, FALSE);

    BlorgPathCacheInsertExists(&path, &meta);

    DIRECTORY_ENTRY_METADATA out = {};
    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&path, &out));
    EXPECT_EQ(meta.Size, out.Size);
    EXPECT_EQ(meta.CreationTime, out.CreationTime);
    EXPECT_EQ(meta.LastAccessedTime, out.LastAccessedTime);
    EXPECT_EQ(meta.LastModifiedTime, out.LastModifiedTime);
    EXPECT_EQ(meta.IsDirectory, out.IsDirectory);
}

TEST_F(PathCacheTest, InsertNotFoundThenLookupReturnsNotFoundWithinTtl)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\missing.mkv");

    BlorgPathCacheInsertNotFound(&path);

    EXPECT_EQ(PathCacheNotFound, BlorgPathCacheLookup(&path, nullptr));
}

TEST_F(PathCacheTest, LookupOfNeverInsertedPathMisses)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\never-inserted.mkv");

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&path, nullptr));
}

//
// A second insert of an already-cached path refreshes the existing entry
// in place (new metadata, new expiry) rather than appending a duplicate --
// PathCacheInsert's "entry already in this bucket" branch.
//
TEST_F(PathCacheTest, ReinsertingACachedPathRefreshesItInPlace)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\epsilon.mkv");
    DIRECTORY_ENTRY_METADATA first = MakeMeta(111, FALSE);
    DIRECTORY_ENTRY_METADATA second = MakeMeta(222, TRUE);

    BlorgPathCacheInsertExists(&path, &first);
    BlorgPathCacheInsertExists(&path, &second);

    DIRECTORY_ENTRY_METADATA out = {};
    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&path, &out));
    EXPECT_EQ(second.Size, out.Size) << "the refresh must overwrite the stale metadata";
    EXPECT_EQ(second.IsDirectory, out.IsDirectory);
}

//
// KeQueryInterruptTime's backing counter ticks once per call rather than
// with wall-clock time, so reaching a 4-second TTL by calling it in a loop
// is not practical -- ShimAdvanceInterruptTime (DispatchModel.c) jumps it
// directly instead.
//
TEST_F(PathCacheTest, LookupAfterTtlExpiryMisses)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\beta.mkv");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(42, FALSE);

    BlorgPathCacheInsertExists(&path, &meta);
    ASSERT_EQ(PathCacheExists, BlorgPathCacheLookup(&path, nullptr))
        << "sanity: the entry must be live before it can prove expiry";

    // PATH_CACHE_TTL_100NS is 4 seconds; 60 seconds clears it with margin.
    ShimAdvanceInterruptTime(60ULL * 10ULL * 1000ULL * 1000ULL);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&path, nullptr));
}

TEST_F(PathCacheTest, TargetedInvalidationRemovesExactlyThatPath)
{
    UNICODE_STRING victim = RTL_CONSTANT_STRING(L"\\media\\movies\\gamma.mkv");
    UNICODE_STRING bystander = RTL_CONSTANT_STRING(L"\\media\\movies\\delta.mkv");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(7, FALSE);

    BlorgPathCacheInsertExists(&victim, &meta);
    BlorgPathCacheInsertExists(&bystander, &meta);

    BlorgPathCacheInvalidate(&victim);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&victim, nullptr));
    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&bystander, nullptr))
        << "invalidating one path evicted an unrelated one";
}

TEST_F(PathCacheTest, InvalidatingAnUncachedPathIsANoOp)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\media\\movies\\never-cached.mkv");

    BlorgPathCacheInvalidate(&path);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&path, nullptr));
}

//
// \Foobar is a distinct sibling of \Foo, not a member of its subtree --
// PathCacheIsUnder's own boundary check exists precisely to keep a plain
// prefix match from confusing the two.
//
TEST_F(PathCacheTest, PrefixInvalidationRemovesSubtreeButNotSiblings)
{
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\media\\movies\\Foo");
    UNICODE_STRING dirItself = RTL_CONSTANT_STRING(L"\\media\\movies\\Foo");
    UNICODE_STRING inDirA = RTL_CONSTANT_STRING(L"\\media\\movies\\Foo\\reel1.mkv");
    UNICODE_STRING inDirB = RTL_CONSTANT_STRING(L"\\media\\movies\\Foo\\reel2.mkv");
    UNICODE_STRING sibling = RTL_CONSTANT_STRING(L"\\media\\movies\\Foobar\\reel1.mkv");
    UNICODE_STRING unrelated = RTL_CONSTANT_STRING(L"\\media\\movies\\Bar\\reel1.mkv");

    DIRECTORY_ENTRY_METADATA meta = MakeMeta(9, FALSE);
    DIRECTORY_ENTRY_METADATA dirMeta = MakeMeta(0, TRUE);

    BlorgPathCacheInsertExists(&inDirA, &meta);
    BlorgPathCacheInsertExists(&inDirB, &meta);
    BlorgPathCacheInsertExists(&dirItself, &dirMeta);
    BlorgPathCacheInsertExists(&sibling, &meta);
    BlorgPathCacheInsertExists(&unrelated, &meta);

    BlorgPathCacheInvalidatePrefix(&dir);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&inDirA, nullptr));
    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&inDirB, nullptr));
    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&dirItself, nullptr))
        << "the directory itself is within its own prefix";

    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&sibling, nullptr))
        << "\\Foobar is not under \\Foo -- a prefix match without the "
           "boundary check would wrongly evict it";
    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&unrelated, nullptr));
}

//
// The volume root is a real, reachable value for this API, not a
// hypothetical: Driver.c creates the root DCB with
// RTL_CONSTANT_STRING(L"\\"), and BlorgDirComplete invalidates with
// BlorgPathCacheInvalidatePrefix(&dcb->FullPath) on every listing publish --
// so a refresh of the volume root passes exactly this Dir.
//
// PathCacheIsUnder's boundary check reads Path->Buffer[Dir->Length /
// sizeof(WCHAR)] and requires it to be '\'. For Dir = "\" that index is
// 1, which for "\alpha.bin" is 'a' -- so every root-level child fails
// the check and the whole subtree sweep silently matches nothing but the
// literal "\" itself. The trailing separator the root shares with the
// first character of every path beneath it is the special case: for
// "\media" the same index lands on the separator *between* the directory
// and the leaf, which is why every non-root directory works.
//
// Consequence in the driver: the stale-negative protection
// BlorgDirComplete documents ("a stale not-found memoized before the
// file appeared on the backend would otherwise shadow the new listing
// until its TTL lapses") does not apply to files in the volume root. A
// file created on the backend after a failed open stays unopenable --
// visible in the directory listing, STATUS_OBJECT_NAME_NOT_FOUND on
// open -- until the TTL expires on its own.
//
TEST_F(PathCacheTest, RootPrefixInvalidationRemovesItsChildren)
{
    UNICODE_STRING root = RTL_CONSTANT_STRING(L"\\");
    UNICODE_STRING rootChild = RTL_CONSTANT_STRING(L"\\rootlevel.bin");
    UNICODE_STRING deeperChild = RTL_CONSTANT_STRING(L"\\media\\nested.bin");

    BlorgPathCacheInsertNotFound(&rootChild);
    BlorgPathCacheInsertNotFound(&deeperChild);

    ASSERT_EQ(PathCacheNotFound, BlorgPathCacheLookup(&rootChild, nullptr))
        << "sanity: the negative entry must be live before invalidation can prove anything";

    BlorgPathCacheInvalidatePrefix(&root);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&rootChild, nullptr))
        << "a root listing refresh must drop memoized results for root-level children -- "
           "otherwise a file that has since appeared on the backend stays unopenable "
           "until the TTL lapses, while being visible in the listing";
    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&deeperChild, nullptr))
        << "the root's subtree is the whole volume, so deeper paths must go too";
}

//
// The non-root case, as a control: identical shape, one path component
// deeper. This one passes both before and after the PathCacheIsUnder fix,
// which is what localizes the defect above to the root specifically
// rather than to prefix invalidation generally.
//
TEST_F(PathCacheTest, NonRootPrefixInvalidationRemovesItsChildren)
{
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\media");
    UNICODE_STRING child = RTL_CONSTANT_STRING(L"\\media\\controlcase.bin");

    BlorgPathCacheInsertNotFound(&child);
    ASSERT_EQ(PathCacheNotFound, BlorgPathCacheLookup(&child, nullptr));

    BlorgPathCacheInvalidatePrefix(&dir);

    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&child, nullptr));
}

//
// Every bucket caps its own occupancy and evicts FIFO once full (see
// PathCache.c's PATH_CACHE_MAX_PER_BUCKET). Driving far more distinct
// paths than any plausible cap through the public API proves the cache
// stays bounded without depending on that constant's exact value.
//
TEST_F(PathCacheTest, InsertUnderPressureEvictsRatherThanGrowingUnbounded)
{
    const int kPaths = 8000;
    std::vector<std::wstring> names(kPaths);
    std::vector<UNICODE_STRING> paths(kPaths);

    for (int i = 0; i < kPaths; ++i)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"\\media\\pressure\\%05d.bin", i);
        names[i] = buf;
        paths[i].Buffer = const_cast<PWSTR>(names[i].c_str());
        paths[i].Length = (USHORT)(names[i].size() * sizeof(wchar_t));
        paths[i].MaximumLength = paths[i].Length;
    }

    DIRECTORY_ENTRY_METADATA meta = MakeMeta(1, FALSE);

    for (int i = 0; i < kPaths; ++i)
    {
        BlorgPathCacheInsertExists(&paths[i], &meta);
    }

    EXPECT_EQ(PathCacheExists, BlorgPathCacheLookup(&paths[kPaths - 1], nullptr))
        << "the most recently inserted entry must survive its own insert";

    int hits = 0;
    for (int i = 0; i < kPaths; ++i)
    {
        if (PathCacheExists == BlorgPathCacheLookup(&paths[i], nullptr))
        {
            hits++;
        }
    }

    EXPECT_LT(hits, kPaths)
        << "all " << kPaths << " distinct paths are still cached -- "
           "eviction under the per-bucket cap did not fire";
}

} // namespace
