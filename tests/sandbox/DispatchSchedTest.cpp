//
// Exhaustive interleaving proof of concurrent dispatch, through the real
// public IRP_MJ entry points -- BlorgCreate -- rather than through an
// internal function called directly.
//
// Every proof before this one in the project tested a function BELOW
// dispatch: BlorgNodeTableLookupPin, PrefetchReleaseRef, the socket
// watchdog's completion routine. This one tests the thing a real I/O
// request actually goes through first: two threads opening the SAME file
// at the same time, both landing in BlorgVolumeCreate's warm-node branch,
// both taking Fcb->Header.Resource exclusive around the composite
// RefCount-increment-plus-ApplyShareAccess mutation Create.c's own comment
// calls out as needing to be atomic.
//
// The claim under proof: no interleaving of two concurrent opens
// - corrupts Header.Resource (double release, wrong-owner release,
//   recursive acquisition -- each already a fatal model violation, so a
//   run that hits one aborts rather than reporting a count),
// - leaves ShareAccess.OpenCount or the FCB's RefCount inconsistent with
//   the number of opens that actually reported success.
//
// OpenExistingFcb and ApplyShareAccess are `static inline` in Create.c, so
// they are not reachable from a different translation unit -- which is
// exactly why this drives BlorgCreate itself rather than them: the
// alternative would be re-declaring driver internals as external, testing
// a copy of the contract rather than the contract.
//
// BLIND SPOT, FOUND BY MUTATION, NOT CLOSED. Deleting
// ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE) /
// ExReleaseResourceLite from OpenExistingFcb -- removing the lock entirely,
// the exact class of bug "concurrent dispatch synchronisation" names --
// does NOT fail this test. It runs FEWER schedules (252 vs the real
// code's 3432) and passes clean.
//
// The reason is structural, not a bug in this test to fix: this scheduler
// only creates a scheduling point at a lock acquire/release or an
// interlocked op. ApplyShareAccess's actual field mutations
// (ShareAccess->OpenCount++ and friends) are PLAIN increments, not
// Interlocked ones -- they were never scheduling points even with the lock
// present; the lock's acquire/release were the only scheduling points in
// this whole function. Delete the lock and the entire open sequence
// becomes one uninterrupted block per thread, so two threads either run it
// fully serialized or not at all as far as this scheduler can arrange --
// it cannot preempt mid-instruction the way real hardware can, because it
// only preempts where the code itself calls out a synchronization
// primitive. A missing lock removes the only hook the scheduler had.
//
// ASan does not help either: nothing here is out-of-bounds, freed, or
// double-freed -- the corruption this mutant causes is a lost update on
// live, valid memory, which is a data-race class ASan does not detect.
// And there is no TSan on Windows to fall back to (verified earlier this
// project). So this specific bug class -- a lock removed entirely, as
// opposed to a lock present but raced around it -- currently has NO
// exhaustive or sanitizer-backed detector in this toolchain. A real-thread
// stress version could catch it on some run, given enough iterations and
// luck, the same way the ORIGINAL vacuous node-table stress test could
// have caught its bug on a lucky run and did not. That is not a
// substitute for what this file's name promises.
//
// What this test DOES prove, honestly: no interleaving of the real
// BlorgCreate/BlorgCleanup/BlorgClose -- AS WRITTEN, with the
// synchronization primitives the driver actually calls -- corrupts
// Header.Resource (double release, wrong-owner release, recursive
// acquisition) or leaves the counters inconsistent GIVEN those primitives
// are present and are the only things guarding the mutation. It is a real
// result. It is not the stronger claim "no synchronization bug can exist
// here", and the mutant above is the proof that the two are different.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "Scheduler.h"
}

namespace
{

//
// Three separate IRPs sharing one FILE_OBJECT, the way real I/O traffic
// does: the kernel never reuses one IRP across IRP_MJ_CREATE, CLEANUP and
// CLOSE, so a test that did would exercise a shape no real request ever
// takes. Reusing the Create IRP for Cleanup was the first version of this
// test's own bug -- it tripped "IoCompleteRequest on an already-completed
// IRP" because the Completed flag from Create's own completion was still
// set, which is a defect in the test's IRP plumbing, not in BlorgFS.
//
struct OpenerState
{
    FILE_OBJECT FileObject;
    IO_SECURITY_CONTEXT SecurityContext;

    IO_STACK_LOCATION CreateStack;
    IRP CreateIrp;

    IO_STACK_LOCATION CleanupStack;
    IRP CleanupIrp;

    IO_STACK_LOCATION CloseStack;
    IRP CloseIrp;

    volatile long Ran;
};

struct DispatchProof
{
    PDEVICE_OBJECT Volume;
    PDCB Root;
    PFCB Vcb;
    PCOMMON_CONTEXT Node;
    UNICODE_STRING Path;
    wchar_t PathBuffer[24];

    //
    // The B: symlink's backing disk device object, normally set once by
    // CreateBlorgDiskDeviceObject at mount and read by OpenExistingFcb to
    // wire FileObject->Vpb on a successful open. Modelled here as a bare
    // DEVICE_OBJECT with a VPB, not through the real disk-device creation
    // path -- BlorgVolumeCreate never touches its contents beyond this one
    // pointer chase, so building the real DDO would add setup with nothing
    // under test.
    //
    DEVICE_OBJECT DiskDevice;
    VPB DiskVpb;

    OpenerState OpenerA;
    OpenerState OpenerB;

    volatile long Violations;
};

//
// Builds one real CREATE IRP the way the I/O manager would for
// FILE_OPEN, generous sharing, read access -- nothing about this opener
// is unusual, which is the point: the race is ordinary concurrent access
// to one file, not a crafted edge case.
//
void PrepareOpener(OpenerState* opener, DispatchProof* proof)
{
    memset(opener, 0, sizeof(*opener));

    opener->FileObject.FileName = proof->Path;
    opener->FileObject.DeviceObject = proof->Volume;

    opener->SecurityContext.DesiredAccess = FILE_READ_DATA;

    opener->CreateStack.MajorFunction = IRP_MJ_CREATE;
    opener->CreateStack.FileObject = &opener->FileObject;
    opener->CreateStack.DeviceObject = proof->Volume;
    opener->CreateStack.Parameters.Create.Options = (ULONG)FILE_OPEN << 24;
    opener->CreateStack.Parameters.Create.ShareAccess =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    opener->CreateStack.Parameters.Create.SecurityContext = &opener->SecurityContext;
    opener->CreateIrp.StackLocation = &opener->CreateStack;

    opener->CleanupStack.MajorFunction = IRP_MJ_CLEANUP;
    opener->CleanupStack.FileObject = &opener->FileObject;
    opener->CleanupStack.DeviceObject = proof->Volume;
    opener->CleanupIrp.StackLocation = &opener->CleanupStack;

    opener->CloseStack.MajorFunction = IRP_MJ_CLOSE;
    opener->CloseStack.FileObject = &opener->FileObject;
    opener->CloseStack.DeviceObject = proof->Volume;
    opener->CloseIrp.StackLocation = &opener->CloseStack;
}

void OpenerThreadA(void* Parameter)
{
    DispatchProof* proof = (DispatchProof*)Parameter;

    BlorgCreate(proof->Volume, &proof->OpenerA.CreateIrp);

    InterlockedIncrement(&proof->OpenerA.Ran);
}

void OpenerThreadB(void* Parameter)
{
    DispatchProof* proof = (DispatchProof*)Parameter;

    BlorgCreate(proof->Volume, &proof->OpenerB.CreateIrp);

    InterlockedIncrement(&proof->OpenerB.Ran);
}

void DispatchProofSetup(void* Parameter)
{
    DispatchProof* proof = (DispatchProof*)Parameter;

    ShimReset();

    proof->Volume = StructsModelCreateVolume();

    //
    // BlorgCreate's device-type switch reads this to route to
    // BlorgVolumeCreate; StructsModelCreateVolume does not set it; a test
    // that forgot this would silently exercise nothing.
    //
    global.VolumeDeviceObject = proof->Volume;

    memset(&proof->DiskDevice, 0, sizeof(proof->DiskDevice));
    memset(&proof->DiskVpb, 0, sizeof(proof->DiskVpb));
    proof->DiskDevice.Vpb = &proof->DiskVpb;
    global.DiskDeviceObject = &proof->DiskDevice;

    BlorgNodeTableInit(proof->Volume);

    UNICODE_STRING rootName;
    rootName.Buffer = const_cast<PWSTR>(L"\\");
    rootName.Length = sizeof(WCHAR);
    rootName.MaximumLength = sizeof(WCHAR);

    BlorgCreateDCB(&proof->Root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, proof->Volume);
    BlorgCreateFCB(&proof->Vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, proof->Volume, 0);

    GetVolumeDeviceExtension(proof->Volume)->RootDcb = proof->Root;
    GetVolumeDeviceExtension(proof->Volume)->Vcb = proof->Vcb;

    wcscpy_s(proof->PathBuffer, L"\\media\\contended.bin");
    proof->Path.Buffer = proof->PathBuffer;
    proof->Path.Length = (USHORT)(wcslen(proof->PathBuffer) * sizeof(wchar_t));
    proof->Path.MaximumLength = proof->Path.Length;

    //
    // Published, idle, zero handles: the file exists and is warm-findable
    // before either opener runs, which is what makes this a race between
    // two opens rather than a race with the cold-open path (already a
    // different, and already node-table-proven, concern).
    //
    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    PCOMMON_CONTEXT node = nullptr;
    InsertByPath(proof->Root, &proof->Path, &meta, proof->Volume, &node);
    BlorgNodeTablePublish(node);
    proof->Node = node;

    PrepareOpener(&proof->OpenerA, proof);
    PrepareOpener(&proof->OpenerB, proof);

    KmSchedSpawn(OpenerThreadA, proof);
    KmSchedSpawn(OpenerThreadB, proof);
}

void DispatchProofTeardown(void* Parameter)
{
    DispatchProof* proof = (DispatchProof*)Parameter;

    //
    // Both threads have already been joined by the time teardown runs, so
    // this reads IoStatus.Status and the node's counters single-threaded --
    // safe, and it is the only point in the replay where "how many opens
    // actually succeeded" and "what the node's own counters say" can be
    // compared without racing either.
    //
    const bool succeededA = NT_SUCCESS(proof->OpenerA.CreateIrp.IoStatus.Status);
    const bool succeededB = NT_SUCCESS(proof->OpenerB.CreateIrp.IoStatus.Status);
    const LONG expectedOpens = (succeededA ? 1 : 0) + (succeededB ? 1 : 0);

    if (proof->Node)
    {
        PFCB fcb = (PFCB)proof->Node;

        if (fcb->ShareAccess.OpenCount != (ULONG)expectedOpens)
        {
            InterlockedIncrement(&proof->Violations);
        }

        if (ReadNoFence64(&proof->Node->RefCount) != expectedOpens)
        {
            InterlockedIncrement(&proof->Violations);
        }

        //
        // Close whatever succeeded, through the real dispatch entries, so
        // the node returns to idle for the next replay -- BlorgCleanup
        // removes share access under Header.Resource, BlorgClose drops
        // the node-table reference the node-table proof already covers.
        //
        if (succeededA)
        {
            BlorgCleanup(proof->Volume, &proof->OpenerA.CleanupIrp);
            BlorgClose(proof->Volume, &proof->OpenerA.CloseIrp);
        }

        if (succeededB)
        {
            BlorgCleanup(proof->Volume, &proof->OpenerB.CleanupIrp);
            BlorgClose(proof->Volume, &proof->OpenerB.CloseIrp);
        }

        ShimDrainWorkItems();
    }

    //
    // Root DCB, VCB and the volume device object itself are rebuilt fresh
    // every replay in Setup, so they must be torn down here or their
    // resources' lock identities leak one per replay -- exactly the
    // "more than 2048 locks modelled" the model reports, correctly, when
    // this was missing. BlorgFreeFileContext is what calls
    // ExDeleteResourceLite, which is what returns the id.
    //
    BlorgNodeTableTeardown();

    if (proof->Root)
    {
        BlorgFreeFileContext(proof->Root, proof->Volume);
        proof->Root = nullptr;
    }

    if (proof->Vcb)
    {
        BlorgFreeFileContext(proof->Vcb, proof->Volume);
        proof->Vcb = nullptr;
    }

    if (proof->Volume)
    {
        StructsModelDestroyVolume(proof->Volume);
        proof->Volume = nullptr;
    }

    proof->Node = nullptr;
}

class DispatchSchedTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("DispatchSchedTest teardown");
    }
};

TEST_F(DispatchSchedTest, NoInterleavingOfConcurrentOpensCorruptsShareState)
{
    static DispatchProof proof;

    proof = {};

    KM_SCHED_RESULT result =
        KmExploreInterleavings(DispatchProofSetup, DispatchProofTeardown, &proof, 20000);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving left ShareAccess/RefCount inconsistent with "
           "the opens that actually succeeded";

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 20000)
        << "hit the schedule cap -- sampled, not exhausted";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

} // namespace
