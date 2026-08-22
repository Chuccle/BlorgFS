//
// Coverage for the real Read.c: BlorgRead/BlorgVolumeRead's dispatch of a
// paging read into the prefetcher/direct-fetch path, the cached (Cc) path,
// BlorgTrimReadToFileSize's end-of-file clamp shared by both, and
// BlorgReadComplete's completion bookkeeping.
//
// Prefetch.c itself has thorough coverage through PrefetchKernelTest.cpp,
// but that suite calls BlorgPrefetchServeRead directly -- deliberately
// bypassing Read.c, per its own file-header comment -- so the dispatch
// entry that actually wires an IRP_MJ_READ into the ring was untested
// before this file. A single paging read here does not arm the ring
// (PREFETCH_ARM_STREAK requires a second contiguous read), so
// BlorgPrefetchServeRead returns STATUS_NOT_FOUND and BlorgVolumeRead
// falls through to the direct-fetch path.
//
// Unlike PrefetchSandbox, DispatchSandbox links the real Client.c rather
// than PrefetchModel.c's simplified fetch queue, so the direct-fetch tests
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

#define CLOSE_STEP \
    { SandboxStepClose, nullptr, 0, STATUS_SUCCESS, TRUE }

class ReadTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SandboxInitialize();

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

        BlorgPrefetchDetach(Fcb);

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
    // One real READ IRP the way the I/O manager builds one. FileObject
    // gets a real (zeroed) SECTION_OBJECT_POINTERS regardless of whether a
    // given test needs it: BlorgVolumeRead dereferences
    // FileObject->SectionObjectPointer unconditionally once past the
    // IRP_PAGING_IO/IRP_NOCACHE flag checks, so a null pointer there would
    // crash a non-paging NOCACHE test rather than just skip the branch.
    //
    struct ReadRequest
    {
        FILE_OBJECT FileObject;
        SECTION_OBJECT_POINTERS SectionObject;
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
        req->FileObject.SectionObjectPointer = &req->SectionObject;

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
// are both false for a negative offset -- and it reaches the prefetcher
// widened to ULONG64, which is why Prefetch.c's containment test is
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
// End-of-file trim (BlorgTrimReadToFileSize), via the paging path
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

///////////////////////////////////////////////////////////////////////////
// Paging path: prefetch miss falls through to the direct fetch
///////////////////////////////////////////////////////////////////////////

//
// One read does not arm the ring (PrefetchKernelTest.cpp proves that
// directly), so BlorgPrefetchServeRead returns STATUS_NOT_FOUND here and
// BlorgVolumeRead falls through to BlorgHttpGetFileMdl -- the "load-bearing,
// not just an optimisation" inline-issue path the file's header comment
// describes: nothing posts this to the FSP queue.
//
TEST_F(ReadTest, PagingReadMissesThePrefetcherAndIssuesADirectFetch)
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
    // Paging reads carry none of BlorgReadComplete's non-paging bookkeeping
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
// Non-paging bookkeeping, via BlorgReadComplete
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
// an issue that completes synchronously runs BlorgReadComplete -- and its
// matching decrement -- before the call returns. That leaves the
// synchronous FAILURE case for this path to settle itself: the client's
// contract is that a non-STATUS_PENDING return means the callback never
// ran, so nothing downstream will ever terminate the fetch just counted.
//
// The gauge is the sharp end. FetchesIssued outrunning
// FetchesCompleted + FetchesFailed only produces a note from
// Compare-BlorgMetrics.ps1 ("in flight at sample time?"), but
// FetchesActive is a gauge with nothing to reset it, so every unsettled
// issue ratchets it -- and FetchesActivePeak -- up for the life of the
// load, quietly poisoning the concurrency numbers the perf tier reports.
//
// A NOCACHE paging read with no MDL is the deterministic way to reach a
// synchronous failure: BlorgHttpGetFileMdl refuses a null TargetMdl
// outright, no pool-failure injection needed. PrefetchPump settles the
// same case for its own counters, which is what makes this the odd one
// out rather than a judgement call.
//
TEST_F(ReadTest, DirectFetchThatFailsToIssueSettlesItsOwnAccounting)
{
    InterlockedExchange64(&BlorgStatisticsGauges.FetchesActive, 0);
    InterlockedExchange64(&BlorgStatisticsGauges.FetchesActivePeak, 0);
    BlorgStatisticsReset();

    ReadRequest* req = PrepareRead(Fcb, 0, 4096, IRP_PAGING_IO | IRP_NOCACHE, 0, nullptr);

    ASSERT_EQ(nullptr, req->Irp.MdlAddress)
        << "this test needs the issue to fail synchronously, which a null MDL guarantees";

    NTSTATUS status = BlorgRead(Volume, &req->Irp);

    ASSERT_NE(STATUS_PENDING, status)
        << "a null target MDL must be refused by the client, not issued";

    Drain();

    EXPECT_EQ(0, ReadNoFence64(&BlorgStatisticsGauges.FetchesActive))
        << "the in-flight gauge was raised for a fetch that never went out and never will";

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);

    EXPECT_EQ(response.Totals.FetchesIssued, response.Totals.FetchesCompleted + response.Totals.FetchesFailed)
        << "every issued fetch must terminate as completed or failed";
}

} // namespace
