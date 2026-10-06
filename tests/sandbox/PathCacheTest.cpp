//
// Functional tests for PathCache.c: the sharded full-path resolution
// cache wired into the create path (via BlorgPathCacheLookup/InsertExists/
// InsertNotFound) and into DirCtrlComplete (via BlorgPathCacheSeedListing
// on a listing publish), and the listing cache beside it. Exercised here through its own public API rather
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

#include "ListingBuilder.h"

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

    BlorgPathCacheInsertExists(&path, &meta, nullptr);

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

    BlorgPathCacheInsertNotFound(&path, nullptr);

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

    BlorgPathCacheInsertExists(&path, &first, nullptr);
    BlorgPathCacheInsertExists(&path, &second, nullptr);

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

    BlorgPathCacheInsertExists(&path, &meta, nullptr);
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

    BlorgPathCacheInsertExists(&victim, &meta, nullptr);
    BlorgPathCacheInsertExists(&bystander, &meta, nullptr);

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

    BlorgPathCacheInsertExists(&inDirA, &meta, nullptr);
    BlorgPathCacheInsertExists(&inDirB, &meta, nullptr);
    BlorgPathCacheInsertExists(&dirItself, &dirMeta, nullptr);
    BlorgPathCacheInsertExists(&sibling, &meta, nullptr);
    BlorgPathCacheInsertExists(&unrelated, &meta, nullptr);

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
// RTL_CONSTANT_STRING(L"\\"), and DirCtrlComplete hands
// &dcb->FullPath to BlorgPathCacheSeedListing on every listing publish,
// which sweeps the same subtree -- so a refresh of the volume root passes
// exactly this Dir.
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
// DirCtrlComplete documents ("a stale not-found memoized before the
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

    BlorgPathCacheInsertNotFound(&rootChild, nullptr);
    BlorgPathCacheInsertNotFound(&deeperChild, nullptr);

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

    BlorgPathCacheInsertNotFound(&child, nullptr);
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
        BlorgPathCacheInsertExists(&paths[i], &meta, nullptr);
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

//
// BlorgPathCacheSeedListing: a published listing becomes the path cache's
// answer for the directory's children. Before it, DirCtrlComplete only
// invalidated, and since a DCB frees its listing when its last handle
// closes, every open after a `dir` paid a fileinfo GET (measured: 300 opens
// after a listing, 300 GETs, 0 path-cache hits). Nothing in the dispatch
// sandbox drives DirCtrlComplete's success path, so the contract is pinned
// here, on the cache, where a regression to "invalidate only" shows up as a
// miss.
//
class PathCacheSeedTest : public PathCacheTest
{
protected:
    static UNICODE_STRING Path(const std::wstring& Text)
    {
        UNICODE_STRING path;
        path.Buffer = const_cast<PWSTR>(Text.c_str());
        path.Length = C_CAST(USHORT, Text.size() * sizeof(wchar_t));
        path.MaximumLength = path.Length;
        return path;
    }

    static void Seed(const std::wstring& Dir, PDIRECTORY_INFO Listing)
    {
        UNICODE_STRING dir = Path(Dir);
        BlorgPathCacheSeedListing(&dir, Listing, nullptr);
        BlorgReleaseDirectoryInfo(Listing);
    }

    static PATH_CACHE_RESULT Lookup(const std::wstring& Text, DIRECTORY_ENTRY_METADATA* Meta = nullptr)
    {
        UNICODE_STRING path = Path(Text);
        return BlorgPathCacheLookup(&path, Meta);
    }
};

TEST_F(PathCacheSeedTest, SeededChildrenHitWithTheListingsMetadata)
{
    Seed(L"\\seed\\basic", BuildSyntheticListing(3, 2));

    DIRECTORY_ENTRY_METADATA out = {};
    ASSERT_EQ(PathCacheExists, Lookup(L"\\seed\\basic\\file2.bin", &out))
        << "a child the listing named must resolve without a fileinfo GET";
    EXPECT_EQ(1002u, out.Size);
    EXPECT_FALSE(out.IsDirectory);

    ASSERT_EQ(PathCacheExists, Lookup(L"\\seed\\basic\\dir1", &out));
    EXPECT_TRUE(out.IsDirectory);
    EXPECT_EQ(0u, out.Size);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\seed\\basic\\file3.bin"))
        << "only names the listing contains may be seeded";
}

//
// The listing is authoritative for its children and for the directory's own
// existence, and says nothing about anything deeper: a child directory may
// have been replaced since its own entries were cached.
//
TEST_F(PathCacheSeedTest, KeepsTheDirectoryReplacesStaleChildrenDropsDeeperEntries)
{
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\seed\\keep");
    UNICODE_STRING staleChild = RTL_CONSTANT_STRING(L"\\seed\\keep\\file0.bin");
    UNICODE_STRING grandchild = RTL_CONSTANT_STRING(L"\\seed\\keep\\dir0\\inner.bin");
    DIRECTORY_ENTRY_METADATA dirMeta = MakeMeta(0, TRUE);

    BlorgPathCacheInsertExists(&dir, &dirMeta, nullptr);
    BlorgPathCacheInsertNotFound(&staleChild, nullptr);
    BlorgPathCacheInsertNotFound(&grandchild, nullptr);

    Seed(L"\\seed\\keep", BuildSyntheticListing(1, 1));

    EXPECT_EQ(PathCacheExists, Lookup(L"\\seed\\keep"))
        << "evicting the directory's own entry is what made a repeated `dir` "
           "pay a fileinfo GET before its dirinfo GET";
    EXPECT_EQ(PathCacheExists, Lookup(L"\\seed\\keep\\file0.bin"))
        << "a not-found memoized before the file appeared must not shadow the listing";
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\seed\\keep\\dir0\\inner.bin"))
        << "entries below a child are not covered by this listing";
}

//
// The volume root's FullPath already ends in its separator; joining it
// naively would seed "\\file0.bin", which no open ever looks up.
//
TEST_F(PathCacheSeedTest, RootListingSeedsSingleSeparatorPaths)
{
    Seed(L"\\", BuildSyntheticListingNamed(L"seedroot.bin", L"seedrootdir"));

    EXPECT_EQ(PathCacheExists, Lookup(L"\\seedroot.bin"));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\seedrootdir"));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\\\seedroot.bin"));
}

//
// On a case-sensitive backend a file and a subdirectory can differ only in
// case. Create.c's listing scan (FindEntryByName) is case-insensitive and
// returns the file, so the seeded entry -- which stands in for that scan
// once the DCB is gone -- must say file too, or the same open would get a
// directory or a file depending on whether the DCB happened to be alive.
//
TEST_F(PathCacheSeedTest, CaseCollisionResolvesToTheFileAsTheListingScanDoes)
{
    Seed(L"\\seed\\case", BuildSyntheticListingNamed(L"Clip", L"clip"));

    DIRECTORY_ENTRY_METADATA out = {};
    ASSERT_EQ(PathCacheExists, Lookup(L"\\seed\\case\\CLIP", &out));
    EXPECT_FALSE(out.IsDirectory);
    EXPECT_EQ(2048u, out.Size);
}

//
// A name is one component. A Linux host allows a file named "a\b", and
// seeding it would answer an open of \seed\slash\a\b whether a exists or
// not.
//
TEST_F(PathCacheSeedTest, ANameHoldingABackslashIsNotSeeded)
{
    Seed(L"\\seed\\slash", BuildSyntheticListingNamed(L"a\\b", L"c"));

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\seed\\slash\\a\\b"));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\seed\\slash\\c"));
}

//
// The cache evicts FIFO per bucket, so seeding a huge directory in full
// would flush every other entry for names that are mostly never opened.
// Seeding stops at PATH_CACHE_SEED_MAX, keeping the first entries in
// listing order. Two thousand files is past any cap that leaves room for
// the rest of the cache, so this does not depend on the constant's value.
//
TEST_F(PathCacheSeedTest, VeryLargeListingSeedsOnlyItsFirstEntries)
{
    Seed(L"\\seed\\huge", BuildSyntheticListing(2000, 0));

    EXPECT_EQ(PathCacheExists, Lookup(L"\\seed\\huge\\file0.bin"));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\seed\\huge\\file1999.bin"))
        << "a 2000-entry listing was seeded in full, flushing the rest of the cache";
}

//
// The listing cache (BlorgPathCacheLookupListing/PublishListing): what lets a
// re-list skip its dirinfo GET, and the ticket protocol that keeps a listing
// fetched before a change from being served after it. DirCtrlTest.cpp drives
// it through real queries; the time windows and the races are pinned here,
// where the clock can be jumped and each verdict read directly.
//
class PathCacheListingTest : public PathCacheSeedTest
{
protected:
    static void Publish(const std::wstring& Dir, PDIRECTORY_INFO Listing, const PATH_CACHE_TICKET* Ticket, BOOLEAN ExpectCurrent)
    {
        UNICODE_STRING dir = Path(Dir);

        ASSERT_NE(nullptr, Listing);
        EXPECT_EQ(ExpectCurrent, BlorgPathCachePublishListing(&dir, Listing, Ticket));
        BlorgReleaseDirectoryInfo(Listing);
    }

    static PDIRECTORY_INFO LookupListing(const std::wstring& Dir, BOOLEAN AllowStale, BOOLEAN* Stale = nullptr, BOOLEAN* RefreshOwed = nullptr)
    {
        UNICODE_STRING dir = Path(Dir);
        return BlorgPathCacheLookupListing(&dir, AllowStale, Stale, RefreshOwed, nullptr);
    }

    static std::wstring FirstFileName(const DIRECTORY_INFO* Listing)
    {
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(const_cast<PDIRECTORY_INFO>(Listing), 0);
        return std::wstring(file->Name, file->NameLength);
    }

    static constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;
};

TEST_F(PathCacheListingTest, FreshListingIsServedWithoutOwingARefresh)
{
    Publish(L"\\lst\\fresh", BuildSyntheticListing(2, 0), nullptr, TRUE);

    BOOLEAN stale = TRUE;
    BOOLEAN owed = TRUE;
    PDIRECTORY_INFO listing = LookupListing(L"\\lst\\fresh", FALSE, &stale, &owed);

    ASSERT_NE(nullptr, listing) << "a listing within the TTL must answer a re-list";
    EXPECT_EQ(2u, listing->FileCount);
    EXPECT_FALSE(stale);
    EXPECT_FALSE(owed);

    BlorgReleaseDirectoryInfo(listing);
}

//
// Past the TTL a listing still answers a directory query (AllowStale) and
// never an open's not-found (Create.c passes FALSE), and of every query that
// sees it stale exactly one is told to refetch. A refresh per query would
// put the request back on every re-list of a busy directory, which is what
// the cache exists to remove.
//
TEST_F(PathCacheListingTest, StaleListingAnswersOnlyQueriesAndOwesOneRefresh)
{
    Publish(L"\\lst\\stale", BuildSyntheticListing(1, 0), nullptr, TRUE);
    ShimAdvanceInterruptTime(5 * kSecond);

    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\stale", FALSE))
        << "a stale listing must not answer an open's not-found";

    BOOLEAN stale = FALSE;
    BOOLEAN owed = FALSE;
    PDIRECTORY_INFO first = LookupListing(L"\\lst\\stale", TRUE, &stale, &owed);
    ASSERT_NE(nullptr, first);
    EXPECT_TRUE(stale);
    EXPECT_TRUE(owed) << "the first stale query owes the refresh";

    PDIRECTORY_INFO second = LookupListing(L"\\lst\\stale", TRUE, &stale, &owed);
    ASSERT_NE(nullptr, second);
    EXPECT_TRUE(stale);
    EXPECT_FALSE(owed) << "the refresh is owed once per snapshot, not once per query";

    BlorgReleaseDirectoryInfo(first);
    BlorgReleaseDirectoryInfo(second);
}

//
// The stale window is bounded: a directory nobody has listed for a while is
// fetched in the foreground rather than shown as it was long ago.
//
TEST_F(PathCacheListingTest, ListingPastTheStaleWindowIsGone)
{
    Publish(L"\\lst\\old", BuildSyntheticListing(1, 0), nullptr, TRUE);
    ShimAdvanceInterruptTime(31 * kSecond);

    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\old", TRUE));
}

//
// Two fetches of one directory can complete in either order. The one issued
// later saw the newer directory, so an earlier-issued listing completing
// after it must neither replace it nor seed the path cache (the FALSE
// verdict). Without the IssueTime check the slower, older response would
// win and roll the listing back.
//
TEST_F(PathCacheListingTest, LaterIssuedFetchWinsWhicheverCompletesFirst)
{
    PATH_CACHE_TICKET older;
    PATH_CACHE_TICKET newer;

    BlorgPathCacheTakeTicket(&older);
    ShimAdvanceInterruptTime(1);
    BlorgPathCacheTakeTicket(&newer);

    Publish(L"\\lst\\order", BuildSyntheticListingNamed(L"newer.bin", L"d"), &newer, TRUE);
    Publish(L"\\lst\\order", BuildSyntheticListingNamed(L"older.bin", L"d"), &older, FALSE);

    PDIRECTORY_INFO listing = LookupListing(L"\\lst\\order", FALSE);
    ASSERT_NE(nullptr, listing);
    EXPECT_EQ(L"newer.bin", FirstFileName(listing));
    BlorgReleaseDirectoryInfo(listing);
}

//
// A result read before an invalidation must not be cached after it. The
// invalidation here is of a different path than the one inserted, which is
// the conservative side of the protocol (see BlorgPathCacheTakeTicket): an
// insert cannot know which invalidations covered what it read, so it yields
// to any.
//
TEST_F(PathCacheListingTest, TicketTakenBeforeAnInvalidationIsRefused)
{
    PATH_CACHE_TICKET ticket;
    BlorgPathCacheTakeTicket(&ticket);

    UNICODE_STRING changed = RTL_CONSTANT_STRING(L"\\lst\\raced\\gone.bin");
    BlorgPathCacheInvalidate(&changed);

    Publish(L"\\lst\\raced", BuildSyntheticListing(1, 0), &ticket, FALSE);
    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\raced", TRUE));

    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\lst\\raced\\file0.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(1, FALSE);
    BlorgPathCacheInsertExists(&file, &meta, &ticket);
    BlorgPathCacheInsertNotFound(&changed, &ticket);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\lst\\raced\\file0.bin"));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\lst\\raced\\gone.bin"));

    PATH_CACHE_TICKET after;
    BlorgPathCacheTakeTicket(&after);
    Publish(L"\\lst\\raced", BuildSyntheticListing(1, 0), &after, TRUE);
    EXPECT_NE(nullptr, LookupListing(L"\\lst\\raced", FALSE));
}

//
// A change to a path is a change to the listing of the directory it sits
// in, so invalidating the path must drop that listing too; otherwise a
// re-list would keep showing a file that was just found to be gone.
//
TEST_F(PathCacheListingTest, InvalidatingAChildDropsTheParentsListing)
{
    Publish(L"\\lst\\parent", BuildSyntheticListing(1, 0), nullptr, TRUE);
    Publish(L"\\lst\\parent\\dir0", BuildSyntheticListing(1, 0), nullptr, TRUE);
    Publish(L"\\lst\\sibling", BuildSyntheticListing(1, 0), nullptr, TRUE);

    UNICODE_STRING child = RTL_CONSTANT_STRING(L"\\lst\\parent\\dir0");
    BlorgPathCacheInvalidate(&child);

    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\parent", TRUE));
    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\parent\\dir0", TRUE))
        << "a directory's own listing goes with it";

    PDIRECTORY_INFO sibling = LookupListing(L"\\lst\\sibling", FALSE);
    EXPECT_NE(nullptr, sibling) << "an unrelated listing must survive";
    BlorgReleaseDirectoryInfo(sibling);
}

//
// A repeat `dir /s` answers every directory from the listing cache, which
// holds only if sibling paths spread across its buckets. The kernel's path
// hash does not spread them on its own (see BlorgHashPath): this tree, the
// shape of the guest measurement, packed up to 12 listings into one bucket
// of 8, and two thirds of a repeat walk went back to the server. The shim
// hashes with the kernel's algorithm so the clustering is real here.
//
TEST_F(PathCacheListingTest, EveryListingOfADeepTreeStaysCached)
{
    std::vector<std::wstring> dirs = { L"\\prof\\r1\\tree" };

    for (int i = 0; i < 40; ++i)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"\\prof\\r1\\tree\\d%02d", i);
        dirs.push_back(buf);

        for (int k = 0; k < 2; ++k)
        {
            swprintf_s(buf, L"\\prof\\r1\\tree\\d%02d\\s%d", i, k);
            dirs.push_back(buf);
        }
    }

    for (const auto& dir : dirs)
    {
        Publish(dir, BuildSyntheticListing(5, 0), nullptr, TRUE);
    }

    int missing = 0;

    for (const auto& dir : dirs)
    {
        PDIRECTORY_INFO listing = LookupListing(dir, FALSE);
        missing += (listing == nullptr);
        BlorgReleaseDirectoryInfo(listing);
    }

    EXPECT_EQ(0, missing) << "of " << dirs.size() << " listings published back to back";
}

//
// A handle enumerates the snapshot it took; the cache dropping its own
// reference must not free it underneath. ASan turns a regression here into
// a use-after-free report at the FileCount read.
//
TEST_F(PathCacheListingTest, SnapshotOutlivesItsCacheEntry)
{
    Publish(L"\\lst\\held", BuildSyntheticListing(3, 0), nullptr, TRUE);

    PDIRECTORY_INFO held = LookupListing(L"\\lst\\held", FALSE);
    ASSERT_NE(nullptr, held);

    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\lst\\held");
    BlorgPathCacheInvalidatePrefix(&dir);

    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\held", TRUE));
    EXPECT_EQ(3u, held->FileCount);
    EXPECT_EQ(L"file0.bin", FirstFileName(held));

    BlorgReleaseDirectoryInfo(held);
}

//
// A listing larger than the whole byte budget can never be kept, and
// evicting its bucket's live listings to try only loses them. A thousand
// small listings leave nearly every bucket holding some; whichever were
// kept must all still be there after the large one is refused.
//
TEST_F(PathCacheListingTest, AListingTheBudgetCannotHoldEvictsNothing)
{
    std::vector<std::wstring> kept;

    for (int i = 0; i < 1000; ++i)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"\\budget\\small\\d%03d", i);
        Publish(buf, BuildSyntheticListing(1, 0), nullptr, TRUE);
    }

    for (int i = 0; i < 1000; ++i)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"\\budget\\small\\d%03d", i);

        PDIRECTORY_INFO listing = LookupListing(buf, FALSE);

        if (listing)
        {
            kept.push_back(buf);
        }

        BlorgReleaseDirectoryInfo(listing);
    }

    Publish(L"\\budget\\huge", BuildSyntheticListing(70000, 0), nullptr, TRUE);

    PDIRECTORY_INFO huge = LookupListing(L"\\budget\\huge", FALSE);
    EXPECT_EQ(nullptr, huge);
    BlorgReleaseDirectoryInfo(huge);

    int missing = 0;

    for (const auto& dir : kept)
    {
        PDIRECTORY_INFO listing = LookupListing(dir, FALSE);
        missing += (nullptr == listing);
        BlorgReleaseDirectoryInfo(listing);
    }

    EXPECT_EQ(0, missing) << "of " << kept.size() << " listings kept before the large one";
}

//
// What the server marked no-store (Client.c's HttpForbidsStoring) is refused
// by both caches. The server sends it for an answer it could not vouch for
// against its own change feed -- a load an invalidation overtook, or a path
// reached through a link -- so caching it could outlive a change the feed
// has already reported, and nothing would ever invalidate it again.
//
TEST_F(PathCacheListingTest, NoStoreResultsAreNeverCached)
{
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\lst\\nostore\\file0.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(1, FALSE);
    meta.NoStore = TRUE;

    BlorgPathCacheInsertExists(&file, &meta, nullptr);
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\lst\\nostore\\file0.bin"));

    PDIRECTORY_INFO listing = BuildSyntheticListing(1, 0);
    ASSERT_NE(nullptr, listing);
    listing->NoStore = TRUE;

    Publish(L"\\lst\\nostore", listing, nullptr, FALSE);
    EXPECT_EQ(nullptr, LookupListing(L"\\lst\\nostore", TRUE));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\lst\\nostore\\file0.bin"))
        << "a refused listing must not seed its children either";
}

//
// BlorgPathCacheFollowFeed: what the change feed changes about how long an
// entry is trusted. Each test leaves the feed down, so the short TTL every
// other test assumes is back for the next one.
//
class PathCacheFeedTest : public PathCacheListingTest
{
protected:
    void TearDown() override
    {
        BlorgPathCacheFollowFeed(FALSE);
    }
};

//
// With the feed live, every change is invalidated as it is reported, so an
// entry outlives the short TTL. Inserted after the feed came up, as the
// ordering in BlorgPathCacheFollowFeed requires.
//
TEST_F(PathCacheFeedTest, LiveFeedKeepsEntriesPastTheShortTtl)
{
    BlorgPathCacheFollowFeed(TRUE);

    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\feed\\long\\file.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(5, FALSE);
    BlorgPathCacheInsertExists(&file, &meta, nullptr);
    Publish(L"\\feed\\long", BuildSyntheticListing(1, 0), nullptr, TRUE);

    ShimAdvanceInterruptTime(60 * kSecond);

    EXPECT_EQ(PathCacheExists, Lookup(L"\\feed\\long\\file.bin"));

    BOOLEAN stale = TRUE;
    PDIRECTORY_INFO listing = LookupListing(L"\\feed\\long", FALSE, &stale);
    ASSERT_NE(nullptr, listing);
    EXPECT_FALSE(stale) << "a minute is well inside the feed's lifetime";
    BlorgReleaseDirectoryInfo(listing);
}

//
// The feed's lifetime is still a bound: a change the server's watcher never
// sees is never reported, and only expiry would ever correct it.
//
TEST_F(PathCacheFeedTest, LiveFeedLifetimeIsStillBounded)
{
    BlorgPathCacheFollowFeed(TRUE);

    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\feed\\bounded\\file.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(5, FALSE);
    BlorgPathCacheInsertExists(&file, &meta, nullptr);

    ShimAdvanceInterruptTime(6 * 60 * kSecond);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\feed\\bounded\\file.bin"));
}

//
// A not-found keeps the short TTL with the feed live: a 404 reached through a
// symlink is not marked no-store, and a create behind the link is reported by
// the target's path, so nothing would ever invalidate it.
//
TEST_F(PathCacheFeedTest, LiveFeedKeepsANotFoundOnlyForTheShortTtl)
{
    BlorgPathCacheFollowFeed(TRUE);

    UNICODE_STRING missing = RTL_CONSTANT_STRING(L"\\feed\\link\\missing.bin");
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\feed\\link\\file.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(5, FALSE);
    BlorgPathCacheInsertNotFound(&missing, nullptr);
    BlorgPathCacheInsertExists(&file, &meta, nullptr);

    ShimAdvanceInterruptTime(5 * kSecond);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\feed\\link\\missing.bin"));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\feed\\link\\file.bin"));
}

//
// Lifetime is applied when an entry is read, so the feed going down shortens
// every entry already held at once. Fixing expiry at insert instead would
// leave entries trusted for minutes with nothing reporting changes to them.
//
TEST_F(PathCacheFeedTest, FeedGoingDownShortensEntriesAlreadyHeld)
{
    BlorgPathCacheFollowFeed(TRUE);

    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\feed\\down\\file.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(5, FALSE);
    BlorgPathCacheInsertExists(&file, &meta, nullptr);
    Publish(L"\\feed\\down", BuildSyntheticListing(1, 0), nullptr, TRUE);

    ShimAdvanceInterruptTime(10 * kSecond);
    ASSERT_EQ(PathCacheExists, Lookup(L"\\feed\\down\\file.bin"));

    BlorgPathCacheFollowFeed(FALSE);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\feed\\down\\file.bin"));

    BOOLEAN stale = FALSE;
    PDIRECTORY_INFO listing = LookupListing(L"\\feed\\down", TRUE, &stale);
    ASSERT_NE(nullptr, listing) << "ten seconds is still inside the stale grace";
    EXPECT_TRUE(stale);
    BlorgReleaseDirectoryInfo(listing);
}

//
// Going live drops everything held before: those entries were read when no
// feed covered them, and the long lifetime must not apply to them. A ticket
// taken before the flush is refused for the same reason.
//
TEST_F(PathCacheFeedTest, GoingLiveDropsWhatWasReadBeforeIt)
{
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\feed\\before\\file.bin");
    DIRECTORY_ENTRY_METADATA meta = MakeMeta(5, FALSE);
    BlorgPathCacheInsertExists(&file, &meta, nullptr);
    Publish(L"\\feed\\before", BuildSyntheticListing(1, 0), nullptr, TRUE);

    PATH_CACHE_TICKET ticket;
    BlorgPathCacheTakeTicket(&ticket);

    BlorgPathCacheFollowFeed(TRUE);

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\feed\\before\\file.bin"));
    EXPECT_EQ(nullptr, LookupListing(L"\\feed\\before", TRUE));

    UNICODE_STRING late = RTL_CONSTANT_STRING(L"\\feed\\before\\late.bin");
    BlorgPathCacheInsertExists(&late, &meta, &ticket);
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\feed\\before\\late.bin"))
        << "a read ticketed before the feed came up was cached under it";
}


//
// A ticket held outside the caches (a resident FCB's stamp, Create.c) is
// judged by the same rule an entry is: any invalidation fails it, and so
// does age past the lifetime in force when it is asked. A ticket never
// taken is never current.
//
TEST_F(PathCacheFeedTest, TicketStaysCurrentUntilAnInvalidationOrTheLifetime)
{
    PATH_CACHE_TICKET never = {};
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&never));

    PATH_CACHE_TICKET ticket;
    BlorgPathCacheTakeTicket(&ticket);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&ticket));

    UNICODE_STRING elsewhere = RTL_CONSTANT_STRING(L"\\feed\\ticket\\other.bin");
    BlorgPathCacheInvalidate(&elsewhere);
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&ticket))
        << "the stamp cannot tell which path changed, so any change must fail it";

    BlorgPathCacheTakeTicket(&ticket);
    ShimAdvanceInterruptTime(5 * kSecond);
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&ticket)) << "past the short TTL with the feed down";

    BlorgPathCacheFollowFeed(TRUE);
    BlorgPathCacheTakeTicket(&ticket);
    ShimAdvanceInterruptTime(60 * kSecond);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&ticket)) << "the feed's lifetime applies while it is live";

    BlorgPathCacheFollowFeed(FALSE);
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&ticket)) << "the feed going down shortens a held stamp at once";
}

} // namespace
