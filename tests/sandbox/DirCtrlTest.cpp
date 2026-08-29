//
// Coverage for the real DirCtrl.c: BlorgVolumeDirectoryControl's
// QUERY_DIRECTORY enumeration (MatchPattern, EnumerateDirectoryEntries, and
// all three FILE_*_DIR_INFORMATION fill routines), NOTIFY_CHANGE_DIRECTORY
// registration, and the dispatch-entry device-type routing -- none of
// which any other sandbox target drives.
//
// Most tests here pre-populate dcb->CachedListing directly (via the
// shared ListingBuilder.h, the same builder CreateDirectoryTest.cpp uses), the way
// a warm directory's second and later queries actually resolve -- cheap,
// and keeps the enumeration/pattern-matching tests independent of the
// network. The regression test below is the exception: it drives a real
// BlorgHttpGetDirectoryInfo call (scripted to stall, via SandboxSocket.h)
// to prove a real second query sees a real outstanding fetch, not a
// hand-built stand-in for one. DirCtrlComplete's *success* path --
// actually parsing a delivered FlatBuffers listing -- is still untested;
// that's Client.c's HttpDeserializeDirectoryInfo gap, not duplicated here.
//

#include <gtest/gtest.h>

#include <cwchar>
#include <memory>
#include <vector>

extern "C" {
#include "SandboxSocket.h"

// Not declared in any header -- DirCtrl.c's only other caller is
// BlorgDirectoryControl itself.
NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp);
}

#include "ListingBuilder.h"

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
        global.VolumeDeviceObject = nullptr;

        SandboxDrainCompletions();
        ShimDrainWorkItems();
        BlorgCleanupWskClient();

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
    // Publishes a listing on the DCB the way DirCtrlComplete would have --
    // every test that isn't specifically about the cache-miss path starts
    // from a warm directory so BlorgVolumeDirectoryControl never reaches
    // the network fetch at all. The listing's layout arithmetic lives in
    // ListingBuilder.h, shared with CreateDirectoryTest.cpp rather than
    // copied per fixture.
    //
    void SeedListing(int fileCount, int subDirCount)
    {
        Dcb->CachedListing = BuildSyntheticListing(fileCount, subDirCount);
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
// (ccb->SearchPattern set, dcb->CachedListing still NULL, because
// DirCtrlComplete's failure branch never publishes) is identical to the
// state a still-outstanding fetch would leave, since dcb->CachedListing
// only ever transitions on a *successful* publish. A second
// QUERY_DIRECTORY IRP on the same handle, issued right after (legal for a
// caller with an asynchronous/overlapped handle -- Create.c never sets
// FO_SYNCHRONOUS_IO on a directory open, and a real caller could just as
// easily land here mid-flight rather than after a failure), must not
// report STATUS_NO_MORE_FILES: NULL means "no listing yet", never "empty
// directory" (a genuinely empty one still publishes a real zero-count
// DIRECTORY_INFO).
//
// Before the DirCtrl.c fix (gating the fetch-issuing check on
// `(initialQuery || restartScan) && !netDone && !dcb->CachedListing`),
// this second call fell straight through to `ccb->Entries =
// dcb->CachedListing` (still NULL) and returned STATUS_NO_MORE_FILES --
// confirmed directly against the real driver before the fix landed. The
// fix drops the initialQuery/restartScan gate, so a second query in this
// state now issues its own fetch too, which DirCtrlComplete's existing
// duplicate-fetch handling (see its header comment) already knows how to
// resolve safely.
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
    ASSERT_EQ(nullptr, Dcb->CachedListing) << "the first fetch must not have published a listing";

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

} // namespace
