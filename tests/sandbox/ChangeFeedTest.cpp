//
// Tests for ChangeFeed.c: what one poll's answer does to the path and
// listing caches, and what it reports to directory-change notification.
// BlorgChangeFeedReceive is driven directly with hand-built batches, and
// BlorgChangeFeedPoll, the feed thread's loop body, against a scripted
// server; the sandbox's PsCreateSystemThread runs nothing. What a batch
// looks like on the wire is ClientTest.cpp's concern.
//
// These live beside PathCacheTest.cpp, in the binary whose global test
// environment initialises the path cache, because every verdict here is a
// cache lookup.
//

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include "SandboxSocket.h"
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
        PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&dir, TRUE, nullptr, nullptr, nullptr);
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
// The follower is started from a mount, in whatever process triggered it,
// and waited for from System at unload, so its handle must not live in the
// mounting process's table.
//
TEST_F(ChangeFeedTest, TheFollowerIsStartedWithAKernelHandle)
{
    EXPECT_NE(0u, ShimSystemThreadAttributes & OBJ_KERNEL_HANDLE);
}

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

///////////////////////////////////////////////////////////////////////////
// The follower's polls
///////////////////////////////////////////////////////////////////////////

//
// An answer to the first poll: a reset to epoch 9, generation 3, as
// ClientTest.cpp's kResetBatch encodes it.
//
#define RESET_ANSWER(Inline) \
    { SandboxStepDeliver, (const unsigned char*)(kResetAnswer), sizeof(kResetAnswer) - 1, STATUS_SUCCESS, Inline }

static const char kResetAnswer[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 80\r\n\r\n"
    "\x14\x00\x00\x00\x10\x00\x24\x00\x1c\x00\x14\x00\x13\x00\x0c\x00"
    "\x08\x00\x04\x00\x10\x00\x00\x00\x20\x00\x00\x00\x20\x00\x00\x00"
    "\x20\x00\x00\x00\x00\x00\x00\x01\x03\x00\x00\x00\x00\x00\x00\x00"
    "\x09\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\x78\x00\x00\x00";

class ChangeFeedPollTest : public ChangeFeedTest
{
protected:
    void SetUp() override
    {
        BlorgPathCacheCleanup();
        BlorgPathCacheInit();
        SandboxInitialize();
        ChangeFeedTest::SetUp();
    }

    void TearDown() override
    {
        Drain();
        BlorgCleanupWskClient();
        ChangeFeedTest::TearDown();
    }

    static void Drain()
    {
        SandboxDrainCompletions();
        ShimDrainWorkItems();
    }

    //
    // Runs BlorgChangeFeedPoll on a thread of its own, as the feed thread
    // would, and answers the poll from this one once it is issued: the poll
    // blocks until its answer arrives, and only a drain delivers it. It is
    // issued once it has queued work for a drain or is parked on a Stall.
    //
    void StartPoll()
    {
        Answered = false;
        Poller = std::thread([this] { Backoff = BlorgChangeFeedPoll(); Answered = true; });

        while (!Answered && 0 == ShimPendingWorkItems() && 0 == SandboxSocketsParked())
        {
            std::this_thread::yield();
        }
    }

    ULONG FinishPoll()
    {
        while (!Answered)
        {
            Drain();
            std::this_thread::yield();
        }

        Poller.join();
        return Backoff;
    }

    ULONG Poll()
    {
        StartPoll();
        return FinishPoll();
    }

    static std::string LastRequestLine()
    {
        SIZE_T length = 0;
        const char* text = C_CAST(const char*, SandboxLastRequest(&length));
        std::string request(text, length);
        return request.substr(0, request.find("\r\n"));
    }

    std::thread Poller;
    std::atomic<bool> Answered{ false };
    ULONG Backoff = 0;
};

//
// A poll that fails waits 1 s before the next, doubling while failures
// continue and holding at 30 s, so a server that is down for an hour is
// polled about twice a minute. One answer puts the next poll straight back
// on the wire and starts the doubling over.
//
TEST_F(ChangeFeedPollTest, FailedPollsBackOffDoublingToTheCapAndAnAnswerResetsIt)
{
    const ULONG expected[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000 };

    for (ULONG backoff : expected)
    {
        SandboxFailNextAcquiresWith(1, STATUS_CONNECTION_REFUSED);
        EXPECT_EQ(backoff, Poll());
        EXPECT_EQ(0, global.ChangeFeedLive);
    }

    static const SANDBOX_STEP answer[] = { RESET_ANSWER(TRUE) };
    SandboxSetPeerScript(answer, RTL_NUMBER_OF(answer));

    EXPECT_EQ(0u, Poll());
    EXPECT_EQ(1, global.ChangeFeedLive);

    SandboxFailNextAcquiresWith(1, STATUS_CONNECTION_REFUSED);
    EXPECT_EQ(1000u, Poll()) << "a success must start the doubling over";
    EXPECT_EQ(0, global.ChangeFeedLive);
}

//
// Each poll asks from where the last answer left the follower, so the
// server sends only what changed since.
//
TEST_F(ChangeFeedPollTest, ThePollAfterAnAnswerAsksFromWhereItLeftOff)
{
    static const SANDBOX_STEP answer[] = { RESET_ANSWER(TRUE) };

    SandboxSetPeerScript(answer, RTL_NUMBER_OF(answer));
    ASSERT_EQ(0u, Poll());
    EXPECT_EQ("GET /get_changes?epoch=0&since=0 HTTP/1.1", LastRequestLine());

    SandboxSetPeerScript(answer, RTL_NUMBER_OF(answer));
    ASSERT_EQ(0u, Poll());
    EXPECT_EQ("GET /get_changes?epoch=9&since=3 HTTP/1.1", LastRequestLine());
}

//
// A poll the server holds open is waited out, never abandoned: its
// completion writes the follower's state, so returning early would leave it
// writing into a feed that had moved on, or into a driver being unloaded.
//
TEST_F(ChangeFeedPollTest, AHeldPollIsWaitedForUntilTheServerAnswers)
{
    static const SANDBOX_STEP held[] =
    {
        { SandboxStepStall, nullptr, 0, STATUS_SUCCESS, FALSE },
        RESET_ANSWER(FALSE)
    };

    SandboxSetPeerScript(held, RTL_NUMBER_OF(held));
    StartPoll();

    Drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(Answered) << "the poll returned with the server still holding it";
    EXPECT_EQ(0, global.ChangeFeedLive);

    SandboxResumeStalled();

    EXPECT_EQ(0u, FinishPoll());
    EXPECT_EQ(1, global.ChangeFeedLive);
}

//
// A poll that cannot even be issued fails without waiting for an answer
// that will never come, and is backed off and acted on like any other
// failure. An unterminated Host makes the request impossible to build.
//
TEST_F(ChangeFeedPollTest, APollThatCannotBeIssuedFailsWithoutWaiting)
{
    GoLive();

    char host[BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES];
    memset(host, 'h', sizeof(host));

    PSTR savedHost = global.RemoteHostAnsi;
    global.RemoteHostAnsi = host;

    ULONG backoff = BlorgChangeFeedPoll();

    global.RemoteHostAnsi = savedHost;

    EXPECT_EQ(1000u, backoff);
    EXPECT_EQ(0, global.ChangeFeedLive);
    EXPECT_EQ(0u, ShimPendingWorkItems()) << "nothing was sent";
}

} // namespace
