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

    if (!NT_SUCCESS(InsertByPath(proof->Root, &proof->Path, &meta, proof->Volume, &node)) || !node)
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

        GetVolumeDeviceExtension(Volume)->RootDcb = Root;
        GetVolumeDeviceExtension(Volume)->Vcb = Vcb;
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

} // namespace
