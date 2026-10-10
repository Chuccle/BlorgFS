//
// Directory create/open coverage over the real Create.c:
// CreateCheckDirectoryAccess, CreateOpenExistingDcb, CreateOpenRootDcb,
// CreateBreakHandleOplockOnSharingViolation, CreateSplitPathLeaf and
// CreateFindEntryByName -- none of which any other sandbox target drives.
// DispatchTest/DispatchSchedTest/DispatchStressTest all open FILES through
// BlorgCreate; a directory open takes a structurally different branch in
// BlorgVolumeCreate (FILE_NON_DIRECTORY_FILE checks, CreateOpenExistingDcb's
// CCB allocation, the root-path shortcut) that a file-only opener never
// touches. The same plumbing drives the reopen of a resident file after the
// server's copy changed (FcbReopenTest, at the end), the one file-open
// branch those targets do not reach.
//
// CreateCheckFileAccess and CreateCheckDirectoryAccess are `static inline`
// in Create.c, unreachable from any other translation unit -- the same
// situation DispatchTest.cpp documents for CreateOpenExistingFcb. Matching
// that file's approach, this drives them through real directory- and
// file-open IRPs rather than declaring them extern, which would test a copy
// of the mask rather than the mask the driver actually applies.
//
// The volume is read-only, so both checks apply a single read-only mask;
// the bit-by-bit tests below cover the file and the directory mask.
//
// Most cold paths here resolve through a fresh listing of the parent in
// the listing cache, which is exactly how a warm directory's children
// resolve once DirCtrlComplete has published its listing. That path
// exercises CreateSplitPathLeaf and both of CreateFindEntryByName's loops
// for free. FcbReopenTest drives the re-drive that consumes what
// CreateComplete stashes directly, and CreateNetworkTest, at the end, the
// lookup itself: a real HTTP round trip through Client.c and the
// SandboxSocket peer into CreateComplete, what it remembers, and which pass
// counts the create.
//
// DispatchSandbox.vcxproj lists this TU BEFORE DispatchSchedTest.cpp, not
// alphabetically or by habit: KmExploreInterleavings (Scheduler.c) turns
// lock-id recycling OFF for the rest of the process once its 3432-schedule
// exploration finishes (deliberately -- recycling is only sound
// single-threaded, and DispatchStressTest/this file both use real
// threads/repeated real ERESOURCEs). Every ERESOURCE these fixtures create
// afterward would burn a fresh, never-reclaimed id out of KM_MAX_LOCKS
// (2048), and running after DispatchSchedTest was enough to exhaust that
// budget mid-suite. Running first avoids it; it does not fix the
// underlying one-way recycling switch, which is Scheduler.c's concern, not
// this file's.
//

#include <gtest/gtest.h>

#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <string>

extern "C" {
#include "SandboxSocket.h"

NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject);

// Not declared in any header -- FspWorkQueue.c's only other caller is
// PsCreateSystemThread, which is a no-op in this sandbox. ReadTest.cpp runs
// it the same way, on a thread of its own.
VOID BlorgFspDispatch(PVOID StartContext);
}

#include "ListingBuilder.h"

namespace
{

class CreateDirectoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);

        global.VolumeDeviceObject = Volume;

        //
        // CreateOpenExistingDcb/CreateOpenRootDcb wire FileObject->Vpb from
        // global.DiskDeviceObject on every successful open; without it
        // they dereference a null Vpb pointer.
        //
        memset(&DiskDevice, 0, sizeof(DiskDevice));
        memset(&DiskVpb, 0, sizeof(DiskVpb));
        DiskDevice.Vpb = &DiskVpb;
        global.DiskDeviceObject = &DiskDevice;

        ASSERT_EQ(STATUS_SUCCESS, BlorgNodeTableInit(Volume));

        UNICODE_STRING rootName = Path(L"\\");

        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateDCB(&Root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, Volume));
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, Volume, 0));

        BlorgGetVolumeDeviceExtension(Volume)->RootDcb = Root;
        BlorgGetVolumeDeviceExtension(Volume)->Vcb = Vcb;
    }

    void TearDown() override
    {
        //
        // Unlike NodeTableTest.cpp (which this fixture's FreeTree()
        // otherwise mirrors), these tests drive real BlorgClose calls, and
        // a close on an idle node defers its free to the reap worker's
        // queued IO_WORKITEM (BlorgNodeDeferReap) rather than freeing it
        // inline. Draining that queue here, before BlorgNodeTableTeardown
        // clears the per-node OnReapList claims, keeps FreeTree()'s walk
        // and the worker from both being live over the same tree --
        // exactly the ordering DispatchStressTest/DispatchSchedTest use
        // after their own closes.
        //
        ShimDrainWorkItems();

        BlorgNodeTableTeardown();

        FreeTree();

        //
        // PathCacheTest.cpp registers a ::testing::Environment that calls
        // BlorgPathCacheInit() once for the whole process -- gtest runs a
        // registered Environment's SetUp/TearDown regardless of
        // --gtest_filter, so PathCache.Ready is TRUE here whether or not
        // PathCacheTest.cpp's own tests are selected. The listing-hit
        // tests below genuinely call BlorgPathCacheInsertExists/InsertNotFound
        // (Create.c), which is real cache state, not scaffolding -- and it
        // outlives this fixture's own tree, since PathCache is a
        // process-global structure this test doesn't otherwise touch.
        // Sweeping it here is what a real invalidation (rename/delete)
        // would eventually do to the same entries, and it is what keeps
        // one test's cache entries from being live at another test's
        // quiescence check.
        //
        // "\media" rather than the root: every path any test here inserts
        // lives under it, so it is the tightest sweep that covers them.
        // The root would work too -- PathCacheIsUnder handles a Dir that
        // ends in its own separator (see PathCache.c) -- but sweeping the
        // whole cache from a fixture that only owns one subtree would
        // quietly evict another fixture's entries.
        //
        UNICODE_STRING mediaSubtree = Path(L"\\media");
        BlorgPathCacheInvalidatePrefix(&mediaSubtree);

        StructsModelDestroyVolume(Volume);

        KmAssertQuiescent("CreateDirectoryTest teardown");
    }

    //
    // Leaf-first teardown of whatever a test built, mirroring
    // NodeTableTest.cpp -- it bypasses the reap protocol entirely rather
    // than requiring every test to close everything it opened through the
    // real dispatch path first.
    //
    void FreeTree()
    {
        while (!IsListEmpty(&Root->ChildrenList))
        {
            PCOMMON_CONTEXT node = CONTAINING_RECORD(Root->ChildrenList.Flink, COMMON_CONTEXT, Links);

            while ((BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(node)) &&
                   !IsListEmpty(&C_CAST(PDCB, node)->ChildrenList))
            {
                node = CONTAINING_RECORD(C_CAST(PDCB, node)->ChildrenList.Flink, COMMON_CONTEXT, Links);
            }

            BlorgFreeFileContext(node, Volume);
        }

        BlorgFreeFileContext(Root, Volume);
        Root = nullptr;

        BlorgFreeFileContext(Vcb, Volume);
        Vcb = nullptr;
    }

    //
    // A node built and published the way a completed cold open leaves one
    // (see BlorgInsertByPath/BlorgNodeTablePublish in Create.c), a file
    // stamped as just read so the warm path trusts it (CreateFcbIsCurrent).
    //
    PCOMMON_CONTEXT MakePublishedNode(const wchar_t* path, BOOLEAN IsDirectory)
    {
        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = 4096;
        meta.IsDirectory = IsDirectory;

        UNICODE_STRING name = Path(path);
        PCOMMON_CONTEXT node = nullptr;

        EXPECT_EQ(STATUS_SUCCESS, BlorgInsertByPath(Root, &name, &meta, Volume, &node));

        if (node)
        {
            if (!IsDirectory)
            {
                BlorgPathCacheTakeTicket(&C_CAST(PFCB, node)->MetaTicket);
            }

            BlorgNodeTablePublish(node);
        }

        return node;
    }

    //
    // Publishes a synthetic listing of Dir with one file and one
    // subdirectory entry into the listing cache, the way DirCtrlComplete
    // would have -- so BlorgVolumeCreate's listing-hit branch (CreateFindEntryByName,
    // reached without any network round trip) can be driven directly. The
    // layout arithmetic lives in ListingBuilder.h, shared with
    // DirCtrlTest.cpp rather than copied.
    //
    static void PublishListing(const wchar_t* dir, const wchar_t* fileName, const wchar_t* subdirName)
    {
        UNICODE_STRING dirName = Path(dir);
        PDIRECTORY_INFO listing = BuildSyntheticListingNamed(fileName, subdirName);

        ASSERT_NE(nullptr, listing);
        EXPECT_TRUE(BlorgPathCachePublishListing(&dirName, listing, nullptr));
        BlorgReleaseDirectoryInfo(listing);
    }

    static UNICODE_STRING Path(const wchar_t* path)
    {
        UNICODE_STRING name;
        name.Buffer = const_cast<PWSTR>(path);
        name.Length = (USHORT)(wcslen(path) * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        return name;
    }

    struct CreateOpener
    {
        FILE_OBJECT FileObject;
        IO_SECURITY_CONTEXT SecurityContext;
        IO_STACK_LOCATION CreateStack;
        IRP CreateIrp;
        IO_STACK_LOCATION CleanupStack;
        IRP CleanupIrp;
        IO_STACK_LOCATION CloseStack;
        IRP CloseIrp;
    };

    //
    // One real CREATE IRP the way the I/O manager builds one, plus the
    // matching CLEANUP/CLOSE IRPs on the same file object -- the kernel
    // never reuses one IRP across the three (see DispatchSchedTest.cpp),
    // so building all three up front avoids that class of test bug.
    //
    void PrepareOpener(CreateOpener* opener, const UNICODE_STRING& path,
        ACCESS_MASK desiredAccess, USHORT shareAccess, ULONG extraOptions)
    {
        memset(opener, 0, sizeof(*opener));

        opener->FileObject.FileName = path;
        opener->FileObject.DeviceObject = Volume;

        opener->SecurityContext.DesiredAccess = desiredAccess;

        opener->CreateStack.MajorFunction = IRP_MJ_CREATE;
        opener->CreateStack.FileObject = &opener->FileObject;
        opener->CreateStack.DeviceObject = Volume;
        opener->CreateStack.Parameters.Create.Options = ((ULONG)FILE_OPEN << 24) | extraOptions;
        opener->CreateStack.Parameters.Create.ShareAccess = shareAccess;
        opener->CreateStack.Parameters.Create.SecurityContext = &opener->SecurityContext;
        opener->CreateIrp.StackLocation = &opener->CreateStack;

        opener->CleanupStack.MajorFunction = IRP_MJ_CLEANUP;
        opener->CleanupStack.FileObject = &opener->FileObject;
        opener->CleanupStack.DeviceObject = Volume;
        opener->CleanupIrp.StackLocation = &opener->CleanupStack;

        opener->CloseStack.MajorFunction = IRP_MJ_CLOSE;
        opener->CloseStack.FileObject = &opener->FileObject;
        opener->CloseStack.DeviceObject = Volume;
        opener->CloseIrp.StackLocation = &opener->CloseStack;
    }

    void CloseOpener(CreateOpener* opener)
    {
        BlorgCleanup(Volume, &opener->CleanupIrp);
        BlorgClose(Volume, &opener->CloseIrp);
    }

    static const USHORT kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    PDEVICE_OBJECT Volume = nullptr;
    PDCB Root = nullptr;
    PFCB Vcb = nullptr;
    DEVICE_OBJECT DiskDevice{};
    VPB DiskVpb{};
};

///////////////////////////////////////////////////////////////////////////
// CreateCheckDirectoryAccess / CreateOpenExistingDcb
///////////////////////////////////////////////////////////////////////////

TEST_F(CreateDirectoryTest, OpenExistingDcbSucceedsWithReadOnlyAccessMask)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, node);

    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media"),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(node, opener.FileObject.FsContext);
    EXPECT_NE(nullptr, opener.FileObject.FsContext2)
        << "CreateOpenExistingDcb must allocate a CCB for the directory handle";
    EXPECT_EQ((ULONG_PTR)FILE_OPENED, opener.CreateIrp.IoStatus.Information);

    CloseOpener(&opener);
}

//
// The entire reason CreateCheckDirectoryAccess exists separately from
// CreateCheckFileAccess: a directory's read-only mask additionally permits
// FILE_ADD_SUBDIRECTORY/FILE_ADD_FILE/FILE_DELETE_CHILD, because adding or
// removing a child is a normal directory operation, not a data write. The
// same bits against a FILE must still be denied -- proving the allowance
// is specific to directories, not a general relaxation that would let a
// "read-only" file handle claim child-mutation rights it makes no sense
// for a file to have.
//
TEST_F(CreateDirectoryTest, DirectoryReadOnlyMaskAllowsChildMutationBitsThatFileMaskDenies)
{
    PCOMMON_CONTEXT dir = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, dir);
    PCOMMON_CONTEXT file = MakePublishedNode(L"\\clip.bin", FALSE);
    ASSERT_NE(nullptr, file);

    const ACCESS_MASK childMutationBits = FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD;

    CreateOpener dirOpener;
    PrepareOpener(&dirOpener, Path(L"\\media"), childMutationBits, kShareAll, 0);
    BlorgCreate(Volume, &dirOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, dirOpener.CreateIrp.IoStatus.Status)
        << "FILE_ADD_SUBDIRECTORY/FILE_ADD_FILE/FILE_DELETE_CHILD are inside "
           "CreateCheckDirectoryAccess's read-only mask";

    CloseOpener(&dirOpener);

    CreateOpener fileOpener;
    PrepareOpener(&fileOpener, Path(L"\\clip.bin"), childMutationBits, kShareAll, 0);
    BlorgCreate(Volume, &fileOpener.CreateIrp);

    EXPECT_EQ(STATUS_ACCESS_DENIED, fileOpener.CreateIrp.IoStatus.Status)
        << "the same bits are outside CreateCheckFileAccess's read-only mask -- a "
           "file has no children to add or delete";
    EXPECT_EQ(nullptr, fileOpener.FileObject.FsContext);
}

//
// MAXIMUM_ALLOWED means "grant whatever I am entitled to", and what the
// caller is entitled to has already been settled by the time this check
// runs -- the devices are FILE_DEVICE_SECURE_OPEN, so the I/O manager
// resolved the bit against the device security descriptor and set the
// handle's granted access from it. Denying it here turned a read handle
// the caller was entitled to into ACCESS_DENIED, which is what every
// application that opens with MAXIMUM_ALLOWED out of habit was getting.
//
// Files and directories both, since the two masks are maintained
// separately and an omission in one is invisible from the other.
//
TEST_F(CreateDirectoryTest, MaximumAllowedIsInsideTheReadOnlyMask)
{
    PCOMMON_CONTEXT dir = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, dir);
    PCOMMON_CONTEXT file = MakePublishedNode(L"\\clip.bin", FALSE);
    ASSERT_NE(nullptr, file);

    CreateOpener dirOpener;
    PrepareOpener(&dirOpener, Path(L"\\media"), MAXIMUM_ALLOWED, kShareAll, 0);
    BlorgCreate(Volume, &dirOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, dirOpener.CreateIrp.IoStatus.Status)
        << "a directory opened with MAXIMUM_ALLOWED was refused a read handle";

    CloseOpener(&dirOpener);

    CreateOpener fileOpener;
    PrepareOpener(&fileOpener, Path(L"\\clip.bin"), MAXIMUM_ALLOWED, kShareAll, 0);
    BlorgCreate(Volume, &fileOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, fileOpener.CreateIrp.IoStatus.Status)
        << "a file opened with MAXIMUM_ALLOWED was refused a read handle";

    CloseOpener(&fileOpener);
}

//
// The access-mask policy stated once, as a set, and checked one bit at a
// time through a real open.
//
// Both predicates are "reject if any bit falls outside the permitted set",
// which is monotone in bits: if every single-bit mask is decided correctly
// then every combination is too. So driving all 32 bits pins the whole
// policy, and it pins it against the mask below rather than against
// whatever the code currently happens to do -- which is what makes it
// useful either as a regression test or as an equivalence check when the
// two predicates are restructured.
//
// The file and directory sets differ by exactly the three child-mutation
// bits, and that difference is the only reason two predicates exist.
//
TEST_F(CreateDirectoryTest, ReadOnlyAccessMaskIsDecidedBitByBit)
{
    const ACCESS_MASK permittedOnAFile =
        DELETE | READ_CONTROL | WRITE_OWNER | WRITE_DAC | SYNCHRONIZE |
        ACCESS_SYSTEM_SECURITY | FILE_READ_DATA | FILE_READ_EA | FILE_WRITE_EA |
        FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_EXECUTE |
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | MAXIMUM_ALLOWED;

    const ACCESS_MASK permittedOnADirectory =
        permittedOnAFile | FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD;

    PCOMMON_CONTEXT dir = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, dir);
    PCOMMON_CONTEXT file = MakePublishedNode(L"\\clip.bin", FALSE);
    ASSERT_NE(nullptr, file);

    for (int bit = 0; bit < 32; ++bit)
    {
        const ACCESS_MASK mask = (ACCESS_MASK)(1u << bit);

        CreateOpener fileOpener;
        PrepareOpener(&fileOpener, Path(L"\\clip.bin"), mask, kShareAll, 0);
        BlorgCreate(Volume, &fileOpener.CreateIrp);

        const NTSTATUS expectedOnAFile =
            (mask & permittedOnAFile) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;

        EXPECT_EQ(expectedOnAFile, fileOpener.CreateIrp.IoStatus.Status)
            << "file open with access bit 0x" << std::hex << mask;

        if (NT_SUCCESS(fileOpener.CreateIrp.IoStatus.Status))
        {
            CloseOpener(&fileOpener);
        }

        CreateOpener dirOpener;
        PrepareOpener(&dirOpener, Path(L"\\media"), mask, kShareAll, 0);
        BlorgCreate(Volume, &dirOpener.CreateIrp);

        const NTSTATUS expectedOnADirectory =
            (mask & permittedOnADirectory) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;

        EXPECT_EQ(expectedOnADirectory, dirOpener.CreateIrp.IoStatus.Status)
            << "directory open with access bit 0x" << std::hex << mask;

        if (NT_SUCCESS(dirOpener.CreateIrp.IoStatus.Status))
        {
            CloseOpener(&dirOpener);
        }
    }

    CreateOpener wholeMask;
    PrepareOpener(&wholeMask, Path(L"\\clip.bin"), permittedOnAFile, kShareAll, 0);
    BlorgCreate(Volume, &wholeMask.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, wholeMask.CreateIrp.IoStatus.Status)
        << "the permitted set must be accepted in full, not only one bit at a time";

    CloseOpener(&wholeMask);
}

TEST_F(CreateDirectoryTest, OpenExistingDcbDeniesAccessOutsideReadOnlyMask)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, node);

    //
    // GENERIC_WRITE, not one of the FILE_WRITE_DATA/FILE_APPEND_DATA bits:
    // every low FILE_* bit CreateCheckDirectoryAccess's read-only mask omits for
    // files (WRITE_DATA=0x2, APPEND_DATA=0x4) aliases a directory-specific
    // bit the SAME mask explicitly allows (ADD_FILE=0x2, ADD_SUBDIRECTORY
    // =0x4 -- see the test above), so those bits cannot demonstrate a
    // directory-mask rejection at all. GENERIC_WRITE is outside the
    // read-only mask AND the full mask, so it actually exercises the
    // rejection this test is named for.
    //
    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media"), GENERIC_WRITE, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_ACCESS_DENIED, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(nullptr, opener.FileObject.FsContext)
        << "a denied open must not wire up the file object";
    EXPECT_EQ(0, ReadNoFence64(&node->RefCount))
        << "CreateCheckDirectoryAccess must reject before CreateOpenExistingDcb takes a reference";
}

///////////////////////////////////////////////////////////////////////////
// CreateOpenRootDcb
///////////////////////////////////////////////////////////////////////////

TEST_F(CreateDirectoryTest, OpenRootDcbSucceedsWithReadOnlyAccessMask)
{
    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\"), FILE_LIST_DIRECTORY | SYNCHRONIZE, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(Root, opener.FileObject.FsContext);
    EXPECT_NE(nullptr, opener.FileObject.FsContext2)
        << "CreateOpenRootDcb must allocate a CCB just like CreateOpenExistingDcb";

    CloseOpener(&opener);
}

TEST_F(CreateDirectoryTest, OpenRootDcbDeniesAccessOutsideReadOnlyMask)
{
    // See the comment in OpenExistingDcbDeniesAccessOutsideReadOnlyMask
    // for why GENERIC_WRITE rather than a FILE_* data/append bit.
    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\"), GENERIC_WRITE, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_ACCESS_DENIED, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(nullptr, opener.FileObject.FsContext);
    EXPECT_EQ(0, ReadNoFence64(&Root->RefCount));
}

///////////////////////////////////////////////////////////////////////////
// CreateBreakHandleOplockOnSharingViolation
///////////////////////////////////////////////////////////////////////////

//
// A second, incompatible directory open must see the FIRST opener's
// sharing violation come back out, not something the oplock break
// invented or swallowed. FsRtlOplockBreakH is stubbed to always return
// STATUS_SUCCESS in this model (DispatchModel.c), which is the "no handle
// oplock to break" case -- exactly the branch that returns ShareStatus
// unchanged rather than the break's own status.
//
TEST_F(CreateDirectoryTest, SharingViolationOnDirectoryOpenTriggersOplockBreakAndPreservesStatus)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\media\\locked", TRUE);
    ASSERT_NE(nullptr, node);

    CreateOpener first;
    PrepareOpener(&first, Path(L"\\media\\locked"), FILE_LIST_DIRECTORY, 0 /* exclusive */, 0);
    BlorgCreate(Volume, &first.CreateIrp);
    ASSERT_EQ(STATUS_SUCCESS, first.CreateIrp.IoStatus.Status);

    CreateOpener second;
    PrepareOpener(&second, Path(L"\\media\\locked"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &second.CreateIrp);

    EXPECT_EQ(STATUS_SHARING_VIOLATION, second.CreateIrp.IoStatus.Status);
    EXPECT_EQ(nullptr, second.FileObject.FsContext2)
        << "a failed directory open must not leak the CCB CreateOpenExistingDcb "
           "allocated before the share-access check failed";

    PDCB dcb = C_CAST(PDCB, node);
    EXPECT_EQ(1u, dcb->ShareAccess.OpenCount)
        << "the second, rejected opener must not have been counted";
    EXPECT_EQ(1, ReadNoFence64(&node->RefCount));

    CloseOpener(&first);
}

//
// FILE_COMPLETE_IF_OPLOCKED means the caller explicitly asked not to
// trigger a break, so CreateBreakHandleOplockOnSharingViolation must take its
// early-return branch and hand the sharing violation straight back
// without calling FsRtlOplockBreakH at all -- the other half of that
// function's one `if`, not exercised by the unconditional-break test above.
//
TEST_F(CreateDirectoryTest, SharingViolationWithCompleteIfOplockedSkipsTheOplockBreak)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\media\\locked", TRUE);
    ASSERT_NE(nullptr, node);

    CreateOpener first;
    PrepareOpener(&first, Path(L"\\media\\locked"), FILE_LIST_DIRECTORY, 0 /* exclusive */, 0);
    BlorgCreate(Volume, &first.CreateIrp);
    ASSERT_EQ(STATUS_SUCCESS, first.CreateIrp.IoStatus.Status);

    CreateOpener second;
    PrepareOpener(&second, Path(L"\\media\\locked"), FILE_LIST_DIRECTORY, kShareAll,
        FILE_COMPLETE_IF_OPLOCKED);
    BlorgCreate(Volume, &second.CreateIrp);

    EXPECT_EQ(STATUS_SHARING_VIOLATION, second.CreateIrp.IoStatus.Status);

    CloseOpener(&first);
}

///////////////////////////////////////////////////////////////////////////
// CreateSplitPathLeaf / CreateFindEntryByName, via a cached parent listing
///////////////////////////////////////////////////////////////////////////

//
// A directory neither in the node table nor the path cache, but present in
// its parent's already-cached listing, is exactly how a warm directory's
// children resolve day to day -- and it reaches CreateSplitPathLeaf and
// CreateFindEntryByName's subdirectory loop without a network round trip.
//
TEST_F(CreateDirectoryTest, NewSubdirectoryResolvedThroughCachedParentListingIsOpenedAndPublished)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    PublishListing(L"\\media", L"clip.bin", L"movies");

    CreateOpener dirOpener;
    PrepareOpener(&dirOpener, Path(L"\\media\\movies"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &dirOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, dirOpener.CreateIrp.IoStatus.Status);
    ASSERT_NE(nullptr, dirOpener.FileObject.FsContext);
    EXPECT_EQ(BLORGFS_DCB_SIGNATURE, GET_NODE_TYPE(dirOpener.FileObject.FsContext))
        << "CreateFindEntryByName's subdirectory match must produce IsDirectory=TRUE";

    CloseOpener(&dirOpener);

    //
    // Same listing, the file half -- CreateFindEntryByName's OTHER loop
    // (FileCount, checked before SubDirCount).
    //
    CreateOpener fileOpener;
    PrepareOpener(&fileOpener, Path(L"\\media\\clip.bin"), FILE_READ_DATA, kShareAll, 0);
    BlorgCreate(Volume, &fileOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, fileOpener.CreateIrp.IoStatus.Status);
    ASSERT_NE(nullptr, fileOpener.FileObject.FsContext);
    EXPECT_EQ(BLORGFS_FCB_SIGNATURE, GET_NODE_TYPE(fileOpener.FileObject.FsContext));

    CloseOpener(&fileOpener);
}

TEST_F(CreateDirectoryTest, LeafNotInCachedParentListingReturnsObjectNameNotFound)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    PublishListing(L"\\media", L"clip.bin", L"movies");

    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media\\ghost"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status)
        << "a leaf absent from both loops of a resolved listing must fail "
           "without falling through to a network lookup";
}

//
// An open answered from a cached listing caches its answer as old as the
// fetch behind the listing, not as the open: it goes when the listing
// would have, rather than a lifetime after the listing's last use.
//
TEST_F(CreateDirectoryTest, EntrySeededFromACachedListingIsAsOldAsTheListing)
{
    constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;

    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    PublishListing(L"\\media", L"clip.bin", L"movies");
    ShimAdvanceInterruptTime(3 * kSecond);

    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media\\movies"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &opener.CreateIrp);
    ASSERT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);
    CloseOpener(&opener);

    ShimAdvanceInterruptTime(2 * kSecond);

    UNICODE_STRING movies = Path(L"\\media\\movies");
    EXPECT_EQ(PathCacheMiss, BlorgPathCacheLookup(&movies, nullptr))
        << "the listing was fetched five seconds ago, past the four the cache trusts";
}

///////////////////////////////////////////////////////////////////////////
// Relative opens -- RelatedFileObject path concatenation
///////////////////////////////////////////////////////////////////////////

//
// An open with OBJECT_ATTRIBUTES.RootDirectory set (openat-style: a
// directory handle plus a leaf name) is the one shape where
// BlorgVolumeCreate has to build the full path itself rather than take
// FileObject->FileName as-is. Nothing else in the suite drives it, so the
// concatenation had never run against a parent deeper than the root.
//
// Two claims here, and the memory-safety one is why this test exists at
// all: the joined path must be assembled inside the block that was
// allocated for it, and it must come out as parent + '\' + leaf. The shim
// pool's tail guard is what makes the first claim an assertion rather than
// a hope -- a write past the block trips it on free, whatever the
// allocator would have done with those bytes in the kernel.
//
// A parent of "\\media" is deliberately deeper than the root: a root-
// relative open (parent name "\", two bytes) is the one length where a
// byte-vs-WCHAR mixup in the destination offset lands in the right place
// by coincidence, so testing only that would prove nothing.
//
TEST_F(CreateDirectoryTest, RelativeOpenBuildsJoinedPathWithinItsAllocation)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    PCOMMON_CONTEXT leaf = MakePublishedNode(L"\\media\\clip.bin", FALSE);
    ASSERT_NE(nullptr, leaf);

    CreateOpener parentOpener;
    PrepareOpener(&parentOpener, Path(L"\\media"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    BlorgCreate(Volume, &parentOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, parentOpener.CreateIrp.IoStatus.Status);

    CreateOpener relativeOpener;
    PrepareOpener(&relativeOpener, Path(L"clip.bin"), FILE_READ_DATA, kShareAll, 0);
    relativeOpener.FileObject.RelatedFileObject = &parentOpener.FileObject;

    BlorgCreate(Volume, &relativeOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, relativeOpener.CreateIrp.IoStatus.Status)
        << "\"clip.bin\" relative to a handle on \\media must resolve to \\media\\clip.bin";
    EXPECT_EQ(leaf, relativeOpener.FileObject.FsContext)
        << "the relative open resolved to a different node than the absolute path does";

    CloseOpener(&relativeOpener);
    CloseOpener(&parentOpener);
}

//
// The root-relative case, which is the common one in practice and the one
// length the joining arithmetic gets right by accident. It is here for the
// separator rather than the bounds: the parent's name is already "\\", so
// appending another one would send "\\\\clip.bin" to the backend and to
// the node table -- a path that matches nothing the absolute open
// produces.
//
TEST_F(CreateDirectoryTest, RootRelativeOpenDoesNotDoubleTheSeparator)
{
    PCOMMON_CONTEXT leaf = MakePublishedNode(L"\\clip.bin", FALSE);
    ASSERT_NE(nullptr, leaf);

    CreateOpener rootOpener;
    PrepareOpener(&rootOpener, Path(L"\\"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    BlorgCreate(Volume, &rootOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, rootOpener.CreateIrp.IoStatus.Status);

    CreateOpener relativeOpener;
    PrepareOpener(&relativeOpener, Path(L"clip.bin"), FILE_READ_DATA, kShareAll, 0);
    relativeOpener.FileObject.RelatedFileObject = &rootOpener.FileObject;

    BlorgCreate(Volume, &relativeOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, relativeOpener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(leaf, relativeOpener.FileObject.FsContext)
        << "a doubled separator resolves to a path no absolute open ever produces";

    CloseOpener(&relativeOpener);
    CloseOpener(&rootOpener);
}

//
// The tests above resolve on the warm path, which looks the joined path up
// in the node table and never walks the tree. A relative open that misses
// there walks it, and the walk takes a full path: started from the parent
// instead of the root, "\\media\\clip.bin" goes looking for a child named
// "media" under \media. An extracted archive is exactly that shape, and
// here the walk would land on \media\media\clip.bin and hand its FCB to an
// open of \media\clip.bin.
//
TEST_F(CreateDirectoryTest, ColdRelativeOpenWalksFromTheRootNotTheParent)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    ASSERT_NE(nullptr, MakePublishedNode(L"\\media\\media", TRUE));

    PCOMMON_CONTEXT nested = MakePublishedNode(L"\\media\\media\\clip.bin", FALSE);
    ASSERT_NE(nullptr, nested);

    PublishListing(L"\\media", L"clip.bin", L"media");

    CreateOpener parentOpener;
    PrepareOpener(&parentOpener, Path(L"\\media"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    BlorgCreate(Volume, &parentOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, parentOpener.CreateIrp.IoStatus.Status);

    CreateOpener relativeOpener;
    PrepareOpener(&relativeOpener, Path(L"clip.bin"), FILE_READ_DATA, kShareAll, 0);
    relativeOpener.FileObject.RelatedFileObject = &parentOpener.FileObject;

    BlorgCreate(Volume, &relativeOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, relativeOpener.CreateIrp.IoStatus.Status);

    PFCB opened = C_CAST(PFCB, relativeOpener.FileObject.FsContext);
    ASSERT_NE(nullptr, opened);
    EXPECT_NE(nested, C_CAST(PCOMMON_CONTEXT, opened))
        << "the open of \\media\\clip.bin was answered with \\media\\media\\clip.bin";

    UNICODE_STRING expected = Path(L"\\media\\clip.bin");
    EXPECT_TRUE(RtlEqualUnicodeString(&expected, &opened->FullPath, TRUE));
    EXPECT_EQ(parent, C_CAST(PCOMMON_CONTEXT, opened->ParentDcb));
    EXPECT_EQ(2048, opened->Header.FileSize.QuadPart)
        << "the size must come from \\media's listing entry";

    CloseOpener(&relativeOpener);
    CloseOpener(&parentOpener);
}

//
// A relative open's own file object names only its leaf, so a relative open
// against it cannot take the parent half from there: "movies" joined with
// "clip.bin" is a path with no leading separator, which no node, path
// cache entry or listing is keyed by.
//
TEST_F(CreateDirectoryTest, RelativeOpenAgainstARelativelyOpenedDirectoryUsesItsFullPath)
{
    ASSERT_NE(nullptr, MakePublishedNode(L"\\media", TRUE));
    ASSERT_NE(nullptr, MakePublishedNode(L"\\media\\movies", TRUE));

    PCOMMON_CONTEXT leaf = MakePublishedNode(L"\\media\\movies\\clip.bin", FALSE);
    ASSERT_NE(nullptr, leaf);

    CreateOpener mediaOpener;
    PrepareOpener(&mediaOpener, Path(L"\\media"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    BlorgCreate(Volume, &mediaOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, mediaOpener.CreateIrp.IoStatus.Status);

    CreateOpener moviesOpener;
    PrepareOpener(&moviesOpener, Path(L"movies"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    moviesOpener.FileObject.RelatedFileObject = &mediaOpener.FileObject;
    BlorgCreate(Volume, &moviesOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, moviesOpener.CreateIrp.IoStatus.Status);

    CreateOpener leafOpener;
    PrepareOpener(&leafOpener, Path(L"clip.bin"), FILE_READ_DATA, kShareAll, 0);
    leafOpener.FileObject.RelatedFileObject = &moviesOpener.FileObject;
    BlorgCreate(Volume, &leafOpener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, leafOpener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(leaf, leafOpener.FileObject.FsContext);

    if (NT_SUCCESS(leafOpener.CreateIrp.IoStatus.Status))
    {
        CloseOpener(&leafOpener);
    }

    CloseOpener(&moviesOpener);
    CloseOpener(&mediaOpener);
}

//
// A relative open that resolves nowhere locally has to go out to the
// network, and the first pass runs on the caller's thread rather than an
// FSP worker, so it reposts itself and returns. That repost is the one
// exit in BlorgVolumeCreate that leaves the joined path behind: every
// other early return frees it, and the second pass builds its own copy
// from the file object, so the first pass's buffer has no owner left.
//
// The absolute-open form of the same miss allocates nothing (the path is
// FileObject->FileName, borrowed), which is why only the relative form
// can show this. The pool delta across the call is the assertion -- this
// branch allocates nothing else, so a failed repost must leave the count
// exactly where it started.
//
// STATUS_DEVICE_REMOVED rather than STATUS_PENDING is BlorgFsdPostRequest's
// ThreadsActive gate: DispatchSandbox never starts the real FSP workers
// (see ReadTest.cpp's CachedReadMissWithWaitReachesFsdPostRequest). The
// gate fires before the queue insert, so the IRP is still ours and the
// leak is attributable to this call and nothing else.
//
TEST_F(CreateDirectoryTest, RelativeOpenThatRepostsToTheFspFreesItsJoinedPath)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    CreateOpener parentOpener;
    PrepareOpener(&parentOpener, Path(L"\\media"), FILE_LIST_DIRECTORY | FILE_TRAVERSE, kShareAll, 0);
    BlorgCreate(Volume, &parentOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, parentOpener.CreateIrp.IoStatus.Status);

    const size_t before = ShimPoolOutstanding();

    CreateOpener missOpener;
    PrepareOpener(&missOpener, Path(L"nowhere.bin"), FILE_READ_DATA, kShareAll, 0);
    missOpener.FileObject.RelatedFileObject = &parentOpener.FileObject;

    BlorgCreate(Volume, &missOpener.CreateIrp);

    ASSERT_EQ(STATUS_DEVICE_REMOVED, missOpener.CreateIrp.IoStatus.Status)
        << "this test needs the create to reach BlorgFsdPostRequest and be refused there";

    EXPECT_EQ(before, ShimPoolOutstanding())
        << "the joined path built for the first pass was not freed before reposting";

    CloseOpener(&parentOpener);
}

///////////////////////////////////////////////////////////////////////////
// BlorgInsertByPath -- a resident file standing where a directory is expected
///////////////////////////////////////////////////////////////////////////

//
// Only DCB carries a ChildrenList; in FCB that offset is the start of
// FILE_LOCK. So descending into a resident FILE as though it were the
// next directory does not fail cleanly -- it walks a list head made of
// whatever FsRtlInitializeFileLock left there and dereferences the
// result. BlorgSearchByPath rejects this shape; BlorgInsertByPath is the other half
// of the same walk and has to reject it too.
//
// Driven through BlorgInsertByPath directly, the way MakePublishedNode does:
// reaching it through a real create means the backend has to claim the
// path exists, and the point here is the tree walk, not the round trip
// that authorises it. In production that authorisation is ordinary -- a
// path that was a file when its FCB was created and is a directory by the
// time a child is opened, with the stale FCB still resident.
//
TEST_F(CreateDirectoryTest, InsertByPathRejectsAFileStandingInForADirectory)
{
    PCOMMON_CONTEXT dir = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, dir);

    PCOMMON_CONTEXT file = MakePublishedNode(L"\\media\\clip.bin", FALSE);
    ASSERT_NE(nullptr, file);

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 128;
    meta.IsDirectory = FALSE;

    UNICODE_STRING throughTheFile = Path(L"\\media\\clip.bin\\inner.bin");
    PCOMMON_CONTEXT inserted = reinterpret_cast<PCOMMON_CONTEXT>(~0ull);

    NTSTATUS status = BlorgInsertByPath(Root, &throughTheFile, &meta, Volume, &inserted);

    EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, status)
        << "a file cannot be an intermediate path component";
    EXPECT_EQ(nullptr, inserted);

    //
    // The rejection must leave the tree exactly as it was: the file keeps
    // its place under \media, and nothing was grafted underneath it.
    //
    UNICODE_STRING filePath = Path(L"\\media\\clip.bin");
    EXPECT_EQ(file, BlorgSearchByPath(Root, &filePath));

    UNICODE_STRING mediaPath = Path(L"\\media");
    EXPECT_EQ(dir, BlorgSearchByPath(Root, &mediaPath));
}

//
// The terminal-component case, which is NOT the one above: a walk whose
// last component is an existing file is the ordinary "already resident"
// result, and must keep reporting that rather than being swept up by the
// intermediate-component rejection.
//
TEST_F(CreateDirectoryTest, InsertByPathTreatsAnExistingFileLeafAsAlreadyPresent)
{
    ASSERT_NE(nullptr, MakePublishedNode(L"\\media", TRUE));

    PCOMMON_CONTEXT file = MakePublishedNode(L"\\media\\clip.bin", FALSE);
    ASSERT_NE(nullptr, file);

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 128;
    meta.IsDirectory = FALSE;

    UNICODE_STRING samePath = Path(L"\\media\\clip.bin");
    PCOMMON_CONTEXT inserted = reinterpret_cast<PCOMMON_CONTEXT>(~0ull);

    EXPECT_EQ(STATUS_SUCCESS, BlorgInsertByPath(Root, &samePath, &meta, Volume, &inserted));
    EXPECT_EQ(nullptr, inserted) << "nothing new is created for a path that already resolves";
}

TEST_F(CreateDirectoryTest, FailedColdOpenDefersItsInsertedNodeForReap)
{
    //
    // Targets the SearchByPath-hit failure arm specifically: the node
    // already sits in the tree (pre-inserted here, unpublished, no
    // handles), the open resolves to it, and the access check rejects.
    // Its only route out of the tree is BlorgVolumeCreate's own idle-test
    // defer -- nothing will ever close it. The freshly-inserted sibling
    // arm frees inline and is deliberately NOT the shape under test.
    //
    // Probe is the tree itself: a cold SearchByPath under the VCB resource
    // finds a stranded node and misses a reaped one, whatever the
    // lookaside lists are caching. Deterministic, which is what makes this
    // a mutation target rather than an argument.
    //
    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 1024;

    UNICODE_STRING clipPath = Path(L"\\clip.bin");

    PCOMMON_CONTEXT node = nullptr;
    ASSERT_EQ(STATUS_SUCCESS, BlorgInsertByPath(Root, &clipPath, &meta, Volume, &node));
    ASSERT_NE(nullptr, node);

    PublishListing(L"\\", L"clip.bin", L"movies");

    CreateOpener opener;
    PrepareOpener(&opener, clipPath,
        FILE_READ_DATA | FILE_WRITE_DATA, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    ASSERT_EQ(STATUS_ACCESS_DENIED, opener.CreateIrp.IoStatus.Status)
        << "the open must fail after resolving the node for this path to be exercised";

    SIZE_T postOpen = ShimPoolOutstanding();

    ShimDrainWorkItems();

    //
    // The open's path-cache entry and the root's listing are both cache
    // state this test created (production frees them via TTL or
    // invalidation), so drop them before teardown's quiescence floor;
    // invalidating a path also drops its parent's listing.
    //
    UNICODE_STRING clipSeed = Path(L"\\clip.bin");
    BlorgPathCacheInvalidate(&clipSeed);

    ExAcquireResourceExclusiveLite(Vcb->Header.Resource, TRUE);
    PCOMMON_CONTEXT stranded = BlorgSearchByPath(Root, &clipPath);
    ExReleaseResourceLite(Vcb->Header.Resource);

    EXPECT_EQ(nullptr, stranded)
        << "the resolved node was never reaped after its open failed";
}

///////////////////////////////////////////////////////////////////////////
// Reopening a resident file -- CreateFcbIsCurrent / CreateFcbRefresh
///////////////////////////////////////////////////////////////////////////

//
// A file read and closed stays resident while Cc holds its file object:
// cleaned up, its close still owed. These drive the next open of it after
// the server's copy changed, which is what the warm path used to answer
// with the old size. The change arrives the way the change feed delivers
// one: the path is invalidated, then the path cache learns the new answer.
//
class FcbReopenTest : public CreateDirectoryTest
{
protected:
    void SetUp() override
    {
        CreateDirectoryTest::SetUp();

        ASSERT_NE(nullptr, MakePublishedNode(L"\\media", TRUE));
        File = C_CAST(PFCB, MakePublishedNode(L"\\media\\clip.bin", FALSE));
        ASSERT_NE(nullptr, File);

        Stats = BlorgStatisticsForCurrentProcessor();
        ASSERT_NE(nullptr, Stats);
    }

    NTSTATUS Open(CreateOpener* opener)
    {
        PrepareOpener(opener, Path(L"\\media\\clip.bin"), FILE_READ_DATA, kShareAll, 0);
        BlorgCreate(Volume, &opener->CreateIrp);
        return opener->CreateIrp.IoStatus.Status;
    }

    static void ServerChanged(ULONG64 size, ULONG64 lastModified)
    {
        UNICODE_STRING path = Path(L"\\media\\clip.bin");
        BlorgPathCacheInvalidate(&path);

        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = size;
        meta.LastModifiedTime = lastModified;
        BlorgPathCacheInsertExists(&path, &meta, nullptr);
    }

    //
    // The pass CreateComplete re-queues once the network answered: the
    // result stashed on the IRP the way it leaves it, consumed at the top
    // of BlorgVolumeCreate on the FSP thread.
    //
    NTSTATUS Redrive(CreateOpener* opener, const wchar_t* path, const PATH_CACHE_TICKET& ticket, BOOLEAN noStore)
    {
        PCREATE_NET_RESULT stash = C_CAST(PCREATE_NET_RESULT,
            ExAllocatePoolZero(NonPagedPoolNx, sizeof(CREATE_NET_RESULT), 'CRET'));
        EXPECT_NE(nullptr, stash);
        stash->Ticket = ticket;
        stash->Meta.Size = 4096;
        stash->Meta.NoStore = noStore;

        PrepareOpener(opener, Path(path), FILE_READ_DATA, kShareAll, 0);
        opener->CreateIrp.Tail.Overlay.DriverContext[0] =
            C_CAST(PVOID, C_CAST(ULONG_PTR, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP | IRP_CONTEXT_FLAG_NET_DONE));
        opener->CreateIrp.Tail.Overlay.DriverContext[1] = stash;

        return BlorgVolumeCreate(&opener->CreateIrp, &opener->CreateStack, Volume);
    }

    //
    // Runs one FSP worker on a thread of its own until every opener's
    // create has completed, then stops the queue and waits for it to exit.
    // The queue must already be running.
    //
    static void RunFspWorkerUntilCompleted(std::initializer_list<CreateOpener*> openers)
    {
        HANDLE worker = CreateThread(NULL, 0, [](LPVOID) -> DWORD { BlorgFspDispatch(NULL); return 0; }, NULL, 0, NULL);
        ASSERT_NE((HANDLE)NULL, worker);

        const DWORD start = GetTickCount();

        for (CreateOpener* opener : openers)
        {
            while (0 == ReadNoFence(&opener->CreateIrp.CompletionCount) && GetTickCount() - start < 30000)
            {
                SwitchToThread();
            }
        }

        BlorgDestroyWorkQueue();
        EXPECT_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker, 30000));
        CloseHandle(worker);
    }

    PFCB File = nullptr;
    PBLORGFS_STATISTICS Stats = nullptr;
};

TEST_F(FcbReopenTest, ReopenAfterTheServerChangedTheFileTakesTheNewSize)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));
    BlorgCleanup(Volume, &first.CleanupIrp);

    ServerChanged(5096, 7);

    const LONG purgesBefore = ShimCachePurges();
    const ULONG64 refreshesBefore = Stats->FcbRefreshes;

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));

    EXPECT_EQ(File, second.FileObject.FsContext)
        << "the resident FCB is refreshed in place, not replaced by a second one for the same file";
    EXPECT_EQ(5096, File->Header.FileSize.QuadPart);
    EXPECT_EQ(5096, File->Header.AllocationSize.QuadPart);
    EXPECT_EQ(7u, File->LastModifiedTime);
    EXPECT_EQ(purgesBefore + 1, ShimCachePurges()) << "the old pages must go before the new size is taken";
    EXPECT_EQ(refreshesBefore + 1, Stats->FcbRefreshes);

    CloseOpener(&second);
    BlorgClose(Volume, &first.CloseIrp);
}

//
// The stamp is as old as the read behind the path-cache entry that answered
// the open, not as the open: an FCB refreshed from an entry near the end of
// its lifetime is trusted only for what is left of it.
//
TEST_F(FcbReopenTest, RefreshFromTheCacheIsStampedWithTheEntrysAge)
{
    constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;

    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));
    BlorgCleanup(Volume, &first.CleanupIrp);

    ServerChanged(5096, 7);
    ShimAdvanceInterruptTime(3 * kSecond);

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));
    ASSERT_EQ(5096, File->Header.FileSize.QuadPart);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&File->MetaTicket));

    ShimAdvanceInterruptTime(2 * kSecond);

    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&File->MetaTicket))
        << "the size came from a read five seconds old, past the four the cache trusts";

    CloseOpener(&second);
    BlorgClose(Volume, &first.CloseIrp);
}

//
// A file first resolved from the network is stamped with the ticket its
// lookup was issued under, so its next reopen inside the lifetime is
// answered warm. An answer the server marked no-store vouches for nothing.
//
TEST_F(FcbReopenTest, FileResolvedFromTheNetworkIsStampedWithItsRead)
{
    constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;

    PATH_CACHE_TICKET read;
    BlorgPathCacheTakeTicket(&read);
    ShimAdvanceInterruptTime(kSecond);

    CreateOpener opener;
    ASSERT_EQ(STATUS_SUCCESS, Redrive(&opener, L"\\media\\fresh.bin", read, FALSE));

    PFCB fresh = C_CAST(PFCB, opener.FileObject.FsContext);
    ASSERT_NE(nullptr, fresh);
    EXPECT_EQ(read.IssueTime, fresh->MetaTicket.IssueTime);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&fresh->MetaTicket));

    CloseOpener(&opener);
}

TEST_F(FcbReopenTest, FileResolvedFromACachedListingIsStampedWithTheListingsAge)
{
    constexpr ULONG64 kSecond = 10ULL * 1000ULL * 1000ULL;

    PDIRECTORY_INFO listing = BuildSyntheticListingNamed(L"fresh.bin", L"sub");
    ASSERT_NE(nullptr, listing);
    UNICODE_STRING media = Path(L"\\media");
    EXPECT_TRUE(BlorgPathCachePublishListing(&media, listing, nullptr));
    BlorgReleaseDirectoryInfo(listing);

    ShimAdvanceInterruptTime(3 * kSecond);

    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media\\fresh.bin"), FILE_READ_DATA, kShareAll, 0);
    BlorgCreate(Volume, &opener.CreateIrp);
    ASSERT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);

    PFCB fresh = C_CAST(PFCB, opener.FileObject.FsContext);
    ASSERT_NE(nullptr, fresh);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&fresh->MetaTicket));

    ShimAdvanceInterruptTime(2 * kSecond);

    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&fresh->MetaTicket))
        << "the size came from a listing fetched five seconds ago";

    CloseOpener(&opener);
}

TEST_F(FcbReopenTest, FileResolvedFromANoStoreAnswerIsNotStamped)
{
    PATH_CACHE_TICKET read;
    BlorgPathCacheTakeTicket(&read);

    CreateOpener opener;
    ASSERT_EQ(STATUS_SUCCESS, Redrive(&opener, L"\\media\\aliased.bin", read, TRUE));

    PFCB fresh = C_CAST(PFCB, opener.FileObject.FsContext);
    ASSERT_NE(nullptr, fresh);
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&fresh->MetaTicket));

    CloseOpener(&opener);
}

TEST_F(FcbReopenTest, ReopenWhileAHandleIsOpenSharesTheCopyThatHandleHas)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));

    ServerChanged(5096, 7);

    const LONG purgesBefore = ShimCachePurges();

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));

    EXPECT_EQ(File, second.FileObject.FsContext);
    EXPECT_EQ(4096, File->Header.FileSize.QuadPart)
        << "changing the size under an open handle would tear the view it is reading";
    EXPECT_EQ(purgesBefore, ShimCachePurges());

    CloseOpener(&second);
    CloseOpener(&first);
}

TEST_F(FcbReopenTest, ReopenOfAFileRemovedOnTheServerIsNotFound)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));
    BlorgCleanup(Volume, &first.CleanupIrp);

    UNICODE_STRING path = Path(L"\\media\\clip.bin");
    BlorgPathCacheInvalidate(&path);
    BlorgPathCacheInsertNotFound(&path, nullptr);

    CreateOpener second;
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, Open(&second))
        << "a resident FCB must not answer for a file the server no longer has";

    BlorgClose(Volume, &first.CloseIrp);
}

TEST_F(FcbReopenTest, ReopenOfAnUnchangedFileIsAnsweredWarm)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));
    BlorgCleanup(Volume, &first.CleanupIrp);

    ServerChanged(4096, 0);

    const LONG purgesBefore = ShimCachePurges();
    const ULONG64 refreshesBefore = Stats->FcbRefreshes;
    const ULONG64 lookupsBefore = Stats->MetaDataReads;

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));

    EXPECT_EQ(File, second.FileObject.FsContext);
    EXPECT_EQ(purgesBefore, ShimCachePurges()) << "nothing changed, so nothing cached may be dropped";
    EXPECT_EQ(refreshesBefore, Stats->FcbRefreshes);
    EXPECT_EQ(lookupsBefore, Stats->MetaDataReads) << "a warm reopen resolves nothing, so it counts no lookup";

    CloseOpener(&second);
    BlorgClose(Volume, &first.CloseIrp);
}

//
// A user-mapped view outlives its handle and keeps the old pages, which
// is when the purge fails. The FCB then keeps its old copy whole rather
// than taking a size its cached pages disagree with. Opens until the next
// reported change do not retry the purge, and the first after it, with
// the view gone, takes the new size.
//
TEST_F(FcbReopenTest, PurgeRefusedKeepsTheOldCopyUntilTheNextOpen)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));
    BlorgCleanup(Volume, &first.CleanupIrp);

    ServerChanged(5096, 7);

    const ULONG64 deferredBefore = Stats->FcbRefreshesDeferred;

    ShimRefuseNextCachePurge();

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));
    EXPECT_EQ(4096, File->Header.FileSize.QuadPart);
    EXPECT_EQ(0u, File->LastModifiedTime);
    EXPECT_EQ(deferredBefore + 1, Stats->FcbRefreshesDeferred);
    CloseOpener(&second);

    const LONG purgesBefore = ShimCachePurges();

    CreateOpener third;
    ASSERT_EQ(STATUS_SUCCESS, Open(&third));
    EXPECT_EQ(4096, File->Header.FileSize.QuadPart);
    EXPECT_EQ(purgesBefore, ShimCachePurges()) << "every open retrying a purge the view still blocks";
    CloseOpener(&third);

    ServerChanged(5096, 7);

    CreateOpener fourth;
    ASSERT_EQ(STATUS_SUCCESS, Open(&fourth));
    EXPECT_EQ(5096, File->Header.FileSize.QuadPart);
    CloseOpener(&fourth);

    BlorgClose(Volume, &first.CloseIrp);
}

//
// Every file handle has a CCB of its own. Cc keeps the read-ahead granule
// per file object, and the cached read path keeps what it told Cc on the
// handle's CCB (ReadAdaptGranularity, Read.c); a file open used to leave
// FsContext2 NULL, and the granule lived on the FCB, where one handle's
// first read reset another's. An open the share check refuses frees the
// CCB it was given, and each close frees its handle's, which the volume's
// CCB lookaside list checks at teardown.
//
TEST_F(FcbReopenTest, EachFileHandleHasACcbOfItsOwn)
{
    CreateOpener first;
    PrepareOpener(&first, Path(L"\\media\\clip.bin"), FILE_READ_DATA, FILE_SHARE_READ, 0);
    BlorgCreate(Volume, &first.CreateIrp);
    ASSERT_EQ(STATUS_SUCCESS, first.CreateIrp.IoStatus.Status);

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));

    PCCB firstCcb = C_CAST(PCCB, first.FileObject.FsContext2);
    PCCB secondCcb = C_CAST(PCCB, second.FileObject.FsContext2);

    ASSERT_NE(nullptr, firstCcb);
    ASSERT_NE(nullptr, secondCcb);
    EXPECT_NE(firstCcb, secondCcb) << "two handles sharing one CCB share one read-ahead granule";
    EXPECT_EQ(BLORGFS_CCB_SIGNATURE, GET_NODE_TYPE(firstCcb));
    EXPECT_EQ(BLORGFS_CCB_SIGNATURE, GET_NODE_TYPE(secondCcb));

    CreateOpener refused;
    PrepareOpener(&refused, Path(L"\\media\\clip.bin"), FILE_READ_DATA, 0, 0);
    BlorgCreate(Volume, &refused.CreateIrp);

    EXPECT_EQ(STATUS_SHARING_VIOLATION, refused.CreateIrp.IoStatus.Status);
    EXPECT_EQ(nullptr, refused.FileObject.FsContext2);
    EXPECT_EQ(2u, File->ShareAccess.OpenCount);

    CloseOpener(&second);
    CloseOpener(&first);
}

//
// An exclusive oplock (batch, filter, level 1 or RWH) is granted only to
// the sole open handle, so the count handed to FsRtl must be the handles
// not yet cleaned up, as fastfat's UncleanCount is. It used to be
// RefCount, which drops at close: the file object Cc keeps for its cache
// map is cleaned up when its handle closes but closed only when Cc lets it
// go, so after one cached read the next opener counted two and its batch
// oplock was refused. Here the first handle is cleaned up and its close
// held back, as Cc holds it.
//
TEST_F(FcbReopenTest, AnExclusiveOplockCountsHandlesNotYetCleanedUp)
{
    CreateOpener first;
    ASSERT_EQ(STATUS_SUCCESS, Open(&first));

    CreateOpener second;
    ASSERT_EQ(STATUS_SUCCESS, Open(&second));

    IO_STACK_LOCATION stack = {};
    stack.MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    stack.MinorFunction = IRP_MN_USER_FS_REQUEST;
    stack.DeviceObject = Volume;
    stack.FileObject = &second.FileObject;
    stack.Parameters.FileSystemControl.FsControlCode = FSCTL_REQUEST_BATCH_OPLOCK;

    IRP request = {};
    request.StackLocation = &stack;

    ShimForceNextOplockRequestExclusive();
    EXPECT_EQ(STATUS_PENDING, BlorgFileSystemControl(Volume, &request));
    EXPECT_EQ(2u, ShimLastOplockOpenCount()) << "two handles are open";

    BlorgCleanup(Volume, &first.CleanupIrp);

    IRP retry = {};
    retry.StackLocation = &stack;

    ShimForceNextOplockRequestExclusive();
    EXPECT_EQ(STATUS_PENDING, BlorgFileSystemControl(Volume, &retry));
    EXPECT_EQ(1u, ShimLastOplockOpenCount())
        << "a file object cleaned up but not yet closed was counted as an open handle";

    CloseOpener(&second);
    BlorgClose(Volume, &first.CloseIrp);
}

//
// A create that misses outside the FSP posts itself, and the worker looks
// the path up again. Both passes used to count the lookup, so a posted miss
// counted two MetaDataReads and two PathCacheMisses, and neither pass
// counted the create: BlorgCreate counted only what the FSD pass finished,
// and the worker completed the rest uncounted. The parent's listing is
// published between the post and the worker's pass, so the worker answers
// both opens from it, one found and one not, and the FSD pass is the one
// that posted them. A third open of the name the worker learned is gone is
// a path-cache hit the FSD pass finishes itself.
//
TEST_F(FcbReopenTest, ACreatePostedToTheFspIsCountedOnceByThePassThatFinishesIt)
{
    ASSERT_EQ(STATUS_SUCCESS, BlorgCreateWorkQueue());

    const ULONG64 successesBefore = Stats->SuccessfulCreates;
    const ULONG64 failuresBefore = Stats->FailedCreates;
    const ULONG64 lookupsBefore = Stats->MetaDataReads;
    const ULONG64 missesBefore = Stats->PathCacheMisses;
    const ULONG64 hitsBefore = Stats->PathCacheHits;

    CreateOpener found;
    PrepareOpener(&found, Path(L"\\media\\movie.bin"), FILE_READ_DATA, kShareAll, 0);
    ASSERT_EQ(STATUS_PENDING, BlorgCreate(Volume, &found.CreateIrp));

    CreateOpener ghost;
    PrepareOpener(&ghost, Path(L"\\media\\ghost.bin"), FILE_READ_DATA, kShareAll, 0);
    ASSERT_EQ(STATUS_PENDING, BlorgCreate(Volume, &ghost.CreateIrp));

    EXPECT_EQ(lookupsBefore, Stats->MetaDataReads) << "the FSD pass counted a lookup it only posted";
    EXPECT_EQ(missesBefore, Stats->PathCacheMisses);

    PublishListing(L"\\media", L"movie.bin", L"movies");

    RunFspWorkerUntilCompleted({ &found, &ghost });

    ASSERT_EQ(1u, found.CreateIrp.CompletionCount);
    ASSERT_EQ(1u, ghost.CreateIrp.CompletionCount);
    EXPECT_EQ(STATUS_SUCCESS, found.CreateIrp.IoStatus.Status);
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, ghost.CreateIrp.IoStatus.Status);

    EXPECT_EQ(successesBefore + 1, Stats->SuccessfulCreates) << "a create the worker finished was not counted";
    EXPECT_EQ(failuresBefore + 1, Stats->FailedCreates);
    EXPECT_EQ(lookupsBefore + 2, Stats->MetaDataReads) << "a posted miss was counted on both passes";
    EXPECT_EQ(missesBefore + 2, Stats->PathCacheMisses);
    EXPECT_EQ(hitsBefore, Stats->PathCacheHits);

    CreateOpener again;
    PrepareOpener(&again, Path(L"\\media\\ghost.bin"), FILE_READ_DATA, kShareAll, 0);
    BlorgCreate(Volume, &again.CreateIrp);

    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, again.CreateIrp.IoStatus.Status);
    EXPECT_EQ(failuresBefore + 2, Stats->FailedCreates);
    EXPECT_EQ(lookupsBefore + 3, Stats->MetaDataReads);
    EXPECT_EQ(hitsBefore + 1, Stats->PathCacheHits) << "a path-cache answer was not counted as a hit";
    EXPECT_EQ(missesBefore + 2, Stats->PathCacheMisses);

    CloseOpener(&found);
}

///////////////////////////////////////////////////////////////////////////
// The cold network open -- CreateComplete
///////////////////////////////////////////////////////////////////////////

//
// A DirectoryEntryMetadata for a 4096-byte file, the bytes ClientTest.cpp's
// kFileInfo holds.
//
static const char kCreateFileInfo[] =
    "\x14\x00\x00\x00\x00\x00\x00\x00\x0c\x00\x24\x00\x1c\x00\x14\x00"
    "\x0c\x00\x04\x00\x0c\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00"
    "\x02\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x10\x00\x00\x00\x00\x00\x00";

//
// A path that neither the node table, the path cache nor a cached listing
// answers is looked up on the server. The pass that sends it, driven here
// the way the worker that dequeued it runs it, issues the lookup and pends;
// CreateComplete memoizes the answer and either fails the IRP or re-queues
// it with the result stashed on it, and the FSP worker's second pass opens
// from the stash. The lookup is the real Client.c request against the peer
// scripted through SandboxSocket.h, so the answer comes back through the
// chain a server's does. A create CreateComplete fails, or whose re-queue
// is refused, is completed there, which neither BlorgCreate nor the worker
// sees, so it counts the create itself.
//
class CreateNetworkTest : public FcbReopenTest
{
protected:
    void SetUp() override
    {
        SandboxInitialize();
        FcbReopenTest::SetUp();
    }

    void TearDown() override
    {
        Drain();

        if (QueueRunning)
        {
            BlorgDestroyWorkQueue();
            QueueRunning = FALSE;
        }

        BlorgCleanupWskClient();

        FcbReopenTest::TearDown();
    }

    //
    // The script refers to the response until the test drains, so both
    // live on the fixture. A stalled peer accepts the request and answers
    // only once SandboxResumeStalled lets it.
    //
    void Respond(const char* statusAndHeaders, const char* body, SIZE_T length, BOOLEAN stall = FALSE)
    {
        Response = std::string(statusAndHeaders) + "Content-Length: " + std::to_string(length) +
            "\r\n\r\n" + std::string(body, length);

        SIZE_T count = 0;

        if (stall)
        {
            Script[count++] = { SandboxStepStall, nullptr, 0, STATUS_SUCCESS, FALSE };
        }

        Script[count++] = { SandboxStepDeliver, C_CAST(const unsigned char*, Response.data()), Response.size(), STATUS_SUCCESS, FALSE };
        SandboxSetPeerScript(Script, count);
    }

    void StartQueue()
    {
        ASSERT_EQ(STATUS_SUCCESS, BlorgCreateWorkQueue());
        QueueRunning = TRUE;
    }

    //
    // A work item can issue more I/O, so this runs until neither side has
    // anything left, as ClientTest.cpp's does.
    //
    void Drain()
    {
        do
        {
            SandboxDrainCompletions();
        } while (ShimDrainWorkItems() > 0);
    }

    NTSTATUS SendToTheNetwork(CreateOpener* opener, const wchar_t* path)
    {
        PrepareOpener(opener, Path(path), FILE_READ_DATA, kShareAll, 0);
        opener->CreateIrp.Tail.Overlay.DriverContext[0] =
            C_CAST(PVOID, C_CAST(ULONG_PTR, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP));

        return BlorgVolumeCreate(&opener->CreateIrp, &opener->CreateStack, Volume);
    }

    //
    // Lets one worker take the re-queued IRP, then stops the queue once it
    // has completed it.
    //
    void RunRequeuedPass(CreateOpener* opener)
    {
        RunFspWorkerUntilCompleted({ opener });
        QueueRunning = FALSE;
    }

    static PCREATE_NET_RESULT Stash(CreateOpener* opener)
    {
        return C_CAST(PCREATE_NET_RESULT, opener->CreateIrp.Tail.Overlay.DriverContext[1]);
    }

    static BOOLEAN NetDone(CreateOpener* opener)
    {
        return BooleanFlagOn(C_CAST(ULONG_PTR, opener->CreateIrp.Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_NET_DONE);
    }

    static PATH_CACHE_RESULT Cached(const wchar_t* path, DIRECTORY_ENTRY_METADATA* meta)
    {
        UNICODE_STRING name = Path(path);
        return BlorgPathCacheLookup(&name, meta);
    }

    std::string Response;
    SANDBOX_STEP Script[2] = {};
    BOOLEAN QueueRunning = FALSE;
};

TEST_F(CreateNetworkTest, ACreateTheNetworkLookupFailsIsCountedOnce)
{
    Respond("HTTP/1.1 404 Not Found\r\n", "", 0);

    const ULONG64 failuresBefore = Stats->FailedCreates;
    const ULONG64 lookupsBefore = Stats->MetaDataReads;
    const ULONG64 missesBefore = Stats->PathCacheMisses;

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\gone.bin"));

    Drain();

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(failuresBefore + 1, Stats->FailedCreates) << "a create the network lookup failed was not counted";
    EXPECT_EQ(lookupsBefore + 1, Stats->MetaDataReads) << "the pass that went to the network did not count its miss";
    EXPECT_EQ(missesBefore + 1, Stats->PathCacheMisses);
}

//
// The FSP queue is not started here, so CreateComplete's re-queue of a
// found file is refused, which is what a volume tearing down does to it.
//
TEST_F(CreateNetworkTest, ACreateWhoseRequeueIsRefusedIsCountedOnce)
{
    Respond("HTTP/1.1 200 OK\r\n", kCreateFileInfo, sizeof(kCreateFileInfo) - 1);

    const ULONG64 successesBefore = Stats->SuccessfulCreates;
    const ULONG64 failuresBefore = Stats->FailedCreates;

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\fresh.bin"));

    Drain();

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    EXPECT_EQ(STATUS_DEVICE_REMOVED, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(failuresBefore + 1, Stats->FailedCreates) << "a create whose re-queue was refused was not counted";
    EXPECT_EQ(successesBefore, Stats->SuccessfulCreates);
}

//
// The whole cold open: one lookup, the answer remembered, the IRP handed
// back to the queue carrying it rather than completed, and a second pass
// that opens from what it carries instead of asking again. The file is
// stamped with the lookup's ticket, so its next open inside the lifetime
// is answered warm.
//
TEST_F(CreateNetworkTest, FoundOnTheServerOpensFromTheStashedAnswer)
{
    Respond("HTTP/1.1 200 OK\r\n", kCreateFileInfo, sizeof(kCreateFileInfo) - 1);
    StartQueue();

    const ULONG64 lookupsBefore = Stats->FileInfoRequests;

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\net.bin"));

    Drain();

    EXPECT_EQ(lookupsBefore + 1, Stats->FileInfoRequests);
    ASSERT_EQ(0u, opener.CreateIrp.CompletionCount)
        << "a found file goes back to the FSP queue for its open, not to the caller";
    EXPECT_TRUE(NetDone(&opener));
    ASSERT_NE(nullptr, Stash(&opener));
    EXPECT_EQ(4096u, Stash(&opener)->Meta.Size);
    EXPECT_FALSE(Stash(&opener)->Meta.IsDirectory);

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheExists, Cached(L"\\media\\net.bin", &cached));
    EXPECT_EQ(4096u, cached.Size);

    RunRequeuedPass(&opener);

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    ASSERT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);
    EXPECT_FALSE(NetDone(&opener));
    EXPECT_EQ(nullptr, Stash(&opener));

    PFCB fcb = C_CAST(PFCB, opener.FileObject.FsContext);
    ASSERT_NE(nullptr, fcb);
    EXPECT_EQ(4096, fcb->Header.FileSize.QuadPart);
    EXPECT_TRUE(BlorgPathCacheTicketCurrent(&fcb->MetaTicket));
    EXPECT_EQ(lookupsBefore + 1, Stats->FileInfoRequests)
        << "the second pass must open from the stash, not look the file up again";

    CloseOpener(&opener);
}

//
// A 404 is a definitive answer: the open fails with it, and the next open
// of the same path is answered from the path cache without a lookup.
//
TEST_F(CreateNetworkTest, NotFoundOnTheServerFailsTheOpenAndIsRemembered)
{
    Respond("HTTP/1.1 404 Not Found\r\n", "", 0);

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\gone.bin"));

    Drain();

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(nullptr, opener.FileObject.FsContext);
    EXPECT_EQ(nullptr, Stash(&opener));

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheNotFound, Cached(L"\\media\\gone.bin", &cached));

    const ULONG64 lookupsBefore = Stats->FileInfoRequests;

    CreateOpener again;
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, SendToTheNetwork(&again, L"\\media\\gone.bin"));
    EXPECT_EQ(lookupsBefore, Stats->FileInfoRequests)
        << "a remembered not-found must answer the next open without a lookup";
}

//
// A file created on the server while the lookup was in flight arrives as
// an invalidation of its path. The 404 the server answered before that
// still fails this open, but must not be remembered: cached, it would
// hide the new file from every open until the entry expired.
//
TEST_F(CreateNetworkTest, NotFoundOvertakenByAnInvalidationIsNotRemembered)
{
    Respond("HTTP/1.1 404 Not Found\r\n", "", 0, TRUE);

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\late.bin"));

    Drain();

    ASSERT_EQ(1u, SandboxSocketsParked());
    ASSERT_EQ(0u, opener.CreateIrp.CompletionCount);

    UNICODE_STRING path = Path(L"\\media\\late.bin");
    BlorgPathCacheInvalidate(&path);

    SandboxResumeStalled();
    Drain();

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status);

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheMiss, Cached(L"\\media\\late.bin", &cached))
        << "a not-found read before the invalidation was cached after it";
}

//
// Only a 404 says the file is not there. A server error says nothing about
// it, and remembering one as not-found would fail every open of a file
// that exists until the entry expired.
//
TEST_F(CreateNetworkTest, ServerErrorFailsTheOpenAndIsNotRemembered)
{
    Respond("HTTP/1.1 503 Service Unavailable\r\n", "", 0);

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\busy.bin"));

    Drain();

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    EXPECT_FALSE(NT_SUCCESS(opener.CreateIrp.IoStatus.Status));
    EXPECT_NE(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status);

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheMiss, Cached(L"\\media\\busy.bin", &cached));
}

//
// An answer the server marked no-store still opens the file, but the path
// cache does not keep it and the FCB is not stamped with it, so the next
// open asks again.
//
TEST_F(CreateNetworkTest, NoStoreAnswerOpensTheFileButVouchesForNothing)
{
    Respond("HTTP/1.1 200 OK\r\nCache-Control: no-store\r\n", kCreateFileInfo, sizeof(kCreateFileInfo) - 1);
    StartQueue();

    CreateOpener opener;
    ASSERT_EQ(STATUS_PENDING, SendToTheNetwork(&opener, L"\\media\\aliased.bin"));

    Drain();

    ASSERT_EQ(0u, opener.CreateIrp.CompletionCount);
    ASSERT_NE(nullptr, Stash(&opener));
    EXPECT_TRUE(Stash(&opener)->Meta.NoStore);

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheMiss, Cached(L"\\media\\aliased.bin", &cached))
        << "an answer the server marked no-store was kept in the path cache";

    RunRequeuedPass(&opener);

    ASSERT_EQ(1u, opener.CreateIrp.CompletionCount);
    ASSERT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);

    PFCB fcb = C_CAST(PFCB, opener.FileObject.FsContext);
    ASSERT_NE(nullptr, fcb);
    EXPECT_EQ(4096, fcb->Header.FileSize.QuadPart);
    EXPECT_FALSE(BlorgPathCacheTicketCurrent(&fcb->MetaTicket));

    CloseOpener(&opener);
}

//
// A lookup that fails before it is issued never reaches CreateComplete, so
// the pass that tried it owns its context and returns the failure for the
// worker loop to complete the IRP with.
//
TEST_F(CreateNetworkTest, LookupThatFailsToIssueReturnsItsStatusAndFreesItsContext)
{
    const SIZE_T poolBefore = ShimPoolOutstanding();
    const ULONG socketsBefore = SandboxSocketsCreated();

    ShimFailNextWorkItem();

    CreateOpener opener;
    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, SendToTheNetwork(&opener, L"\\media\\never.bin"));

    Drain();

    EXPECT_EQ(0u, opener.CreateIrp.CompletionCount);
    EXPECT_EQ(socketsBefore, SandboxSocketsCreated());
    EXPECT_EQ(poolBefore, ShimPoolOutstanding())
        << "the lookup's context leaked when the lookup was never issued";

    DIRECTORY_ENTRY_METADATA cached = {};
    EXPECT_EQ(PathCacheMiss, Cached(L"\\media\\never.bin", &cached));
}

} // namespace
