//
// Tests for ChangeFeed.c: what one poll's answer does to the path and
// listing caches, and what it reports to directory-change notification.
// BlorgChangeFeedReceive is driven directly with hand-built batches; the
// thread around it only issues polls and waits, and the sandbox's
// PsCreateSystemThread runs nothing. What a batch looks like on the wire is
// ClientTest.cpp's concern.
//
// These live beside PathCacheTest.cpp, in the binary whose global test
// environment initialises the path cache, because every verdict here is a
// cache lookup.
//

#include <gtest/gtest.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "..\..\src\Driver.h"
}

#include "ListingBuilder.h"

namespace
{

class ChangeFeedTest : public ::testing::Test
{
protected:
    static constexpr ULONG64 kEpoch = 7;
    static constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;

    void SetUp() override
    {
        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);

        SavedSwitch = global.ChangeFeed;
        global.ChangeFeed = TRUE;

        BlorgChangeFeedStart(Volume);
        BlorgPathCacheInvalidateAll();
        ShimNotifyReportsReset();
    }

    void TearDown() override
    {
        BlorgChangeFeedStop();
        global.ChangeFeed = SavedSwitch;

        ShimNotifyReportsReset();
        StructsModelDestroyVolume(Volume);
    }

    //
    // One allocation holding the batch, its entries and their paths, as
    // HttpDeserializeChangeBatch builds it, so BlorgChangeFeedReceive frees
    // it with BlorgFreeChangeBatch exactly as it frees a real one.
    //
    static PCHANGE_BATCH MakeBatch(ULONG64 Epoch, ULONG64 Generation, BOOLEAN Reset,
        const std::vector<std::pair<std::wstring, CHANGE_KIND>>& Changes = {})
    {
        SIZE_T pathBytes = 0;

        for (const auto& change : Changes)
        {
            pathBytes += change.first.size() * sizeof(WCHAR);
        }

        SIZE_T size = sizeof(CHANGE_BATCH) + Changes.size() * sizeof(CHANGE_ENTRY) + pathBytes;
        PCHANGE_BATCH batch = C_CAST(PCHANGE_BATCH, ExAllocatePoolZero(PagedPool, size, 'TCRT'));

        if (!batch)
        {
            return nullptr;
        }

        batch->Epoch = Epoch;
        batch->Generation = Generation;
        batch->Reset = Reset;
        batch->Count = Changes.size();
        batch->Entries = C_CAST(PCHANGE_ENTRY, batch + 1);

        PWCHAR text = C_CAST(PWCHAR, batch->Entries + Changes.size());

        for (SIZE_T i = 0; i < Changes.size(); ++i)
        {
            const std::wstring& path = Changes[i].first;
            USHORT bytes = C_CAST(USHORT, path.size() * sizeof(WCHAR));

            memcpy(text, path.data(), bytes);
            batch->Entries[i].Path.Buffer = text;
            batch->Entries[i].Path.Length = bytes;
            batch->Entries[i].Path.MaximumLength = bytes;
            batch->Entries[i].Kind = Changes[i].second;
            text += path.size();
        }

        return batch;
    }

    //
    // The first answer a fresh follower gets is a reset; this is what brings
    // the feed live before a test applies anything precise.
    //
    static void GoLive(ULONG64 Generation = 1)
    {
        BlorgChangeFeedReceive(STATUS_SUCCESS, MakeBatch(kEpoch, Generation, TRUE));
        ASSERT_EQ(1, global.ChangeFeedLive);
    }

    static void Apply(ULONG64 Generation, const std::vector<std::pair<std::wstring, CHANGE_KIND>>& Changes)
    {
        BlorgChangeFeedReceive(STATUS_SUCCESS, MakeBatch(kEpoch, Generation, FALSE, Changes));
    }

    static UNICODE_STRING Path(const std::wstring& Text)
    {
        UNICODE_STRING path;
        path.Buffer = const_cast<PWSTR>(Text.c_str());
        path.Length = C_CAST(USHORT, Text.size() * sizeof(wchar_t));
        path.MaximumLength = path.Length;
        return path;
    }

    static void Exists(const std::wstring& Text)
    {
        UNICODE_STRING path = Path(Text);
        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = 1;
        BlorgPathCacheInsertExists(&path, &meta, nullptr);
    }

    static void NotFound(const std::wstring& Text)
    {
        UNICODE_STRING path = Path(Text);
        BlorgPathCacheInsertNotFound(&path, nullptr);
    }

    static PATH_CACHE_RESULT Lookup(const std::wstring& Text)
    {
        UNICODE_STRING path = Path(Text);
        return BlorgPathCacheLookup(&path, nullptr);
    }

    static void PublishListing(const std::wstring& Dir)
    {
        UNICODE_STRING dir = Path(Dir);
        PDIRECTORY_INFO listing = BuildSyntheticListing(1, 0);
        ASSERT_NE(nullptr, listing);
        EXPECT_TRUE(BlorgPathCachePublishListing(&dir, listing, nullptr));
        BlorgReleaseDirectoryInfo(listing);
    }

    static bool HasListing(const std::wstring& Dir)
    {
        UNICODE_STRING dir = Path(Dir);
        PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&dir, TRUE, nullptr, nullptr);
        BlorgReleaseDirectoryInfo(listing);
        return nullptr != listing;
    }

    static std::wstring ReportedPath(const SHIM_NOTIFY_REPORT* Report)
    {
        return std::wstring(Report->Path, Report->PathLength / sizeof(WCHAR));
    }

    PDEVICE_OBJECT Volume = nullptr;
    BOOLEAN SavedSwitch = FALSE;
};

//
// The first reset brings the feed live, and only after dropping what was
// cached before it: nothing read then is covered by the feed, so the long
// lifetime must not apply to it (BlorgPathCacheFollowFeed).
//
TEST_F(ChangeFeedTest, FirstResetBringsTheFeedLiveFromAnEmptyCache)
{
    Exists(L"\\cf\\reset\\before.bin");
    ASSERT_EQ(0, global.ChangeFeedLive);

    GoLive();

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\reset\\before.bin"));

    Exists(L"\\cf\\reset\\after.bin");
    ShimAdvanceInterruptTime(60 * kSecond);
    EXPECT_EQ(PathCacheExists, Lookup(L"\\cf\\reset\\after.bin"))
        << "an entry read after the feed came up is trusted past the short TTL";
}

//
// A modification drops the path and the parent listing that quotes its
// size, and nothing else: its siblings did not change, and dropping them
// would cost a request each for nothing.
//
TEST_F(ChangeFeedTest, ModifiedDropsThePathAndItsParentListingOnly)
{
    GoLive();

    Exists(L"\\cf\\mod\\changed.bin");
    Exists(L"\\cf\\mod\\sibling.bin");
    PublishListing(L"\\cf\\mod");

    Apply(2, { { L"\\cf\\mod\\changed.bin", ChangeModified } });

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\mod\\changed.bin"));
    EXPECT_FALSE(HasListing(L"\\cf\\mod"));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\cf\\mod\\sibling.bin"));
    EXPECT_EQ(1, global.ChangeFeedLive);
}

//
// A created directory may replace a cached "not found" for anything beneath
// it, not only for itself -- an open of a file inside it would otherwise be
// refused from the cache until expiry, minutes with the feed live.
//
TEST_F(ChangeFeedTest, CreatedDropsCachedNotFoundsBeneathIt)
{
    GoLive();

    NotFound(L"\\cf\\new");
    NotFound(L"\\cf\\new\\inside.bin");
    PublishListing(L"\\cf");

    Apply(2, { { L"\\cf\\new", ChangeCreated } });

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\new"));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\new\\inside.bin"));
    EXPECT_FALSE(HasListing(L"\\cf")) << "the parent listing now lacks the new entry";
}

//
// A removed directory takes its subtree and its own listing with it.
//
TEST_F(ChangeFeedTest, RemovedDirectoryTakesItsSubtree)
{
    GoLive();

    Exists(L"\\cf\\gone");
    Exists(L"\\cf\\gone\\deep\\file.bin");
    PublishListing(L"\\cf\\gone");
    Exists(L"\\cf\\kept\\file.bin");

    Apply(2, { { L"\\cf\\gone", ChangeRemoved } });

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\gone"));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\gone\\deep\\file.bin"));
    EXPECT_FALSE(HasListing(L"\\cf\\gone"));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\cf\\kept\\file.bin"));
}

//
// Past CHANGE_FEED_PRECISE_STRUCTURAL_MAX created or removed paths the
// batch flushes everything instead of sweeping every bucket per path. At
// the limit it stays precise. The bystander is what tells the two apart.
//
TEST_F(ChangeFeedTest, LargeStructuralBatchFlushesWhatASmallerOneKeeps)
{
    GoLive();

    auto structural = [](int Count)
    {
        std::vector<std::pair<std::wstring, CHANGE_KIND>> changes;

        for (int i = 0; i < Count; ++i)
        {
            changes.push_back({ L"\\cf\\unpacked\\f" + std::to_wstring(i), ChangeCreated });
        }

        return changes;
    };

    Exists(L"\\cf\\bystander.bin");
    Apply(2, structural(64));
    EXPECT_EQ(PathCacheExists, Lookup(L"\\cf\\bystander.bin")) << "64 paths are applied one by one";

    Apply(3, structural(65));
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\bystander.bin")) << "65 paths flush the cache";
    EXPECT_EQ(1, global.ChangeFeedLive) << "a flush is not a reset; the feed stays live";
}

//
// A failed poll takes the feed down at once: entries already held fall back
// to the short TTL rather than living out the long one with nothing
// reporting changes to them. The follower also forgets where it was, so the
// next answer, whatever it says, is acted on as a reset.
//
TEST_F(ChangeFeedTest, FailedPollTakesTheFeedDownAndTheNextAnswerResets)
{
    GoLive();

    Exists(L"\\cf\\fail\\held.bin");
    ShimAdvanceInterruptTime(10 * kSecond);
    ASSERT_EQ(PathCacheExists, Lookup(L"\\cf\\fail\\held.bin"));

    BlorgChangeFeedReceive(STATUS_IO_TIMEOUT, nullptr);

    EXPECT_EQ(0, global.ChangeFeedLive);
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\fail\\held.bin"));

    Exists(L"\\cf\\fail\\meanwhile.bin");
    Apply(2, {});

    EXPECT_EQ(1, global.ChangeFeedLive);
    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\fail\\meanwhile.bin"))
        << "an entry read while the feed was down was carried onto the long lifetime";
}

//
// A batch from another server process is a reset even if it does not say
// so: its generation is not comparable with the one applied so far, so what
// it lists is not everything that changed since.
//
TEST_F(ChangeFeedTest, BatchFromAnotherEpochIsActedOnAsAReset)
{
    GoLive();

    Exists(L"\\cf\\epoch\\held.bin");

    BlorgChangeFeedReceive(STATUS_SUCCESS, MakeBatch(kEpoch + 1, 1, FALSE));

    EXPECT_EQ(PathCacheMiss, Lookup(L"\\cf\\epoch\\held.bin"));
    EXPECT_EQ(1, global.ChangeFeedLive);
}

//
// Every change is reported into its parent directory, so an Explorer window
// open on it refreshes: the offset splits the parent from the name, and the
// filter and action are what a watcher on that kind of change asked for.
// The root has no parent to report into and is skipped.
//
TEST_F(ChangeFeedTest, ChangesAreReportedIntoTheirParentDirectory)
{
    GoLive();
    ShimNotifyReportsReset();

    Apply(2, {
        { L"\\cf\\note\\edited.bin", ChangeModified },
        { L"\\cf\\note\\added", ChangeCreated },
        { L"\\gone.bin", ChangeRemoved },
        { L"\\", ChangeModified },
    });

    ASSERT_EQ(3u, ShimNotifyReportCount());

    const SHIM_NOTIFY_REPORT* edited = ShimNotifyReport(0);
    EXPECT_EQ(L"\\cf\\note\\edited.bin", ReportedPath(edited));
    EXPECT_EQ(sizeof(L"\\cf\\note\\") - sizeof(WCHAR), edited->NameOffset);
    EXPECT_EQ(C_CAST(ULONG, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE), edited->Filter);
    EXPECT_EQ(C_CAST(ULONG, FILE_ACTION_MODIFIED), edited->Action);

    const SHIM_NOTIFY_REPORT* added = ShimNotifyReport(1);
    EXPECT_EQ(C_CAST(ULONG, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME), added->Filter);
    EXPECT_EQ(C_CAST(ULONG, FILE_ACTION_ADDED), added->Action);

    const SHIM_NOTIFY_REPORT* removed = ShimNotifyReport(2);
    EXPECT_EQ(sizeof(WCHAR), removed->NameOffset) << "a root child's parent is the root";
    EXPECT_EQ(C_CAST(ULONG, FILE_ACTION_REMOVED), removed->Action);
}

} // namespace
