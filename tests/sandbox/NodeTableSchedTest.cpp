//
// Exhaustive interleaving proof of the node table's pin protocol, on the
// real Structs.c.
//
// The claim, from Structs.h, that every warm open depends on:
//
//   A node handed back by BlorgNodeTableLookupPin is not freed while the
//   caller still holds that pin.
//
// This is the third attempt at that claim and the first that establishes
// it. The stress test in NodeTableTest.cpp samples interleavings, and an
// earlier version of it passed against a driver whose reap worker did not
// check PinCount at all. CBMC could not be made to check anything here:
// its concurrency encoding refuses programs with shared pointer-typed
// variables, and LIST_ENTRY.Flink is one.
//
// The scheduler runs the same real functions concretely but decides every
// context switch itself, replaying the body once per distinct schedule.
// When it reports that the space is exhausted, no interleaving of these
// threads exists in which the assertions below fail.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "Scheduler.h"
}

namespace
{

//
// Shared observable state. PinHeld is raised strictly between the lookup
// returning a node and the matching unpin -- exactly the window the
// invariant is about.
//
struct PinProof
{
    PDEVICE_OBJECT Volume;
    PDCB Root;
    PFCB Vcb;

    PCOMMON_CONTEXT Node;
    UNICODE_STRING Path;

    volatile long PinHeld;
    volatile LONG Freed;
    volatile long Violations;

    volatile long PinsObserved;
    volatile long RetiresObserved;
    volatile long LeftBehind;
    volatile long PoolAtSetup;
};

UNICODE_STRING MakePath(const wchar_t* text)
{
    UNICODE_STRING name;
    name.Buffer = const_cast<PWSTR>(text);
    name.Length = (USHORT)(wcslen(text) * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    return name;
}

//
// The warm-open path. Callers of BlorgNodeTableLookupPin go on to read the
// node's size, share access and oplock through the returned pointer, so it
// must stay valid until the pin is dropped.
//
void PinningThread(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    PCOMMON_CONTEXT found = BlorgNodeTableLookupPin(&proof->Path);

    if (!found)
    {
        return;
    }

    InterlockedIncrement(&proof->PinsObserved);
    InterlockedExchange(&proof->PinHeld, 1);

    //
    // Each check below is followed by a bail-out rather than a continue.
    // Once the node has been freed under us, going on to read it or to
    // unpin it walks a poisoned TableBucketIndex into the bucket array and
    // crashes -- which is a detection, but a crash reports "something
    // touched bad memory" and takes the whole exploration down with it,
    // losing the schedule count. Recording the violation and stopping
    // keeps the failure attributable.
    //
    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->Violations);
        InterlockedExchange(&proof->PinHeld, 0);
        return;
    }

    //
    // Read through the pin. A freed node reads back the guarded pool's
    // poison, 0xDDDDDDDD, which is negative.
    //
    if (found->PinCount <= 0)
    {
        InterlockedIncrement(&proof->Violations);
        InterlockedExchange(&proof->PinHeld, 0);
        return;
    }

    KmSchedYield();

    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->Violations);
        InterlockedExchange(&proof->PinHeld, 0);
        return;
    }

    InterlockedExchange(&proof->PinHeld, 0);

    BlorgNodeUnpin(found);
}

//
// The close path: drop the node to the reap queue and let the worker run.
// This is the shipping route -- BlorgNodeDeferReap plus NodeReapWorker,
// both real -- rather than a test-only entry point into the retire gate,
// which would have meant exporting a static and verifying a door the
// driver does not have.
//
void RetiringThread(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    BlorgNodeDeferReap(proof->Node);

    ShimDrainWorkItems();

    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->RetiresObserved);

        if (ReadNoFence(&proof->PinHeld))
        {
            InterlockedIncrement(&proof->Violations);
        }
    }
}

//
// One replay. Everything the schedule can touch is built here and torn
// down in PinProofTeardown, so every interleaving runs against the same
// starting state -- otherwise the second replay is a different program
// from the first and the search means nothing.
//
void PinProofSetup(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    proof->PinHeld = 0;
    proof->Freed = 0;

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    PCOMMON_CONTEXT node = nullptr;

    if (!NT_SUCCESS(BlorgInsertByPath(proof->Root, &proof->Path, &meta, proof->Volume, &node)) || !node)
    {
        return;
    }

    BlorgNodeTablePublish(node);

    proof->Node = node;

    ShimWatchFree(node, &proof->Freed);

    KmSchedSpawn(PinningThread, proof);
    KmSchedSpawn(RetiringThread, proof);
}

//
// A schedule in which the reap declined leaves the node published and
// alive. It has to go before the next replay, or the table accumulates one
// node per interleaving and the pool never balances.
//
void PinProofTeardown(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    ShimWatchFree(nullptr, nullptr);

    if (!proof->Node)
    {
        return;
    }

    if (!proof->Freed)
    {
        BlorgNodeDeferReap(proof->Node);
    }

    //
    // Drain unconditionally, and to empty. A schedule can finish with a
    // reap still queued -- the pinning thread's unpin defers one and
    // nothing has run the worker yet -- and the work queue is global shim
    // state, so anything left behind is inherited by the next replay.
    // That makes the next run a different program from this one, which the
    // explorer detects as replay divergence rather than as the leak it is.
    //
    while (ShimDrainWorkItems() > 0)
    {
    }

    //
    // A replay must hand the next one an empty table. A node still findable
    // here means the reap declined and the node stayed linked, so the next
    // replay's lookup walks a longer chain -- a different program, which
    // the explorer sees as replay divergence.
    //
    PCOMMON_CONTEXT stale = BlorgNodeTableLookupPin(&proof->Path);

    if (stale)
    {
        BlorgNodeUnpin(stale);
        InterlockedIncrement(&proof->LeftBehind);
    }

    proof->Node = nullptr;
}

class NodeTableSchedTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();

        Volume = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Volume);
        ASSERT_EQ(STATUS_SUCCESS, BlorgNodeTableInit(Volume));

        UNICODE_STRING rootName = MakePath(L"\\");
        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateDCB(&Root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, Volume));

        ASSERT_EQ(STATUS_SUCCESS,
            BlorgCreateFCB(&Vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, Volume, 0));

        BlorgGetVolumeDeviceExtension(Volume)->RootDcb = Root;
        BlorgGetVolumeDeviceExtension(Volume)->Vcb = Vcb;
    }

    void TearDown() override
    {
        BlorgNodeTableTeardown();

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
        BlorgFreeFileContext(Vcb, Volume);

        StructsModelDestroyVolume(Volume);

        KmAssertQuiescent("NodeTableSchedTest teardown");
    }

    PDEVICE_OBJECT Volume = nullptr;
    PDCB Root = nullptr;
    PFCB Vcb = nullptr;
};

//
// The proof. A published node with no handles and no pins is idle, and so
// eligible for retirement the moment nothing holds it -- the only state in
// which the race is reachable at all. Holding a handle reference here is
// exactly the mistake that made the earlier stress test vacuous.
//
TEST_F(NodeTableSchedTest, NoInterleavingRetiresAPinnedNode)
{
    const wchar_t* path = L"\\media\\contended.bin";

    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(path);

    //
    // Lock granularity, deliberately: threads interleave at push-lock
    // acquire/release and explicit yields, not at every interlocked op. At
    // that granularity the pin/retire arbitration is fully bounded and
    // exhausts in low thousands of schedules -- see
    // NoInterleavingRetiresAPinnedNodeAtomicSample below for why atomic
    // granularity, though strictly stronger, does not run here.
    //
    //
    // 100000, not 20000: raised 2026-08-20 when making ERESOURCE
    // cooperative under exploration (needed for the dispatch proof)
    // revealed that NodeReapWorker's real VCB-resource acquisition -- which
    // this proof's RetiringThread reaches via BlorgNodeDeferReap +
    // ShimDrainWorkItems -- had never actually been a scheduling point
    // before. It silently used the OS-blocking SRWLOCK path even under
    // exploration, worked only because it happened to be uncontended in
    // this 2-thread scenario, and so contributed zero interleavings. The
    // true lock-granularity space is larger than the 7535 first reported;
    // this is the scheduler becoming more correct; the cap follows.
    //
    // Corrected again 2026-08-24, in the other direction: 53676 down to
    // 26718. The model's ERESOURCE acquire claimed the resource on the
    // strength of its wait having returned, without re-testing, so two
    // threads could leave the wait both believing they held it
    // (NtShimSync.c). Roughly half the schedules counted here were reached
    // only through that, and the kernel cannot produce them. Nothing valid
    // was lost -- the re-test blocks a thread only when the resource is
    // genuinely held, which is what the kernel does -- so this is a smaller
    // but honest space. The cap stays where it is; it is a safety net.
    //
    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 100000);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 100000)
        << "hit the schedule cap -- the space was sampled, not exhausted, "
           "so this proves nothing stronger than the stress test does";

    //
    // Coverage, not behaviour. If the lookup never succeeded or the retire
    // never fired, every schedule was trivial and the run says nothing --
    // which is precisely how the earlier stress test passed a broken
    // driver.
    //
    EXPECT_GT(proof.PinsObserved, 0) << "no schedule ever pinned the node";
    EXPECT_GT(proof.RetiresObserved, 0) << "no schedule ever retired the node";

    EXPECT_EQ(0, proof.LeftBehind)
        << "replays left nodes in the table; the next replay is a different program";

    printf("[  sched   ] %d interleavings, max depth %d, %ld pins, %ld retires\n",
        result.Schedules, result.MaxDepth, proof.PinsObserved, proof.RetiresObserved);
}

//
// Atomic-granularity sample. Not run in the default gate.
//
// Turning on KmSchedSetAtomicYields widens the proof to interleavings
// inside BlorgNodeTableLookupPin/BlorgNodeUnpin/BlorgNodeDeferReap/
// NodeReapWorker's own interlocked operations, not just around the push
// lock -- strictly stronger than the test above. It is also strictly more
// expensive: this protocol has roughly a dozen interlocked call sites
// across the two thread bodies, and the number of DISTINCT interleavings
// of that many events grows combinatorially (binomial, not exponential in
// the branching factor, but still large enough that a first attempt at
// 200,000 schedules ran over twenty minutes without reaching either the
// cap or exhaustion).
//
// So this runs as a bounded SAMPLE -- 200 schedules, not a proof. The cap
// is small on purpose and the smallness is itself the finding: growth is
// not the roughly-linear cost a schedule count alone would suggest (200
// finishes in well under a second) -- pushing the cap to 4,000 was tried
// and did not finish in three minutes. This scheduler does no partial-
// order reduction, so depth-first enumeration keeps replaying longer and
// longer shared prefixes to reach each remaining fresh choice as the
// cheap shallow branches are exhausted first; a real POR-based tool
// (dynamic partial-order reduction) would collapse the many equivalent
// orderings of independent events instead of enumerating each one. This
// scheduler does not have that, and pretending a bigger cap would finish
// "soon" would be the same kind of overclaim the vacuous proofs earlier
// in this project turned out to be.
//
TEST_F(NodeTableSchedTest, NoInterleavingRetiresAPinnedNodeAtomicSample)
{
    const wchar_t* path = L"\\media\\contended.bin";

    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 200);

    KmSchedSetAtomicYields(0);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_GT(proof.PinsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);

    printf("[  sched   ] (atomic sample, NOT exhaustive) %d interleavings, max depth %d\n",
        result.Schedules, result.MaxDepth);
}

//
// Revival: the second way a node is lifted off zero, and the one with no
// pin anywhere in it.
//
// The proof above covers the warm path, where BlorgNodeTableLookupPin
// hands back a pinned node and the pin is what the worker checks. The cold
// path does not go through the table at all. BlorgVolumeCreate takes the
// VCB resource exclusive, finds a node with BlorgSearchByPath, and opens
// it -- and if that node is idle, the open is what takes RefCount from 0
// to 1 (Create.c, the firstOpen tests). Nothing holds a pin at any point.
//
// So a different thing has to be doing the work here, and the claim is
// that it is the VCB resource: NodeReapWorker acquires it before it
// touches any node and holds it across every free in the batch, so a
// reviver holding it exclusive cannot be racing a free. That is an
// argument about a lock the worker takes for an unrelated reason
// (batching), which makes it exactly the kind of load-bearing-by-accident
// invariant worth machine-checking rather than re-reading.
//
struct RevivalProof
{
    PDEVICE_OBJECT Volume;
    PDCB Root;
    PFCB Vcb;

    PCOMMON_CONTEXT Node;
    UNICODE_STRING Path;

    volatile long HandleHeld;
    volatile LONG Freed;
    volatile long Violations;

    volatile long RevivalsObserved;
    volatile long RevivedWhileQueued;
    volatile long RetiresObserved;
    volatile long LeftBehind;
};

//
// The cold-open path, reduced to the part that touches lifetime. Same
// bail-out-on-detection discipline as PinningThread: once the node is
// gone, reading it walks poison and takes the exploration down with it.
//
void RevivingThread(void* Parameter)
{
    RevivalProof* proof = (RevivalProof*)Parameter;

    FsRtlEnterFileSystem();
    ExAcquireResourceExclusiveLite(proof->Vcb->Header.Resource, TRUE);

    PCOMMON_CONTEXT found = BlorgSearchByPath(proof->Root, &proof->Path);

    if (!found)
    {
        ExReleaseResourceLite(proof->Vcb->Header.Resource);
        FsRtlExitFileSystem();
        return;
    }

    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->Violations);
        ExReleaseResourceLite(proof->Vcb->Header.Resource);
        FsRtlExitFileSystem();
        return;
    }

    //
    // Coverage, not behaviour: a schedule that revives a node the worker
    // has not been told about yet proves nothing about the hand-off. The
    // case this test exists for is the one where the claim is already
    // taken and the worker is on its way.
    //
    if (ReadNoFence(&found->OnReapList))
    {
        InterlockedIncrement(&proof->RevivedWhileQueued);
    }

    InterlockedIncrement64(&found->RefCount);
    InterlockedExchange(&proof->HandleHeld, 1);
    InterlockedIncrement(&proof->RevivalsObserved);

    BlorgNodeTablePublish(found);

    ExReleaseResourceLite(proof->Vcb->Header.Resource);
    FsRtlExitFileSystem();

    KmSchedYield();

    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->Violations);
        InterlockedExchange(&proof->HandleHeld, 0);
        return;
    }

    //
    // Read through the reference. A freed node reads back the guarded
    // pool's poison, which is negative.
    //
    if (found->RefCount <= 0)
    {
        InterlockedIncrement(&proof->Violations);
        InterlockedExchange(&proof->HandleHeld, 0);
        return;
    }

    InterlockedExchange(&proof->HandleHeld, 0);

    BlorgNodeDereference(found);
}

void RevivalRetiringThread(void* Parameter)
{
    RevivalProof* proof = (RevivalProof*)Parameter;

    BlorgNodeDeferReap(proof->Node);

    ShimDrainWorkItems();

    if (ReadNoFence(&proof->Freed))
    {
        InterlockedIncrement(&proof->RetiresObserved);

        if (ReadNoFence(&proof->HandleHeld))
        {
            InterlockedIncrement(&proof->Violations);
        }
    }
}

//
// Unlike the pin proof above, this one builds its own volume, root and VCB
// every replay instead of borrowing the fixture's.
//
// It has to, because it is the first node-table proof in which a spawned
// thread takes the VCB resource. An ERESOURCE carries lock identity in the
// kernel model, and a replay that ends with any residue on a resource
// shared across replays hands the next replay a different program: the
// reviver blocks at its first acquire where the recorded schedule says it
// should have proceeded, and the explorer reports that -- correctly -- as
// replay divergence rather than as the state leak it is. The pin proof
// never noticed because nothing on its pinning side touched the resource
// at all. This is the same per-replay ownership DispatchSchedTest uses,
// and for the same reason.
//
void RevivalProofSetup(void* Parameter)
{
    RevivalProof* proof = (RevivalProof*)Parameter;

    ShimReset();

    proof->HandleHeld = 0;
    proof->Freed = 0;

    proof->Volume = StructsModelCreateVolume();

    if (!proof->Volume || !NT_SUCCESS(BlorgNodeTableInit(proof->Volume)))
    {
        return;
    }

    UNICODE_STRING rootName = MakePath(L"\\");

    BlorgCreateDCB(&proof->Root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, proof->Volume);
    BlorgCreateFCB(&proof->Vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, proof->Volume, 0);

    if (!proof->Root || !proof->Vcb)
    {
        return;
    }

    BlorgGetVolumeDeviceExtension(proof->Volume)->RootDcb = proof->Root;
    BlorgGetVolumeDeviceExtension(proof->Volume)->Vcb = proof->Vcb;

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    PCOMMON_CONTEXT node = nullptr;

    if (!NT_SUCCESS(BlorgInsertByPath(proof->Root, &proof->Path, &meta, proof->Volume, &node)) || !node)
    {
        return;
    }

    BlorgNodeTablePublish(node);

    proof->Node = node;

    ShimWatchFree(node, &proof->Freed);

    KmSchedSpawn(RevivingThread, proof);
    KmSchedSpawn(RevivalRetiringThread, proof);
}

void RevivalProofTeardown(void* Parameter)
{
    RevivalProof* proof = (RevivalProof*)Parameter;

    ShimWatchFree(nullptr, nullptr);

    if (proof->Node && !proof->Freed)
    {
        BlorgNodeDeferReap(proof->Node);
    }

    while (ShimDrainWorkItems() > 0)
    {
    }

    //
    // Asked of the tree rather than of the table, deliberately. The pin
    // proof asks BlorgNodeTableLookupPin because its table outlives every
    // replay and a node left in a bucket changes the next replay's program.
    // This one builds a fresh table per replay, so the same question --
    // did every schedule end with the node actually reaped -- is the tree's
    // to answer, and asking it here avoids a lookup into a table that is
    // about to be torn down anyway.
    //
    if (proof->Root && !IsListEmpty(&proof->Root->ChildrenList))
    {
        InterlockedIncrement(&proof->LeftBehind);
    }

    BlorgNodeTableTeardown();

    //
    // A schedule that declined the reap leaves the node linked under Root,
    // and freeing Root with a child still on it would take the exploration
    // down before LeftBehind could report it.
    //
    if (proof->Root)
    {
        while (!IsListEmpty(&proof->Root->ChildrenList))
        {
            PCOMMON_CONTEXT child =
                CONTAINING_RECORD(proof->Root->ChildrenList.Flink, COMMON_CONTEXT, Links);

            BlorgFreeFileContext(child, proof->Volume);
        }

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

class NodeTableRevivalSchedTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("NodeTableRevivalSchedTest teardown");
    }
};

TEST_F(NodeTableRevivalSchedTest, NoInterleavingFreesARevivedNode)
{
    const wchar_t* path = L"\\revived.bin";

    static RevivalProof proof;

    proof = {};
    proof.Path = MakePath(path);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(RevivalProofSetup, RevivalProofTeardown, &proof, 200000);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a revived node was freed under its opener";

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 200000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    EXPECT_GT(proof.RevivalsObserved, 0) << "no schedule ever revived the node";
    EXPECT_GT(proof.RetiresObserved, 0) << "no schedule ever retired the node";

    //
    // Without this the run is vacuous in the exact way this project has
    // been caught by twice: every schedule could have revived a node the
    // reap worker had never been told about, which is not the race.
    //
    EXPECT_GT(proof.RevivedWhileQueued, 0)
        << "no schedule revived a node that was already claimed for reap";

    EXPECT_EQ(0, proof.LeftBehind)
        << "replays left nodes in the table; the next replay is a different program";

    printf("[  sched   ] %d interleavings, max depth %d, %ld revivals (%ld while queued), %ld retires\n",
        result.Schedules, result.MaxDepth, proof.RevivalsObserved,
        proof.RevivedWhileQueued, proof.RetiresObserved);
}

} // namespace
