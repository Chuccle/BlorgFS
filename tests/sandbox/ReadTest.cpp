//
// Coverage for the real Read.c: BlorgRead/BlorgVolumeRead's dispatch of a
// paging read into the direct-fetch path, the cached (Cc) path,
// ReadTrimToFileSize's end-of-file clamp shared by both, and
// ReadComplete's completion bookkeeping.
//
// DispatchSandbox links the real Client.c, so the direct-fetch tests
// here script a real peer through SandboxSocket.h -- the same mechanism
// ClientTest.cpp (ClientSandbox) uses -- rather than completing a mocked
// fetch directly. That is deliberate: it proves BlorgVolumeRead builds the
// range/length BlorgHttpGetFileMdl actually sends, not just that it calls
// the function.
//
// The cached path (IRP_NOCACHE clear) goes through DispatchModel.c's Cc*
// stubs: CcCopyReadEx and CcMdlRead always succeed unless
// ShimForceNextCcCopyReadMiss() is armed, which is what makes the
// posted-on-cache-miss branch reachable at all.
//
// The fair-share tests at the end hold one file's read-ahead behind
// another's fetches. Deferred (non-inline) peer steps keep a fetch in
// flight across calls, which is what lets them observe a held IRP at all.
//

#include <gtest/gtest.h>

#include <cwchar>
#include <memory>
#include <vector>

extern "C" {
#include "SandboxSocket.h"

VOID ShimForceNextCcCopyReadMiss(VOID);

// Not declared in any header -- Read.c's only other caller is BlorgRead
// itself. See NonPagingDirectFetchAdvancesFileOffsetAndSetsFastIoOnCompletion
// for why this test needs to call it directly.
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp);
}

#include "DeviceKindScope.h"

namespace
{

const ULONGLONG kFileSize = 64ull * 1024 * 1024;

#define DELIVER(bytes) \
    { SandboxStepDeliver, (const unsigned char*)(bytes), sizeof(bytes) - 1, STATUS_SUCCESS, TRUE }

#define DELIVER_LATER(bytes) \
    { SandboxStepDeliver, (const unsigned char*)(bytes), sizeof(bytes) - 1, STATUS_SUCCESS, FALSE }

#define CLOSE_STEP \
    { SandboxStepClose, nullptr, 0, STATUS_SUCCESS, TRUE }

class ReadTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SandboxInitialize();
        BlorgReadInit();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);
        global.VolumeDeviceObject = Volume;

        UNICODE_STRING name = Path(L"\\media\\clip.bin");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Fcb, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, kFileSize));

        //
        // Standalone here (never inserted into a parent DCB's
        // ChildrenList, unlike CreateDirectoryTest.cpp's BlorgInsertByPath
        // nodes) -- Read.c never touches Links, but BlorgFreeFileContext
        // unconditionally RemoveEntryLists it on free, which needs a
        // self-linked head rather than the zeroed one BlorgCreateFCB
        // leaves behind.
        //
        InitializeListHead(&Fcb->Links);

        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&VcbNode, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, Volume, 0));
    }

    void TearDown() override
    {
        global.VolumeDeviceObject = nullptr;

        SandboxDrainCompletions();
        ShimDrainWorkItems();
        BlorgCleanupWskClient();

        for (PIRP irp : Irps)
        {
            if (irp->MdlAddress)
            {
                IoFreeMdl(irp->MdlAddress);
            }
        }

        Irps.clear();

        BlorgFreeFileContext(Fcb, Volume);
        BlorgFreeFileContext(VcbNode, Volume);
        StructsModelDestroyVolume(Volume);

        //
        // Not ShimPoolOutstanding() == 0 here: DispatchSandbox also links
        // the real Statistics.c, whose StatisticsEnvironment allocates one
        // process-lifetime table (see StatisticsTest.cpp) that is
        // legitimately still live at every test's teardown. KmAssertQuiescent
        // is floor-aware (KmAbsorbBaseline) and is the correct check here.
        //
        KmAssertQuiescent("ReadTest teardown");
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
    // One real READ IRP the way the I/O manager builds one. FileObject's
    // SectionObjectPointer is the FCB's own, as Create.c wires it, whether
    // or not a given test needs it: BlorgVolumeRead dereferences it
    // unconditionally once past the IRP_PAGING_IO/IRP_NOCACHE flag checks,
    // and the fetch scheduler finds the file's nonpaged node through it
    // (ReadFairNode), so a stand-alone one would have it write past the
    // end of something that is not a node.
    //
    struct ReadRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
    };

    ReadRequest* PrepareRead(PFCB fcb, ULONG64 offset, ULONG length, ULONG irpFlags,
        UCHAR minorFunction = 0, unsigned char* buffer = nullptr)
    {
        Requests.push_back(std::make_unique<ReadRequest>());
        ReadRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->FileObject.FsContext = fcb;
        req->FileObject.DeviceObject = Volume;
        req->FileObject.SectionObjectPointer = &fcb->NonPaged->SectionObjectPointers;

        req->Stack.MajorFunction = IRP_MJ_READ;
        req->Stack.MinorFunction = minorFunction;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.DeviceObject = Volume;
        req->Stack.Parameters.Read.Length = length;
        req->Stack.Parameters.Read.ByteOffset.QuadPart = (LONGLONG)offset;

        req->Irp.StackLocation = &req->Stack;
        req->Irp.Flags = irpFlags;

        if (buffer)
        {
            req->Irp.MdlAddress = IoAllocateMdl(buffer, length, FALSE, FALSE, nullptr);
        }

        Irps.push_back(&req->Irp);

        return req;
    }

    unsigned char* NewBuffer(SIZE_T length)
    {
        Buffers.emplace_back(length, 0);
        return Buffers.back().data();
    }

    PDEVICE_OBJECT Volume = nullptr;
    PFCB Fcb = nullptr;
    PFCB VcbNode = nullptr;
    std::vector<std::unique_ptr<ReadRequest>> Requests;
    std::vector<PIRP> Irps;
    std::vector<std::vector<unsigned char>> Buffers;
};

///////////////////////////////////////////////////////////////////////////
// BlorgRead / early validation
///////////////////////////////////////////////////////////////////////////

TEST_F(ReadTest, ZeroLengthReadSucceedsImmediatelyWithNoFetch)
{
    ReadRequest* req = PrepareRead(Fcb, 0, 0, IRP_PAGING_IO, 0, NewBuffer(1));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_SUCCESS, status);
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
    EXPECT_EQ(0u, SandboxSocketsCreated());
}

TEST_F(ReadTest, WrongNodeTypeReturnsInvalidParameter)
{
    ReadRequest* req = PrepareRead(VcbNode, 0, 4096, IRP_PAGING_IO | IRP_NOCACHE,
        0, NewBuffer(4096));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_INVALID_PARAMETER, status);
}

TEST_F(ReadTest, NonVolumeDeviceObjectReturnsInvalidDeviceRequest)
{
    //
    // A bare stack device object is enough now: BlorgDeviceKind classifies
    // by pointer, so nothing reads this object's extension. It used to need
    // a real IoCreateDevice-shaped allocation, because the old magic check
    // dereferenced the extension of whatever it was handed.
    //
    DEVICE_OBJECT diskDevice;
    memset(&diskDevice, 0, sizeof(diskDevice));

    ScopedDeviceKind asDisk(&global.DiskDeviceObject, &diskDevice);

    ReadRequest* req = PrepareRead(Fcb, 0, 4096, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4096));
    req->Stack.DeviceObject = &diskDevice;
    req->FileObject.DeviceObject = &diskDevice;

    NTSTATUS status = BlorgRead(&diskDevice, &req->Irp);

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, status);
}

//
// A negative ByteOffset is refused before the FCB is even looked at. The
// I/O manager screens these out of NtReadFile, so this covers the caller
// the I/O manager does not validate: a kernel component that builds its
// own IRP and fills in Parameters.Read.ByteOffset itself. Nothing
// downstream would have caught it -- the end-of-file trim's comparisons
// are both false for a negative offset -- and it reaches the fetch path
// widened to ULONG64, which is why the offset check in Read.c is
// independently overflow-proof (WrappingOffsetIsNeverServedFromASlot).
//
TEST_F(ReadTest, NegativeByteOffsetIsRejectedBeforeAnyFetch)
{
    const LONGLONG offsets[] = { -1, -4096, MINLONGLONG };

    for (LONGLONG offset : offsets)
    {
        ReadRequest* req = PrepareRead(Fcb, (ULONG64)offset, 4096,
            IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4096));

        NTSTATUS status = BlorgRead(Volume, &req->Irp);

        EXPECT_EQ(STATUS_INVALID_PARAMETER, status) << "offset " << offset;
        EXPECT_EQ(0u, req->Irp.IoStatus.Information) << "offset " << offset;
        EXPECT_EQ(0u, SandboxSocketsCreated()) << "offset " << offset;
    }
}

///////////////////////////////////////////////////////////////////////////
// End-of-file trim (ReadTrimToFileSize), via the paging path
///////////////////////////////////////////////////////////////////////////

TEST_F(ReadTest, PagingReadStartingAtEndOfFileReturnsEndOfFile)
{
    ReadRequest* req = PrepareRead(Fcb, kFileSize, 4096, IRP_PAGING_IO | IRP_NOCACHE,
        0, NewBuffer(4096));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_END_OF_FILE, status);
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
    EXPECT_EQ(0u, SandboxSocketsCreated())
        << "a read entirely past EOF must never reach the fetch issuer";
}

//
// The scripted response's Content-Length is 100, not the 8192 requested --
// Client.c independently rejects a Content-Length that disagrees with the
// range it was asked for, so this only passes if BlorgVolumeRead actually
// trimmed the length down to 100 (the bytes left before EOF) before
// calling BlorgHttpGetFileMdl, not after.
//
TEST_F(ReadTest, PagingReadStraddlingEndOfFileIsTrimmedBeforeTheFetch)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 100\r\n\r\n"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    const ULONG length = 8192;
    const ULONG64 offset = kFileSize - 100;

    unsigned char* buffer = NewBuffer(length);
    ReadRequest* req = PrepareRead(Fcb, offset, length, IRP_PAGING_IO | IRP_NOCACHE, 0, buffer);

    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));
    Drain();

    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(100u, req->Irp.IoStatus.Information)
        << "only the 100 bytes before EOF should have been fetched, not the full 8192 requested";
}

//
// The trim's second comparison is signed arithmetic on values the caller
// does not bound: a backend-declared size near LONGLONG_MAX keeps the
// offset below the first EOF test while offset + length wraps negative,
// which made both trim comparisons false and sent an untrimmed, nonsensical
// range to the fetch. The guard under test refuses the unrepresentable end
// outright; this drives exactly that shape -- offset inside the declared
// size, sum past it in unsigned terms, wrapped in signed ones.
//
TEST_F(ReadTest, UnrepresentableReadEndIsRefusedRatherThanWrapped)
{
    PFCB huge = nullptr;
    UNICODE_STRING name = Path(L"\\media\\huge.bin");

    ASSERT_EQ(STATUS_SUCCESS,
        BlorgCreateFCB(&huge, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, MAXLONGLONG));
    InitializeListHead(&huge->Links);

    const ULONG length = 4096;
    const ULONG64 offset = MAXLONGLONG - 1024;

    ReadRequest* req = PrepareRead(huge, offset, length, IRP_PAGING_IO | IRP_NOCACHE,
        0, NewBuffer(length));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_END_OF_FILE, status);
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
    EXPECT_EQ(0u, SandboxSocketsCreated())
        << "a wrapped end must never reach the fetch issuer as an untrimmed range";

    BlorgFreeFileContext(huge, Volume);
}

//
// Every dispatcher that completes inside its cases must still complete for
// a device kind that matches none of them -- the unknown kind cannot reach
// here through the I/O manager today, but a stranded IRP is one future
// routing change away, so the completion is unconditional. Before the
// default arm existed, BlorgRead returned its error status with the IRP
// uncompleted: no CompletionCount, a caller waiting forever.
//
TEST_F(ReadTest, UnknownDeviceObjectStillCompletesTheIrp)
{
    DEVICE_OBJECT foreignDevice;
    memset(&foreignDevice, 0, sizeof(foreignDevice));

    //
    // Point none of the three identity globals at anything, so
    // BlorgDeviceKind answers Unknown for the synthetic device.
    //
    PDEVICE_OBJECT savedVolume = global.VolumeDeviceObject;
    PDEVICE_OBJECT savedDisk = global.DiskDeviceObject;
    PDEVICE_OBJECT savedFileSystem = global.FileSystemDeviceObject;
    global.VolumeDeviceObject = nullptr;
    global.DiskDeviceObject = nullptr;
    global.FileSystemDeviceObject = nullptr;

    ReadRequest* req = PrepareRead(Fcb, 0, 4096, IRP_PAGING_IO | IRP_NOCACHE,
        0, NewBuffer(4096));

    NTSTATUS status = BlorgRead(&foreignDevice, &req->Irp);

    global.VolumeDeviceObject = savedVolume;
    global.DiskDeviceObject = savedDisk;
    global.FileSystemDeviceObject = savedFileSystem;

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, status);
    EXPECT_EQ(1, req->Irp.CompletionCount)
        << "an unmatched device kind must complete the IRP, not strand it";
}

///////////////////////////////////////////////////////////////////////////
// Paging path: inline direct fetch
///////////////////////////////////////////////////////////////////////////

//
// A paging read goes straight to BlorgHttpGetFileMdl -- the "load-bearing,
// not just an optimisation" inline-issue path the file's header comment
// describes: nothing posts this to the FSP queue.
//
TEST_F(ReadTest, PagingReadIssuesADirectFetchInline)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    const ULONG length = 4;
    unsigned char* buffer = NewBuffer(length);
    ReadRequest* req = PrepareRead(Fcb, 0, length, IRP_PAGING_IO | IRP_NOCACHE, 0, buffer);

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    //
    // The script's Inline=TRUE (DELIVER) means the whole round trip --
    // send, receive, parse, callback -- runs synchronously inside HttpKick,
    // before BlorgHttpGetFileMdl returns. BlorgVolumeRead's own contract is
    // unaffected: it must still report STATUS_PENDING regardless of when
    // the callback actually ran, which is the return value under test here.
    //
    ASSERT_EQ(STATUS_PENDING, status);

    Drain();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(length, req->Irp.IoStatus.Information);
    EXPECT_EQ(0, memcmp(buffer, "WXYZ", length))
        << "the fetched body must land in the caller's MDL";

    //
    // Paging reads carry none of ReadComplete's non-paging bookkeeping
    // -- FO_FILE_FAST_IO_READ must stay clear.
    //
    EXPECT_FALSE(BooleanFlagOn(req->FileObject.Flags, FO_FILE_FAST_IO_READ));
}

TEST_F(ReadTest, FailedDirectFetchCompletesTheIrpWithAFailureStatus)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 16\r\n\r\nAB"),
        CLOSE_STEP
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    const ULONG length = 16;
    ReadRequest* req = PrepareRead(Fcb, 0, length, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(length));

    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));
    Drain();

    EXPECT_FALSE(NT_SUCCESS(req->Irp.IoStatus.Status))
        << "a body truncated before Content-Length must not report success";
    EXPECT_EQ(0u, req->Irp.IoStatus.Information)
        << "a failed fetch must not report a byte count";
}

///////////////////////////////////////////////////////////////////////////
// Non-paging bookkeeping, via ReadComplete
///////////////////////////////////////////////////////////////////////////

//
// The direct-fetch path is only reachable inline for a non-paging read
// when the caller is already running on an FSP worker
// (IRP_CONTEXT_FLAG_IN_FSP) -- otherwise BlorgVolumeRead posts to the FSP
// queue instead. That flag is only ever set by FspWorkQueue.c's own
// re-dispatch (its own coverage gap, not this file's), never by
// BlorgRead's entry-point setup -- BlorgSetupIrpContext in fact asserts
// DriverContext[0] is still 0 when it runs. So this calls BlorgVolumeRead
// directly, the same layer FspWorkQueue.c itself calls into, rather than
// through BlorgRead -- which is what lets a non-paging completion's extra
// bookkeeping (CurrentByteOffset, FO_FILE_FAST_IO_READ) be observed
// without also standing up a real work-queue drive.
//
TEST_F(ReadTest, NonPagingDirectFetchAdvancesFileOffsetAndSetsFastIoOnCompletion)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    const ULONG length = 4;
    ReadRequest* req = PrepareRead(Fcb, 0, length, IRP_NOCACHE, 0, NewBuffer(length));
    req->FileObject.Flags = FO_SYNCHRONOUS_IO;
    req->Irp.Tail.Overlay.DriverContext[0] = (PVOID)(ULONG_PTR)IRP_CONTEXT_FLAG_IN_FSP;

    ASSERT_EQ(STATUS_PENDING, BlorgVolumeRead(&req->Irp, &req->Stack));
    Drain();

    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ((LONGLONG)length, req->FileObject.CurrentByteOffset.QuadPart);
    EXPECT_TRUE(BooleanFlagOn(req->FileObject.Flags, FO_FILE_FAST_IO_READ));
}

///////////////////////////////////////////////////////////////////////////
// Cached path (IRP_NOCACHE clear)
///////////////////////////////////////////////////////////////////////////

TEST_F(ReadTest, CachedReadSucceedsThroughCcCopyReadEx)
{
    const ULONG length = 4096;
    ReadRequest* req = PrepareRead(Fcb, 0, length, 0, 0, NewBuffer(length));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_SUCCESS, status);
    EXPECT_EQ(0u, SandboxSocketsCreated())
        << "the cached path must never touch the network";
}

TEST_F(ReadTest, CachedReadPastEndOfFileReturnsEndOfFile)
{
    ReadRequest* req = PrepareRead(Fcb, kFileSize, 4096, 0, 0, NewBuffer(4096));

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_END_OF_FILE, status);
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
}

TEST_F(ReadTest, CachedMdlReadUsesCcMdlRead)
{
    ReadRequest* req = PrepareRead(Fcb, 0, 4096, 0, IRP_MN_MDL, nullptr);

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_SUCCESS, status);
}

//
// CcCopyReadEx returning FALSE (a would-block miss with Wait=TRUE) is Cc's
// signal to come back on a thread that can wait -- BlorgVolumeRead answers
// by reposting to the FSP queue via BlorgFsdPostRequest rather than looping or
// blocking here. This only reaches BlorgRead's Wait=TRUE call to
// BlorgSetupIrpContext when the file object is marked synchronous.
//
// STATUS_DEVICE_REMOVED, not STATUS_PENDING, is the correct result in this
// harness: BlorgFsdPostRequest's first act is checking FspQueue.ThreadsActive,
// and nothing in DispatchSandbox starts the real FSP worker threads
// (FspWorkQueue.c has no coverage of its own yet -- see the project's
// coverage-closing plan). That gate firing is still Read.c reaching
// BlorgFsdPostRequest on this branch, which is what this test is about; driving
// a posted request all the way through a live queue is FspWorkQueue.c's
// own test to write.
//
TEST_F(ReadTest, CachedReadMissWithWaitReachesFsdPostRequest)
{
    ShimForceNextCcCopyReadMiss();

    ReadRequest* req = PrepareRead(Fcb, 0, 4096, 0, 0, NewBuffer(4096));
    req->FileObject.Flags = FO_SYNCHRONOUS_IO;
    req->Irp.Flags |= IRP_SYNCHRONOUS_API;

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    EXPECT_EQ(STATUS_DEVICE_REMOVED, status)
        << "reached BlorgFsdPostRequest, which refused because the FSP queue isn't running here";
    EXPECT_EQ(0u, SandboxSocketsCreated())
        << "a cache-miss repost must not have gone anywhere near the network";

    ShimDrainWorkItems();
}

//
// Every direct fetch this path issues is counted before the call, because
// an issue that completes synchronously runs ReadComplete -- and its
// matching decrement -- before the call returns. That leaves the
// synchronous FAILURE case for this path to settle itself: the client's
// contract is that a non-STATUS_PENDING return means the callback never
// ran, so nothing downstream will ever terminate the fetch just counted.
//
// In-flight depth is derived from exactly this difference rather than
// tracked in a gauge, so an issue that never settles does not merely
// produce a note from Compare-BlorgMetrics.ps1 ("in flight at sample
// time?") -- it makes the reported depth wrong for the life of the load,
// since nothing else will ever bring the two counters back level.
//
// A NOCACHE paging read with no MDL is the deterministic way to reach a
// synchronous failure: BlorgHttpGetFileMdl refuses a null TargetMdl
// outright, no pool-failure injection needed.
//
TEST_F(ReadTest, DirectFetchThatFailsToIssueSettlesItsOwnAccounting)
{
    BlorgStatisticsReset();

    ReadRequest* req = PrepareRead(Fcb, 0, 4096, IRP_PAGING_IO | IRP_NOCACHE, 0, nullptr);

    ASSERT_EQ(nullptr, req->Irp.MdlAddress)
        << "this test needs the issue to fail synchronously, which a null MDL guarantees";

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    ASSERT_NE(STATUS_PENDING, status)
        << "a null target MDL must be refused by the client, not issued";

    Drain();

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);

    EXPECT_EQ(0, response.FetchesActive)
        << "in-flight was left raised for a fetch that never went out and never will";

    EXPECT_EQ(response.Totals.FetchesIssued, response.Totals.FetchesCompleted + response.Totals.FetchesFailed)
        << "every issued fetch must terminate as completed or failed";
}

///////////////////////////////////////////////////////////////////////////
// Read-ahead taking fair turns on the link
///////////////////////////////////////////////////////////////////////////

//
// Two files, a player's and a copy's, each read by Cc's read-ahead in
// 4-byte paging reads, with the link's budget set to one read's worth so
// that a second read in flight is past it. Every read here is identical,
// so whatever order they reach the network in is the scheduler's alone.
//
class ReadFairTest : public ReadTest
{
protected:
    void SetUp() override
    {
        ReadTest::SetUp();

        UNICODE_STRING name = Path(L"\\media\\copy.bin");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&CopyFcb, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, kFileSize));
        InitializeListHead(&CopyFcb->Links);

        global.ReadFairBudget = 4;
        BlorgStatisticsReset();
    }

    void TearDown() override
    {
        IoSetTopLevelIrp(nullptr);
        Settle();
        global.ReadFairBudget = 0;
        BlorgFreeFileContext(CopyFcb, Volume);
        ReadTest::TearDown();
    }

    ReadRequest* ReadAhead(PFCB fcb, unsigned char* buffer)
    {
        ReadRequest* req = PrepareRead(fcb, 0, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, buffer);

        IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));
        EXPECT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));
        IoSetTopLevelIrp(nullptr);

        return req;
    }

    //
    // Drain until nothing is left. A released read is issued from a work
    // item and its deferred peer answers on the next completion drain, so
    // one pass of Drain() stops half way through.
    //
    void Settle()
    {
        do
        {
            SandboxDrainCompletions();
        } while (ShimDrainWorkItems() > 0);
    }

    ULONG64 Held()
    {
        BLORGFS_STATISTICS_RESPONSE response;
        BlorgStatisticsQuery(&response);
        return response.Totals.ReadsHeld;
    }

    PFCB CopyFcb = nullptr;
};

//
// Past the budget, read-ahead must not reach the network -- no socket for
// it -- and once the fetch ahead of it completes it must be issued and
// land like any other read. Asserting on sockets rather than on completion
// is the point: a held read and an issued read that has not completed yet
// look the same from the IRP, and only the second one is on the link.
//
// The last read defends the accounting. Every admitted byte must be
// settled, the released read's included; one leaked byte past a budget
// and every later read-ahead on the volume is held behind a fetch that
// will never complete. With the budget raised to two reads, a second read
// on a file that already has one in flight is held only if bytes leaked.
// It is checked by the held count, not by sockets: the earlier fetches
// left a pooled connection the reads may reuse.
//
TEST_F(ReadFairTest, ReadAheadPastTheBudgetWaitsThenIssues)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char* secondBuffer = NewBuffer(4);

    ReadRequest* first = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* second = ReadAhead(CopyFcb, secondBuffer);

    EXPECT_EQ(1u, SandboxSocketsCreated())
        << "read-ahead past the budget went to the network";
    EXPECT_EQ(1ull, Held());
    EXPECT_EQ(0, second->Irp.CompletionCount);

    Settle();

    EXPECT_EQ(1, first->Irp.CompletionCount);
    EXPECT_EQ(1, second->Irp.CompletionCount)
        << "the first fetch's completion never released the held read";
    EXPECT_EQ(STATUS_SUCCESS, second->Irp.IoStatus.Status);
    EXPECT_EQ(4u, second->Irp.IoStatus.Information);
    EXPECT_EQ(0, memcmp(secondBuffer, "WXYZ", 4))
        << "the released read must fetch the range it was held with";

    global.ReadFairBudget = 8;

    ReadRequest* afterFirst = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* afterSecond = ReadAhead(CopyFcb, NewBuffer(4));

    EXPECT_EQ(1ull, Held())
        << "bytes were left counted in flight with nothing on the link";

    Settle();

    EXPECT_EQ(1, afterFirst->Irp.CompletionCount);
    EXPECT_EQ(1, afterSecond->Irp.CompletionCount);
}

//
// A read admitted to the fair share that then fails to issue must give its
// bytes back, or every later read-ahead on the volume is held behind a
// fetch that will never complete. A paging read with no MDL is refused by
// the client outright. With the budget at two reads, the second of two
// reads on another file is held only if the failed read's bytes are still
// counted.
//
TEST_F(ReadFairTest, ReadAheadThatFailsToIssueGivesItsBytesBack)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    global.ReadFairBudget = 8;

    ReadRequest* failed = PrepareRead(CopyFcb, 0, 4, IRP_PAGING_IO | IRP_NOCACHE);

    IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));
    EXPECT_NE(STATUS_PENDING, BlorgRead(Volume, &failed->Irp));
    IoSetTopLevelIrp(nullptr);

    ReadRequest* first = ReadAhead(Fcb, NewBuffer(4));
    ReadRequest* second = ReadAhead(Fcb, NewBuffer(4));

    EXPECT_EQ(0ull, Held())
        << "a read that never reached the link was left counted in flight";

    Settle();

    EXPECT_EQ(1, first->Irp.CompletionCount);
    EXPECT_EQ(1, second->Irp.CompletionCount);
}

//
// The same for a held read: it is issued from a work item once released,
// and if the issue fails there it is completed with the failure and its
// bytes settled, rather than left pending with them counted.
//
TEST_F(ReadFairTest, HeldReadThatFailsToIssueCompletesAndGivesItsBytesBack)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    ReadRequest* first = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* held = ReadAhead(CopyFcb, nullptr);

    ASSERT_EQ(1ull, Held());

    Settle();

    EXPECT_EQ(1, first->Irp.CompletionCount);
    EXPECT_EQ(1, held->Irp.CompletionCount)
        << "a released read that failed to issue was never completed";
    EXPECT_FALSE(NT_SUCCESS(held->Irp.IoStatus.Status));

    global.ReadFairBudget = 8;

    ReadRequest* afterFirst = ReadAhead(Fcb, NewBuffer(4));
    ReadRequest* afterSecond = ReadAhead(Fcb, NewBuffer(4));

    EXPECT_EQ(1ull, Held())
        << "the failed read's bytes were left counted in flight";

    Settle();

    EXPECT_EQ(1, afterFirst->Irp.CompletionCount);
    EXPECT_EQ(1, afterSecond->Irp.CompletionCount);
}

//
// The case the mechanism exists for. Beside three copies a 6 MB/s player
// missed about 29% of its deadlines on the reference link, because each of
// its fetches queued behind the copies' bytes. With a copy's reads already
// waiting, a player's read that arrives after them must still go ahead of
// the copy's later one: its file has fetched less, so its start tag is
// lower, while each queued copy read is charged for every copy read ahead
// of it. Arrival order would send both copy reads first, and a player that
// falls behind would wait behind every copy's backlog.
//
// The budget is two reads here, so both files get their first fetch and
// every later one queues. The first two completions free two slots: one
// goes to the copy's earlier read, whose tag ties the player's and which
// arrived first, and the other must go to the player, not to the copy's
// later read. The peer answers each fetch only on SandboxDrainCompletions,
// and a released read is issued only on ShimDrainWorkItems, so the steps
// below decide exactly which reads were released.
//
TEST_F(ReadFairTest, AReaderThatHasFetchedLessGoesAheadOfAQueuedCopy)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    global.ReadFairBudget = 8;

    ReadRequest* playerFirst = ReadAhead(Fcb, NewBuffer(4));
    ReadRequest* copyFirst = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* copySecond = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* copyThird = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* player = ReadAhead(Fcb, NewBuffer(4));

    ASSERT_EQ(2u, SandboxSocketsCreated());
    ASSERT_EQ(3ull, Held());

    SandboxDrainCompletions();
    ASSERT_EQ(1, playerFirst->Irp.CompletionCount);
    ASSERT_EQ(1, copyFirst->Irp.CompletionCount);
    ShimDrainWorkItems();
    SandboxDrainCompletions();

    EXPECT_EQ(1, copySecond->Irp.CompletionCount);
    EXPECT_EQ(1, player->Irp.CompletionCount)
        << "the copy's queued read went ahead of a player that had fetched less";
    EXPECT_EQ(0, copyThird->Irp.CompletionCount);

    Settle();

    EXPECT_EQ(1, copyThird->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, copyThird->Irp.IoStatus.Status);
}

//
// A file with nothing in flight is never held, however far past the budget
// the link is. Cc keeps a player to one read-ahead at a time, so holding
// that one made it wait for someone else's completion before every fetch
// it ever made; measured, that alone kept it 31% late beside copies after
// fair order had cut its fetches' wait for a first byte from 97 ms to 6.
//
TEST_F(ReadFairTest, AFileWithNothingInFlightIsNeverHeld)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    ReadRequest* copy = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* player = ReadAhead(Fcb, NewBuffer(4));

    EXPECT_EQ(2u, SandboxSocketsCreated())
        << "the player's only read-ahead waited behind a copy past the budget";
    EXPECT_EQ(0ull, Held());

    Settle();

    EXPECT_EQ(1, copy->Irp.CompletionCount);
    EXPECT_EQ(1, player->Irp.CompletionCount);
}

//
// Under the budget nothing waits: a lone reader on a quiet link must run
// exactly as before the budget existed.
//
TEST_F(ReadFairTest, ReadAheadUnderTheBudgetIsIssuedAtOnce)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    global.ReadFairBudget = 8;

    ReadRequest* first = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* second = ReadAhead(Fcb, NewBuffer(4));

    EXPECT_EQ(2u, SandboxSocketsCreated());
    EXPECT_EQ(0ull, Held());

    Settle();

    EXPECT_EQ(1, first->Irp.CompletionCount);
    EXPECT_EQ(1, second->Irp.CompletionCount);
}

//
// A demand fault has an application blocked on it, so however far past the
// budget the link is, it is issued at once.
//
TEST_F(ReadFairTest, DemandFaultIsNeverHeld)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    ReadRequest* readAhead = ReadAhead(CopyFcb, NewBuffer(4));

    ReadRequest* fault = PrepareRead(CopyFcb, 0, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &fault->Irp));

    EXPECT_EQ(2u, SandboxSocketsCreated())
        << "a fault with an application blocked on it was held behind read-ahead";
    EXPECT_EQ(0ull, Held());

    Settle();

    EXPECT_EQ(1, readAhead->Irp.CompletionCount);
    EXPECT_EQ(1, fault->Irp.CompletionCount);
}

//
// Demand is never held, so other files faulting can keep the link past the
// budget for as long as they run, and a release that waits for room then
// never comes. Beside two copies on the reference link that stranded a
// player's held read-ahead for 25 s, and the player's next read behind it,
// while no fetch took more than 417 ms. When a file's last fetch settles,
// its own held read must go out whatever the budget.
//
// The other file's fault stalls so that it is still in flight, and the
// link still past the budget, when the copy's first fetch completes; a
// fault that completed in the same drain would make room and release the
// read on its own. Settling everything would not catch this, so the copy's
// held read is checked while the fault is still parked.
//
TEST_F(ReadFairTest, AFileWhoseLastFetchSettlesGetsItsHeldReadPastTheBudget)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    static const SANDBOX_STEP stalled[] =
    {
        { SandboxStepStall, nullptr, 0, STATUS_SUCCESS, FALSE },
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    unsigned char* heldBuffer = NewBuffer(4);

    ReadRequest* first = ReadAhead(CopyFcb, NewBuffer(4));
    ReadRequest* held = ReadAhead(CopyFcb, heldBuffer);

    ASSERT_EQ(1ull, Held());

    SandboxSetPeerScript(stalled, RTL_NUMBER_OF(stalled));

    ReadRequest* fault = PrepareRead(Fcb, 0, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &fault->Irp));

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    SandboxDrainCompletions();
    ASSERT_EQ(1, first->Irp.CompletionCount);
    ShimDrainWorkItems();
    SandboxDrainCompletions();

    EXPECT_EQ(1, held->Irp.CompletionCount)
        << "a file with nothing in flight was left held while another file's fault kept the link past the budget";
    EXPECT_EQ(STATUS_SUCCESS, held->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(heldBuffer, "WXYZ", 4));
    EXPECT_EQ(0, fault->Irp.CompletionCount);

    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, fault->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, fault->Irp.IoStatus.Status);
}

} // namespace
