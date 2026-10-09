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

#include <algorithm>

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

    //
    // The counters as Setup found them, so Teardown can tell what this one
    // run did.
    //
    long PinsBefore;
    long RetiresBefore;
    long ViolationsBefore;
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
// The same exploration with partial-order reduction on. On a space the
// full search exhausts, both must report the same set of outcomes.
//
static KM_SCHED_RESULT ExploreReduced(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context, int MaxSchedules)
{
    KmSchedSetReduction(1);

    KM_SCHED_RESULT result = KmExploreInterleavings(Setup, Teardown, Context, MaxSchedules);

    KmSchedSetReduction(0);

    return result;
}

//
// The shard of the full search this process runs, from KM_SCHED_SHARD
// ("index/count"), which verify.yml sets to split the long proofs between
// runners. Unset or empty, the whole search runs. Returns the shard count, or 0 for
// a value that does not parse.
//
static int ShardFromEnvironment()
{
    const char* shard = getenv("KM_SCHED_SHARD");
    int index = 0;
    int count = 1;

    if (shard && *shard &&
        (2 != sscanf(shard, "%d/%d", &index, &count) || count < 1 || index < 0 || index >= count))
    {
        return 0;
    }

    KmSchedSetShard(index, count);

    return count;
}

//
// The reduction's gate on real driver code. Every outcome the full search
// reached, or this shard of it, must be one the reduced search reaches.
// The converse holds by construction, since each reduced run is also a run
// of the full search, so an unsharded search must match it exactly.
//
static void ExpectReductionReachesFullOutcomes(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context,
    const KM_SCHED_RESULT& Full, int Shards)
{
    static unsigned __int64 full[65536];
    static unsigned __int64 reduced[65536];

    const int fullCount = KmSchedCopyOutcomes(full, 65536);
    const KM_SCHED_RESULT result = ExploreReduced(Setup, Teardown, Context, 1000000);
    const int reducedCount = KmSchedCopyOutcomes(reduced, 65536);

    EXPECT_EQ(0, result.Deadlocks);
    EXPECT_LT(result.Schedules, 1000000) << "the reduced search hit its schedule cap";

    std::sort(reduced, reduced + reducedCount);

    int missing = 0;

    for (int i = 0; i < fullCount; ++i)
    {
        if (!std::binary_search(reduced, reduced + reducedCount, full[i]))
        {
            missing++;
        }
    }

    EXPECT_EQ(0, missing) << "the full search reached outcomes the reduced search did not";

    if (1 == Shards)
    {
        EXPECT_EQ(Full.Outcomes, result.Outcomes)
            << "the reduced search reached a different number of outcomes";
        EXPECT_EQ(Full.OutcomeDigest, result.OutcomeDigest)
            << "the reduced search reached a different set of outcomes";
    }

    printf("[  sched   ] reduced: %d runs, %d pruned, %d outcomes; full: %d outcomes\n",
        result.Schedules, result.Pruned, result.Outcomes, Full.Outcomes);
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
    KmSchedNoteFootprint(&found->PinCount, sizeof(found->PinCount), 0);

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
    proof->PinsBefore = proof->PinsObserved;
    proof->RetiresBefore = proof->RetiresObserved;
    proof->ViolationsBefore = proof->Violations;

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
// What one run ended in: which of the two threads got through, whether the
// node was freed, and, when it was not, the counts it was left with.
//
unsigned __int64 PinOutcome(const PinProof* proof)
{
    unsigned __int64 outcome =
        (unsigned __int64)(proof->PinsObserved - proof->PinsBefore) |
        ((unsigned __int64)(proof->RetiresObserved - proof->RetiresBefore) << 2) |
        ((unsigned __int64)(proof->Violations - proof->ViolationsBefore) << 4) |
        ((unsigned __int64)(proof->Freed != 0) << 6) |
        ((unsigned __int64)(proof->PinHeld != 0) << 7);

    if (!proof->Freed)
    {
        outcome |= ((unsigned __int64)(proof->Node->PinCount & 0xFF) << 8) |
            ((unsigned __int64)(proof->Node->RefCount & 0xFF) << 16) |
            ((unsigned __int64)(proof->Node->OnReapList != 0) << 24);
    }

    return outcome;
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

    KmSchedNoteOutcome(PinOutcome(proof));

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

//
// The synchronous retire. A create that fails part-way walks back up its
// path freeing each directory it left empty (BlorgReapEmptyAncestorDcbs),
// under the VCB resource exclusive and without the reap queue, so what
// keeps it off a pinned directory is NodeTableTryRetire's own count check
// rather than the worker's.
//
void AncestorRetiringThread(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    FsRtlEnterFileSystem();
    ExAcquireResourceExclusiveLite(proof->Vcb->Header.Resource, TRUE);

    BlorgReapEmptyAncestorDcbs(C_CAST(PDCB, proof->Node), proof->Volume);

    ExReleaseResourceLite(proof->Vcb->Header.Resource);
    FsRtlExitFileSystem();

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
// PinProofSetup with an empty directory in place of the file, which is the
// only kind of node the ancestor walk retires.
//
void DirectoryPinProofSetup(void* Parameter)
{
    PinProof* proof = (PinProof*)Parameter;

    proof->PinHeld = 0;
    proof->Freed = 0;
    proof->PinsBefore = proof->PinsObserved;
    proof->RetiresBefore = proof->RetiresObserved;
    proof->ViolationsBefore = proof->Violations;

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.IsDirectory = TRUE;

    PCOMMON_CONTEXT node = nullptr;

    if (!NT_SUCCESS(BlorgInsertByPath(proof->Root, &proof->Path, &meta, proof->Volume, &node)) || !node)
    {
        return;
    }

    BlorgNodeTablePublish(node);

    proof->Node = node;

    ShimWatchFree(node, &proof->Freed);

    KmSchedSpawn(PinningThread, proof);
    KmSchedSpawn(AncestorRetiringThread, proof);
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
    // acquire/release and explicit yields, not at every interlocked op.
    // This is the space the full search can still exhaust, which makes it
    // the space the partial-order reduction is checked against below.
    // NoAtomicInterleavingRetiresAPinnedNode covers atomic granularity.
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
// Corrected a fourth time, 2026-10-08: 41330 to 4,531,882 here,
// 149769 to 25,847,776 for the revival proof. Both spaces had outgrown
// their caps, and a run that hit the cap printed a note and passed. With
// the lock-id release made proportional to its own edges the full spaces
// finish (82 s and about 25 min in Release), so the caps leave headroom
// and hitting one is a failure again. verify.yml splits both between
// runners with KM_SCHED_SHARD; the caps are per shard.
    //
    const int shards = ShardFromEnvironment();
    ASSERT_NE(0, shards) << "KM_SCHED_SHARD is not index/count";

    KM_SCHED_RESULT result =
        KmExploreInterleavings(PinProofSetup, PinProofTeardown, &proof, 10000000);

    KmSchedSetShard(0, 1);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned node was retired";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 10000000)
        << "hit the schedule cap -- the space was sampled, not exhausted, "
           "so this proves nothing stronger than the stress test does";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);

    ExpectReductionReachesFullOutcomes(PinProofSetup, PinProofTeardown, &proof, result, shards);

    EXPECT_EQ(0, proof.Violations);

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

    long RevivalsBefore;
    long QueuedBefore;
    long RetiresBefore;
    long ViolationsBefore;
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
    KmSchedNoteFootprint(&found->RefCount, sizeof(found->RefCount), 0);

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
    proof->RevivalsBefore = proof->RevivalsObserved;
    proof->QueuedBefore = proof->RevivedWhileQueued;
    proof->RetiresBefore = proof->RetiresObserved;
    proof->ViolationsBefore = proof->Violations;

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

unsigned __int64 RevivalOutcome(const RevivalProof* proof)
{
    unsigned __int64 outcome =
        (unsigned __int64)(proof->RevivalsObserved - proof->RevivalsBefore) |
        ((unsigned __int64)(proof->RevivedWhileQueued - proof->QueuedBefore) << 2) |
        ((unsigned __int64)(proof->RetiresObserved - proof->RetiresBefore) << 4) |
        ((unsigned __int64)(proof->Violations - proof->ViolationsBefore) << 6) |
        ((unsigned __int64)(proof->Freed != 0) << 8) |
        ((unsigned __int64)(proof->HandleHeld != 0) << 9);

    if (!proof->Freed)
    {
        outcome |= ((unsigned __int64)(proof->Node->PinCount & 0xFF) << 16) |
            ((unsigned __int64)(proof->Node->RefCount & 0xFF) << 24) |
            ((unsigned __int64)(proof->Node->OnReapList != 0) << 32);
    }

    return outcome;
}

void RevivalProofTeardown(void* Parameter)
{
    RevivalProof* proof = (RevivalProof*)Parameter;

    ShimWatchFree(nullptr, nullptr);

    if (proof->Node)
    {
        KmSchedNoteOutcome(RevivalOutcome(proof));
    }

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

    const int shards = ShardFromEnvironment();
    ASSERT_NE(0, shards) << "KM_SCHED_SHARD is not index/count";

    KM_SCHED_RESULT result =
        KmExploreInterleavings(RevivalProofSetup, RevivalProofTeardown, &proof, 50000000);

    KmSchedSetShard(0, 1);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a revived node was freed under its opener";

    //
// ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
// assertion after this one would run against corrupted state.
//
ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 50000000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);

    ExpectReductionReachesFullOutcomes(RevivalProofSetup, RevivalProofTeardown, &proof, result, shards);

    EXPECT_EQ(0, proof.Violations);

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

}

//
// Atomic-granularity proofs.
//
// What they add over those proofs is scheduling points at every
// interlocked operation, not just around locks. That is strictly stronger
// -- it is the granularity at which a protocol's own counters can be
// observed mid-update -- and for the revival path it is the granularity
// that matters, because the thing being revived is an InterlockedIncrement64
// on a counter the worker reads to decide whether to free.
//
// The full atomic space does not finish: the pin body ran 140 million
// schedules in an hour without exhausting it. These run with
// partial-order reduction, which explores one order of each run of
// independent steps and exhausts both spaces in a few thousand runs. The
// lock-granularity proofs above are where that reduction is checked
// against the full search.
//
// Scheduler.h records that atomic granularity on the node-table proof
// reported replay divergence at depth 17, and that it had not been tracked
// down. These runs are what re-tests that claim now that the model's
// ERESOURCE no longer hands two threads the same exclusive hold.
//
TEST_F(NodeTableSchedTest, NoAtomicInterleavingRetiresAPinnedNode)
{
    const wchar_t* path = L"\\media\\contended.bin";

    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);
    KmSchedSetWeakMemory(1);

    KM_SCHED_RESULT result =
        ExploreReduced(PinProofSetup, PinProofTeardown, &proof, 1000000);

    KmSchedSetWeakMemory(0);
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
    EXPECT_EQ((long)0, KmSchedRaceCount()) << "the race detector fired on the pin body";
    EXPECT_EQ(0, result.Truncated) << "a schedule hit the depth cap";
    EXPECT_LT(result.Schedules, 1000000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    EXPECT_GT(proof.PinsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);

    printf("[  sched   ] atomic pin proof: %d runs, %d pruned, max depth %d, "
           "%ld pins, %ld retires\n",
        result.Schedules, result.Pruned, result.MaxDepth,
        proof.PinsObserved, proof.RetiresObserved);
}

//
// The pin proof against the synchronous retire path instead of the reap
// worker: a directory pinned by a warm open while a failed create's
// ancestor walk tries to free it. Lock granularity with the full search,
// checked against the reduction, then atomic granularity reduced.
//
TEST_F(NodeTableSchedTest, NoInterleavingRetiresAPinnedDirectory)
{
    PinProof proof = {};
    proof.Volume = Volume;
    proof.Root = Root;
    proof.Vcb = Vcb;
    proof.Path = MakePath(L"\\media");

    KM_SCHED_RESULT result =
        KmExploreInterleavings(DirectoryPinProofSetup, PinProofTeardown, &proof, 1000000);

    EXPECT_EQ(0, proof.Violations)
        << "an interleaving exists in which a pinned directory was retired";

    //
    // ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
    // assertion after this one would run against corrupted state.
    //
    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ(0, result.Truncated) << "a schedule hit the depth cap";
    EXPECT_LT(result.Schedules, 1000000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);

    ExpectReductionReachesFullOutcomes(DirectoryPinProofSetup, PinProofTeardown, &proof, result, 1);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);
    KmSchedSetWeakMemory(1);

    const KM_SCHED_RESULT atomic =
        ExploreReduced(DirectoryPinProofSetup, PinProofTeardown, &proof, 1000000);

    KmSchedSetWeakMemory(0);
    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Violations)
        << "an atomic interleaving exists in which a pinned directory was retired";
    ASSERT_EQ(0, atomic.Deadlocks) << "an atomic schedule deadlocked;";
    EXPECT_EQ((long)0, KmSchedRaceCount()) << "the race detector fired on the directory body";
    EXPECT_EQ(0, atomic.Truncated) << "an atomic schedule hit the depth cap";
    EXPECT_LT(atomic.Schedules, 1000000)
        << "hit the schedule cap -- the atomic space was sampled, not exhausted";

    EXPECT_GT(proof.PinsObserved, 0) << "no schedule ever pinned the directory";
    EXPECT_GT(proof.RetiresObserved, 0) << "no schedule ever retired the directory";
    EXPECT_EQ(0, proof.LeftBehind)
        << "replays left nodes in the table; the next replay is a different program";

    printf("[  sched   ] atomic: %d runs, %d pruned, max depth %d, %ld pins, %ld retires\n",
        atomic.Schedules, atomic.Pruned, atomic.MaxDepth, proof.PinsObserved, proof.RetiresObserved);
}

//
// The last unpin and the last close race to notice the node is idle. Each
// drops its own count and reads the other's, under the bucket lock shared,
// which orders neither against the other: the shape of store buffering.
// What keeps both from reading the other's count from before its drop is
// the full barrier of the interlocked drop, and nothing else. Lose it and
// in some execution neither defers the node, which then stays idle and
// published with nothing left to reap it. Atomic granularity under weak
// memory, the full search checked against the reduction.
//
struct DropProof
{
    PDEVICE_OBJECT Volume;
    PCOMMON_CONTEXT Node;
    UNICODE_STRING Path;

    volatile LONG Freed;
    long Stranded;
    long Reaped;
};

void UnpinningThread(void* Parameter)
{
    BlorgNodeUnpin(((DropProof*)Parameter)->Node);
}

void ClosingThread(void* Parameter)
{
    BlorgNodeDereference(((DropProof*)Parameter)->Node);
}

//
// A node opened once and pinned once, both through the driver.
//
void DropProofSetup(void* Parameter)
{
    DropProof* proof = (DropProof*)Parameter;

    proof->Freed = 0;
    proof->Node = nullptr;

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    PDCB root = BlorgGetVolumeDeviceExtension(proof->Volume)->RootDcb;
    PCOMMON_CONTEXT node = nullptr;

    if (!NT_SUCCESS(BlorgInsertByPath(root, &proof->Path, &meta, proof->Volume, &node)) || !node)
    {
        return;
    }

    BlorgNodeTablePublish(node);

    if (node != BlorgNodeTableLookupPin(&proof->Path))
    {
        return;
    }

    InterlockedIncrement64(&node->RefCount);

    proof->Node = node;

    ShimWatchFree(node, &proof->Freed);

    KmSchedSpawn(UnpinningThread, proof);
    KmSchedSpawn(ClosingThread, proof);
}

//
// Runs the reap worker for whatever the two queued. A node it did not
// free was never deferred: count it, and reap it here so the next replay
// starts from an empty table.
//
void DropProofTeardown(void* Parameter)
{
    DropProof* proof = (DropProof*)Parameter;

    if (!proof->Node)
    {
        ShimWatchFree(nullptr, nullptr);
        return;
    }

    while (ShimDrainWorkItems() > 0)
    {
    }

    const bool freed = 0 != proof->Freed;

    ShimWatchFree(nullptr, nullptr);
    KmSchedNoteOutcome(freed);

    if (freed)
    {
        proof->Reaped++;
    }
    else
    {
        proof->Stranded++;

        BlorgNodeDeferReap(proof->Node);

        while (ShimDrainWorkItems() > 0)
        {
        }
    }

    proof->Node = nullptr;
}

TEST_F(NodeTableSchedTest, NoInterleavingStrandsAnIdleNode)
{
    DropProof proof = {};
    proof.Volume = Volume;
    proof.Path = MakePath(L"\\dropped.bin");

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);
    KmSchedSetWeakMemory(1);

    const KM_SCHED_RESULT result =
        KmExploreInterleavings(DropProofSetup, DropProofTeardown, &proof, 1000000);

    ExpectReductionReachesFullOutcomes(DropProofSetup, DropProofTeardown, &proof, result, 1);

    KmSchedSetWeakMemory(0);
    KmSchedSetAtomicYields(0);
    KmSchedSetRaceDetection(0);

    EXPECT_EQ(0, proof.Stranded)
        << "an execution left the node idle and unreaped: each dropper read the other's "
           "count from before its drop";

    //
    // ASSERT, not EXPECT: a deadlocked schedule abandons its replay, so any
    // assertion after this one would run against corrupted state.
    //
    ASSERT_EQ(0, result.Deadlocks) << "a schedule deadlocked;";
    EXPECT_EQ((long)0, KmSchedRaceCount()) << "the race detector fired on the drop body";
    EXPECT_EQ(0, result.Truncated) << "a schedule hit the depth cap";
    EXPECT_LT(result.Schedules, 1000000)
        << "hit the schedule cap -- the space was sampled, not exhausted";
    EXPECT_GT(proof.Reaped, 0) << "no execution reaped the node";

    printf("[  sched   ] drop proof: %d runs, max depth %d\n", result.Schedules, result.MaxDepth);
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
// NoAtomicInterleavingRetiresAPinnedNode enumerates that space with the
// partial-order reduction on; this one samples it with the reduction off,
// and a few seconds of breadth catches gross granularity regressions -- a shim atomic silently ceasing to be a scheduling
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

TEST_F(NodeTableRevivalSchedTest, NoAtomicInterleavingFreesARevivedNode)
{
    const wchar_t* path = L"\\revived.bin";

    static RevivalProof proof;

    proof = {};
    proof.Path = MakePath(path);

    KmSchedSetAtomicYields(1);
    KmSchedSetRaceDetection(1);
    KmSchedSetWeakMemory(1);

    KM_SCHED_RESULT result =
        ExploreReduced(RevivalProofSetup, RevivalProofTeardown, &proof, 1000000);

    KmSchedSetWeakMemory(0);
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
    EXPECT_EQ((long)0, KmSchedRaceCount()) << "the race detector fired on the revival body";
    EXPECT_EQ(0, result.Truncated) << "a schedule hit the depth cap";
    EXPECT_LT(result.Schedules, 1000000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    EXPECT_GT(proof.RevivalsObserved, 0);
    EXPECT_GT(proof.RetiresObserved, 0);
    EXPECT_GT(proof.RevivedWhileQueued, 0)
        << "no schedule revived a node that was already claimed for reap";

    printf("[  sched   ] atomic revival proof: %d runs, %d pruned, max depth %d, "
           "%ld revivals (%ld while queued), %ld retires\n",
        result.Schedules, result.Pruned, result.MaxDepth, proof.RevivalsObserved,
        proof.RevivedWhileQueued, proof.RetiresObserved);
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

//
// One injected work-item failure and two threads allocating: which one
// fails depends on the order, so the failure's consumption is a shared
// access. Unreported, the reduced search treats the two allocations as
// independent and reports a sleeping step that changed under it.
//
struct WorkItemFailureAudit
{
    PIO_WORKITEM Items[2];
};

void WorkItemFailureAllocate(void* Parameter)
{
    PIO_WORKITEM* item = (PIO_WORKITEM*)Parameter;

    *item = IoAllocateWorkItem(nullptr);
}

void WorkItemFailureSetup(void* Parameter)
{
    WorkItemFailureAudit* audit = (WorkItemFailureAudit*)Parameter;

    audit->Items[0] = nullptr;
    audit->Items[1] = nullptr;
    ShimFailNextWorkItem();

    KmSchedSpawn(WorkItemFailureAllocate, &audit->Items[0]);
    KmSchedSpawn(WorkItemFailureAllocate, &audit->Items[1]);
}

void WorkItemFailureTeardown(void* Parameter)
{
    WorkItemFailureAudit* audit = (WorkItemFailureAudit*)Parameter;

    KmSchedNoteOutcome((nullptr == audit->Items[0]) | ((nullptr == audit->Items[1]) << 1));

    IoFreeWorkItem(audit->Items[0]);
    IoFreeWorkItem(audit->Items[1]);
}

TEST(SchedulerAudit, AnInjectedWorkItemFailureOrdersTheAllocators)
{
    static WorkItemFailureAudit audit;

    KM_SCHED_RESULT full =
        KmExploreInterleavings(WorkItemFailureSetup, WorkItemFailureTeardown, &audit, 1000);
    KM_SCHED_RESULT reduced =
        ExploreReduced(WorkItemFailureSetup, WorkItemFailureTeardown, &audit, 1000);

    EXPECT_EQ(2, full.Outcomes) << "each allocator should be the one that fails in some order";
    EXPECT_EQ(full.Outcomes, reduced.Outcomes);
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

///////////////////////////////////////////////////////////////////////////
// Under atomic yields, every unlocked shared access is a scheduling point:
// the interlocked operations, and the ReadNoFence family the driver uses
// to read what they write.
//
// Two droppers each decrement their own counter and read the other's --
// the shape of BlorgNodeUnpin racing BlorgNodeDereference over a node's
// PinCount and RefCount. When the read is not a scheduling point, each
// drop runs together with its read and the schedule in which both see the
// other at zero is never explored. When an operation is not a scheduling
// point, a two-thread body using only that operation has exactly the two
// schedules that pick which thread starts.
///////////////////////////////////////////////////////////////////////////

struct UnlockedReadAudit
{
    volatile long Left;
    volatile long Right;
    volatile long SawLeft;
    volatile long SawRight;
    volatile long BothSawZero;
};

static void LeftDropper(void* Parameter)
{
    UnlockedReadAudit* audit = (UnlockedReadAudit*)Parameter;

    InterlockedDecrement(&audit->Left);
    audit->SawRight = ReadNoFence(&audit->Right);
}

static void RightDropper(void* Parameter)
{
    UnlockedReadAudit* audit = (UnlockedReadAudit*)Parameter;

    InterlockedDecrement(&audit->Right);
    audit->SawLeft = ReadNoFence(&audit->Left);
}

static void UnlockedReadSetup(void* Parameter)
{
    UnlockedReadAudit* audit = (UnlockedReadAudit*)Parameter;

    audit->Left = 1;
    audit->Right = 1;
    audit->SawLeft = -1;
    audit->SawRight = -1;

    KmSchedSpawn(LeftDropper, audit);
    KmSchedSpawn(RightDropper, audit);
}

static void UnlockedReadTeardown(void* Parameter)
{
    UnlockedReadAudit* audit = (UnlockedReadAudit*)Parameter;

    if (0 == audit->SawLeft && 0 == audit->SawRight)
    {
        audit->BothSawZero++;
    }
}

TEST(SchedulerAudit, UnlockedReadsAreSchedulingPointsUnderAtomicYields)
{
    static UnlockedReadAudit audit;

    audit = {};

    KmSchedSetAtomicYields(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(UnlockedReadSetup, UnlockedReadTeardown, &audit, 20000);

    KmSchedSetAtomicYields(0);

    EXPECT_LT(result.Schedules, 20000)
        << "hit the schedule cap -- the space was sampled, not exhausted";

    EXPECT_GT(audit.BothSawZero, 0)
        << "no schedule had both drops before both reads -- ReadNoFence is "
           "not a scheduling point";
}

enum class SharedAccess
{
    Or,
    Add64,
    ExchangeAdd64,
    CompareExchangePointer,
    ReadNoFence,
    ReadNoFence64,
    ReadAcquire,
    ReadAcquire64,
    ReadPointerAcquire,
    WriteRelease,
    WriteRelease64,
};

struct SharedAccessAudit
{
    SharedAccess Access;
    volatile long Value;
    volatile LONG64 Value64;
    PVOID volatile Pointer;
};

static void SharedAccessThread(void* Parameter)
{
    SharedAccessAudit* audit = (SharedAccessAudit*)Parameter;

    switch (audit->Access)
    {
    case SharedAccess::Or:
        InterlockedOr(&audit->Value, 1);
        break;
    case SharedAccess::Add64:
        InterlockedAdd64(&audit->Value64, 1);
        break;
    case SharedAccess::ExchangeAdd64:
        InterlockedExchangeAdd64(&audit->Value64, 1);
        break;
    case SharedAccess::CompareExchangePointer:
        InterlockedCompareExchangePointer(&audit->Pointer, audit, nullptr);
        break;
    case SharedAccess::ReadNoFence:
        (void)ReadNoFence(&audit->Value);
        break;
    case SharedAccess::ReadNoFence64:
        (void)ReadNoFence64(&audit->Value64);
        break;
    case SharedAccess::ReadAcquire:
        (void)ReadAcquire(&audit->Value);
        break;
    case SharedAccess::ReadAcquire64:
        (void)ReadAcquire64(&audit->Value64);
        break;
    case SharedAccess::ReadPointerAcquire:
        (void)ReadPointerAcquire(&audit->Pointer);
        break;
    case SharedAccess::WriteRelease:
        WriteRelease(&audit->Value, 1);
        break;
    case SharedAccess::WriteRelease64:
        WriteRelease64(&audit->Value64, 1);
        break;
    }
}

static void SharedAccessSetup(void* Parameter)
{
    SharedAccessAudit* audit = (SharedAccessAudit*)Parameter;

    audit->Value = 0;
    audit->Value64 = 0;
    audit->Pointer = nullptr;

    KmSchedSpawn(SharedAccessThread, audit);
    KmSchedSpawn(SharedAccessThread, audit);
}

TEST(SchedulerAudit, EveryUnlockedSharedAccessIsASchedulingPoint)
{
    static SharedAccessAudit audit;

    const SharedAccess accesses[] = {
        SharedAccess::Or,
        SharedAccess::Add64,
        SharedAccess::ExchangeAdd64,
        SharedAccess::CompareExchangePointer,
        SharedAccess::ReadNoFence,
        SharedAccess::ReadNoFence64,
        SharedAccess::ReadAcquire,
        SharedAccess::ReadAcquire64,
        SharedAccess::ReadPointerAcquire,
        SharedAccess::WriteRelease,
        SharedAccess::WriteRelease64,
    };

    KmSchedSetAtomicYields(1);

    for (SharedAccess access : accesses)
    {
        audit.Access = access;

        KM_SCHED_RESULT result =
            KmExploreInterleavings(SharedAccessSetup, nullptr, &audit, 100);

        EXPECT_GT(result.Schedules, 2)
            << "access " << (int)access << " is not a scheduling point";
    }

    KmSchedSetAtomicYields(0);
}

///////////////////////////////////////////////////////////////////////////
// A queued writer holds back new readers. In the kernel, an exclusive
// acquirer waiting on a shared-held push lock makes every later shared
// acquire wait too, so a reader that re-takes the lock shared while a
// writer is queued deadlocks against it. An ERESOURCE grants the re-take,
// because the reader already owns the resource. The model has to make
// both calls the way the kernel does, or that whole class of deadlock is
// invisible to every proof.
///////////////////////////////////////////////////////////////////////////

struct QueuedWriterAudit
{
    EX_PUSH_LOCK PushLock;
    ERESOURCE Resource;
    int UseResource;
};

static void RecursiveReader(void* Parameter)
{
    QueuedWriterAudit* audit = (QueuedWriterAudit*)Parameter;

    if (audit->UseResource)
    {
        ExAcquireResourceSharedLite(&audit->Resource, TRUE);
        KmSchedYield();
        ExAcquireResourceSharedLite(&audit->Resource, TRUE);
        ExReleaseResourceLite(&audit->Resource);
        ExReleaseResourceLite(&audit->Resource);
        return;
    }

    ExAcquirePushLockShared(&audit->PushLock);
    KmSchedYield();
    ExAcquirePushLockShared(&audit->PushLock);
    ExReleasePushLockShared(&audit->PushLock);
    ExReleasePushLockShared(&audit->PushLock);
}

static void QueuedWriter(void* Parameter)
{
    QueuedWriterAudit* audit = (QueuedWriterAudit*)Parameter;

    if (audit->UseResource)
    {
        ExAcquireResourceExclusiveLite(&audit->Resource, TRUE);
        ExReleaseResourceLite(&audit->Resource);
        return;
    }

    ExAcquirePushLockExclusive(&audit->PushLock);
    ExReleasePushLockExclusive(&audit->PushLock);
}

static void QueuedWriterSetup(void* Parameter)
{
    QueuedWriterAudit* audit = (QueuedWriterAudit*)Parameter;

    ExInitializePushLock(&audit->PushLock);
    ExInitializeResourceLite(&audit->Resource);

    KmSchedSpawn(RecursiveReader, audit);
    KmSchedSpawn(QueuedWriter, audit);
}

TEST(SchedulerAudit, QueuedWriterHoldsBackANewPushLockReader)
{
    static QueuedWriterAudit audit;

    audit.UseResource = 0;

    KM_SCHED_RESULT result =
        KmExploreInterleavings(QueuedWriterSetup, nullptr, &audit, 1000);

    EXPECT_GT(result.Deadlocks, 0)
        << "a shared re-take overtook a queued writer -- the kernel blocks it";
}

TEST(SchedulerAudit, QueuedWriterDoesNotHoldBackAnEresourceOwner)
{
    static QueuedWriterAudit audit;

    audit.UseResource = 1;

    KM_SCHED_RESULT result =
        KmExploreInterleavings(QueuedWriterSetup, nullptr, &audit, 1000);

    EXPECT_EQ(0, result.Deadlocks)
        << "a shared re-take by an owner waited behind a queued writer";
    EXPECT_LT(result.Schedules, 1000)
        << "hit the schedule cap -- the space was sampled, not exhausted";
}

///////////////////////////////////////////////////////////////////////////
// An acquire that may not wait fails when the resource is unavailable,
// and the driver posts the request instead. The model used to block
// regardless, so only the waiting path was ever explored.
///////////////////////////////////////////////////////////////////////////

struct NoWaitAudit
{
    ERESOURCE Resource;
    volatile long Refused;
    volatile long Granted;
};

static void NoWaitHolder(void* Parameter)
{
    NoWaitAudit* audit = (NoWaitAudit*)Parameter;

    ExAcquireResourceExclusiveLite(&audit->Resource, TRUE);
    KmSchedYield();
    ExReleaseResourceLite(&audit->Resource);
}

static void NoWaitTrier(void* Parameter)
{
    NoWaitAudit* audit = (NoWaitAudit*)Parameter;

    if (!ExAcquireResourceSharedLite(&audit->Resource, FALSE))
    {
        audit->Refused++;
        return;
    }

    audit->Granted++;
    ExReleaseResourceLite(&audit->Resource);
}

static void NoWaitSetup(void* Parameter)
{
    NoWaitAudit* audit = (NoWaitAudit*)Parameter;

    ExInitializeResourceLite(&audit->Resource);

    KmSchedSpawn(NoWaitHolder, audit);
    KmSchedSpawn(NoWaitTrier, audit);
}

TEST(SchedulerAudit, AcquireThatMayNotWaitFailsWhileHeld)
{
    static NoWaitAudit audit;

    audit.Refused = 0;
    audit.Granted = 0;

    KM_SCHED_RESULT result =
        KmExploreInterleavings(NoWaitSetup, nullptr, &audit, 1000);

    EXPECT_EQ(0, result.Deadlocks);
    EXPECT_LT(result.Schedules, 1000)
        << "hit the schedule cap -- the space was sampled, not exhausted";
    EXPECT_GT(audit.Refused, 0) << "no schedule refused the acquire while it was held";
    EXPECT_GT(audit.Granted, 0) << "no schedule granted the acquire while it was free";
}

///////////////////////////////////////////////////////////////////////////
// Partial-order reduction. A reduced exploration skips orders of steps
// that touch nothing in common, so it must reach exactly the outcomes the
// full one does -- the same final states, the same deadlocks -- in fewer
// schedules. Each body here is explored both ways and the outcome sets
// compared; the bodies are the shapes a reduction gets wrong when its
// footprints are: a lost update through unlocked accesses, a deadlock
// that needs one particular order, and steps that are truly independent.
///////////////////////////////////////////////////////////////////////////

struct LostUpdateAudit
{
    volatile long Counter;
};

static void UnlockedIncrementer(void* Parameter)
{
    LostUpdateAudit* audit = (LostUpdateAudit*)Parameter;

    const long seen = ReadNoFence(&audit->Counter);
    WriteRelease(&audit->Counter, seen + 1);
}

static void LostUpdateSetup(void* Parameter)
{
    LostUpdateAudit* audit = (LostUpdateAudit*)Parameter;

    audit->Counter = 0;

    KmSchedSpawn(UnlockedIncrementer, audit);
    KmSchedSpawn(UnlockedIncrementer, audit);
    KmSchedSpawn(UnlockedIncrementer, audit);
}

static void LostUpdateTeardown(void* Parameter)
{
    KmSchedNoteOutcome((unsigned __int64)((LostUpdateAudit*)Parameter)->Counter);
}

TEST(SchedulerAudit, ReductionReachesEveryOutcomeOfALostUpdate)
{
    static LostUpdateAudit audit;

    KmSchedSetAtomicYields(1);

    const KM_SCHED_RESULT full =
        KmExploreInterleavings(LostUpdateSetup, LostUpdateTeardown, &audit, 100000);
    const KM_SCHED_RESULT reduced =
        ExploreReduced(LostUpdateSetup, LostUpdateTeardown, &audit, 100000);

    KmSchedSetAtomicYields(0);

    ASSERT_LT(full.Schedules, 100000) << "the full space was sampled, not exhausted";
    EXPECT_EQ(3, full.Outcomes) << "three incrementers end on 1, 2 or 3";
    EXPECT_EQ(full.Outcomes, reduced.Outcomes);
    EXPECT_EQ(full.OutcomeDigest, reduced.OutcomeDigest)
        << "the reduced exploration reached a different set of outcomes";
    EXPECT_LT(reduced.Schedules, full.Schedules);
}

//
// Sharding: the shards' runs together must be the unsharded search's, no
// run lost and none counted twice. Two threads of four unlocked
// increments run deeper than the shard depth, so runs are dealt out by
// their prefix rather than one by one.
//
static void RepeatedIncrementer(void* Parameter)
{
    for (int i = 0; i < 4; ++i)
    {
        UnlockedIncrementer(Parameter);
    }
}

static void RepeatedIncrementSetup(void* Parameter)
{
    LostUpdateAudit* audit = (LostUpdateAudit*)Parameter;

    audit->Counter = 0;

    KmSchedSpawn(RepeatedIncrementer, audit);
    KmSchedSpawn(RepeatedIncrementer, audit);
}

TEST(SchedulerAudit, ShardsTogetherRunTheWholeSearch)
{
    static LostUpdateAudit audit;
    static unsigned __int64 whole[64];
    static unsigned __int64 combined[64];

    KmSchedSetAtomicYields(1);

    const KM_SCHED_RESULT full =
        KmExploreInterleavings(RepeatedIncrementSetup, LostUpdateTeardown, &audit, 100000);
    const int wholeCount = KmSchedCopyOutcomes(whole, 64);

    int schedules = 0;
    int unionCount = 0;

    for (int shard = 0; shard < 3; ++shard)
    {
        KmSchedSetShard(shard, 3);

        const KM_SCHED_RESULT part =
            KmExploreInterleavings(RepeatedIncrementSetup, LostUpdateTeardown, &audit, 100000);

        EXPECT_GT(part.Schedules, 0) << "shard " << shard << " ran nothing";
        schedules += part.Schedules;

        unsigned __int64 outcomes[64];
        const int count = KmSchedCopyOutcomes(outcomes, 64);

        for (int i = 0; i < count; ++i)
        {
            if (std::find(combined, combined + unionCount, outcomes[i]) == combined + unionCount)
            {
                combined[unionCount++] = outcomes[i];
            }
        }
    }

    KmSchedSetShard(0, 1);
    KmSchedSetAtomicYields(0);

    ASSERT_LT(full.Schedules, 100000) << "the full space was sampled, not exhausted";
    EXPECT_GT(full.MaxDepth, 12) << "no run reached the shard depth";
    EXPECT_EQ(full.Schedules, schedules) << "the shards ran a different number of schedules";

    std::sort(whole, whole + wholeCount);
    std::sort(combined, combined + unionCount);

    EXPECT_TRUE(std::equal(whole, whole + wholeCount, combined, combined + unionCount))
        << "the shards reached a different set of outcomes";
}

///////////////////////////////////////////////////////////////////////////
// Weak memory. The classic litmus shapes, each explored with and without
// the reduction, across the orderings: what relaxed accesses may return,
// and what release, acquire, an acquire or release read-modify-write, a
// full fence, or the coherence of one location rule out. Outcome counts
// pin the allowed sets: each shape has exactly one outcome that only an
// unordered pair reaches.
///////////////////////////////////////////////////////////////////////////

struct LitmusAudit
{
    KM_SCHED_BODY Left;
    KM_SCHED_BODY Right;
    volatile long X;
    volatile long Y;
    long Seen0;
    long Seen1;
};

static void LitmusSetup(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->X = 0;
    audit->Y = 0;
    audit->Seen0 = 0;
    audit->Seen1 = 0;

    KmSchedSpawn(audit->Left, audit);
    KmSchedSpawn(audit->Right, audit);
}

static void LitmusTeardown(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    KmSchedNoteOutcome((unsigned __int64)(audit->Seen0 | (audit->Seen1 << 4)));
}

static int LitmusOutcomes(KM_SCHED_BODY Left, KM_SCHED_BODY Right, int Weak)
{
    static LitmusAudit audit;

    audit.Left = Left;
    audit.Right = Right;

    KmSchedSetAtomicYields(1);
    KmSchedSetWeakMemory(Weak);

    const KM_SCHED_RESULT full = KmExploreInterleavings(LitmusSetup, LitmusTeardown, &audit, 100000);
    const KM_SCHED_RESULT reduced = ExploreReduced(LitmusSetup, LitmusTeardown, &audit, 100000);

    KmSchedSetWeakMemory(0);
    KmSchedSetAtomicYields(0);

    EXPECT_LT(full.Schedules, 100000) << "the full space was sampled, not exhausted";
    EXPECT_EQ(full.Outcomes, reduced.Outcomes);
    EXPECT_EQ(full.OutcomeDigest, reduced.OutcomeDigest)
        << "the reduced exploration reached a different set of outcomes";

    return full.Outcomes;
}

//
// Store buffering: each thread writes one location and reads the other.
// Only a full fence between the two keeps both reads from missing.
//
static void StoreReleaseLoadY(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteRelease(&audit->X, 1);
    audit->Seen0 = ReadNoFence(&audit->Y);
}

static void StoreReleaseLoadX(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteRelease(&audit->Y, 1);
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryLetsBothStoresBeBuffered)
{
    EXPECT_EQ(3, LitmusOutcomes(StoreReleaseLoadY, StoreReleaseLoadX, 0))
        << "sequential consistency has one thread see the other's store";
    EXPECT_EQ(4, LitmusOutcomes(StoreReleaseLoadY, StoreReleaseLoadX, 1))
        << "neither thread saw the other's store in no schedule";
}

static void ExchangeLoadY(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    InterlockedExchange(&audit->X, 1);
    audit->Seen0 = ReadNoFence(&audit->Y);
}

static void ExchangeLoadX(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    InterlockedExchange(&audit->Y, 1);
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryKeepsInterlockedStoresOrdered)
{
    EXPECT_EQ(3, LitmusOutcomes(ExchangeLoadY, ExchangeLoadX, 1))
        << "the full fences of two interlocked operations let both loads miss";
}

static void ExchangeAcquireLoadY(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    InterlockedExchangeAcquire(&audit->X, 1);
    audit->Seen0 = ReadNoFence(&audit->Y);
}

static void ExchangeAcquireLoadX(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    InterlockedExchangeAcquire(&audit->Y, 1);
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryLetsAcquireExchangesBeBuffered)
{
    EXPECT_EQ(4, LitmusOutcomes(ExchangeAcquireLoadY, ExchangeAcquireLoadX, 1))
        << "an acquire exchange ordered the load after it as a full fence would";
}

static void StoreFenceLoadY(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    KeMemoryBarrier();
    audit->Seen0 = ReadNoFence(&audit->Y);
}

static void StoreFenceLoadX(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->Y, 1);
    KeMemoryBarrier();
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryKeepsFencedStoresOrdered)
{
    EXPECT_EQ(3, LitmusOutcomes(StoreFenceLoadY, StoreFenceLoadX, 1))
        << "two full fences let both loads miss";
}

//
// Message passing: one thread writes data then a flag, the other reads
// the flag then the data. Seeing the flag without the data needs the
// writer or the reader to leave its pair unordered.
//
static void PublishDataThenFlag(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteRelease(&audit->X, 1);
    WriteRelease(&audit->Y, 1);
}

static void StoreDataThenFlag(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    WriteNoFence(&audit->Y, 1);
}

static void StoreDataFenceFlag(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    KeMemoryBarrier();
    WriteNoFence(&audit->Y, 1);
}

static void StoreDataReleaseFlag(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    InterlockedIncrementRelease(&audit->Y);
}

static void StoreDataUnfencedFlag(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    InterlockedIncrementNoFence(&audit->Y);
}

static void ReadFlagThenData(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = ReadNoFence(&audit->Y);
    audit->Seen1 = ReadNoFence(&audit->X);
}

static void AcquireFlagThenData(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = ReadAcquire(&audit->Y);
    audit->Seen1 = ReadNoFence(&audit->X);
}

static void ReadFlagFenceData(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = ReadNoFence(&audit->Y);
    KeMemoryBarrier();
    audit->Seen1 = ReadNoFence(&audit->X);
}

static void OrAcquireFlagThenData(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = InterlockedOrAcquire(&audit->Y, 0);
    audit->Seen1 = ReadNoFence(&audit->X);
}

static void OrUnfencedFlagThenData(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = InterlockedOrNoFence(&audit->Y, 0);
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryLetsAnUnfencedReaderSeeTheFlagBeforeTheData)
{
    EXPECT_EQ(4, LitmusOutcomes(PublishDataThenFlag, ReadFlagThenData, 1))
        << "no schedule read the flag set and the data unset";
    EXPECT_EQ(4, LitmusOutcomes(PublishDataThenFlag, OrUnfencedFlagThenData, 1))
        << "an unfenced read-modify-write of the flag ordered the data read after it";
}

TEST(SchedulerAudit, WeakMemoryLetsAnUnfencedWriterPublishTheFlagBeforeTheData)
{
    EXPECT_EQ(4, LitmusOutcomes(StoreDataThenFlag, AcquireFlagThenData, 1))
        << "an unfenced flag write released the data written before it";
    EXPECT_EQ(4, LitmusOutcomes(StoreDataUnfencedFlag, AcquireFlagThenData, 1))
        << "an unfenced read-modify-write of the flag released the data written before it";
}

TEST(SchedulerAudit, WeakMemoryKeepsAnAcquiringReaderAfterTheData)
{
    EXPECT_EQ(3, LitmusOutcomes(PublishDataThenFlag, AcquireFlagThenData, 1))
        << "an acquire that read the flag still missed the data released before it";
    EXPECT_EQ(3, LitmusOutcomes(PublishDataThenFlag, OrAcquireFlagThenData, 1))
        << "an acquire read-modify-write of the flag still missed the data";
    EXPECT_EQ(3, LitmusOutcomes(PublishDataThenFlag, ReadFlagFenceData, 1))
        << "a full fence after reading the flag still missed the data";
}

TEST(SchedulerAudit, WeakMemoryKeepsAReleasingWriterBeforeTheFlag)
{
    EXPECT_EQ(3, LitmusOutcomes(StoreDataFenceFlag, AcquireFlagThenData, 1))
        << "a full fence before writing the flag did not release the data";
    EXPECT_EQ(3, LitmusOutcomes(StoreDataReleaseFlag, AcquireFlagThenData, 1))
        << "a release read-modify-write of the flag did not release the data";
}

static void StoreOneThenTwo(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    WriteNoFence(&audit->X, 1);
    WriteNoFence(&audit->X, 2);
}

static void ReadTwice(void* Parameter)
{
    LitmusAudit* audit = (LitmusAudit*)Parameter;

    audit->Seen0 = ReadNoFence(&audit->X);
    audit->Seen1 = ReadNoFence(&audit->X);
}

TEST(SchedulerAudit, WeakMemoryNeverReadsALocationBackwards)
{
    EXPECT_EQ(6, LitmusOutcomes(StoreOneThenTwo, ReadTwice, 1))
        << "two reads of one location saw its writes out of order, or missed an order";
}

TEST(SchedulerAudit, ReductionStillFindsADeadlockThatNeedsOneOrder)
{
    static QueuedWriterAudit audit;

    audit.UseResource = 0;

    const KM_SCHED_RESULT full =
        KmExploreInterleavings(QueuedWriterSetup, nullptr, &audit, 1000);
    const KM_SCHED_RESULT reduced =
        ExploreReduced(QueuedWriterSetup, nullptr, &audit, 1000);

    ASSERT_GT(full.Deadlocks, 0);
    EXPECT_GT(reduced.Deadlocks, 0)
        << "the reduction skipped the only order in which a queued writer "
           "holds back a reader's re-take";
}

struct IndependentAudit
{
    volatile long Own[3];
};

static void OwnCounterWorker(void* Parameter)
{
    volatile long* own = (volatile long*)Parameter;

    InterlockedIncrement(own);
    InterlockedIncrement(own);
    InterlockedIncrement(own);
}

static void IndependentSetup(void* Parameter)
{
    IndependentAudit* audit = (IndependentAudit*)Parameter;

    for (int i = 0; i < 3; ++i)
    {
        audit->Own[i] = 0;
        KmSchedSpawn(OwnCounterWorker, (void*)&audit->Own[i]);
    }
}

static void IndependentTeardown(void* Parameter)
{
    IndependentAudit* audit = (IndependentAudit*)Parameter;

    KmSchedNoteOutcome(((unsigned __int64)audit->Own[0] << 32) |
        ((unsigned __int64)audit->Own[1] << 16) | (unsigned __int64)audit->Own[2]);
}

TEST(SchedulerAudit, ReductionRunsIndependentStepsInOneOrder)
{
    static IndependentAudit audit;

    KmSchedSetAtomicYields(1);

    const KM_SCHED_RESULT full =
        KmExploreInterleavings(IndependentSetup, IndependentTeardown, &audit, 100000);
    const KM_SCHED_RESULT reduced =
        ExploreReduced(IndependentSetup, IndependentTeardown, &audit, 100000);

    KmSchedSetAtomicYields(0);

    ASSERT_LT(full.Schedules, 100000) << "the full space was sampled, not exhausted";
    EXPECT_EQ(1, full.Outcomes);
    EXPECT_EQ(full.OutcomeDigest, reduced.OutcomeDigest);

    //
    // Three threads touching only their own counter commute everywhere:
    // one order is explored, and every other run stops at its first
    // branch.
    //
    EXPECT_EQ(1, reduced.Schedules - reduced.Pruned)
        << "the reduction explored more than one order of steps that commute";
}

//
// The reduction's own check on its footprints. The writer changes a flag
// without recording it, and the flag decides what the reader's next step
// touches. The reduction takes the two first steps as independent and
// puts the reader to sleep across the write; when a pruned run later
// resumes the reader, its step no longer matches the footprint it was
// put to sleep with, which is what an unrecorded shared access looks like.
//
struct UnrecordedWriteAudit
{
    long Flag;
    volatile long Left;
    volatile long Right;
};

static void FlagDependentReader(void* Parameter)
{
    UnrecordedWriteAudit* audit = (UnrecordedWriteAudit*)Parameter;

    (void)ReadNoFence(audit->Flag ? &audit->Left : &audit->Right);
}

static void UnrecordedFlagWriter(void* Parameter)
{
    UnrecordedWriteAudit* audit = (UnrecordedWriteAudit*)Parameter;

    audit->Flag = 1;
    KmSchedYield();
}

static void UnrecordedWriteSetup(void* Parameter)
{
    UnrecordedWriteAudit* audit = (UnrecordedWriteAudit*)Parameter;

    audit->Flag = 0;

    KmSchedSpawn(FlagDependentReader, audit);
    KmSchedSpawn(UnrecordedFlagWriter, audit);
}

TEST(SchedulerAudit, ReductionReportsAnUnrecordedSharedWrite)
{
    static UnrecordedWriteAudit audit;

    KmExpectViolation(KmViolationLifetime);

    ExploreReduced(UnrecordedWriteSetup, nullptr, &audit, 1000);

    EXPECT_EQ(KmViolationLifetime, KmTakeViolation())
        << "a step whose footprint changed under an unrecorded write went unreported";
}

//
// A pool block freed by one thread and read by another. The quarantine
// keeps the memory mapped, so the read itself returns poison rather than
// faulting; the scheduler is what has to notice it.
//
struct FreedBlockAudit
{
    volatile long* Block;
};

static void BlockFreer(void* Parameter)
{
    ExFreePool((PVOID)((FreedBlockAudit*)Parameter)->Block);
}

static void BlockReader(void* Parameter)
{
    (void)ReadNoFence(((FreedBlockAudit*)Parameter)->Block);
}

static void FreedBlockSetup(void* Parameter)
{
    FreedBlockAudit* audit = (FreedBlockAudit*)Parameter;

    audit->Block = (volatile long*)ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(long), 'tduA');

    KmSchedSpawn(BlockFreer, audit);
    KmSchedSpawn(BlockReader, audit);
}

TEST(SchedulerAudit, ReadOfAFreedPoolBlockIsReported)
{
    static FreedBlockAudit audit;

    KmExpectViolation(KmViolationPool);

    KmExploreInterleavings(FreedBlockSetup, nullptr, &audit, 1000);

    EXPECT_EQ(KmViolationPool, KmTakeViolation())
        << "a read of a freed pool block went unreported";
}

} // namespace
