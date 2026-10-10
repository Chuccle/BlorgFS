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

// Not declared in any header either; FspWorkQueueStressTest.cpp runs it the
// same way, on a thread of its own.
VOID BlorgFspDispatch(PVOID StartContext);
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
    // end of something that is not a node. Each request is its own handle,
    // with the CCB Create.c gives every file open, which is where the
    // cached path keeps the read-ahead granule Cc was told on it.
    //
    struct ReadRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
        CCB Ccb;
    };

    ReadRequest* PrepareRead(PFCB fcb, ULONG64 offset, ULONG length, ULONG irpFlags,
        UCHAR minorFunction = 0, unsigned char* buffer = nullptr)
    {
        Requests.push_back(std::make_unique<ReadRequest>());
        ReadRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->Ccb.NodeTypeCode = BLORGFS_CCB_SIGNATURE;
        req->Ccb.NodeByteSize = sizeof(CCB);

        req->FileObject.FsContext = fcb;
        req->FileObject.FsContext2 = &req->Ccb;
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
// A posted non-paging read reaches the direct-fetch path on an FSP worker
// (IRP_CONTEXT_FLAG_IN_FSP), with its buffer locked by the post. That flag
// is only ever set by FspWorkQueue.c's own re-dispatch, never by
// BlorgRead's entry-point setup -- BlorgSetupIrpContext in fact asserts
// DriverContext[0] is still 0 when it runs. So this calls BlorgVolumeRead
// directly, the same layer FspWorkQueue.c itself calls into, rather than
// through BlorgRead -- which is what lets a posted non-paging completion's
// extra bookkeeping (CurrentByteOffset, FO_FILE_FAST_IO_READ) be observed
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
// The same miss with the queue running: the FSD pass posts, and a worker
// serves the read. Each pass used to count the read's bytes toward the
// read-ahead window, and the worker's pass recorded no latency, because the
// queue had cleared the arrival stamp. The idle gap that decides whether a
// reader is greedy then included the whole stall, so an overlapped copy
// never grew its granule. Counted once, after the copy, and timed by the
// worker, the read looks the same as one served without a post.
//
TEST_F(ReadTest, APostedCachedReadIsCountedOnceAndTimedByTheWorker)
{
    ASSERT_EQ(STATUS_SUCCESS, BlorgCreateWorkQueue());

    HANDLE worker = CreateThread(NULL, 0, [](LPVOID) -> DWORD { BlorgFspDispatch(NULL); return 0; }, NULL, 0, NULL);
    ASSERT_NE((HANDLE)NULL, worker);

    const ULONG length = 4096;
    const ULONG64 consumed = Fcb->ReadAheadConsumedBytes;
    const ULONG64 samples = BlorgStatisticsForCurrentProcessor()->UserReadSamples;

    ShimForceNextCcCopyReadMiss();
    ShimSetNextCcCopyReadInformation(length);

    ReadRequest* req = PrepareRead(Fcb, 0, length, 0, 0, NewBuffer(length));
    req->FileObject.Flags = FO_SYNCHRONOUS_IO;
    req->Irp.Flags |= IRP_SYNCHRONOUS_API;

    EXPECT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));

    const DWORD start = GetTickCount();

    while (0 == ReadNoFence(&req->Irp.CompletionCount) && GetTickCount() - start < 30000)
    {
        SwitchToThread();
    }

    BlorgDestroyWorkQueue();
    EXPECT_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker, 30000));
    CloseHandle(worker);

    ASSERT_EQ(1u, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(consumed + length, Fcb->ReadAheadConsumedBytes)
        << "a posted read counted toward the read-ahead window once per pass";
    EXPECT_EQ(samples + 1, BlorgStatisticsForCurrentProcessor()->UserReadSamples)
        << "the worker's completion recorded no latency: the arrival stamp did not survive the queue";
    EXPECT_NE(0, Fcb->ReadIdleLastEndQpc);
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

//
// Nothing else bounds demand, so the fetch limit has to. A mapped read
// faulting a page at a time issued 10,100 fetches at once in the guest,
// one connection each, and the connects that overran the server's accept
// queue timed out and failed the reads. Past READ_FAIR_MAX_FETCHES a fault
// must not reach the network until a fetch settles, and must then be
// issued and land like any other.
//
TEST_F(ReadFairTest, DemandPastTheFetchLimitWaitsForAFetchToSettle)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    std::vector<ReadRequest*> faults;

    for (ULONG i = 0; i <= READ_FAIR_MAX_FETCHES; i++)
    {
        ReadRequest* fault = PrepareRead(Fcb, i * 4ull, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
        ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &fault->Irp));
        faults.push_back(fault);
    }

    EXPECT_EQ(C_CAST(ULONG, READ_FAIR_MAX_FETCHES), SandboxSocketsCreated())
        << "a fault past the fetch limit went to the network";
    EXPECT_EQ(0ull, Held())
        << "a fault waiting for the fetch limit was counted as held read-ahead";

    Settle();

    for (ReadRequest* fault : faults)
    {
        EXPECT_EQ(1, fault->Irp.CompletionCount);
        EXPECT_EQ(STATUS_SUCCESS, fault->Irp.IoStatus.Status);
        EXPECT_EQ(4u, fault->Irp.IoStatus.Information);
    }
}

//
// A fault past the fetch limit is released from a work item, so one that
// cannot have a work item cannot wait. It fails, with nothing sent, rather
// than going out past the limit. Admission used to allocate the work item
// only when an unlocked look at the link said a wait was coming, and went
// ahead without one whenever the locked look disagreed.
//
TEST_F(ReadFairTest, DemandPastTheFetchLimitWithoutAWorkItemFailsRatherThanGoingOut)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER_LATER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    std::vector<ReadRequest*> faults;

    for (ULONG i = 0; i < READ_FAIR_MAX_FETCHES; i++)
    {
        ReadRequest* fault = PrepareRead(Fcb, i * 4ull, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
        ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &fault->Irp));
        faults.push_back(fault);
    }

    ShimFailNextWorkItem();

    ReadRequest* over = PrepareRead(Fcb, READ_FAIR_MAX_FETCHES * 4ull, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));

    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, BlorgRead(Volume, &over->Irp));
    EXPECT_EQ(1, over->Irp.CompletionCount);
    EXPECT_EQ(C_CAST(ULONG, READ_FAIR_MAX_FETCHES), SandboxSocketsCreated())
        << "a fault with nothing to wait on went to the network past the fetch limit";

    Settle();

    for (ReadRequest* fault : faults)
    {
        EXPECT_EQ(1, fault->Irp.CompletionCount);
        EXPECT_EQ(STATUS_SUCCESS, fault->Irp.IoStatus.Status);
    }
}

//
// A fault has an application blocked on it and read-ahead does not, so
// when a slot frees, a waiting fault takes it ahead of read-ahead that
// was held first. Every fetch but one stalls, so exactly one slot frees
// on the first drain; the read-ahead's file has nothing in flight, so the
// budget alone would never have held it.
//
TEST_F(ReadFairTest, WaitingDemandGoesAheadOfHeldReadAhead)
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
    SandboxSetPeerScript(stalled, RTL_NUMBER_OF(stalled));

    for (ULONG i = 0; i + 1 < READ_FAIR_MAX_FETCHES; i++)
    {
        ReadRequest* fault = PrepareRead(Fcb, i * 4ull, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
        ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &fault->Irp));
    }

    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    ReadRequest* last = PrepareRead(Fcb, 0, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, NewBuffer(4));
    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &last->Irp));

    ReadRequest* readAhead = ReadAhead(CopyFcb, NewBuffer(4));

    unsigned char* waitingBuffer = NewBuffer(4);
    ReadRequest* waiting = PrepareRead(Fcb, 4, 4, IRP_PAGING_IO | IRP_NOCACHE, 0, waitingBuffer);
    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &waiting->Irp));

    ASSERT_EQ(C_CAST(ULONG, READ_FAIR_MAX_FETCHES), SandboxSocketsCreated());

    SandboxDrainCompletions();
    ASSERT_EQ(1, last->Irp.CompletionCount);
    ShimDrainWorkItems();
    SandboxDrainCompletions();

    EXPECT_EQ(1, waiting->Irp.CompletionCount)
        << "the slot a settled fetch freed did not go to the waiting fault";
    EXPECT_EQ(STATUS_SUCCESS, waiting->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(waitingBuffer, "WXYZ", 4));
    EXPECT_EQ(0, readAhead->Irp.CompletionCount)
        << "read-ahead took the freed slot ahead of a fault an application is blocked on";

    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, readAhead->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, readAhead->Irp.IoStatus.Status);
}

//
// The fast I/O gate admits reads of a file and nothing else: a write, or
// any operation on a directory, goes back as an IRP before FsRtl is asked
// about byte-range locks.
//
TEST_F(ReadTest, FastIoGateAllowsOnlyFileReads)
{
    FILE_OBJECT file = {};
    LARGE_INTEGER offset = {};
    IO_STATUS_BLOCK status = {};

    file.FsContext = Fcb;
    EXPECT_TRUE(BlorgFastIoCheckIfPossible(&file, &offset, 4, TRUE, 0, TRUE, &status, Volume));
    EXPECT_FALSE(BlorgFastIoCheckIfPossible(&file, &offset, 4, TRUE, 0, FALSE, &status, Volume));

    file.FsContext = VcbNode;
    EXPECT_FALSE(BlorgFastIoCheckIfPossible(&file, &offset, 4, TRUE, 0, TRUE, &status, Volume));
    EXPECT_FALSE(BlorgFastIoCheckIfPossible(&file, &offset, 4, TRUE, 0, FALSE, &status, Volume));
}

//
// Cc calls the lazy-write and read-ahead callbacks itself, so no read
// through the copy stub reaches them. Each acquire marks the thread as
// Cc's top-level IRP and each release must clear it again, along with the
// FCB's lazy writer, and the kernel model checks the resources balance.
//
TEST_F(ReadTest, CacheManagerCallbacksBalanceResourcesAndTopLevelIrp)
{
    const PVOID lazyWriter = global.LazyWriteThread;

    KeEnterCriticalRegion();

    EXPECT_TRUE(BlorgAcquireNodeForLazyWrite(Fcb, TRUE));
    EXPECT_EQ(PsGetCurrentThread(), Fcb->LazyWriteThread);
    EXPECT_EQ(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP), IoGetTopLevelIrp());
    BlorgReleaseNodeFromLazyWrite(Fcb);
    EXPECT_EQ(nullptr, Fcb->LazyWriteThread);
    EXPECT_EQ(nullptr, IoGetTopLevelIrp());

    EXPECT_TRUE(BlorgAcquireNodeForReadAhead(Fcb, TRUE));
    EXPECT_EQ(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP), IoGetTopLevelIrp());
    BlorgReleaseNodeFromReadAhead(Fcb);
    EXPECT_EQ(nullptr, IoGetTopLevelIrp());

    KeLeaveCriticalRegion();

    global.LazyWriteThread = lazyWriter;
}

//
// A fast read FsRtl served counts as one user read and as bytes consumed
// from the read-ahead window. One it declined comes back as an IRP that
// counts itself, so it must leave both alone, even though IoStatus still
// holds the byte count of the read before it.
//
TEST_F(ReadTest, FastIoReadCountsOnlyHandledReads)
{
    FILE_OBJECT file = {};
    LARGE_INTEGER offset = {};
    IO_STATUS_BLOCK status = {};
    unsigned char buffer[64] = {};

    file.FsContext = Fcb;

    const ULONG64 samples = BlorgStatisticsForCurrentProcessor()->UserReadSamples;
    const ULONG64 consumed = Fcb->ReadAheadConsumedBytes;

    ShimSetNextCcCopyReadInformation(sizeof(buffer));
    EXPECT_TRUE(BlorgFastIoRead(&file, &offset, sizeof(buffer), TRUE, 0, buffer, &status, Volume));
    EXPECT_EQ(STATUS_SUCCESS, status.Status);
    EXPECT_EQ(sizeof(buffer), status.Information);
    EXPECT_EQ(samples + 1, BlorgStatisticsForCurrentProcessor()->UserReadSamples);
    EXPECT_EQ(consumed + sizeof(buffer), Fcb->ReadAheadConsumedBytes);

    const LONG64 completed = Fcb->ReadIdleLastEndQpc;
    EXPECT_NE(0, completed);

    ShimForceNextCcCopyReadMiss();
    EXPECT_FALSE(BlorgFastIoRead(&file, &offset, sizeof(buffer), TRUE, 0, buffer, &status, Volume));
    EXPECT_EQ(samples + 1, BlorgStatisticsForCurrentProcessor()->UserReadSamples);
    EXPECT_EQ(consumed + sizeof(buffer), Fcb->ReadAheadConsumedBytes);
    EXPECT_EQ(completed, Fcb->ReadIdleLastEndQpc);
}

//
// A non-paging non-cached read (FILE_FLAG_NO_BUFFERING) arrives at
// PASSIVE_LEVEL in the requester's own context, which is where its buffer
// has to be locked, and nothing after the lock blocks. It used to be posted
// to the FSP anyway, a worker hop and a context switch per read. The queue
// is not running here, so a post is refused with STATUS_DEVICE_REMOVED; a
// read issued inline without locking has no MDL for the body to land in.
//
TEST_F(ReadTest, ANonPagingUncachedReadLocksItsBufferAndIssuesInline)
{
    static const SANDBOX_STEP script[] =
    {
        DELIVER("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ")
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    const ULONG length = 4;
    unsigned char* buffer = NewBuffer(length);
    ReadRequest* req = PrepareRead(Fcb, 0, length, IRP_NOCACHE);
    req->Irp.UserBuffer = buffer;
    req->FileObject.Flags = FO_SYNCHRONOUS_IO;

    ASSERT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));

    Drain();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(length, req->Irp.IoStatus.Information);
    EXPECT_EQ(0, memcmp(buffer, "WXYZ", length)) << "the body must land in the caller's own buffer";
    EXPECT_EQ((LONGLONG)length, req->FileObject.CurrentByteOffset.QuadPart);
    ASSERT_NE(nullptr, req->Irp.MdlAddress) << "the user buffer was never locked";
    EXPECT_TRUE(req->Irp.MdlAddress->Locked);

    ShimReleaseIrpMdl(&req->Irp);
}

///////////////////////////////////////////////////////////////////////////
// Adaptive read-ahead, one granule per handle
///////////////////////////////////////////////////////////////////////////

//
// Cc keeps the read-ahead granule per file object, so two handles on one
// file each have their own, and the policy must move only the one whose
// read closed the window. It used to keep one granule per file: a second
// handle's first read reset it to the starting value, and the first
// handle's next vote then doubled or halved that rather than its own. A
// copy grown to 2 MB was told 256 KB, and a player still at 128 KB that
// voted shrink after a copy had grown was told 1 MB.
//
// The windows here are closed by fast reads that each consume a whole
// window, with the evidence a window is judged on -- how much was fetched,
// how far Cc honoured the granule, the stream's streak, the consumer's
// idle share -- set on the FCB just before. What is checked is what Cc
// was told on each file object, which is the only thing that reaches Cc.
//
class ReadAheadHandleTest : public ReadTest
{
protected:
    void SetUp() override
    {
        ReadTest::SetUp();

        SavedGranularity = global.ReadAheadGranularity;
        SavedMaxGranularity = global.ReadAheadMaxGranularity;
        SavedAdapt = global.ReadAheadAdapt;
        SavedSlackGrowth = global.ReadAheadSlackGrowth;

        global.ReadAheadGranularity = READ_AHEAD_GRANULARITY;
        global.ReadAheadMaxGranularity = READ_AHEAD_MAX_GRANULARITY;
        global.ReadAheadAdapt = TRUE;
        global.ReadAheadSlackGrowth = TRUE;

        ShimReadAheadGranularityReset();
    }

    void TearDown() override
    {
        global.ReadAheadGranularity = SavedGranularity;
        global.ReadAheadMaxGranularity = SavedMaxGranularity;
        global.ReadAheadAdapt = SavedAdapt;
        global.ReadAheadSlackGrowth = SavedSlackGrowth;

        ShimReadAheadGranularityReset();

        ReadTest::TearDown();
    }

    //
    // A handle's first cached read, which sets up its cache map and tells
    // Cc the starting granule. The model's CcInitializeCacheMap leaves
    // PrivateCacheMap alone, so it is set here the way the real one sets it.
    //
    ReadRequest* OpenHandle()
    {
        ReadRequest* req = PrepareRead(Fcb, 0, 4096, 0, 0, NewBuffer(4096));

        EXPECT_EQ(STATUS_SUCCESS, BlorgRead(Volume, &req->Irp));

        req->FileObject.PrivateCacheMap = &req->Ccb;

        return req;
    }

    //
    // One fast read that closes a window on File judged at Granule: a
    // window is two granules, or 256 KB when that is larger.
    //
    void CloseWindow(PFILE_OBJECT File, ULONG Granule, ULONG64 Fetched)
    {
        const ULONG window = (2 * Granule > 256 * 1024) ? (2 * Granule) : (256 * 1024);
        LARGE_INTEGER offset = {};
        IO_STATUS_BLOCK status = {};
        unsigned char buffer[16] = {};

        Fcb->ReadAheadConsumedBytes = 0;
        Fcb->ReadAheadFetchedBytes = Fetched;
        Fcb->ReadIdleLastEndQpc = 0;

        ShimSetNextCcCopyReadInformation(window);
        EXPECT_TRUE(BlorgFastIoRead(File, &offset, window, TRUE, 0, buffer, &status, Volume));
    }

    //
    // A window a copy closes: nothing fetched beyond what it consumed, Cc
    // honouring the whole granule, the current stream sixteen reads long,
    // and the consumer idle for one tick in a thousand.
    //
    void GrowWindow(PFILE_OBJECT File, ULONG Granule)
    {
        Fcb->ReadMaxPagingBytes = Granule;
        Fcb->Streams[Fcb->ReadLastStreamIndex].Streak = 16;
        Fcb->ReadIdleTicks = 1;
        Fcb->ReadBusyTicks = 1000;

        CloseWindow(File, Granule, 0);
    }

    //
    // A window that fetched eight times what it consumed.
    //
    void ShrinkWindow(PFILE_OBJECT File, ULONG Granule)
    {
        const ULONG64 window = (2 * Granule > 256 * 1024) ? (2 * Granule) : (256 * 1024);

        Fcb->ReadMaxPagingBytes = 0;
        Fcb->ReadIdleTicks = 0;
        Fcb->ReadBusyTicks = 0;

        CloseWindow(File, Granule, window * 8);
    }

    void GrowToCeiling(PFILE_OBJECT File)
    {
        for (ULONG granule = READ_AHEAD_GRANULARITY; granule < READ_AHEAD_MAX_GRANULARITY; granule *= 2)
        {
            GrowWindow(File, granule);
            GrowWindow(File, granule);
            ASSERT_EQ(granule * 2, ShimReadAheadGranularity(File))
                << "two agreeing grow votes double the granule";
        }
    }

    ULONG SavedGranularity = 0;
    ULONG SavedMaxGranularity = 0;
    BOOLEAN SavedAdapt = FALSE;
    BOOLEAN SavedSlackGrowth = FALSE;
};

TEST_F(ReadAheadHandleTest, ASecondHandleLeavesTheFirstHandlesGrownGranuleAlone)
{
    ReadRequest* copy = OpenHandle();
    ASSERT_EQ(READ_AHEAD_GRANULARITY, ShimReadAheadGranularity(&copy->FileObject));

    GrowToCeiling(&copy->FileObject);
    ASSERT_EQ(READ_AHEAD_MAX_GRANULARITY, ShimReadAheadGranularity(&copy->FileObject));

    ReadRequest* player = OpenHandle();
    EXPECT_EQ(READ_AHEAD_GRANULARITY, ShimReadAheadGranularity(&player->FileObject));

    GrowWindow(&copy->FileObject, READ_AHEAD_MAX_GRANULARITY);
    GrowWindow(&copy->FileObject, READ_AHEAD_MAX_GRANULARITY);

    EXPECT_EQ(READ_AHEAD_MAX_GRANULARITY, ShimReadAheadGranularity(&copy->FileObject))
        << "the copy's grow vote doubled the granule the player's first read set, not its own";
    EXPECT_EQ(READ_AHEAD_GRANULARITY, ShimReadAheadGranularity(&player->FileObject));
}

TEST_F(ReadAheadHandleTest, AShrinkVotedOnOneHandleHalvesThatHandlesOwnGranule)
{
    ReadRequest* copy = OpenHandle();
    ReadRequest* player = OpenHandle();

    GrowToCeiling(&copy->FileObject);

    ShrinkWindow(&player->FileObject, READ_AHEAD_GRANULARITY);
    ShrinkWindow(&player->FileObject, READ_AHEAD_GRANULARITY);

    EXPECT_EQ(READ_AHEAD_GRANULARITY / 2, ShimReadAheadGranularity(&player->FileObject))
        << "the player's shrink halved the copy's grown granule, raising its own";
    EXPECT_EQ(READ_AHEAD_MAX_GRANULARITY, ShimReadAheadGranularity(&copy->FileObject));
}

///////////////////////////////////////////////////////////////////////////
// ReadAdaptGranularity
///////////////////////////////////////////////////////////////////////////

//
// One handle reading a file, and what the policy tells Cc about its
// read-ahead granularity as it goes. Every assertion is on what reaches
// Cc (CcSetReadAheadGranularity, recorded per file object by
// DispatchModel.c) and on the ReadAdapt counters, never on where the
// driver keeps its window. ReadAheadHandleTest sets and restores the
// read-ahead configuration; the granule starts at READ_AHEAD_GRANULARITY.
//
// Cc's read-ahead is modelled by paging reads on the same file object,
// which is what counts as fetched. Their fetches are refused before they
// are issued (no MDL, as in DirectFetchThatFailsToIssueSettlesItsOwnAccounting):
// a paging read is counted toward the window when it is dispatched, so the
// network has nothing to add. The application's reads are fast I/O reads
// whose byte count closes a window, and the time each spends inside the
// copy against the time between them is what says whether the reader ever
// idles.
//
class ReadAdaptTest : public ReadAheadHandleTest
{
protected:
    void SetUp() override
    {
        ReadAheadHandleTest::SetUp();

        BlorgStatisticsReset();
        Stats = BlorgStatisticsForCurrentProcessor();
        ASSERT_NE(nullptr, Stats);

        ASSERT_EQ(STATUS_SUCCESS, BlorgCreateCCB(&Ccb, Volume));

        Handle.FsContext = Fcb;
        Handle.FsContext2 = Ccb;
        Handle.DeviceObject = Volume;
        Handle.SectionObjectPointer = &Fcb->NonPaged->SectionObjectPointers;
    }

    void TearDown() override
    {
        ShimSetCcCopyReadTicks(0);
        BlorgFreeFileContext(Ccb, Volume);

        ReadAheadHandleTest::TearDown();
    }

    //
    // The handle's first cached read, which sets up its cache map and
    // tells Cc the starting granule. Later reads find the cache map there,
    // as the real CcInitializeCacheMap leaves it.
    //
    void Open()
    {
        ReadRequest* req = PrepareRead(Fcb, 0, PAGE_SIZE, 0, 0, NewBuffer(PAGE_SIZE));
        req->Stack.FileObject = &Handle;

        ASSERT_EQ(STATUS_SUCCESS, BlorgRead(Volume, &req->Irp));
        ASSERT_EQ(kGranule, ShimReadAheadGranularity(&Handle));

        Handle.PrivateCacheMap = &Handle;
    }

    //
    // Count paging reads of Length bytes, each starting Gap bytes past
    // where the last one ended.
    //
    void ReadAhead(ULONG count, ULONG length, ULONG64 gap)
    {
        for (ULONG i = 0; i < count; ++i)
        {
            NextOffset += gap;

            ReadRequest* req = PrepareRead(Fcb, NextOffset, length, IRP_PAGING_IO | IRP_NOCACHE);
            req->Stack.FileObject = &Handle;

            EXPECT_NE(STATUS_PENDING, BlorgRead(Volume, &req->Irp));

            NextOffset += length;
        }
    }

    void Consume(ULONG bytes)
    {
        LARGE_INTEGER offset = {};
        IO_STATUS_BLOCK status = {};
        unsigned char buffer[16] = {};

        ShimSetNextCcCopyReadInformation(bytes);
        EXPECT_TRUE(BlorgFastIoRead(&Handle, &offset, bytes, TRUE, 0, buffer, &status, Volume));
    }

    //
    // A window in which Cc fetched four times what the reader took.
    //
    void WastefulWindow()
    {
        ReadAhead(8, kGranule, kGranule);
        Consume(2 * kGranule);
    }

    //
    // A window that earns growth: a run of adjacent paging reads, one of
    // them a whole granule, so Cc honoured what it was asked for; little
    // more fetched than consumed; and a reader that spends its time inside
    // the copy rather than between reads.
    //
    void GreedySequentialWindow()
    {
        ShimSetCcCopyReadTicks(kCopyTicks);
        ReadAhead(16, PAGE_SIZE, 0);
        ReadAhead(1, kGranule, 0);
        Consume(2 * kGranule);
        ShimSetCcCopyReadTicks(0);
    }

    static constexpr ULONG kGranule = READ_AHEAD_GRANULARITY;
    static constexpr LONG64 kCopyTicks = 1000000;

    FILE_OBJECT Handle = {};
    PCCB Ccb = nullptr;
    PBLORGFS_STATISTICS Stats = nullptr;
    ULONG64 NextOffset = 0;
};

//
// Read-ahead fetching far more than the reader takes halves the granule,
// but only once two windows in a row say so: one window's ratio is not
// evidence.
//
TEST_F(ReadAdaptTest, WastedReadAheadShrinksTheGranuleAfterTwoWindowsAgree)
{
    Open();

    const LONG setsBefore = ShimReadAheadGranularitySets();

    WastefulWindow();

    EXPECT_EQ(1u, Stats->ReadAdaptWindows);
    EXPECT_EQ(1u, Stats->ReadAdaptVotesShrink);
    EXPECT_EQ(setsBefore, ShimReadAheadGranularitySets())
        << "one wasteful window moved the granule on its own";

    WastefulWindow();

    EXPECT_EQ(2u, Stats->ReadAdaptVotesShrink);
    EXPECT_EQ(setsBefore + 1, ShimReadAheadGranularitySets());
    EXPECT_EQ(kGranule / 2, ShimReadAheadGranularity(&Handle));
    EXPECT_EQ(1u, Stats->ReadAheadShrinks);
    EXPECT_EQ(0u, Stats->ReadAheadGrows);
}

//
// Growth needs a greedy sequential reader whose granule Cc honours, two
// windows running. Each window that lacks one of those -- a reader that
// pauses between reads, a reader seeking about, read-ahead Cc capped below
// the granule -- votes nothing, and clears the vote before it, so the
// granule only doubles on the last pair.
//
TEST_F(ReadAdaptTest, GreedySequentialReaderGrowsTheGranuleOnlyWhenEverySignalAgrees)
{
    Open();

    const LONG setsBefore = ShimReadAheadGranularitySets();

    GreedySequentialWindow();
    EXPECT_EQ(1u, Stats->ReadAdaptVotesGrow);

    ReadAhead(16, PAGE_SIZE, 0);
    ReadAhead(1, kGranule, 0);
    ShimAdvancePerformanceCounter(kCopyTicks);
    Consume(2 * kGranule);
    EXPECT_EQ(1u, Stats->ReadAdaptVotesGrow) << "a reader that pauses between reads has a deadline";

    GreedySequentialWindow();
    EXPECT_EQ(2u, Stats->ReadAdaptVotesGrow);

    ShimSetCcCopyReadTicks(kCopyTicks);
    ReadAhead(16, PAGE_SIZE, kGranule);
    ReadAhead(1, kGranule, kGranule);
    Consume(2 * kGranule);
    ShimSetCcCopyReadTicks(0);
    EXPECT_EQ(2u, Stats->ReadAdaptVotesGrow) << "a reader seeking about is not sequential";

    GreedySequentialWindow();
    EXPECT_EQ(3u, Stats->ReadAdaptVotesGrow);

    ShimSetCcCopyReadTicks(kCopyTicks);
    ReadAhead(17, PAGE_SIZE, 0);
    Consume(2 * kGranule);
    ShimSetCcCopyReadTicks(0);
    EXPECT_EQ(3u, Stats->ReadAdaptVotesGrow) << "Cc never read a whole granule, so a larger one changes nothing";

    EXPECT_EQ(6u, Stats->ReadAdaptWindows);
    EXPECT_EQ(0u, Stats->ReadAdaptVotesShrink);
    EXPECT_EQ(setsBefore, ShimReadAheadGranularitySets())
        << "the granule moved without two agreeing windows in a row";

    GreedySequentialWindow();
    GreedySequentialWindow();

    EXPECT_EQ(5u, Stats->ReadAdaptVotesGrow);
    EXPECT_EQ(setsBefore + 1, ShimReadAheadGranularitySets());
    EXPECT_EQ(2 * kGranule, ShimReadAheadGranularity(&Handle));
    EXPECT_EQ(1u, Stats->ReadAheadGrows);
    EXPECT_EQ(0u, Stats->ReadAheadShrinks);
}

//
// ReadAheadAdapt=0 pins the granule where it started, however clear the
// evidence.
//
TEST_F(ReadAdaptTest, AdaptOffPinsTheGranule)
{
    global.ReadAheadAdapt = FALSE;

    Open();

    const LONG setsBefore = ShimReadAheadGranularitySets();

    WastefulWindow();
    WastefulWindow();
    WastefulWindow();

    EXPECT_EQ(setsBefore, ShimReadAheadGranularitySets());
    EXPECT_EQ(kGranule, ShimReadAheadGranularity(&Handle));
    EXPECT_EQ(0u, Stats->ReadAdaptWindows);
}

} // namespace
