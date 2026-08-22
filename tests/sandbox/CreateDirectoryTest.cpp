//
// Directory create/open coverage over the real Create.c: CheckDirectoryAccess,
// OpenExistingDcb, OpenRootDcb, BreakHandleOplockOnSharingViolation,
// SplitPathLeaf and FindEntryByName -- none of which any other sandbox
// target drives. DispatchTest/DispatchSchedTest/DispatchStressTest all open
// FILES through BlorgCreate; a directory open takes a structurally
// different branch in BlorgVolumeCreate (FILE_NON_DIRECTORY_FILE checks,
// OpenExistingDcb's CCB allocation, the root-path shortcut) that a
// file-only opener never touches.
//
// CheckFileAccess and CheckDirectoryAccess are `static inline` in
// Create.c, unreachable from any other translation unit -- the same
// situation DispatchTest.cpp documents for OpenExistingFcb. Matching that
// file's approach, this drives them through real directory- and file-open
// IRPs rather than declaring them extern, which would test a copy of the
// mask rather than the mask the driver actually applies.
//
// One branch is intentionally NOT covered here: CheckFileAccess and
// CheckDirectoryAccess's IsReadOnly=FALSE side (the FullMask that allows
// FILE_WRITE_DATA). All four call sites -- OpenExistingFcb, OpenExistingDcb,
// OpenVcb, OpenRootDcb -- hardcode IsReadOnly=TRUE, and BlorgFS has no
// write path yet (see the write-path-unimplemented note), so FullMask has
// no caller to reach it through. Testing it would mean calling the static
// inline through a synthetic wrapper -- exactly the "copy of the contract"
// this file avoids elsewhere -- so it stays untested until a write path
// gives it a real caller.
//
// BlorgCreateComplete (the async BlorgHttpGetFileInformation completion)
// is also still 0%: reaching it means scripting a real HTTP round trip
// through the real Client.c and SandboxSocket peer, which is follow-on
// work, not done here. What IS covered without a network round trip is
// the OTHER way BlorgVolumeCreate resolves a cold path: a pre-populated
// parent DCB->CachedListing, which is exactly how a warm directory's
// children resolve once BlorgDirComplete has cached its listing. That
// path exercises SplitPathLeaf and both of FindEntryByName's loops for
// free.
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

extern "C" {
#include "..\..\src\Driver.h"
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
        // OpenExistingDcb/OpenRootDcb wire FileObject->Vpb from
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

        GetVolumeDeviceExtension(Volume)->RootDcb = Root;
        GetVolumeDeviceExtension(Volume)->Vcb = Vcb;
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
        // PathCacheInit() once for the whole process -- gtest runs a
        // registered Environment's SetUp/TearDown regardless of
        // --gtest_filter, so PathCache.Ready is TRUE here whether or not
        // PathCacheTest.cpp's own tests are selected. The listing-hit
        // tests below genuinely call PathCacheInsertExists/InsertNotFound
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
        PathCacheInvalidatePrefix(&mediaSubtree);

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
    // (see InsertByPath/BlorgNodeTablePublish in Create.c).
    //
    PCOMMON_CONTEXT MakePublishedNode(const wchar_t* path, BOOLEAN IsDirectory)
    {
        DIRECTORY_ENTRY_METADATA meta = {};
        meta.Size = 4096;
        meta.IsDirectory = IsDirectory;

        UNICODE_STRING name = Path(path);
        PCOMMON_CONTEXT node = nullptr;

        EXPECT_EQ(STATUS_SUCCESS, InsertByPath(Root, &name, &meta, Volume, &node));

        if (node)
        {
            BlorgNodeTablePublish(node);
        }

        return node;
    }

    //
    // A synthetic parent-directory listing with one file and one
    // subdirectory entry, built the way BlorgDirComplete would have cached
    // one -- so BlorgVolumeCreate's listing-hit branch (FindEntryByName,
    // reached without any network round trip) can be driven directly. The
    // layout arithmetic lives in ListingBuilder.h, shared with
    // DirCtrlTest.cpp rather than copied.
    //
    static PDIRECTORY_INFO BuildListing(const wchar_t* fileName, const wchar_t* subdirName)
    {
        return BuildSyntheticListingNamed(fileName, subdirName);
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
// CheckDirectoryAccess / OpenExistingDcb
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
        << "OpenExistingDcb must allocate a CCB for the directory handle";
    EXPECT_EQ((ULONG_PTR)FILE_OPENED, opener.CreateIrp.IoStatus.Information);

    CloseOpener(&opener);
}

//
// The entire reason CheckDirectoryAccess exists separately from
// CheckFileAccess: a directory's read-only mask additionally permits
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
           "CheckDirectoryAccess's read-only mask";

    CloseOpener(&dirOpener);

    CreateOpener fileOpener;
    PrepareOpener(&fileOpener, Path(L"\\clip.bin"), childMutationBits, kShareAll, 0);
    BlorgCreate(Volume, &fileOpener.CreateIrp);

    EXPECT_EQ(STATUS_ACCESS_DENIED, fileOpener.CreateIrp.IoStatus.Status)
        << "the same bits are outside CheckFileAccess's read-only mask -- a "
           "file has no children to add or delete";
    EXPECT_EQ(nullptr, fileOpener.FileObject.FsContext);
}

TEST_F(CreateDirectoryTest, OpenExistingDcbDeniesAccessOutsideReadOnlyMask)
{
    PCOMMON_CONTEXT node = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, node);

    //
    // GENERIC_WRITE, not one of the FILE_WRITE_DATA/FILE_APPEND_DATA bits:
    // every low FILE_* bit CheckDirectoryAccess's read-only mask omits for
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
        << "CheckDirectoryAccess must reject before OpenExistingDcb takes a reference";
}

///////////////////////////////////////////////////////////////////////////
// OpenRootDcb
///////////////////////////////////////////////////////////////////////////

TEST_F(CreateDirectoryTest, OpenRootDcbSucceedsWithReadOnlyAccessMask)
{
    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\"), FILE_LIST_DIRECTORY | SYNCHRONIZE, kShareAll, 0);

    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_SUCCESS, opener.CreateIrp.IoStatus.Status);
    EXPECT_EQ(Root, opener.FileObject.FsContext);
    EXPECT_NE(nullptr, opener.FileObject.FsContext2)
        << "OpenRootDcb must allocate a CCB just like OpenExistingDcb";

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
// BreakHandleOplockOnSharingViolation
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
        << "a failed directory open must not leak the CCB OpenExistingDcb "
           "allocated before the share-access check failed";

    PDCB dcb = C_CAST(PDCB, node);
    EXPECT_EQ(1u, dcb->ShareAccess.OpenCount)
        << "the second, rejected opener must not have been counted";
    EXPECT_EQ(1, ReadNoFence64(&node->RefCount));

    CloseOpener(&first);
}

//
// FILE_COMPLETE_IF_OPLOCKED means the caller explicitly asked not to
// trigger a break, so BreakHandleOplockOnSharingViolation must take its
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
// SplitPathLeaf / FindEntryByName, via a cached parent listing
///////////////////////////////////////////////////////////////////////////

//
// A directory neither in the node table nor the path cache, but present in
// its parent's already-cached listing, is exactly how a warm directory's
// children resolve day to day -- and it reaches SplitPathLeaf and
// FindEntryByName's subdirectory loop without a network round trip.
//
TEST_F(CreateDirectoryTest, NewSubdirectoryResolvedThroughCachedParentListingIsOpenedAndPublished)
{
    PCOMMON_CONTEXT parent = MakePublishedNode(L"\\media", TRUE);
    ASSERT_NE(nullptr, parent);

    PDCB parentDcb = C_CAST(PDCB, parent);
    parentDcb->CachedListing = BuildListing(L"clip.bin", L"movies");

    CreateOpener dirOpener;
    PrepareOpener(&dirOpener, Path(L"\\media\\movies"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &dirOpener.CreateIrp);

    ASSERT_EQ(STATUS_SUCCESS, dirOpener.CreateIrp.IoStatus.Status);
    ASSERT_NE(nullptr, dirOpener.FileObject.FsContext);
    EXPECT_EQ(BLORGFS_DCB_SIGNATURE, GET_NODE_TYPE(dirOpener.FileObject.FsContext))
        << "FindEntryByName's subdirectory match must produce IsDirectory=TRUE";

    CloseOpener(&dirOpener);

    //
    // Same listing, the file half -- FindEntryByName's OTHER loop
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

    PDCB parentDcb = C_CAST(PDCB, parent);
    parentDcb->CachedListing = BuildListing(L"clip.bin", L"movies");

    CreateOpener opener;
    PrepareOpener(&opener, Path(L"\\media\\ghost"), FILE_LIST_DIRECTORY, kShareAll, 0);
    BlorgCreate(Volume, &opener.CreateIrp);

    EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, opener.CreateIrp.IoStatus.Status)
        << "a leaf absent from both loops of a resolved listing must fail "
           "without falling through to a network lookup";
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
// STATUS_DEVICE_REMOVED rather than STATUS_PENDING is FsdPostRequest's
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
        << "this test needs the create to reach FsdPostRequest and be refused there";

    EXPECT_EQ(before, ShimPoolOutstanding())
        << "the joined path built for the first pass was not freed before reposting";

    CloseOpener(&parentOpener);
}

///////////////////////////////////////////////////////////////////////////
// InsertByPath -- a resident file standing where a directory is expected
///////////////////////////////////////////////////////////////////////////

//
// Only DCB carries a ChildrenList; in FCB that offset is the start of
// FILE_LOCK. So descending into a resident FILE as though it were the
// next directory does not fail cleanly -- it walks a list head made of
// whatever FsRtlInitializeFileLock left there and dereferences the
// result. SearchByPath rejects this shape; InsertByPath is the other half
// of the same walk and has to reject it too.
//
// Driven through InsertByPath directly, the way MakePublishedNode does:
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

    NTSTATUS status = InsertByPath(Root, &throughTheFile, &meta, Volume, &inserted);

    EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, status)
        << "a file cannot be an intermediate path component";
    EXPECT_EQ(nullptr, inserted);

    //
    // The rejection must leave the tree exactly as it was: the file keeps
    // its place under \media, and nothing was grafted underneath it.
    //
    UNICODE_STRING filePath = Path(L"\\media\\clip.bin");
    EXPECT_EQ(file, SearchByPath(Root, &filePath));

    UNICODE_STRING mediaPath = Path(L"\\media");
    EXPECT_EQ(dir, SearchByPath(Root, &mediaPath));
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

    EXPECT_EQ(STATUS_SUCCESS, InsertByPath(Root, &samePath, &meta, Volume, &inserted));
    EXPECT_EQ(nullptr, inserted) << "nothing new is created for a path that already resolves";
}

} // namespace
