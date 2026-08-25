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
// Corrected a third time, 2026-08-24, upward: 26718 to 41330 here,
// 55890 to 149769 for the revival proof. The claim-carrying wait API
// (Scheduler.h) moved the acquire's claim from after the wait's final
// yield to before it, under the baton. One consequence is that a
// contender meeting a just-claimed lock now genuinely BLOCKS, and every
// block and resume is a recorded scheduling point -- where the old shape
// let it sail through the still-unclaimed window without waiting. The
// larger space is the honest cost of mutual exclusion made visible;
// both proofs remain exhaustive under their caps with zero divergence,
// deadlock and violation, and SchedulerAudit pins the underlying
// invariants directly.
    //
    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 100000);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

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
    KmSchedSetRaceDetection(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 200);

    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

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

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

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

//
// Atomic-granularity soaks. DISABLED_, so they are run deliberately
// (--gtest_also_run_disabled_tests) and never by the gate: these take
// tens of minutes, where the lock-granularity proofs above take seconds.
//
// What they add over those proofs is scheduling points at every
// interlocked operation, not just around locks. That is strictly stronger
// -- it is the granularity at which a protocol's own counters can be
// observed mid-update -- and for the revival path it is the granularity
// that matters, because the thing being revived is an InterlockedIncrement64
// on a counter the worker reads to decide whether to free.
//
// They are SAMPLES unless the schedule count comes back under the cap.
// This scheduler does no partial-order reduction, so depth-first
// enumeration replays ever-longer shared prefixes as the cheap shallow
// branches are exhausted, and the atomic space for a two-thread body is
// large. Read the printed count before calling either of these a proof.
//
// Scheduler.h records that atomic granularity on the node-table proof
// reported replay divergence at depth 17, and that it had not been tracked
// down. These runs are what re-tests that claim now that the model's
// ERESOURCE no longer hands two threads the same exclusive hold.
//
TEST_F(NodeTableSchedTest, DISABLED_AtomicPinSoak)
{
    const wchar_t* path = L"\\media\\contended.bin";

    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 2000000);

    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, proof.LeftBehind) << "replays left nodes in the table";

    EXPECT_GT(proof.PinsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);

    printf("[  sched   ] atomic pin soak: %d interleavings, max depth %d, "
           "%ld pins, %ld retires, exhausted=%s\n",
        result.Schedules, result.MaxDepth, proof.PinsObserved, proof.RetiresObserved,
        (result.Schedules < 2000000) ? "YES" : "NO (sampled)");
}

//

// Positive control for the happens-before race detector. Two threads

// increment a plain long with no lock and no interlocked op, registering

// the accesses manually; the detector must flag the overlap. Without

// this test, silence elsewhere proves nothing -- a detector that never

// fires is indistinguishable from one that never works.

//

namespace {



struct RaceControlProof

{

    long Value;

};



void RacyWriter(void* Parameter)

{

    RaceControlProof* proof = (RaceControlProof*)Parameter;



    for (int i = 0; i < 3; ++i)

    {

        KmSchedNoteAccess(&proof->Value, 1);

        proof->Value++;

        KmSchedYield();

    }

}



void RaceControlSetup(void* Parameter)

{

    KmSchedSpawn(RacyWriter, Parameter);

    KmSchedSpawn(RacyWriter, Parameter);

}



void RaceControlTeardown(void* Parameter)

{

    (void)Parameter;

}



}  // namespace



TEST(SchedulerAudit, RaceDetectorFlagsUnsynchronizedAccess)

{

    RaceControlProof proof = {};

    proof.Value = 0;



    KmSchedSetRaceDetection(1);
    KmExpectViolation(KmViolationLifetime);

    KmExploreInterleavings(RaceControlSetup, RaceControlTeardown, &proof, 20);

    KmSchedSetRaceDetection(0);



    EXPECT_GT(KmSchedRaceCount(), 0)

        << "the race detector never fired on a deliberately racy body";
    EXPECT_EQ(KmViolationLifetime, KmTakeViolation())
        << "the detector fired but the model did not record the violation";

}



//
// A random sample through the atomic-granularity space of the pin body.
// The DISABLED_ soak below enumerates that space; this one exists for the
// gate, where a few seconds of breadth catches gross granularity
// regressions -- a shim atomic silently ceasing to be a scheduling
// point, say -- without paying for enumeration. Seeded, so a failure
// reproduces exactly; on a hit, raise the count and re-run before
// believing the seed was lucky.
//
TEST_F(NodeTableSchedTest, RandomAtomicPinSmoke)
{
    const wchar_t* path = L"\\media\\contended.bin";

    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavingsSeeded(PinProofSetup, PinProofTeardown, &proof, 50000, 0x5DEECE66u);

    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Violations)
        << "a sampled interleaving retired a pinned node";

    //
    // ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so
    // any assertion after this one would run against corrupted state.
    //
    ASSERT_EQ(0, result.Deadlocks) << "a sampled schedule deadlocked;";
    EXPECT_EQ((long)0, KmSchedRaceCount())
        << "the race detector fired on the pin body";
    EXPECT_EQ(0, result.Truncated) << "a sampled schedule hit the depth cap";

    EXPECT_GT(proof.PinsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);

    printf("[  sched   ] atomic pin random smoke: %d interleavings, max depth %d\n",
        result.Schedules, result.MaxDepth);
}

TEST_F(NodeTableRevivalSchedTest, DISABLED_AtomicRevivalSoak)
{
    const wchar_t* path = L"\\revived.bin";

    static RevivalProof proof;

    proof = {};
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(RevivalProofSetup, RevivalProofTeardown, &proof, 2000000);

    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a revived node was freed under its opener";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, proof.LeftBehind) << "replays left nodes linked under the root";

    EXPECT_GT(proof.RevivalsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);
    EXPECT_GT(proof.RevivedWhileQueued, 0)
        << "no schedule revived a node that was already claimed for reap";

    printf("[  sched   ] atomic revival soak: %d interleavings, max depth %d, "
           "%ld revivals (%ld while queued), %ld retires, exhausted=%s\n",
        result.Schedules, result.MaxDepth, proof.RevivalsObserved,
        proof.RevivedWhileQueued, proof.RetiresObserved,
        (result.Schedules < 2000000) ? "YES" : "NO (sampled)");
}

///////////////////////////////////////////////////////////////////////////
// Scheduler audit repros. Two minimal bodies that isolate defects in the
// scheduler/shim machinery itself, away from the node table whose proofs
// normally exercise it. Each is small enough to explore exhaustively in
// well under a second, so a failure is attributable to a specific
// interleaving rather than to a soak sample.
//
// Both defects below were proven against the code as it stood; both are
// predicate test and THEN yields the baton once more before returning, so
// a caller that claimed without re-testing -- KmAcquireLock was the only
// one -- has a scheduling point sitting between its last check and its
// claim. Two threads could both pass the check while the lock was free and
// both claim afterwards. This is the same promotion-is-not-running class
// as the ERESOURCE defect, surviving in the one primitive whose caller
// was not given an outer re-test loop.
//
// Repro 2 defends unwind liveness: when a schedule deadlocked, every parked
// thread was woken with Current == -1 and KmSchedWaitUntil returned early.
// A shim whose acquire looped on the predicate re-tested, found it false,
// and waited again -- forever, because nothing would ever run. RunOnce's
// five-second join expires, the handle is closed on a live thread, and
// the zombie keeps driving scheduler state into the next replay. The
// assertion is wall-clock: a correct unwind lets eight all-deadlock
// replays finish in milliseconds; the defect costs at least five seconds
// per deadlocked schedule and usually diverges outright.
///////////////////////////////////////////////////////////////////////////

struct SpinGrantAudit
{
    KM_LOCK Lock;
    volatile long Holders;
    volatile long MaxHolders;
    volatile long DoubleGrants;
};

void SpinGrantThread(void* Parameter)
{
    SpinGrantAudit* audit = (SpinGrantAudit*)Parameter;

    unsigned char oldIrql = KmAcquireLock(&audit->Lock);

    long now = InterlockedIncrement(&audit->Holders);

    long seen = ReadNoFence(&audit->MaxHolders);

    while (now > seen &&
           InterlockedCompareExchange(&audit->MaxHolders, now, seen) != seen)
    {
        seen = ReadNoFence(&audit->MaxHolders);
    }

    //
    // Bail without releasing once the invariant is broken, in EITHER
    // direction. A second claim overwrites ExclusiveOwner, so the FIRST
    // holder's release would fail the model's owner check and abort the
    // process -- which reports "something aborted" rather than "two
    // threads held the lock". Recording here and refusing every further
    // release keeps the failure attributable to the acquisition itself.
    //
    if (ReadNoFence(&audit->MaxHolders) > 1)
    {
        InterlockedIncrement(&audit->DoubleGrants);
        return;
    }

    KmSchedYield();

    InterlockedDecrement(&audit->Holders);

    if (audit->Lock.OwnerThread != KmSchedThreadId())
    {
        //
        // Our grant was stolen while we yielded: the concurrent-holders
        // count above caught it, and releasing now would be the model's
        // abort, not ours to make.
        //
        InterlockedIncrement(&audit->DoubleGrants);
        return;
    }

    KmReleaseLock(&audit->Lock, oldIrql);
}

void SpinGrantSetup(void* Parameter)
{
    SpinGrantAudit* audit = (SpinGrantAudit*)Parameter;

    audit->Holders = 0;
    KmInitializeLock(&audit->Lock, "spin-grant-audit");

    KmSchedSpawn(SpinGrantThread, audit);
    KmSchedSpawn(SpinGrantThread, audit);
}

TEST(SchedulerAudit, NoInterleavingDoubleGrantsTheSpinLock)
{
    static SpinGrantAudit audit;

    audit.MaxHolders = 0;
    audit.DoubleGrants = 0;

    KmExploreInterleavings(SpinGrantSetup, nullptr, &audit, 20000);

    EXPECT_EQ(0, ReadNoFence(&audit.DoubleGrants))
        << "an interleaving exists in which two threads held the spin lock "
           "-- claim-after-yield TOCTOU in KmAcquireLock";
}

struct UnwindAudit
{
    ERESOURCE Resource;
};

void UnwindHolderAndVanish(void* Parameter)
{
    UnwindAudit* audit = (UnwindAudit*)Parameter;

    ExAcquireResourceExclusiveLite(&audit->Resource, TRUE);

    //
    // Deliberately never releases. The resource ends up exclusively held
    // by a thread that has finished -- the exact "held by nobody" state
    // the original double-grant left behind, and the shape the soak
    // reported ("thread 1 waiting=eresource exclusive", partner Done).
    //
}

void UnwindWaiter(void* Parameter)
{
    UnwindAudit* audit = (UnwindAudit*)Parameter;

    //
    // Unreachable in any correct world: the only possible owner has
    // finished, so the predicate can never hold again.
    //
    ExAcquireResourceExclusiveLite(&audit->Resource, TRUE);
}

void UnwindSetup(void* Parameter)
{
    UnwindAudit* audit = (UnwindAudit*)Parameter;

    ExInitializeResourceLite(&audit->Resource);

    KmSchedSpawn(UnwindHolderAndVanish, audit);
    KmSchedSpawn(UnwindWaiter, audit);
}

TEST(SchedulerAudit, DeadlockedScheduleUnwindsPromptly)
{
    static UnwindAudit audit;

    ULONGLONG started = GetTickCount64();

    KM_SCHED_RESULT result =
        KmExploreInterleavings(UnwindSetup, nullptr, &audit, 8);

    ULONGLONG elapsedMs = GetTickCount64() - started;

    EXPECT_GT(result.Deadlocks, 0)
        << "the engineered deadlock never happened -- the repro proves nothing";

    EXPECT_LT(elapsedMs, 3000)
        << "eight all-deadlock replays took " << elapsedMs << "ms: a waiter "
           "spins between block and unwind-wake instead of exiting, the join "
           "times out, and the abandoned thread corrupts later replays";

    printf("[  sched   ] unwind liveness: %d schedules, %d deadlocks, %llu ms\n",
        result.Schedules, result.Deadlocks, elapsedMs);
}

///////////////////////////////////////////////////////////////////////////
// Regression: a wait issued from SETUP or TEARDOWN -- on the exploring
// fiber itself -- used to spin forever when its predicate could not hold,
// because HandOff cannot park the explorer and no worker runs between two
// predicate tests. A replay that ends holding state (the
// UnwindHolderAndVanish shape above) plus a teardown that touches that
// state hung the whole suite silently instead of reporting. The scheduler
// now reports a violation and grants under the baton, exactly as the
// abandoned drain does. This test pins BOTH halves: the run must finish
// promptly AND the violation must be recorded. On the unfixed scheduler
// this test does not fail -- it hangs, which is the defect.
///////////////////////////////////////////////////////////////////////////

struct LeakAudit
{
    volatile long Grant;
};

static int LeakFreePredicate(void* Context)
{
    return 0 == ReadNoFence(&((LeakAudit*)Context)->Grant);
}

static void LeakTakeClaim(void* Context)
{
    InterlockedExchange(&((LeakAudit*)Context)->Grant, 1);
}

static void LeakHolderAndVanish(void* Parameter)
{
    LeakAudit* audit = (LeakAudit*)Parameter;

    KmSchedWaitUntilClaim(LeakFreePredicate, audit, LeakTakeClaim, audit,
        "leak-audit grant");

    //
    // Deliberately never released: the replay ends cleanly -- every thread
    // Done, nobody Blocked, so no deadlock is detected and Abandoned stays
    // clear -- with the grant still held.
    //
}

static void LeakSetup(void* Parameter)
{
    LeakAudit* audit = (LeakAudit*)Parameter;

    audit->Grant = 0;

    KmSchedSpawn(LeakHolderAndVanish, audit);
}

static void LeakWaitingTeardown(void* Parameter)
{
    LeakAudit* audit = (LeakAudit*)Parameter;

    KmSchedWaitUntilClaim(LeakFreePredicate, audit, LeakTakeClaim, audit,
        "leak-audit teardown");
}

TEST(SchedulerAudit, TeardownWaitOnAReplayLeftHoldFailsLoudly)
{
    static LeakAudit audit;

    ULONGLONG started = GetTickCount64();

    KmExpectViolation(KmViolationLifetime);

    //
    // One schedule is the point: exactly one teardown wait, so exactly one
    // expected violation is consumed. More replays would fire the guard a
    // second time with the expectation already taken, aborting the process.
    //
    KmExploreInterleavings(LeakSetup, LeakWaitingTeardown, &audit, 1);

    ULONGLONG elapsedMs = GetTickCount64() - started;

    EXPECT_LT(elapsedMs, 3000)
        << "a teardown wait on state a replay left held took " << elapsedMs
        << "ms: the explorer is spinning on a predicate nothing can satisfy";

    EXPECT_EQ(KmViolationLifetime, KmTakeViolation())
        << "the unsatisfiable teardown wait neither failed loudly nor "
           "completed -- replay-left-behind state went unreported";
}

///////////////////////////////////////////////////////////////////////////
// Regression: an unmatched SHARED release used to decrement SchedState
// blind, driving it negative. Every later sharable waiter then blocked
// against a count no release would ever bring back to zero -- a spurious
// deadlock attributed to the driver when the bug is the unmatched release.
// Both primitives now reject the release at the offending call.
///////////////////////////////////////////////////////////////////////////

static EX_PUSH_LOCK UnmatchedPushLock;
static ERESOURCE UnmatchedResource;

static void UnmatchedPushReleaseSetup(void* Parameter)
{
    (void)Parameter;

    //
    // Runs on the exploring fiber with the exploration active, so this
    // takes the modelled counter path rather than the SRWLOCK path.
    //
    ExReleasePushLockShared(&UnmatchedPushLock);
}

TEST(SchedulerAudit, UnmatchedSharedPushLockReleaseIsRejected)
{
    ExInitializePushLock(&UnmatchedPushLock);

    KmExpectViolation(KmViolationLockOwner);

    KmExploreInterleavings(UnmatchedPushReleaseSetup, nullptr, nullptr, 1);

    EXPECT_EQ(KmViolationLockOwner, KmTakeViolation())
        << "releasing a push lock shared without holding it went unreported";

    EXPECT_EQ(0, UnmatchedPushLock.SchedState)
        << "the rejected release still drove the shared count off zero";
}

static void UnmatchedResourceReleaseSetup(void* Parameter)
{
    (void)Parameter;

    ExReleaseResourceLite(&UnmatchedResource);
}

TEST(SchedulerAudit, UnmatchedSharedResourceReleaseIsRejected)
{
    ASSERT_EQ(STATUS_SUCCESS, ExInitializeResourceLite(&UnmatchedResource));

    KmExpectViolation(KmViolationLockOwner);

    KmExploreInterleavings(UnmatchedResourceReleaseSetup, nullptr, nullptr, 1);

    EXPECT_EQ(KmViolationLockOwner, KmTakeViolation())
        << "releasing a resource without holding it went unreported";

    EXPECT_EQ(0, UnmatchedResource.SchedState)
        << "the rejected release still drove the shared count off zero";
}

///////////////////////////////////////////////////////////////////////////
// Regression: KmAcquireLockShared took its CRITICAL_SECTION even under
// systematic exploration. One reader yielding while holding it parked the
// host thread on the second reader's enter -- an instant, silent hang.
// The shared path now goes through the same claim-under-the-baton
// machinery as every other primitive, so readers genuinely interleave and
// genuinely share. This pins both: the exploration must exhaust (no hang,
// no deadlock) and some schedule must observe both readers inside at once
// -- which the old serialising CS path could never produce even when it
// did not hang.
///////////////////////////////////////////////////////////////////////////

struct SharedLockAudit
{
    KM_LOCK Lock;
    volatile long Holders;
    volatile long MaxHolders;
};

static void SharedReader(void* Parameter)
{
    SharedLockAudit* audit = (SharedLockAudit*)Parameter;

    KmAcquireLockShared(&audit->Lock);

    long now = InterlockedIncrement(&audit->Holders);

    long seen = ReadNoFence(&audit->MaxHolders);

    while (now > seen &&
           InterlockedCompareExchange(&audit->MaxHolders, now, seen) != seen)
    {
        seen = ReadNoFence(&audit->MaxHolders);
    }

    //
    // Still holding across this yield. Under the old CRITICAL_SECTION
    // path this is where the second reader's acquire wedged the host
    // thread; under the cooperative path it is a scheduling point the
    // explorer can use to run the second reader into the lock.
    //
    KmSchedYield();

    InterlockedDecrement(&audit->Holders);

    KmReleaseLockShared(&audit->Lock);
}

static void SharedReaderSetup(void* Parameter)
{
    SharedLockAudit* audit = (SharedLockAudit*)Parameter;

    audit->Holders = 0;
    KmInitializeLock(&audit->Lock, "shared-reader-audit");

    KmSchedSpawn(SharedReader, audit);
    KmSchedSpawn(SharedReader, audit);
}

TEST(SchedulerAudit, SharedAcquiresInterleaveCooperatively)
{
    static SharedLockAudit audit;

    audit.MaxHolders = 0;

    KM_SCHED_RESULT result =
        KmExploreInterleavings(SharedReaderSetup, nullptr, &audit, 20000);

    EXPECT_EQ(0, result.Deadlocks) << "a shared-only workload deadlocked";
    EXPECT_EQ(0, result.Truncated) << "the shared-reader space hit the depth cap";
    EXPECT_LT(result.Schedules, 20000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    EXPECT_EQ(2, ReadNoFence(&audit.MaxHolders))
        << "no schedule ever held the lock by both readers at once -- the "
           "cooperative shared path is serialising, not sharing";
}

///////////////////////////////////////////////////////////////////////////
// Regression: replay divergence was checked against the runnable COUNT
// alone. Two replays whose per-thread states differ at a recorded depth
// pass that check whenever the runnable sets happen to agree in size --
// and they also pass a runnable-SET check when the difference is between
// Done and Blocked, because a finished thread appears in neither set.
// The explorer now compares every thread's full scheduler state at each
// recorded depth.
//
// The body below crosses, on every second replay, which of two movers
// opens which of two gates. What differs between variants is WHICH worker
// each promotion wakes -- the same leak shape as a state leak that
// redirects which thread proceeds. Some schedules make that difference
// count-visible (a worker that proceeds in one variant and blocks in the
// other changes the next point's runnable count), and the count-only
// check catches those; but many divergences here are equal-count
// identity swaps -- the diagnostics show "2 runnable, state 0x0024,
// expected 0x0021" -- which only the full state-vector comparison sees.
// Alternating the pairing per replay guarantees some replay always walks
// a prefix recorded under the opposite pairing, whichever subtree the
// depth-first search is in.
///////////////////////////////////////////////////////////////////////////

struct DivergenceAudit
{
    volatile long GateForZero;
    volatile long GateForOne;
    int Crossed;
};

static int GateForZeroIsOpen(void* Context)
{
    return 1 == ReadNoFence(&((DivergenceAudit*)Context)->GateForZero);
}

static int GateForOneIsOpen(void* Context)
{
    return 1 == ReadNoFence(&((DivergenceAudit*)Context)->GateForOne);
}

static void GrantAndProceed(void* Context)
{
    (void)Context;
}

static void GatedWorkerZero(void* Parameter)
{
    DivergenceAudit* audit = (DivergenceAudit*)Parameter;

    KmSchedWaitUntilClaim(GateForZeroIsOpen, audit, GrantAndProceed, audit,
        "divergence gate zero");
}

static void GatedWorkerOne(void* Parameter)
{
    DivergenceAudit* audit = (DivergenceAudit*)Parameter;

    KmSchedWaitUntilClaim(GateForOneIsOpen, audit, GrantAndProceed, audit,
        "divergence gate one");
}

static void MoverZero(void* Parameter)
{
    DivergenceAudit* audit = (DivergenceAudit*)Parameter;

    //
    // Straight pairing opens this mover's own gate; crossed pairing opens
    // the other worker's. Either way exactly one gate opens here, so the
    // runnable COUNT after this point is identical across variants -- only
    // the identity of the promoted worker differs.
    //
    volatile long* gate = audit->Crossed ? &audit->GateForOne : &audit->GateForZero;

    InterlockedExchange(gate, 1);
}

static void MoverOne(void* Parameter)
{
    DivergenceAudit* audit = (DivergenceAudit*)Parameter;

    volatile long* gate = audit->Crossed ? &audit->GateForZero : &audit->GateForOne;

    InterlockedExchange(gate, 1);
}

static int DivergenceReplays = 0;

static void DivergenceSetup(void* Parameter)
{
    DivergenceAudit* audit = (DivergenceAudit*)Parameter;

    ++DivergenceReplays;

    audit->Crossed = (0 != (DivergenceReplays % 2));
    audit->GateForZero = 0;
    audit->GateForOne = 0;

    KmSchedSpawn(GatedWorkerZero, audit);
    KmSchedSpawn(GatedWorkerOne, audit);
    KmSchedSpawn(MoverZero, audit);
    KmSchedSpawn(MoverOne, audit);
}

TEST(SchedulerAudit, ReplayDivergenceInThreadStateIsCaught)
{
    DivergenceAudit audit = {};
    DivergenceReplays = 0;

    KmExpectViolation(KmViolationLifetime);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(DivergenceSetup, nullptr, &audit, 5000);

    EXPECT_EQ(KmViolationLifetime, KmTakeViolation())
        << "a body whose per-thread states changed across replays -- equal "
           "runnable counts by construction -- went undetected: replay "
           "isolation is not fully checked";

    EXPECT_GT(result.Schedules, 1)
        << "the diverging replay never ran -- the repro proves nothing";
    EXPECT_LT(result.Schedules, 5000)
        << "hit the schedule cap before exhausting the space";
}

} // namespace
