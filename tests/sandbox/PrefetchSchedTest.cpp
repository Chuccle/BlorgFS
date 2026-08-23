//
// Exhaustive interleaving proof of the prefetch ring's lifetime protocol,
// on the real Prefetch.c.
//
// The claim, from Prefetch.c's own comments: RefCount is dropped by
// PrefetchReleaseRef from three places -- a completing fetch, a detaching
// FCB, and the pump's queued-work reference -- and whichever drop reaches
// zero frees the ring. A ring touched after that free is a kernel
// use-after-free at DISPATCH_LEVEL, on non-paged memory a fetch completion
// was about to write into.
//
// PrefetchKernelTest already stress-tests detach racing completions
// (DetachRacingCompletionsIsSafe) and serve racing completions. This
// explores every interleaving of the two-reference case directly: one
// fetch in flight, one FCB attachment, both racing to be the one that
// frees.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "Scheduler.h"

//
// Declared here rather than in a shared header, matching
// PrefetchKernelTest.cpp: these are PrefetchModel.c's test-only surface,
// not part of the driver's own headers.
//
VOID PrefetchModelReset(VOID);
LONG PrefetchModelFetchesPending(VOID);
int  PrefetchModelCompleteNextFetch(NTSTATUS Status);
int  PrefetchModelSettle(NTSTATUS Status);
VOID BlorgPrefetchReleaseChunkPool(VOID);
}

namespace
{

struct RingProof
{
    FCB Fcb;
    wchar_t PathBuffer[16];

    unsigned char* Buffer;
    PIRP Irp;
    PMDL Mdl;

    volatile long Detached;
    volatile long Completed;
};

//
// The transport side: the fetch this ring issued finishes and the
// completion runs, which is one of the three RefCount owners.
//
void CompletionThread(void* Parameter)
{
    RingProof* proof = (RingProof*)Parameter;

    PrefetchModelCompleteNextFetch(STATUS_SUCCESS);

    InterlockedIncrement(&proof->Completed);
}

//
// The close side: the FCB is going away, so its attachment reference is
// dropped. This is the same call BlorgFreeFileContext makes.
//
void DetachThread(void* Parameter)
{
    RingProof* proof = (RingProof*)Parameter;

    BlorgPrefetchDetach(&proof->Fcb);

    InterlockedIncrement(&proof->Detached);
}

void RingProofSetup(void* Parameter)
{
    RingProof* proof = (RingProof*)Parameter;

    ShimReset();
    PrefetchModelReset();

    memset(&proof->Fcb, 0, sizeof(proof->Fcb));

    proof->Fcb.FullPath.Buffer = proof->PathBuffer;
    proof->Fcb.FullPath.Length = (USHORT)(wcslen(proof->PathBuffer) * sizeof(wchar_t));
    proof->Fcb.FullPath.MaximumLength = proof->Fcb.FullPath.Length;
    proof->Fcb.Header.FileSize.QuadPart = 64 * 1024 * 1024;

    proof->Detached = 0;
    proof->Completed = 0;

    //
    // Arm the ring the way a real sequential stream does: two contiguous
    // chunk reads. Both are served inline against whatever the ring
    // already has resident (nothing, on a cold ring), which is enough to
    // cross PREFETCH_ARM_STREAK.
    //
    static unsigned char bufferA[PREFETCH_CHUNK];
    static unsigned char bufferB[PREFETCH_CHUNK];

    PMDL mdlA = IoAllocateMdl(bufferA, PREFETCH_CHUNK, FALSE, FALSE, nullptr);
    PIRP irpA = IoAllocateIrp(1, FALSE);
    irpA->MdlAddress = mdlA;
    BlorgPrefetchServeRead(&proof->Fcb, irpA, 0, PREFETCH_CHUNK);
    IoFreeIrp(irpA);
    IoFreeMdl(mdlA);

    PMDL mdlB = IoAllocateMdl(bufferB, PREFETCH_CHUNK, FALSE, FALSE, nullptr);
    PIRP irpB = IoAllocateIrp(1, FALSE);
    irpB->MdlAddress = mdlB;
    BlorgPrefetchServeRead(&proof->Fcb, irpB, PREFETCH_CHUNK, PREFETCH_CHUNK);
    IoFreeIrp(irpB);
    IoFreeMdl(mdlB);

    if (!proof->Fcb.PrefetchRing)
    {
        return;
    }

    //
    // A third chunk read that lands past what arming already resolved
    // inline is what leaves a fetch genuinely in flight for the
    // completion thread to race against the detach.
    //
    static unsigned char bufferC[PREFETCH_CHUNK];

    proof->Mdl = IoAllocateMdl(bufferC, PREFETCH_CHUNK, FALSE, FALSE, nullptr);
    proof->Irp = IoAllocateIrp(1, FALSE);
    proof->Irp->MdlAddress = proof->Mdl;

    BlorgPrefetchServeRead(&proof->Fcb, proof->Irp, 2ULL * PREFETCH_CHUNK, PREFETCH_CHUNK);

    if (0 == PrefetchModelFetchesPending())
    {
        return;
    }

    KmSchedSpawn(CompletionThread, proof);
    KmSchedSpawn(DetachThread, proof);
}

void RingProofTeardown(void* Parameter)
{
    RingProof* proof = (RingProof*)Parameter;

    //
    // Whichever side did not run to completion this schedule: finish
    // cleanly, the way a real unload drains outstanding I/O before
    // freeing anything.
    //
    if (!proof->Detached)
    {
        BlorgPrefetchDetach(&proof->Fcb);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    BlorgPrefetchReleaseChunkPool();

    if (proof->Irp)
    {
        IoFreeIrp(proof->Irp);
        proof->Irp = nullptr;
    }

    if (proof->Mdl)
    {
        IoFreeMdl(proof->Mdl);
        proof->Mdl = nullptr;
    }
}

class PrefetchSchedTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("PrefetchSchedTest teardown");
    }
};

//
// No interleaving of a completing fetch and a detaching FCB frees the ring
// twice, or leaves either side touching it afterwards. Freed rings are
// tracked the same way the node-table proof tracks freed nodes: the
// allocator quarantines rather than releases, so a touch-after-free reads
// poison instead of faulting, and the model's own object accounting
// catches a double free at the moment it happens.
//
TEST_F(PrefetchSchedTest, NoInterleavingDoubleFreesTheRing)
{
    static RingProof proof;

    proof = {};
    wcscpy_s(proof.PathBuffer, L"/media/big.bin");

    //
    // Required: PrefetchReleaseRef's whole arbitration is
    // InterlockedDecrement racing InterlockedDecrement. At lock
    // granularity alone the explorer would find only the two whole-thread
    // orderings the existing stress test already covers.
    //
    KmSchedSetAtomicYields(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(RingProofSetup, RingProofTeardown, &proof, 50000);

    KmSchedSetAtomicYields(0);

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 50000)
        << "hit the schedule cap -- sampled, not exhausted";

    EXPECT_GT(proof.Detached, 0) << "no schedule ever ran the detach";
    EXPECT_GT(proof.Completed, 0) << "no schedule ever ran the completion";

    printf("[  sched   ] %d interleavings, max depth %d\n", result.Schedules, result.MaxDepth);
}

} // namespace
