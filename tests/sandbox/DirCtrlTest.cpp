//
// Coverage for the real DirCtrl.c: BlorgVolumeDirectoryControl's
// QUERY_DIRECTORY enumeration (MatchPattern, EnumerateDirectoryEntries, and
// all three FILE_*_DIR_INFORMATION fill routines), NOTIFY_CHANGE_DIRECTORY
// registration, and the dispatch-entry device-type routing -- none of
// which any other sandbox target drives.
//
// Most tests here publish a listing of the directory into the listing cache
// directly (via the shared ListingBuilder.h, the same builder
// CreateDirectoryTest.cpp uses), the way a warm directory's queries actually
// resolve -- cheap, and keeps the enumeration/pattern-matching tests
// independent of the network. The regression test below is the exception: it drives a real
// BlorgHttpGetDirectoryInfo call (scripted to stall, via SandboxSocket.h)
// to prove a real second query sees a real outstanding fetch, not a
// hand-built stand-in for one. DirCtrlComplete's *success* path --
// parses a delivered FlatBuffers subtree in the publication test below,
// which also checks the descendant cache answers.
//

#include <gtest/gtest.h>

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "SandboxSocket.h"

// Not declared in any header -- DirCtrl.c's only other caller is
// BlorgDirectoryControl itself.
NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp);
}

#include "ListingBuilder.h"
#include "SubtreeResponse.h"

#include "DeviceKindScope.h"

namespace
{

#define CLOSE_STEP \
    { SandboxStepClose, nullptr, 0, STATUS_SUCCESS, TRUE }

class DirCtrlTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        OriginalSubtreeEntries = global.SubtreeEntries;
        SandboxInitialize();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);
        global.VolumeDeviceObject = Volume;

        UNICODE_STRING dirName = Path(L"\\media");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateDCB(&Dcb, (CSHORT)BLORGFS_DCB_SIGNATURE, &dirName, Volume));
        InitializeListHead(&Dcb->Links);

        ASSERT_EQ(STATUS_SUCCESS, BlorgCreateCCB(&Ccb, Volume));

        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&WrongTypeNode, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, Volume, 0));
    }

    void TearDown() override
    {
        global.SubtreeEntries = OriginalSubtreeEntries;
        global.VolumeDeviceObject = nullptr;

        SandboxDrainCompletions();
        ShimDrainWorkItems();
        BlorgCleanupWskClient();

        BlorgPathCacheInvalidatePrefix(&Dcb->FullPath);

        BlorgFreeFileContext(Dcb, Volume);
        BlorgFreeFileContext(Ccb, Volume);
        BlorgFreeFileContext(WrongTypeNode, Volume);
        StructsModelDestroyVolume(Volume);

        KmAssertQuiescent("DirCtrlTest teardown");
    }

    void Drain()
    {
        SandboxDrainCompletions();
        ShimDrainWorkItems();
    }

    static UNICODE_STRING Path(const wchar_t* path)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(path);
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    //
    // Publishes a listing of the DCB's directory the way DirCtrlComplete
    // would have -- every test that isn't specifically about the cache-miss
    // path starts from a warm directory so BlorgVolumeDirectoryControl never
    // reaches the network fetch at all. The listing's layout arithmetic lives
    // in ListingBuilder.h, shared with CreateDirectoryTest.cpp rather than
    // copied per fixture. The cache takes its own reference and the
    // builder's is dropped here, so once the cache lets go a handle's
    // snapshot is all that keeps the listing alive.
    //
    void Publish(PDIRECTORY_INFO listing)
    {
        ASSERT_NE(nullptr, listing);
        EXPECT_TRUE(BlorgPathCachePublishListing(&Dcb->FullPath, listing, nullptr));
        BlorgReleaseDirectoryInfo(listing);
    }

    void SeedListing(int fileCount, int subDirCount)
    {
        Publish(BuildSyntheticListing(fileCount, subDirCount));
    }

    struct QueryRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
    };

    //
    // IRP_CONTEXT_FLAG_IN_FSP | IRP_CONTEXT_FLAG_WAIT are set directly on
    // DriverContext[0] rather than via BlorgSetupIrpContext (which the top
    // -level BlorgDirectoryControl calls and which asserts DriverContext[0]
    // starts at 0) -- the same reason ReadTest.cpp's non-paging direct
    // -fetch test calls BlorgVolumeRead directly. IN_FSP is what lets a
    // first query with an explicit pattern proceed inline instead of
    // reposting to the (not running, in this harness) FSP queue; WAIT is
    // what makes the resource acquisitions non-failing.
    //
    QueryRequest* PrepareQuery(PVOID fsContext, PVOID ccb, PUNICODE_STRING fileName,
        FILE_INFORMATION_CLASS infoClass, PVOID buffer, ULONG length, ULONG slFlags = 0)
    {
        Requests.push_back(std::make_unique<QueryRequest>());
        QueryRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->FileObject.FsContext = fsContext;
        req->FileObject.FsContext2 = ccb;
        req->FileObject.DeviceObject = Volume;

        req->Stack.MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
        req->Stack.MinorFunction = IRP_MN_QUERY_DIRECTORY;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.DeviceObject = Volume;
        req->Stack.Flags = (UCHAR)slFlags;
        req->Stack.Parameters.QueryDirectory.FileName = fileName;
        req->Stack.Parameters.QueryDirectory.FileInformationClass = infoClass;
        req->Stack.Parameters.QueryDirectory.Length = length;

        req->Irp.StackLocation = &req->Stack;
        req->Irp.UserBuffer = buffer;
        req->Irp.RequestorMode = KernelMode;
        req->Irp.Tail.Overlay.DriverContext[0] =
            (PVOID)(ULONG_PTR)(IRP_CONTEXT_FLAG_IN_FSP | IRP_CONTEXT_FLAG_WAIT);

        return req;
    }

    ULONG OriginalSubtreeEntries = 0;
    PDEVICE_OBJECT Volume = nullptr;
    PDCB Dcb = nullptr;
    PCCB Ccb = nullptr;
    PFCB WrongTypeNode = nullptr;
    std::vector<std::unique_ptr<QueryRequest>> Requests;
};

///////////////////////////////////////////////////////////////////////////
// MatchPattern, via a query with an explicit search pattern
///////////////////////////////////////////////////////////////////////////

TEST_F(DirCtrlTest, WildcardPatternMatchesOnlyEntriesSatisfyingIt)
{
    SeedListing(2, 1);

    UNICODE_STRING pattern = Path(L"file*.bin");
    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, &pattern, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    NTSTATUS status = BlorgVolumeDirectoryControl(&req->Irp, &req->Stack);

    ASSERT_EQ(STATUS_SUCCESS, status);
    auto* first = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer);

    //
    // The length is driver-reported, so it is pinned before being used as
    // the memcmp bound: a zero or truncated length would make the name
    // comparison pass without comparing the name.
    //
    ASSERT_EQ(sizeof(L"file0.bin") - sizeof(WCHAR), first->FileNameLength);
    EXPECT_EQ(0, memcmp(first->FileName, L"file0.bin", first->FileNameLength));
}

TEST_F(DirCtrlTest, ExactPatternWithNoWildcardsRequiresAnExactMatch)
{
    SeedListing(2, 0);

    UNICODE_STRING pattern = Path(L"file1.bin");
    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, &pattern, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
    auto* first = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer);

    ASSERT_EQ(sizeof(L"file1.bin") - sizeof(WCHAR), first->FileNameLength);
    EXPECT_EQ(0, memcmp(first->FileName, L"file1.bin", first->FileNameLength))
        << "file0.bin must have been skipped -- no wildcard means exact comparison";
    EXPECT_EQ(0u, first->NextEntryOffset) << "only one entry can match an exact pattern";
}

TEST_F(DirCtrlTest, NoFileNameMatchesEveryEntry)
{
    SeedListing(1, 1);

    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    NTSTATUS status = BlorgVolumeDirectoryControl(&req->Irp, &req->Stack);

    ASSERT_EQ(STATUS_SUCCESS, status);
    EXPECT_TRUE(BooleanFlagOn(Ccb->Flags, CCB_FLAG_MATCH_ALL));

    auto* first = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer);

    ASSERT_EQ(sizeof(L"file0.bin") - sizeof(WCHAR), first->FileNameLength);
    EXPECT_EQ(0, memcmp(first->FileName, L"file0.bin", first->FileNameLength));
    ASSERT_NE(0u, first->NextEntryOffset) << "the subdirectory entry must follow";

    auto* second = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer + first->NextEntryOffset);

    ASSERT_EQ(sizeof(L"dir0") - sizeof(WCHAR), second->FileNameLength);
    EXPECT_EQ(0, memcmp(second->FileName, L"dir0", second->FileNameLength))
        << "files are indexed before subdirectories";
}

///////////////////////////////////////////////////////////////////////////
// EnumerateDirectoryEntries: buffer sizing and resume
///////////////////////////////////////////////////////////////////////////

TEST_F(DirCtrlTest, BufferTooSmallForTheFirstEntryOverflows)
{
    SeedListing(1, 0);

    unsigned char buffer[4] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

//
// The one-entry size comes from the driver, not from a copy of
// AlignEntrySize: a first query with room to spare reports the stride it
// chose in the first entry's NextEntryOffset, and that is what sizes the
// deliberately-just-too-small buffer for the second query. Recomputing the
// 8-byte rounding here instead would be a second implementation of it,
// free to drift silently the day DirCtrl.c changes its alignment.
//
TEST_F(DirCtrlTest, PartialFillAfterAtLeastOneEntrySucceedsRatherThanOverflowing)
{
    SeedListing(3, 0);

    unsigned char probeBuffer[512] = {};
    QueryRequest* probe = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        probeBuffer, sizeof(probeBuffer));
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&probe->Irp, &probe->Stack));

    auto* firstEntry = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(probeBuffer);
    ULONG oneEntrySize = firstEntry->NextEntryOffset;
    ASSERT_NE(0u, oneEntrySize) << "the probe must have written more than one entry to report a stride";

    Ccb->CurrentIndex = 0;

    std::vector<unsigned char> buffer(oneEntrySize + 4, 0);
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer.data(), (ULONG)buffer.size());

    NTSTATUS status = BlorgVolumeDirectoryControl(&req->Irp, &req->Stack);

    EXPECT_EQ(STATUS_SUCCESS, status)
        << "a fill failure after at least one entry already fit must not surface as overflow "
           "-- that is what produces the Explorer ERROR_MORE_DATA popup on a normal listing";
    EXPECT_EQ(1u, Ccb->CurrentIndex) << "resume must point at the entry that didn't fit";
}

TEST_F(DirCtrlTest, ReturnSingleEntryStopsAfterOneMatchEvenWithRoomForMore)
{
    SeedListing(3, 0);

    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer), SL_RETURN_SINGLE_ENTRY);

    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));

    auto* first = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer);
    EXPECT_EQ(0u, first->NextEntryOffset)
        << "SL_RETURN_SINGLE_ENTRY must stop after the first match regardless of buffer room";
    EXPECT_EQ(1u, Ccb->CurrentIndex);
}

TEST_F(DirCtrlTest, EmptyListingReturnsNoMoreFiles)
{
    SeedListing(0, 0);

    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_NO_MORE_FILES, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, RestartScanResetsCurrentIndexToZero)
{
    SeedListing(2, 0);

    unsigned char firstBuffer[512] = {};
    QueryRequest* firstReq = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        firstBuffer, sizeof(firstBuffer), SL_RETURN_SINGLE_ENTRY);
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&firstReq->Irp, &firstReq->Stack));
    ASSERT_EQ(1u, Ccb->CurrentIndex);

    unsigned char secondBuffer[512] = {};
    QueryRequest* secondReq = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        secondBuffer, sizeof(secondBuffer), SL_RETURN_SINGLE_ENTRY | SL_RESTART_SCAN);
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&secondReq->Irp, &secondReq->Stack));

    auto* second = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(secondBuffer);

    ASSERT_EQ(sizeof(L"file0.bin") - sizeof(WCHAR), second->FileNameLength);
    EXPECT_EQ(0, memcmp(second->FileName, L"file0.bin", second->FileNameLength))
        << "SL_RESTART_SCAN must re-serve the first entry, not resume from index 1";
}

///////////////////////////////////////////////////////////////////////////
// The other two FILL_ROUTINE instantiations
///////////////////////////////////////////////////////////////////////////

TEST_F(DirCtrlTest, FileIdBothDirectoryInformationFillsAnEntry)
{
    SeedListing(1, 0);

    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileIdBothDirectoryInformation,
        buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
    auto* entry = reinterpret_cast<PFILE_ID_BOTH_DIR_INFORMATION>(buffer);

    ASSERT_EQ(sizeof(L"file0.bin") - sizeof(WCHAR), entry->FileNameLength);
    EXPECT_EQ(0, memcmp(entry->FileName, L"file0.bin", entry->FileNameLength));
}

TEST_F(DirCtrlTest, FileFullDirectoryInformationFillsAnEntry)
{
    SeedListing(1, 0);

    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileFullDirectoryInformation,
        buffer, sizeof(buffer));

    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
    auto* entry = reinterpret_cast<PFILE_FULL_DIR_INFORMATION>(buffer);

    ASSERT_EQ(sizeof(L"file0.bin") - sizeof(WCHAR), entry->FileNameLength);
    EXPECT_EQ(0, memcmp(entry->FileName, L"file0.bin", entry->FileNameLength));
}

TEST_F(DirCtrlTest, UnimplementedInformationClassesReportNotImplemented)
{
    SeedListing(1, 0);
    unsigned char buffer[512] = {};

    for (FILE_INFORMATION_CLASS cls : { FileDirectoryInformation, FileIdFullDirectoryInformation, FileNamesInformation })
    {
        QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, cls, buffer, sizeof(buffer));
        EXPECT_EQ(STATUS_NOT_IMPLEMENTED, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));

        // Fresh CCB state for the next class in the loop -- each iteration
        // is otherwise a restart-scan-equivalent initial query.
        Ccb->CurrentIndex = 0;
        RtlZeroMemory(&Ccb->Flags, sizeof(Ccb->Flags));
        if (Ccb->SearchPattern.Buffer)
        {
            RtlFreeUnicodeString(&Ccb->SearchPattern);
            RtlZeroMemory(&Ccb->SearchPattern, sizeof(Ccb->SearchPattern));
        }
    }
}

TEST_F(DirCtrlTest, UnknownInformationClassReturnsInvalidInfoClass)
{
    SeedListing(1, 0);
    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, (FILE_INFORMATION_CLASS)999,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_INFO_CLASS, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

///////////////////////////////////////////////////////////////////////////
// Validation and dispatch-entry routing
///////////////////////////////////////////////////////////////////////////

TEST_F(DirCtrlTest, WrongNodeTypeReturnsInvalidParameter)
{
    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(WrongTypeNode, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, MissingCcbReturnsInvalidParameter)
{
    unsigned char buffer[512] = {};
    QueryRequest* req = PrepareQuery(Dcb, nullptr, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, NotifyChangeDirectoryReturnsPending)
{
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation, nullptr, 0);
    req->Stack.MinorFunction = IRP_MN_NOTIFY_CHANGE_DIRECTORY;
    req->Stack.Parameters.NotifyDirectory.CompletionFilter = FILE_NOTIFY_CHANGE_FILE_NAME;

    EXPECT_EQ(STATUS_PENDING, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, NotifyChangeDirectoryOnWrongNodeTypeReturnsInvalidParameter)
{
    QueryRequest* req = PrepareQuery(WrongTypeNode, Ccb, nullptr, FileBothDirectoryInformation, nullptr, 0);
    req->Stack.MinorFunction = IRP_MN_NOTIFY_CHANGE_DIRECTORY;

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, UnhandledMinorFunctionReturnsInvalidDeviceRequest)
{
    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation, nullptr, 0);
    req->Stack.MinorFunction = 0x7F;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgVolumeDirectoryControl(&req->Irp, &req->Stack));
}

TEST_F(DirCtrlTest, NonVolumeDeviceReturnsInvalidDeviceRequest)
{
    PDEVICE_OBJECT diskDevice = StructsModelCreateVolume();
    ASSERT_NE(nullptr, diskDevice);
    ScopedDeviceKind asDisk(&global.DiskDeviceObject, diskDevice);

    QueryRequest* req = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation, nullptr, 0);
    req->Stack.DeviceObject = diskDevice;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgDirectoryControl(diskDevice, &req->Irp));

    StructsModelDestroyVolume(diskDevice);
}

///////////////////////////////////////////////////////////////////////////
// Regression: a second query landing while the first's cache-miss fetch
// is still outstanding
///////////////////////////////////////////////////////////////////////////

//
// End-to-end, not hand-built state: a real first QUERY_DIRECTORY with a
// pattern runs all the way through pattern setup and into a real
// BlorgHttpGetDirectoryInfo call. The peer closes immediately, so the
// fetch fails fast rather than genuinely stalling -- simpler to clean up
// than a parked connection, and the state it leaves behind
// (ccb->SearchPattern set, ccb->Entries still NULL, because
// DirCtrlComplete's failure branch never installs a snapshot) is identical
// to the state a still-outstanding fetch would leave, since ccb->Entries
// only ever gains a snapshot from a cache hit or a *successful* fetch. A second
// QUERY_DIRECTORY IRP on the same handle, issued right after (legal for a
// caller with an asynchronous/overlapped handle -- Create.c never sets
// FO_SYNCHRONOUS_IO on a directory open, and a real caller could just as
// easily land here mid-flight rather than after a failure), must not
// report STATUS_NO_MORE_FILES: NULL means "no listing yet", never "empty
// directory" (a genuinely empty one still publishes a real zero-count
// DIRECTORY_INFO).
//
// Before the DirCtrl.c fix, the fetch was gated on
// `(initialQuery || restartScan) && !netDone`, so this second call fell
// straight through to a NULL listing and returned STATUS_NO_MORE_FILES --
// confirmed directly against the real driver before the fix landed. A
// snapshot is now looked for whenever the handle has none, so a second
// query in this state issues its own fetch too, and DirCtrlComplete
// installs whichever completes first (see its header comment).
//
TEST_F(DirCtrlTest, SecondQueryWhileFirstFetchIsOutstandingDoesNotReportNoMoreFiles)
{
    static const SANDBOX_STEP script[] = { CLOSE_STEP };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    UNICODE_STRING pattern = Path(L"file*.bin");
    unsigned char firstBuffer[512] = {};
    QueryRequest* firstReq = PrepareQuery(Dcb, Ccb, &pattern, FileBothDirectoryInformation,
        firstBuffer, sizeof(firstBuffer));

    ASSERT_EQ(STATUS_PENDING, BlorgVolumeDirectoryControl(&firstReq->Irp, &firstReq->Stack));
    Drain();

    ASSERT_NE(nullptr, Ccb->SearchPattern.Buffer) << "the first call must have set the pattern";
    ASSERT_EQ(nullptr, Ccb->Entries) << "the failed first fetch must not have left a snapshot";

    static const SANDBOX_STEP secondScript[] = { CLOSE_STEP };
    SandboxSetPeerScript(secondScript, RTL_NUMBER_OF(secondScript));

    unsigned char secondBuffer[512] = {};
    QueryRequest* secondReq = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        secondBuffer, sizeof(secondBuffer));

    NTSTATUS status = BlorgVolumeDirectoryControl(&secondReq->Irp, &secondReq->Stack);

    EXPECT_NE(STATUS_NO_MORE_FILES, status)
        << "a second query racing the first query's outstanding cache-miss fetch must not "
           "report an empty directory";
    EXPECT_EQ(STATUS_PENDING, status)
        << "it should retry the fetch, same as the first call";

    Drain();
}

//
// A real network listing publishes its descendant listings separately.
// Observe all three cached answers and the handle snapshot, rather than
// only checking the client's decoder.
// This fixture has no FSP workers: completion publishes the snapshot, then
// its requeue fails with STATUS_DEVICE_REMOVED and completes the query.
//
TEST_F(DirCtrlTest, NetworkSubtreePublishesDescendantListings)
{
    global.SubtreeEntries = 64;
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
        std::to_string(sizeof(kSubtreeOutOfOrder) - 1) + "\r\n\r\n" +
        std::string(kSubtreeOutOfOrder, sizeof(kSubtreeOutOfOrder) - 1);
    const SANDBOX_STEP script[] = {
        { SandboxStepDeliver, C_CAST(const unsigned char*, response.data()), response.size(), STATUS_SUCCESS, FALSE }
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));
    const ULONG64 before = BlorgStatisticsForCurrentProcessor()->ListingsPrefetched;
    UNICODE_STRING pattern = Path(L"*");
    unsigned char buffer[512] = {};
    QueryRequest* query = PrepareQuery(Dcb, Ccb, &pattern, FileBothDirectoryInformation,
        buffer, sizeof(buffer));
    ASSERT_EQ(STATUS_PENDING, BlorgVolumeDirectoryControl(&query->Irp, &query->Stack));
    Drain();
    ASSERT_EQ(1, query->Irp.CompletionCount);
    ASSERT_EQ(STATUS_DEVICE_REMOVED, query->Irp.IoStatus.Status);
    ASSERT_NE(nullptr, Ccb->Entries);
    EXPECT_EQ(0u, Ccb->Entries->DescendantCount);
    EXPECT_EQ(nullptr, Ccb->Entries->Descendants);
    EXPECT_EQ(before + 3, BlorgStatisticsForCurrentProcessor()->ListingsPrefetched);
    UNICODE_STRING rootFile = Path(L"\\media\\r.bin");
    DIRECTORY_ENTRY_METADATA rootMeta = {};
    ASSERT_EQ(PathCacheExists, BlorgPathCacheLookup(&rootFile, &rootMeta));
    EXPECT_EQ(4096u, rootMeta.Size);

    struct ExpectedListing
    {
        const wchar_t* Path;
        const wchar_t* File;
    };
    const ExpectedListing expected[] = {
        { L"\\media\\a", L"a.bin" },
        { L"\\media\\b", L"b.bin" },
        { L"\\media\\a\\c", L"c.bin" }
    };
    for (const ExpectedListing& item : expected)
    {
        UNICODE_STRING path = Path(item.Path);
        PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&path, FALSE, nullptr, nullptr, nullptr);
        ASSERT_NE(nullptr, listing) << item.Path;
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(listing, 0);
        EXPECT_NE(nullptr, file);
        if (file)
        {
            EXPECT_EQ(item.File, std::wstring(file->Name, file->NameLength));
        }
        BlorgReleaseDirectoryInfo(listing);
    }
}

///////////////////////////////////////////////////////////////////////////
// Listing snapshots and serve-stale
///////////////////////////////////////////////////////////////////////////

//
// A handle enumerates the snapshot it took on its initial query until it
// restarts. A newer listing replacing the cached one mid-enumeration must
// not move entries under the handle's index -- resuming at index 1 of a
// different listing skips or repeats names, which is what a `dir` racing a
// refresh would show. The restart is what picks the newer listing up. A
// test that only ever enumerated one listing could not tell a shared
// listing from a snapshot.
//
TEST_F(DirCtrlTest, NewerListingDoesNotMoveAnEnumerationInProgress)
{
    SeedListing(3, 0);

    unsigned char buffer[512] = {};
    auto* entry = reinterpret_cast<PFILE_BOTH_DIR_INFORMATION>(buffer);

    QueryRequest* first = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer), SL_RETURN_SINGLE_ENTRY);
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&first->Irp, &first->Stack));
    ASSERT_EQ(1u, Ccb->CurrentIndex);

    Publish(BuildSyntheticListingNamed(L"other.bin", L"otherdir"));

    memset(buffer, 0, sizeof(buffer));
    QueryRequest* resume = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer), SL_RETURN_SINGLE_ENTRY);
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&resume->Irp, &resume->Stack));

    ASSERT_EQ(sizeof(L"file1.bin") - sizeof(WCHAR), entry->FileNameLength);
    EXPECT_EQ(0, memcmp(entry->FileName, L"file1.bin", entry->FileNameLength))
        << "the resumed query must continue the handle's own snapshot";

    memset(buffer, 0, sizeof(buffer));
    QueryRequest* restart = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer), SL_RETURN_SINGLE_ENTRY | SL_RESTART_SCAN);
    ASSERT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&restart->Irp, &restart->Stack));

    ASSERT_EQ(sizeof(L"other.bin") - sizeof(WCHAR), entry->FileNameLength);
    EXPECT_EQ(0, memcmp(entry->FileName, L"other.bin", entry->FileNameLength))
        << "a restart scan must take the newer listing";
}

//
// Past the fresh window a re-list is still answered at once from the cached
// snapshot, and exactly one query owes the refetch: a second handle listing
// the same stale directory must not issue another. The refresh's peer closes
// at once, so it fails and the snapshot stays as it was -- the case where a
// per-query refresh would turn every re-list of a slow directory into its
// own request. Counted through ListingRefreshes, the one place a refresh is
// visible without a scripted listing response.
//
TEST_F(DirCtrlTest, StaleListingAnswersAtOnceAndOwesOneBackgroundRefresh)
{
    SeedListing(1, 0);
    ShimAdvanceInterruptTime(5ULL * 10ULL * 1000ULL * 1000ULL);

    static const SANDBOX_STEP script[] = { CLOSE_STEP };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    PBLORGFS_STATISTICS stats = BlorgStatisticsForCurrentProcessor();
    ASSERT_NE(nullptr, stats);
    const ULONG64 refreshesBefore = stats->ListingRefreshes;
    const ULONG64 staleHitsBefore = stats->ListingCacheStaleHits;

    unsigned char buffer[512] = {};
    QueryRequest* first = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));
    EXPECT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&first->Irp, &first->Stack))
        << "a stale listing must answer without waiting on the wire";
    EXPECT_EQ(refreshesBefore + 1, stats->ListingRefreshes);
    Drain();

    PCCB secondCcb = nullptr;
    ASSERT_EQ(STATUS_SUCCESS, BlorgCreateCCB(&secondCcb, Volume));

    QueryRequest* second = PrepareQuery(Dcb, secondCcb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));
    EXPECT_EQ(STATUS_SUCCESS, BlorgVolumeDirectoryControl(&second->Irp, &second->Stack));
    EXPECT_EQ(refreshesBefore + 1, stats->ListingRefreshes)
        << "the refresh is owed once per snapshot, not once per query";
    EXPECT_EQ(staleHitsBefore + 2, stats->ListingCacheStaleHits);

    BlorgFreeFileContext(secondCcb, Volume);
}

//
// A miss outside the FSP is posted and looked up again there, so it is
// counted there, where the fetch is issued, and only once. The FSP queue
// is not running in this harness, so the post is refused, which is enough
// to show the query reached it.
//
TEST_F(DirCtrlTest, AMissPostedToTheFspIsCountedOnce)
{
    static const SANDBOX_STEP script[] = { CLOSE_STEP };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    PBLORGFS_STATISTICS stats = BlorgStatisticsForCurrentProcessor();
    ASSERT_NE(nullptr, stats);
    const ULONG64 missesBefore = stats->ListingCacheMisses;

    unsigned char buffer[512] = {};
    QueryRequest* posted = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));
    posted->Irp.Tail.Overlay.DriverContext[0] = (PVOID)(ULONG_PTR)IRP_CONTEXT_FLAG_WAIT;

    EXPECT_EQ(STATUS_DEVICE_REMOVED, BlorgVolumeDirectoryControl(&posted->Irp, &posted->Stack));
    EXPECT_EQ(missesBefore, stats->ListingCacheMisses)
        << "a miss was counted before the FSP looked it up again";

    QueryRequest* inFsp = PrepareQuery(Dcb, Ccb, nullptr, FileBothDirectoryInformation,
        buffer, sizeof(buffer));

    EXPECT_EQ(STATUS_PENDING, BlorgVolumeDirectoryControl(&inFsp->Irp, &inFsp->Stack));
    EXPECT_EQ(missesBefore + 1, stats->ListingCacheMisses);

    Drain();
}

} // namespace
