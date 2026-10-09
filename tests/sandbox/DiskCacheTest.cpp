//
// Coverage for the real DiskCache.c: the cache file it opens, the reads it
// serves from it, and the fetched blocks it writes into it.
// DiskCacheIndexTest.cpp covers what the cache decides to hold; this is
// what it does with a file underneath, which DiskCacheModel.c provides.
//
// Every read here goes in through BlorgRead, as a paging read does, so the
// Read.c side is under test too: the fetch a partly held read issues is a
// real one, scripted through SandboxSocket.h, and a read the cache fails
// has to come back through ReadRefetchWorker and be fetched whole.
//
// Blocks only reach the cache on a second miss (DiskCacheIndex.c), so a
// test that wants a hit reads the range twice before asking for it a third
// time. That is the shape the guest's own mapped step relies on, and it is
// why a hit test that read once would quietly measure nothing.
//

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "SandboxSocket.h"
#include "Scheduler.h"
}

#include "DeviceKindScope.h"

namespace
{

//
// One block, and a file of four. Small enough that a test can hold every
// byte it expects, large enough to cover more than one block in a read.
//
const ULONG kBlock = DISK_CACHE_BLOCK_SIZE;
const ULONGLONG kFileSize = 4ull * DISK_CACHE_BLOCK_SIZE;

//
// The version the file is served at, in both the forms the two sides use:
// the FCB's last-write time, and the entity tag a response carries it in
// ("<seconds>.<nanoseconds>-<size>" in hex, HttpParseFileVersion).
//
const ULONG64 kModifiedTime = (1ull * 10000000) + 116444736000000000ull;
const char kEtag[] = "\"1.0-40000\"";

//
// What the last disk-cache read handed straight to BlorgDiskCacheRead
// completed with, for the tests that bypass Read.c.
//
NTSTATUS DirectStatus = STATUS_PENDING;

VOID DirectComplete(PIRP Irp, NTSTATUS Status, ULONG Valid, ULONG Fetches, BOOLEAN Refetch)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Valid);
    UNREFERENCED_PARAMETER(Fetches);
    UNREFERENCED_PARAMETER(Refetch);
    DirectStatus = Status;
}

class DiskCacheTest : public ::testing::Test
{
protected:
    struct ReadRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
    };

    //
    // The path cache is process-global (PathCacheTest.cpp's environment
    // initializes it once), and invalidating it only bumps a generation, so
    // entries earlier suites cached stay allocated until something reaps
    // them. SandboxInitialize resets the model's object ledger; freed after
    // that, those entries are allocations the model no longer remembers
    // creating, and the global teardown's BlorgPathCacheCleanup reports
    // them as destroyed more times than created. Emptying the cache first,
    // while the ledger still counts them, keeps it exact.
    //
    void SetUp() override
    {
        BlorgPathCacheCleanup();
        BlorgPathCacheInit();
        SandboxInitialize();
        BlorgReadInit();
        DiskCacheModelReset();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);
        global.VolumeDeviceObject = Volume;
        global.FileSystemDeviceObject = &FileSystemDevice;

        UNICODE_STRING name = Path(L"\\media\\clip.bin");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Fcb, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, kFileSize));
        InitializeListHead(&Fcb->Links);
        Fcb->LastModifiedTime = kModifiedTime;

        Body.resize(kFileSize);

        for (SIZE_T i = 0; i < Body.size(); ++i)
        {
            Body[i] = (unsigned char)(i * 7 + (i >> 13));
        }

        BlorgStatisticsReset();
    }

    void TearDown() override
    {
        Settle();

        global.ReadFairBudget = 0;
        BlorgDiskCacheCleanup();

        global.FileSystemDeviceObject = nullptr;
        global.VolumeDeviceObject = nullptr;

        BlorgCleanupWskClient();

        for (PIRP irp : Irps)
        {
            if (irp->MdlAddress)
            {
                IoFreeMdl(irp->MdlAddress);
            }
        }

        Irps.clear();

        if (Other)
        {
            BlorgFreeFileContext(Other, Volume);
        }

        BlorgFreeFileContext(Fcb, Volume);
        StructsModelDestroyVolume(Volume);
        DiskCacheModelReset();

        KmAssertQuiescent("DiskCacheTest teardown");
    }

    static UNICODE_STRING Path(const wchar_t* path)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(path);
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    void StartCache(ULONG sizeMb = 1)
    {
        UNICODE_STRING path = Path(L"\\??\\C:\\ProgramData\\BlorgFS\\BlockCache.bin");
        ASSERT_EQ(STATUS_SUCCESS, BlorgDiskCacheInitialize(&path, sizeMb));
        ASSERT_TRUE(BlorgDiskCacheLive());
    }

    //
    // Drain until nothing is left: a fill is issued from a work item, and a
    // read the cache failed is re-fetched from one whose peer answers on
    // the next completion drain, so one pass stops half way.
    //
    void Settle()
    {
        do
        {
            SandboxDrainCompletions();
        } while (ShimDrainWorkItems() > 0);
    }

    //
    // One read of Length bytes at Offset, the way Cc's read-ahead issues
    // one, with its own buffer and its own MDL, on Fcb unless another file
    // is named. The peer answers one fetch of FetchLength bytes at
    // FetchOffset, which is the whole read unless the cache is expected to
    // hold part of it: the script replays what it was given whatever the
    // request asks, so a test that expects a short fetch has to say so --
    // and asserts on the request itself as well.
    //
    unsigned char* Read(ULONG64 Offset, ULONG Length, ULONG64 FetchOffset = MAXULONG64,
        ULONG FetchLength = 0, PFCB Fcb2 = nullptr)
    {
        Answer((MAXULONG64 == FetchOffset) ? Offset : FetchOffset, FetchLength ? FetchLength : Length);

        unsigned char* buffer = nullptr;
        ReadRequest* req = Start(Offset, Length, &buffer, FALSE, Fcb2);

        Settle();

        EXPECT_EQ(1, req->Irp.CompletionCount);
        LastStatus = req->Irp.IoStatus.Status;
        LastInformation = (ULONG)req->Irp.IoStatus.Information;

        return buffer;
    }

    //
    // The read alone, left in flight against whatever the peer was last
    // scripted with. A speculative one is issued as Cc's read-ahead is,
    // under the cache's top-level IRP, which is what lets the fair share
    // hold it.
    //
    ReadRequest* Start(ULONG64 Offset, ULONG Length, unsigned char** Buffer, BOOLEAN Speculative = FALSE,
        PFCB Fcb2 = nullptr)
    {
        PFCB fcb = Fcb2 ? Fcb2 : Fcb;
        unsigned char* buffer = PageAlignedBuffer(Length);

        Requests.push_back(std::make_unique<ReadRequest>());
        ReadRequest* req = Requests.back().get();
        memset(req, 0, sizeof(*req));

        req->FileObject.FsContext = fcb;
        req->FileObject.DeviceObject = Volume;
        req->FileObject.SectionObjectPointer = &fcb->NonPaged->SectionObjectPointers;

        req->Stack.MajorFunction = IRP_MJ_READ;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.DeviceObject = Volume;
        req->Stack.Parameters.Read.Length = Length;
        req->Stack.Parameters.Read.ByteOffset.QuadPart = (LONGLONG)Offset;

        req->Irp.StackLocation = &req->Stack;
        req->Irp.Flags = IRP_PAGING_IO | IRP_NOCACHE;
        req->Irp.MdlAddress = IoAllocateMdl(buffer, Length, FALSE, FALSE, nullptr);

        Irps.push_back(&req->Irp);

        if (Speculative)
        {
            IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));
        }

        EXPECT_EQ(STATUS_PENDING, BlorgRead(Volume, &req->Irp));
        IoSetTopLevelIrp(nullptr);

        *Buffer = buffer;
        return req;
    }

    // Reads the range twice, which is what admits its blocks.
    void Warm(ULONG64 Offset, ULONG Length, PFCB Fcb2 = nullptr)
    {
        Read(Offset, Length, MAXULONG64, 0, Fcb2);
        Read(Offset, Length, MAXULONG64, 0, Fcb2);
    }

    //
    // A buffer a paging read's MDL could describe: page aligned and a
    // whole number of pages, which is what the cache requires of a read
    // before it will serve it (BlorgDiskCacheRead). A std::vector's own
    // pointer is not aligned, and a read handed one is fetched instead --
    // which looks exactly like a cache that is not holding anything.
    //
    unsigned char* PageAlignedBuffer(ULONG Length)
    {
        Buffers.emplace_back(Length + PAGE_SIZE, 0);

        unsigned char* data = Buffers.back().data();

        return data + ((PAGE_SIZE - ((ULONG_PTR)data & (PAGE_SIZE - 1))) & (PAGE_SIZE - 1));
    }

    BLORGFS_STATISTICS Totals()
    {
        BLORGFS_STATISTICS_RESPONSE response;
        BlorgStatisticsQuery(&response);
        return response.Totals;
    }

    //
    // Scripts one 206 holding Length bytes of Body at Offset, as the
    // server would answer a ranged GET for them; with Stall, a peer that
    // holds it back until SandboxResumeStalled. Each script is kept, since
    // a socket in flight goes on replaying the one it was acquired with.
    //
    void Answer(ULONG64 Offset, ULONG Length, BOOLEAN Stall = FALSE)
    {
        AnswerInTurn({ { Offset, Length } }, Stall);
    }

    //
    // The same for fetches made one after another on one connection, which
    // goes back to the pool between them and answers each with the next.
    //
    void AnswerInTurn(std::initializer_list<std::pair<ULONG64, ULONG>> Fetches, BOOLEAN Stall = FALSE)
    {
        Scripts.emplace_back();
        std::vector<SANDBOX_STEP>& steps = Scripts.back();

        SANDBOX_STEP step = {};
        step.Status = STATUS_SUCCESS;
        step.Inline = FALSE;

        if (Stall)
        {
            step.Kind = SandboxStepStall;
            steps.push_back(step);
        }

        for (const auto& fetch : Fetches)
        {
            const ULONG64 end = fetch.first + fetch.second < Body.size() ? fetch.first + fetch.second : Body.size();
            const ULONG length = (ULONG)(end - fetch.first);

            Responses.push_back(std::make_unique<std::string>(
                "HTTP/1.1 206 Partial Content\r\nContent-Length: " + std::to_string(length) +
                "\r\nEtag: " + std::string(ScriptedEtag) + "\r\n\r\n"));
            Responses.back()->append((const char*)Body.data() + fetch.first, length);

            step.Kind = SandboxStepDeliver;
            step.Data = (const unsigned char*)Responses.back()->data();
            step.Length = Responses.back()->size();
            steps.push_back(step);
        }

        SandboxSetPeerScript(steps.data(), steps.size());
    }

    //
    // Takes Count of the fair share's fetch slots with reads of another
    // file that stall on the link, each of a block nothing else reads, so
    // none of them is a hit or a fill. SandboxResumeStalled lets them go.
    //
    void OccupyFetches(ULONG Count)
    {
        if (!Other)
        {
            UNICODE_STRING name = Path(L"\\media\\other.bin");
            const ULONG64 size = 2ull * READ_FAIR_MAX_FETCHES * kBlock;

            ASSERT_EQ(STATUS_SUCCESS,
                BlorgCreateFCB(&Other, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, size));
            InitializeListHead(&Other->Links);
            Other->LastModifiedTime = kModifiedTime;
        }

        Answer(0, kBlock, TRUE);

        for (ULONG i = 0; i < Count; ++i)
        {
            unsigned char* buffer = nullptr;
            Start((ULONG64)(OtherBlock++) * kBlock, kBlock, &buffer, FALSE, Other);
        }

        Settle();
    }

    struct SnapshotProof
    {
        DiskCacheTest* Fixture;
        ULONG64 Size;
        ULONG64 Time;
        ULONG64 Trimmed;
        ULONG Bad;
    };

    static void SnapshotWriter(void* Context)
    {
        SnapshotProof* proof = C_CAST(SnapshotProof*, Context);
        PFCB fcb = proof->Fixture->Fcb;
        ExAcquireResourceExclusiveLite(fcb->Header.PagingIoResource, TRUE);
        fcb->Header.FileSize.QuadPart = 2 * kBlock;
        KmSchedYield();
        fcb->LastModifiedTime = kModifiedTime + 10000000;
        ExReleaseResourceLite(fcb->Header.PagingIoResource);
    }

    static void SnapshotReader(void* Context)
    {
        SnapshotProof* proof = C_CAST(SnapshotProof*, Context);
        unsigned char* buffer = nullptr;
        proof->Fixture->Start(kBlock, 3 * kBlock, &buffer);
        PNON_PAGED_NODE node = proof->Fixture->Fcb->NonPaged;
        proof->Size = node->DiskCacheSize;
        proof->Time = node->DiskCacheModifiedTime;
        proof->Trimmed = proof->Fixture->Totals().NonCachedReadBytes;
    }

    static void SnapshotSetup(void* Context)
    {
        SnapshotProof* proof = C_CAST(SnapshotProof*, Context);
        proof->Fixture->Fcb->Header.FileSize.QuadPart = kFileSize;
        proof->Fixture->Fcb->LastModifiedTime = kModifiedTime;
        proof->Fixture->ScriptedEtag = "unversioned";
        BlorgStatisticsReset();
        proof->Fixture->Answer(kBlock, 3 * kBlock);
        KmSchedSpawn(SnapshotWriter, Context);
        KmSchedSpawn(SnapshotReader, Context);
    }

    static void SnapshotTeardown(void* Context)
    {
        SnapshotProof* proof = C_CAST(SnapshotProof*, Context);
        const bool oldVersion = kFileSize == proof->Size && kModifiedTime == proof->Time &&
            3 * kBlock == proof->Trimmed;
        const bool newVersion = 2 * kBlock == proof->Size &&
            kModifiedTime + 10000000 == proof->Time && kBlock == proof->Trimmed;
        proof->Bad += !oldVersion && !newVersion;
        proof->Fixture->Settle();
        BlorgCleanupWskClient();
    }

    DEVICE_OBJECT FileSystemDevice = {};
    PFCB Other = nullptr;
    ULONG OtherBlock = 0;
    PDEVICE_OBJECT Volume = nullptr;
    PFCB Fcb = nullptr;
    NTSTATUS LastStatus = STATUS_SUCCESS;
    ULONG LastInformation = 0;
    const char* ScriptedEtag = kEtag;
    std::vector<unsigned char> Body;
    std::vector<std::unique_ptr<std::string>> Responses;
    std::vector<std::vector<SANDBOX_STEP>> Scripts;
    std::vector<std::unique_ptr<ReadRequest>> Requests;
    std::vector<PIRP> Irps;
    std::vector<std::vector<unsigned char>> Buffers;
};

///////////////////////////////////////////////////////////////////////////
// The cache file
///////////////////////////////////////////////////////////////////////////

//
// A store nobody trustworthy owns is refused rather than repaired: any
// user can create a directory under ProgramData, so the path may already
// hold a file whose DACL they chose, and this driver writes as SYSTEM.
// Refused leaves the cache off, which every entry point then no-ops.
//
TEST_F(DiskCacheTest, AStoreOwnedByAUserIsRefused)
{
    DiskCacheModelSetOwner(SeExports->SeAliasUsersSid);

    UNICODE_STRING path = Path(L"\\??\\C:\\ProgramData\\BlorgFS\\BlockCache.bin");

    EXPECT_EQ(STATUS_ACCESS_DENIED, BlorgDiskCacheInitialize(&path, 1));
    EXPECT_FALSE(BlorgDiskCacheLive());
}

//
// A store on this driver's own volume would send the cache's I/O back into
// this driver, so it is refused. The check is on the file object's device,
// which is the only thing that tells the two apart once the path has been
// opened.
//
TEST_F(DiskCacheTest, AStoreOnThisVolumeIsRefused)
{
    UNICODE_STRING path = Path(L"\\??\\B:\\BlockCache.bin");

    {
        //
        // The model's own device is the one the cache file's handle leads
        // to, so pointing the driver's disk-device global at it is what
        // makes the store look like it is on this volume.
        //
        ScopedDeviceKind asDisk(&global.DiskDeviceObject, DiskCacheModelDevice());

        EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgDiskCacheInitialize(&path, 1));
    }

    EXPECT_FALSE(BlorgDiskCacheLive());
}

//
// A cache that was never started must leave every read alone, which is
// what a driver with no DiskCacheMb does: nothing is served, nothing is
// written, and the read is fetched as it always was.
//
TEST_F(DiskCacheTest, WithNoCacheEveryReadIsFetched)
{
    unsigned char* buffer = Read(0, kBlock);

    EXPECT_EQ(STATUS_SUCCESS, LastStatus);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), kBlock));
    EXPECT_EQ(0u, DiskCacheModelIrps(IRP_MJ_WRITE));
    EXPECT_EQ(0u, Totals().DiskCacheFills);
}

///////////////////////////////////////////////////////////////////////////
// Fills
///////////////////////////////////////////////////////////////////////////

//
// Second-miss admission, end to end: the first fetch of a block only
// records it, the second writes it. Counted rather than inferred from the
// hit below, so a regression that fills on every fetch -- doubling the
// writes a scan costs -- fails here and not somewhere downstream.
//
TEST_F(DiskCacheTest, ABlockIsWrittenOnItsSecondFetchNotItsFirst)
{
    StartCache();

    Read(0, kBlock);

    EXPECT_EQ(1u, Totals().DiskCacheFirstMisses);
    EXPECT_EQ(0u, DiskCacheModelIrps(IRP_MJ_WRITE));

    Read(0, kBlock);

    EXPECT_EQ(1u, DiskCacheModelIrps(IRP_MJ_WRITE));
    EXPECT_EQ(1u, Totals().DiskCacheFills);
}

//
// A fetch that read another version of the file than the open's FCB names
// is not kept under the open's key: the tag the server answered with is
// the only thing that can tell them apart, and a block kept under the
// wrong key would be served as the file's current contents.
//
TEST_F(DiskCacheTest, AFetchOfAnotherVersionIsNotKept)
{
    StartCache();

    ScriptedEtag = "\"2.0-40000\"";

    Read(0, kBlock);
    Read(0, kBlock);

    EXPECT_EQ(2u, Totals().DiskCacheStale);
    EXPECT_EQ(0u, Totals().DiskCacheFills);
    EXPECT_EQ(0u, DiskCacheModelIrps(IRP_MJ_WRITE));
}

//
// A failed write frees the slot rather than leaving it to be served, and
// turns the cache off like any other failed I/O on the file: the store may
// be gone, and nothing more should be sent to it.
//
TEST_F(DiskCacheTest, AFailedWriteFreesTheSlotAndTurnsTheCacheOff)
{
    StartCache();

    Read(0, kBlock);
    DiskCacheModelFailNext(IRP_MJ_WRITE, STATUS_DEVICE_REMOVED);
    Read(0, kBlock);

    EXPECT_EQ(1u, Totals().DiskCacheFillFailures);
    EXPECT_EQ(0u, Totals().DiskCacheFills);
    EXPECT_FALSE(BlorgDiskCacheLive());
}

///////////////////////////////////////////////////////////////////////////
// Reads served from the cache
///////////////////////////////////////////////////////////////////////////

//
// The whole point: a read whose every block is held comes off the local
// file and never reaches the link. Asserted on the fetch count, not on the
// bytes alone -- a read served from the file and a read fetched again look
// identical in the buffer, and only one of them is a hit.
//
TEST_F(DiskCacheTest, AReadWhoseBlocksAreAllHeldIsServedFromTheFile)
{
    StartCache();

    Warm(0, 2 * kBlock);

    const ULONG64 fetched = Totals().FetchesIssued;
    const ULONG64 diskReads = Totals().NonCachedDiskReads;

    unsigned char* buffer = Read(0, 2 * kBlock);

    EXPECT_EQ(STATUS_SUCCESS, LastStatus);
    EXPECT_EQ(2 * kBlock, LastInformation);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(fetched, Totals().FetchesIssued) << "a held read must not reach the network";
    EXPECT_EQ(diskReads, Totals().NonCachedDiskReads);
    EXPECT_EQ(1u, Totals().DiskCacheHits);
    EXPECT_EQ(2 * (ULONG64)kBlock, Totals().DiskCacheHitBytes);
}

//
// The partial hit. A read holding one of its two blocks asks the server
// only for the other one, and the two halves have to land in the right
// places in one buffer. Serving only reads that were held whole -- what
// this replaced -- served almost nothing on a file larger than the cache,
// because the clock leaves what it keeps scattered.
//
// The range is asserted on the request the driver actually built: a
// partial hit that re-fetched the whole read, or fetched the wrong run,
// would still return the right bytes here and cost everything the hit was
// worth.
//
TEST_F(DiskCacheTest, APartlyHeldReadFetchesOnlyTheRunItLacks)
{
    StartCache();

    Warm(0, kBlock);

    const ULONG64 diskReads = Totals().NonCachedDiskReads;

    unsigned char* buffer = Read(0, 2 * kBlock, kBlock, kBlock);

    EXPECT_EQ(STATUS_SUCCESS, LastStatus);
    EXPECT_EQ(2 * kBlock, LastInformation);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock))
        << "the held run and the fetched one must both land at their own offset";
    EXPECT_EQ(1u, Totals().DiskCachePartialHits);
    EXPECT_EQ(0u, Totals().DiskCacheHits);
    EXPECT_EQ((ULONG64)kBlock, Totals().DiskCacheHitBytes)
        << "only the blocks read from the file count as hit bytes";
    EXPECT_EQ(diskReads + 1, Totals().NonCachedDiskReads)
        << "a read that fetched part of itself still left the cache";

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=65536-131071")) << text;
}

//
// A read the cache fails part way cannot be handed back to its caller --
// parts of it are already writing into the caller's buffer -- so it is
// completed as a failure, and Read.c fetches it whole from a work item.
// The reader sees the file's bytes either way, which is the contract a
// disposable cache has to keep.
//
TEST_F(DiskCacheTest, AFailedCacheReadIsFetchedWholeAndStillServesTheRightBytes)
{
    StartCache();

    Warm(0, kBlock);

    DiskCacheModelFailNext(IRP_MJ_READ, STATUS_DEVICE_REMOVED);

    unsigned char* buffer = Read(0, kBlock);

    EXPECT_EQ(STATUS_SUCCESS, LastStatus) << "the reader must not see the cache's failure";
    EXPECT_EQ(kBlock, LastInformation);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), kBlock));
    EXPECT_EQ(1u, Totals().DiskCacheReadFailures);
    EXPECT_FALSE(BlorgDiskCacheLive()) << "a failed read on the cache file turns it off";
}

//
// A fetched run of another version than the held blocks beside it would
// make the read return the file half old and half new. Nothing is copied
// from it, and the read is fetched whole, as it was before the cache could
// serve part of one.
//
TEST_F(DiskCacheTest, APartlyHeldReadWhoseFetchIsAnotherVersionIsFetchedWhole)
{
    StartCache();

    Warm(0, kBlock);

    ScriptedEtag = "\"2.0-40000\"";
    AnswerInTurn({ { kBlock, kBlock }, { 0, 2 * kBlock } });

    unsigned char* buffer = nullptr;
    ReadRequest* req = Start(0, 2 * kBlock, &buffer);

    Settle();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(2 * kBlock, req->Irp.IoStatus.Information);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(0u, Totals().DiskCachePartialHits);
    EXPECT_EQ(0u, Totals().DiskCacheReadFailures) << "the cache file did nothing wrong";

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=0-131071")) << text;
}

//
// A read is looked up under the version its own BlorgDiskCacheNoteFile
// noted, not under what the node records by the time it looks. A read from
// a handle opened before the FCB was refreshed (Create.c) can note the old
// version between a reopened handle's read noting the new one and looking
// it up; looked up through the node, that read was served the old
// version's blocks. The block is still there under the old version.
//
TEST_F(DiskCacheTest, AReadIsLookedUpUnderTheVersionItNoted)
{
    StartCache();

    Warm(0, kBlock);

    UNICODE_STRING name = Path(L"\\media\\clip.bin");
    DISK_CACHE_KEY reopened;
    DISK_CACHE_KEY earlier;
    BlorgDiskCacheNoteFile(Fcb->NonPaged, &name, kFileSize, (2ull * 10000000) + 116444736000000000ull, &reopened);
    BlorgDiskCacheNoteFile(Fcb->NonPaged, &name, kFileSize, kModifiedTime, &earlier);

    IRP irp = {};
    irp.MdlAddress = IoAllocateMdl(PageAlignedBuffer(kBlock), kBlock, FALSE, FALSE, nullptr);

    ULONG fetches = 0;
    EXPECT_FALSE(BlorgDiskCacheRead(&irp, &reopened, &name, 0, kBlock, kBlock, &fetches, DirectComplete))
        << "a read of the new version was served the old version's block";

    DirectStatus = STATUS_PENDING;
    ASSERT_TRUE(BlorgDiskCacheRead(&irp, &earlier, &name, 0, kBlock, kBlock, &fetches, DirectComplete));
    Settle();

    EXPECT_EQ(STATUS_SUCCESS, DirectStatus);
    EXPECT_EQ(0u, fetches);
    EXPECT_EQ(1u, Totals().DiskCacheHits);

    IoFreeMdl(irp.MdlAddress);
}

//
// The version a fetched run is checked against is the one the read pinned
// its held blocks under, not whatever the file's node says when the run
// arrives. A reopen after a change on the server refreshes the FCB, and the
// next read moves the node's key to the new version, while a read that
// started before can still be in flight; checked against the node, its
// new-version run would be accepted beside its old-version blocks.
//
TEST_F(DiskCacheTest, AFetchedRunIsCheckedAgainstTheVersionTheReadStartedWith)
{
    StartCache();

    Warm(0, kBlock);

    ScriptedEtag = "\"2.0-40000\"";
    AnswerInTurn({ { kBlock, kBlock }, { 0, 2 * kBlock } }, TRUE);

    unsigned char* buffer = nullptr;
    ReadRequest* req = Start(0, 2 * kBlock, &buffer);
    Settle();

    UNICODE_STRING name = Path(L"\\media\\clip.bin");
    DISK_CACHE_KEY refreshed;
    BlorgDiskCacheNoteFile(Fcb->NonPaged, &name, kFileSize, (2ull * 10000000) + 116444736000000000ull, &refreshed);

    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(0u, Totals().DiskCachePartialHits)
        << "a run of the new version was served beside blocks of the old one";

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=0-131071")) << text;
}

//
// The same check once the cache has gone off. A failed write elsewhere
// turns the cache off while a partly held read has a run in flight; the
// held blocks were already read, so the run must still be of their
// version. Admission used to answer that question, and with the cache off
// it said yes to anything.
//
TEST_F(DiskCacheTest, AFetchedRunIsCheckedAfterTheCacheGoesOff)
{
    StartCache();

    Warm(0, kBlock);
    Read(2 * kBlock, kBlock);

    ScriptedEtag = "\"2.0-40000\"";
    AnswerInTurn({ { kBlock, kBlock }, { 0, 2 * kBlock } }, TRUE);

    unsigned char* buffer = nullptr;
    ReadRequest* req = Start(0, 2 * kBlock, &buffer);
    Settle();

    ScriptedEtag = kEtag;
    DiskCacheModelFailNext(IRP_MJ_WRITE, STATUS_DEVICE_REMOVED);
    Read(2 * kBlock, kBlock);

    ASSERT_FALSE(BlorgDiskCacheLive()) << "the second miss's fill should have failed and turned the cache off";

    ScriptedEtag = "\"2.0-40000\"";
    Answer(0, 2 * kBlock);

    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(0u, Totals().DiskCachePartialHits)
        << "a run of another version was served because the cache had gone off";
}

//
// A fetch that fails fails the read, as it would a read the cache had no
// part in. Fetching it whole would only ask the same server again, and the
// failure is not the cache file's to be counted against it.
//
TEST_F(DiskCacheTest, AFailedFetchOfAPartlyHeldReadFailsTheReadOnce)
{
    StartCache();

    Warm(0, kBlock);

    const ULONG64 issued = Totals().FetchesIssued;

    SandboxFailNextAcquiresWith(1, STATUS_CONNECTION_REFUSED);
    Read(0, 2 * kBlock, kBlock, kBlock);

    EXPECT_FALSE(NT_SUCCESS(LastStatus));
    EXPECT_EQ(issued + 1, Totals().FetchesIssued) << "the read was fetched again whole";
    EXPECT_EQ(0u, Totals().DiskCacheReadFailures);
    EXPECT_TRUE(BlorgDiskCacheLive());
}

//
// A fetched run that reaches end of file stops there, as a whole fetch
// would, and the read is still exactly the file's bytes up to it.
//
TEST_F(DiskCacheTest, APartlyHeldReadEndingAtEndOfFileFetchesOnlyToIt)
{
    StartCache();

    const ULONG size = 3 * kBlock + kBlock / 2;
    UNICODE_STRING name = Path(L"\\media\\short.bin");
    PFCB shortFile = nullptr;

    ASSERT_EQ(STATUS_SUCCESS,
        BlorgCreateFCB(&shortFile, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, size));
    InitializeListHead(&shortFile->Links);
    shortFile->LastModifiedTime = kModifiedTime;

    Body.resize(size);
    ScriptedEtag = "\"1.0-38000\"";

    Warm(0, kBlock, shortFile);

    unsigned char* buffer = Read(0, 4 * kBlock, kBlock, size - kBlock, shortFile);

    EXPECT_EQ(STATUS_SUCCESS, LastStatus);
    EXPECT_EQ(size, LastInformation);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), size));
    EXPECT_EQ(1u, Totals().DiskCachePartialHits);
    EXPECT_EQ((ULONG64)kBlock, Totals().DiskCacheHitBytes);

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=65536-229375")) << text;

    Settle();
    BlorgFreeFileContext(shortFile, Volume);
}

//
// A partly held read still fetches, so it takes its turn on the link like
// any fetch: read-ahead past the budget is held, with nothing read from
// the cache file or the server. Once released it is offered to the cache
// again and fetches only the run it lacks, rather than fetching again
// what the cache holds.
//
TEST_F(DiskCacheTest, APartlyHeldReadAheadPastTheBudgetIsHeld)
{
    StartCache();

    Warm(0, kBlock);

    global.ReadFairBudget = kBlock;

    Answer(2 * kBlock, kBlock, TRUE);

    unsigned char* ahead = nullptr;
    ReadRequest* inFlight = Start(2 * kBlock, kBlock, &ahead, TRUE);
    SandboxDrainCompletions();

    const ULONG64 issued = Totals().FetchesIssued;
    const ULONG cacheReads = DiskCacheModelIrps(IRP_MJ_READ);

    unsigned char* buffer = nullptr;
    ReadRequest* held = Start(0, 2 * kBlock, &buffer, TRUE);

    EXPECT_EQ(1u, Totals().ReadsHeld);
    EXPECT_EQ(issued, Totals().FetchesIssued);
    EXPECT_EQ(cacheReads, DiskCacheModelIrps(IRP_MJ_READ));

    Answer(kBlock, kBlock);
    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, inFlight->Irp.CompletionCount);
    EXPECT_EQ(1, held->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, held->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(1u, Totals().DiskCachePartialHits);
    EXPECT_EQ((ULONG64)kBlock, Totals().DiskCacheHitBytes);

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=65536-131071")) << text;
}

//
// Refresh can change a stamp after fair admission held the IRP. Release
// must trim to the new size and publish that same key, so old warm blocks
// cannot supply the head while the network supplies the new tail.
//
TEST_F(DiskCacheTest, FairQueueReleaseResamplesSizeAndTimeBeforeUsingWarmBlocks)
{
    StartCache();
    Warm(0, kBlock);
    global.ReadFairBudget = kBlock;
    Answer(2 * kBlock, kBlock, TRUE);
    unsigned char* ahead = nullptr;
    ReadRequest* inFlight = Start(2 * kBlock, kBlock, &ahead, TRUE);
    SandboxDrainCompletions();
    unsigned char* buffer = nullptr;
    ReadRequest* held = Start(0, 3 * kBlock, &buffer, TRUE);
    ASSERT_EQ(1u, Totals().ReadsHeld);
    ExAcquireResourceExclusiveLite(Fcb->Header.PagingIoResource, TRUE);
    Fcb->Header.FileSize.QuadPart = 2 * kBlock;
    Fcb->LastModifiedTime = kModifiedTime + 10000000;
    ExReleaseResourceLite(Fcb->Header.PagingIoResource);
    Body.assign(2 * kBlock, 0xA6);
    ScriptedEtag = "\"2.0-20000\"";
    Answer(0, 2 * kBlock);
    SandboxResumeStalled();
    Settle();
    EXPECT_EQ(1, inFlight->Irp.CompletionCount);
    EXPECT_EQ(1, held->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, held->Irp.IoStatus.Status);
    EXPECT_EQ(2 * kBlock, held->Irp.IoStatus.Information);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 2 * kBlock));
    EXPECT_EQ(2 * kBlock, Fcb->NonPaged->DiskCacheSize);
    EXPECT_EQ(kModifiedTime + 10000000, Fcb->NonPaged->DiskCacheModifiedTime);
}

//
// Each run a partly held read fetches is a request of its own, and every
// request counts against READ_FAIR_MAX_FETCHES. With one slot left, the
// read's own admission, its two holes are fetched as one run, the held
// block between them given up; it used to send both, one past the limit.
// The slots it did not use are given back: once everything settles, a
// full limit's worth of reads all go out at once.
//
TEST_F(DiskCacheTest, APartlyHeldReadWithOneFetchSlotLeftFetchesOneRun)
{
    StartCache();

    Warm(0, kBlock);
    Warm(2 * kBlock, kBlock);

    OccupyFetches(READ_FAIR_MAX_FETCHES - 1);

    const ULONG64 issued = Totals().FetchesIssued;

    Answer(kBlock, 3 * kBlock);

    unsigned char* buffer = nullptr;
    ReadRequest* req = Start(0, 4 * kBlock, &buffer);

    EXPECT_EQ(issued + 1, Totals().FetchesIssued) << "a partly held read went past the fetch limit";

    SIZE_T length = 0;
    const unsigned char* request = SandboxLastRequest(&length);
    const std::string text((const char*)request, length);

    EXPECT_NE(std::string::npos, text.find("Range: bytes=65536-262143")) << text;

    Settle();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), 4 * kBlock));
    EXPECT_EQ(1u, Totals().DiskCachePartialHits);
    EXPECT_EQ((ULONG64)kBlock, Totals().DiskCacheHitBytes);

    SandboxResumeStalled();
    Settle();

    const ULONG64 settled = Totals().FetchesIssued;

    OccupyFetches(READ_FAIR_MAX_FETCHES);

    EXPECT_EQ(settled + READ_FAIR_MAX_FETCHES, Totals().FetchesIssued)
        << "a fetch slot the partly held read took was never given back";

    SandboxResumeStalled();
    Settle();
}

//
// A read the cache holds whole is never admitted: it does not touch the
// link. If the cache file fails it, it is fetched after all, and that
// fetch waits for the fetch limit like any other; it used to go out
// past a full one.
//
TEST_F(DiskCacheTest, AWholeHitTheCacheFailsWaitsForTheFetchLimit)
{
    StartCache();

    Warm(0, kBlock);

    OccupyFetches(READ_FAIR_MAX_FETCHES);

    const ULONG64 issued = Totals().FetchesIssued;

    DiskCacheModelFailNext(IRP_MJ_READ, STATUS_DEVICE_REMOVED);

    unsigned char* buffer = nullptr;
    ReadRequest* req = Start(0, kBlock, &buffer);
    Settle();

    EXPECT_EQ(1u, Totals().DiskCacheReadFailures);
    EXPECT_EQ(0, req->Irp.CompletionCount);
    EXPECT_EQ(issued, Totals().FetchesIssued) << "the fetch of a failed hit went past the fetch limit";

    Answer(0, kBlock);
    SandboxResumeStalled();
    Settle();

    EXPECT_EQ(1, req->Irp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, req->Irp.IoStatus.Status);
    EXPECT_EQ(0, memcmp(buffer, Body.data(), kBlock));
}

//
// Fetched blocks waiting for their write are bounded by
// DISK_CACHE_FILL_BACKLOG, so a disk slower than the link costs hit rate
// rather than memory. The writes are held back here by the cache file
// until the end, and the bound is written once the file lets them through.
// Every other block is dropped, or is a first miss again where the ghost
// table, small for a small cache, forgot it.
//
TEST_F(DiskCacheTest, FetchedBlocksPastTheFillBacklogAreDropped)
{
    StartCache(64);

    const ULONG chunk = DISK_CACHE_MAX_READ_BLOCKS * kBlock;
    const ULONG chunks = 9;
    const ULONG64 backlogBlocks = (32ull * 1024 * 1024) / kBlock;
    UNICODE_STRING name = Path(L"\\media\\large.bin");
    PFCB large = nullptr;

    ASSERT_EQ(STATUS_SUCCESS,
        BlorgCreateFCB(&large, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, (ULONG64)chunk * chunks));
    InitializeListHead(&large->Links);
    large->LastModifiedTime = kModifiedTime;

    Body.resize((SIZE_T)chunk * chunks, 0x3C);
    ScriptedEtag = "\"1.0-2400000\"";

    for (ULONG i = 0; i < chunks; ++i)
    {
        Read((ULONG64)i * chunk, chunk, MAXULONG64, 0, large);
    }

    const ULONG64 firstMisses = Totals().DiskCacheFirstMisses;

    DiskCacheModelHold(TRUE);

    for (ULONG i = 0; i < chunks; ++i)
    {
        Answer((ULONG64)i * chunk, chunk);

        unsigned char* buffer = nullptr;
        ReadRequest* req = Start((ULONG64)i * chunk, chunk, &buffer, FALSE, large);
        Settle();

        EXPECT_EQ(1, req->Irp.CompletionCount);
    }

    DiskCacheModelHold(FALSE);
    DiskCacheModelCompleteHeld();
    Settle();

    EXPECT_EQ(backlogBlocks, Totals().DiskCacheFills);
    EXPECT_GT(Totals().DiskCacheDropped, 0u);
    EXPECT_EQ(chunks * DISK_CACHE_MAX_READ_BLOCKS,
        Totals().DiskCacheFills + Totals().DiskCacheDropped + (Totals().DiskCacheFirstMisses - firstMisses))
        << "every fetched block is filled, dropped or seen for the first time";
    EXPECT_EQ(0u, Totals().DiskCacheFillFailures);

    BlorgFreeFileContext(large, Volume);
}

//
// A failed fill has already consumed its block's second-miss ghost tag.
// Its first retry fills the other two blocks; one more admission fills
// the failed block. Check all allocation stages, including a partial EOF
// block, so successful retry alone cannot hide leaked reservations or an
// incorrect remaining-block count.
//
TEST_F(DiskCacheTest, FillAllocationFailuresCountTheRemainingBlocksAndRollBack)
{
    for (ULONG length : {3 * kBlock, 3 * kBlock - 1})
    {
        for (LONG stage : {0, 1, 2})
        {
            StartCache(8);
            DISK_CACHE_KEY key;
            const ULONG64 size = (length & (kBlock - 1)) ? length : kFileSize;
            BlorgDiskCacheNoteFile(Fcb->NonPaged, &Fcb->FullPath, size, kModifiedTime, &key);
            FILE_BUFFER body = {};
            body.BodyBuffer = C_CAST(PCHAR, Body.data());
            body.BodyBufferSize = Body.size();
            body.HasVersion = TRUE;
            body.VersionSize = size;
            body.VersionTime = kModifiedTime;
            ASSERT_TRUE(BlorgDiskCacheAdmit(Fcb->NonPaged, &body, 0, length));

            const ULONG64 dropped = Totals().DiskCacheDropped;
            const ULONG64 filled = Totals().DiskCacheFills;
            if (2 == stage)
            {
                ShimFailNextMdlAllocation();
            }
            else
            {
                ShimPoolFailAt(stage);
            }
            ASSERT_TRUE(BlorgDiskCacheAdmit(Fcb->NonPaged, &body, 0, length));
            ShimPoolFailAt(-1);
            EXPECT_EQ(dropped + 3, Totals().DiskCacheDropped);
            ASSERT_TRUE(BlorgDiskCacheAdmit(Fcb->NonPaged, &body, 0, length));
            Settle();
            EXPECT_EQ(filled + 2, Totals().DiskCacheFills);
            ASSERT_TRUE(BlorgDiskCacheAdmit(Fcb->NonPaged, &body, 0, length));
            Settle();
            EXPECT_EQ(filled + 3, Totals().DiskCacheFills);
            BlorgDiskCacheCleanup();
        }
    }
}

//
// A caller's length and key can disagree even when held blocks exist.
// Reject before pinning or I/O; a hit/miss assertion alone misses a read
// that silently rounds beyond the version's declared end. A pending pool
// failure must remain available after rejecting an offset beyond EOF.
//
TEST_F(DiskCacheTest, ValidBytesCannotExtendBeyondTheImmutableKey)
{
    StartCache(8);
    Fcb->Header.FileSize.QuadPart = kBlock - 1;
    Body.resize(kBlock - 1);
    ScriptedEtag = "\"1.0-ffff\"";
    Warm(0, kBlock);
    ASSERT_EQ(STATUS_SUCCESS, LastStatus);
    ASSERT_EQ(kBlock - 1, LastInformation);
    DISK_CACHE_KEY key;
    BlorgDiskCacheNoteFile(Fcb->NonPaged, &Fcb->FullPath, kBlock - 1, kModifiedTime, &key);
    IRP irp = {};
    irp.MdlAddress = IoAllocateMdl(PageAlignedBuffer(kBlock), kBlock, FALSE, FALSE, nullptr);
    ULONG fetches = 0;
    const ULONG reads = DiskCacheModelIrps(IRP_MJ_READ);
    EXPECT_FALSE(BlorgDiskCacheRead(&irp, &key, &Fcb->FullPath, 0, kBlock, kBlock,
        &fetches, DirectComplete));
    EXPECT_EQ(reads, DiskCacheModelIrps(IRP_MJ_READ));
    EXPECT_EQ(0u, fetches);
    ShimPoolFailAt(0);
    EXPECT_FALSE(BlorgDiskCacheRead(&irp, &key, &Fcb->FullPath, key.Size + 1, kBlock, 1,
        &fetches, DirectComplete));
    PVOID probe = ExAllocatePoolZero(NonPagedPoolNx, 1, 'tseT');
    EXPECT_EQ(nullptr, probe) << "an offset past immutable EOF must reject before allocation";
    if (probe)
    {
        ExFreePool(probe);
    }
    ShimPoolFailAt(-1);
    EXPECT_EQ(0u, fetches);
    EXPECT_EQ(reads, DiskCacheModelIrps(IRP_MJ_READ));
    DirectStatus = STATUS_PENDING;
    ASSERT_TRUE(BlorgDiskCacheRead(&irp, &key, &Fcb->FullPath, 0, kBlock, kBlock - 1,
        &fetches, DirectComplete));
    Settle();
    EXPECT_EQ(STATUS_SUCCESS, DirectStatus);
    EXPECT_EQ(reads + 1, DiskCacheModelIrps(IRP_MJ_READ));
    IoFreeMdl(irp.MdlAddress);
}

//
// Pause a refresh between its size and timestamp writes while it holds
// PagingIoResource. The actual BlorgRead must wait or use the whole old
// stamp: trimming and cache-key publication must describe one version.
// Deleting ReadSnapshotFile's lock exposes the mixed stamp in this test,
// because the writer's explicit yield remains a scheduling point.
//
TEST_F(DiskCacheTest, PagingResourceKeepsTrimAndCacheKeyInOneVersion)
{
    StartCache(8);
    SnapshotProof proof = {};
    proof.Fixture = this;
    KmSchedSetAtomicYields(0);
    KmSchedSetReduction(0);
    const KM_SCHED_RESULT result = KmExploreInterleavings(SnapshotSetup, SnapshotTeardown,
        &proof, 50000);
    EXPECT_LT(result.Schedules, 50000);
    EXPECT_EQ(0, result.Truncated);
    EXPECT_EQ(0, result.Deadlocks);
    EXPECT_EQ(0u, proof.Bad);
}

//
// Past DISK_CACHE_MAX_READ_BLOCKS the cache does not try: a read that
// large is nothing Cc or Mm issues, and it would not fit the slots a read
// carries. It is fetched instead, which is only visible in that nothing
// was served.
//
TEST_F(DiskCacheTest, AReadBeyondTheBlockLimitIsFetchedRatherThanServed)
{
    StartCache(8);

    UNICODE_STRING name = Path(L"\\media\\big.bin");
    PFCB big = nullptr;
    const ULONG length = (DISK_CACHE_MAX_READ_BLOCKS + 1) * kBlock;

    ASSERT_EQ(STATUS_SUCCESS,
        BlorgCreateFCB(&big, (CSHORT)BLORGFS_FCB_SIGNATURE, &name, Volume, length));
    InitializeListHead(&big->Links);
    big->LastModifiedTime = kModifiedTime;

    Body.resize(length, 0x5A);

    Warm(0, length, big);
    Read(0, length, MAXULONG64, 0, big);

    EXPECT_EQ(0u, Totals().DiskCacheHits);
    EXPECT_EQ(0u, Totals().DiskCachePartialHits);
    EXPECT_EQ(0u, DiskCacheModelIrps(IRP_MJ_READ));

    Settle();
    BlorgFreeFileContext(big, Volume);
}

} // namespace
