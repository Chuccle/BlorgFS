//
// Kernel-behaviour tests for the real Prefetch.c: the driver's most
// concurrency-critical code.
//
// The ring is guarded by a spin lock but its correctness rests on more
// than mutual exclusion:
//
//   Lifetime      RefCount = FCB attachment + one per in-flight fetch +
//                 one per queued pump. Whoever drops it to zero frees the
//                 ring, so a late completion must never touch freed
//                 memory -- and a detach with fetches still outstanding
//                 must not free early.
//   Ordering      A park pumps BEFORE publishing the waiter. Prefetch.h
//                 calls this a lifetime invariant, not a nicety: once the
//                 waiter is visible its completion can finish the IRP,
//                 after which the FCB and the ring's reference can be
//                 gone.
//   Delivery      One waiter per slot, delivered exactly once, with the
//                 bytes of the slot it parked on.
//   Generation    A seek bumps the generation; in-flight fetches issued
//                 under an older one must be discarded rather than served.
//   Issuance      Every fetch is issued at PASSIVE_LEVEL, never from a
//                 completion -- which is why the pump is a work item.
//
// The model checks all of that while these run: the fetch issuer aborts
// if called above PASSIVE, the pool is guarded, work-item double-queue is
// a violation, IRP double-completion is a violation, and every fixture
// asserts quiescence on teardown.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"

int  PrefetchModelCompletionCount(PIRP Irp);
NTSTATUS PrefetchModelCompletionStatus(PIRP Irp);
SIZE_T PrefetchModelCompletionBytes(PIRP Irp);
VOID PrefetchModelSetFillByte(unsigned char Value);
VOID PrefetchModelFailNextIssues(LONG Count);
LONG PrefetchModelFetchesIssued(VOID);
LONG PrefetchModelFetchesPending(VOID);
int  PrefetchModelCompleteNextFetch(NTSTATUS Status);
int  PrefetchModelCompleteAllFetches(NTSTATUS Status);
int  PrefetchModelSettle(NTSTATUS Status);
VOID PrefetchModelReset(VOID);

//
// The single counter block these targets use in place of the driver's
// per-processor table (NoStatisticsStub.c), so a test can assert on what
// the code under test actually counted.
//
extern BLORGFS_STATISTICS ShimStatistics;
}

namespace
{

const ULONG64 kFileSize = 64ull * 1024 * 1024;

class PrefetchKernelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        PrefetchModelReset();

        memset(&Fcb, 0, sizeof(Fcb));

        //
        // The ring snapshots the path at arm time and never dereferences
        // the FCB afterwards, so this only has to be a valid string.
        //
        Fcb.FullPath.Buffer = PathBuffer;
        Fcb.FullPath.Length = (USHORT)(wcslen(PathBuffer) * sizeof(wchar_t));
        Fcb.FullPath.MaximumLength = Fcb.FullPath.Length;

        Fcb.Header.FileSize.QuadPart = (LONGLONG)kFileSize;
    }

    void TearDown() override
    {
        //
        // Detach is the driver's teardown path. Anything still live after
        // it -- a ring, a buffer, a work item -- is a leak that a real
        // unload would turn into a use-after-unload.
        //
        BlorgPrefetchDetach(&Fcb);

        PrefetchModelSettle(STATUS_SUCCESS);
        ShimDrainWorkItems();

        //
        // Chunks outlive the ring on purpose -- they go back to the
        // driver-wide pool for the next stream rather than to the
        // allocator. That is invisible to a per-test quiescence check,
        // which sees only that pool allocations are still live, so the
        // pool is reclaimed here to keep recycled chunks from reading as
        // leaked ones.
        //
        BlorgPrefetchReleaseChunkPool();

        for (PIRP irp : Irps)
        {
            IoFreeIrp(irp);
        }

        Irps.clear();

        for (PMDL mdl : Mdls)
        {
            IoFreeMdl(mdl);
        }

        Mdls.clear();

        KmAssertQuiescent("PrefetchKernelTest teardown");
    }

    //
    // A paging read the way BlorgVolumeRead presents one: an IRP carrying
    // a locked MDL for the caller's buffer.
    //
    PIRP MakeRead(unsigned char* buffer, SIZE_T length)
    {
        PMDL mdl = IoAllocateMdl(buffer, (ULONG)length, FALSE, FALSE, nullptr);
        PIRP irp = IoAllocateIrp(1, FALSE);

        irp->MdlAddress = mdl;

        Mdls.push_back(mdl);
        Irps.push_back(irp);

        return irp;
    }

    //
    // Walks a sequential stream forward one chunk at a time, which is what
    // arms the ring: PREFETCH_ARM_STREAK contiguous reads.
    //
    NTSTATUS Serve(PIRP irp, ULONG64 offset, ULONG length)
    {
        return BlorgPrefetchServeRead(&Fcb, irp, offset, length);
    }

    FCB Fcb;
    wchar_t PathBuffer[16] = L"/media/big.bin";
    std::vector<PIRP> Irps;
    std::vector<PMDL> Mdls;
    std::vector<std::vector<unsigned char>> Buffers;

    unsigned char* NewBuffer(SIZE_T length)
    {
        Buffers.emplace_back(length, 0);
        return Buffers.back().data();
    }
};

///////////////////////////////////////////////////////////////////////////
// Arming and issuance
///////////////////////////////////////////////////////////////////////////

//
// A single read must not arm the ring: one probe read is not a stream,
// and arming on it would spend a 4 MB pipeline on a random access.
//
TEST_F(PrefetchKernelTest, SingleReadDoesNotArmTheRing)
{
    unsigned char* buffer = NewBuffer(PREFETCH_CHUNK);
    PIRP irp = MakeRead(buffer, PREFETCH_CHUNK);

    EXPECT_EQ(STATUS_NOT_FOUND, Serve(irp, 0, PREFETCH_CHUNK));
    EXPECT_EQ(nullptr, Fcb.PrefetchRing) << "the ring armed on a single read";
    EXPECT_EQ(0, PrefetchModelFetchesIssued());
}

//
// The second contiguous read is what arms it. That read still misses --
// the chunk in hand is already being fetched by the caller -- but the
// pipeline starts filling behind it.
//
TEST_F(PrefetchKernelTest, SecondSequentialReadArmsAndIssues)
{
    unsigned char* first = NewBuffer(PREFETCH_CHUNK);
    unsigned char* second = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(first, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);

    EXPECT_EQ(STATUS_NOT_FOUND, Serve(MakeRead(second, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK));

    ASSERT_NE(nullptr, Fcb.PrefetchRing) << "a two-read streak should arm the ring";
    EXPECT_GT(PrefetchModelFetchesIssued(), 0) << "arming should start filling the pipeline";
}

//
// Every fetch must be issued at PASSIVE. The model aborts otherwise, so
// this passing is the assertion -- it is what proves the pump work item
// is doing its job rather than fetches leaking out of a completion.
//
TEST_F(PrefetchKernelTest, FetchesAreOnlyIssuedAtPassiveLevel)
{
    unsigned char* first = NewBuffer(PREFETCH_CHUNK);
    unsigned char* second = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(first, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(second, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    // Completions run at DISPATCH and may queue the pump; the pump runs
    // at PASSIVE and issues. If that separation were broken the model
    // would have aborted inside BlorgHttpGetFileMdl.
    PrefetchModelSettle(STATUS_SUCCESS);

    SUCCEED();
}

///////////////////////////////////////////////////////////////////////////
// Hit path
///////////////////////////////////////////////////////////////////////////

//
// Once a chunk has been fetched ahead, the read that wants it is served
// by copy with no round trip -- the entire point of the ring. The fill
// byte proves the bytes came from that slot rather than from anywhere
// else.
//
TEST_F(PrefetchKernelTest, ResidentChunkIsServedAsAHit)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    PrefetchModelSetFillByte(0x5A);
    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);

    unsigned char* target = NewBuffer(PREFETCH_CHUNK);
    PIRP irp = MakeRead(target, PREFETCH_CHUNK);

    NTSTATUS status = Serve(irp, 2 * PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_EQ(STATUS_SUCCESS, status) << "a resident chunk should be a hit, not a miss or a park";
    EXPECT_EQ(PREFETCH_CHUNK, irp->IoStatus.Information);

    for (SIZE_T i = 0; i < PREFETCH_CHUNK; ++i)
    {
        ASSERT_EQ(0x5A, target[i]) << "hit delivered bytes from the wrong slot at offset " << i;
    }
}

//
// A read whose range is entirely covered by a resident slot is served from
// it even when it starts inside the slot rather than on its boundary. Under
// the old exact-offset lookup this was a miss that paid a full round trip
// re-fetching bytes the ring already held.
//
// The fill byte is what makes this meaningful: it proves the copy came from
// the correct position inside the slot, not merely that a hit was reported.
// A containment lookup that forgot to offset the copy would still return
// STATUS_SUCCESS here while handing the reader the wrong bytes -- silent
// data corruption, and far worse than the miss it replaced.
//
TEST_F(PrefetchKernelTest, InteriorOffsetCoveredByASlotIsServedFromWithinIt)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    PrefetchModelSetFillByte(0x7E);
    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);

    const ULONG interiorLength = 4096;
    unsigned char* target = NewBuffer(interiorLength);
    PIRP irp = MakeRead(target, interiorLength);

    NTSTATUS status = Serve(irp, (2 * PREFETCH_CHUNK) + interiorLength, interiorLength);

    ASSERT_EQ(STATUS_SUCCESS, status)
        << "the slot covers this range, so containment lookup must serve it as a hit";
    EXPECT_EQ(interiorLength, irp->IoStatus.Information);

    for (SIZE_T i = 0; i < interiorLength; ++i)
    {
        ASSERT_EQ(0x7E, target[i]) << "interior hit copied from the wrong offset at " << i;
    }
}

//
// The park counterpart. A read admitted by containment onto a slot that is
// still in flight must be delivered from its own offset inside that slot
// when the fetch lands, not from the head of the buffer.
//
TEST_F(PrefetchKernelTest, InteriorParkIsDeliveredFromItsOffsetWithinTheSlot)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);
    ASSERT_GT(PrefetchModelFetchesPending(), 0) << "the slot must still be in flight to park on";

    const ULONG interiorLength = 4096;
    unsigned char* target = NewBuffer(interiorLength);
    PIRP irp = MakeRead(target, interiorLength);

    NTSTATUS status = Serve(irp, (2 * PREFETCH_CHUNK) + interiorLength, interiorLength);

    ASSERT_EQ(STATUS_PENDING, status) << "an in-flight slot covering the range should be parked on";

    PrefetchModelSetFillByte(0x3C);
    PrefetchModelSettle(STATUS_SUCCESS);

    EXPECT_EQ(1, PrefetchModelCompletionCount(irp)) << "a parked paging IRP must complete exactly once";
    EXPECT_EQ(STATUS_SUCCESS, PrefetchModelCompletionStatus(irp));
    EXPECT_EQ(interiorLength, PrefetchModelCompletionBytes(irp));

    for (SIZE_T i = 0; i < interiorLength; ++i)
    {
        ASSERT_EQ(0x3C, target[i]) << "interior park delivered from the wrong offset at " << i;
    }
}

//
// The containment test is the memory-safety boundary for the copy that
// follows it, so it must reject an offset that no honest caller would
// produce. Read.c screens negative offsets out at dispatch, but this
// deliberately does not go through Read.c: the point is that the ring is
// safe on its own, for whatever a kernel caller with a hand-built IRP
// hands it.
//
// A negative LONGLONG offset arrives here widened to ULONG64, so -4096 is
// 2^64-4096. The arithmetic is chosen so the pre-fix predicate,
// (Offset - RangeOffset) + Length > Hot.Length, wraps to exactly zero and
// therefore reports coverage: for a slot based at Base, a read of
// Base + Magnitude bytes at offset -Magnitude sums to 2^64. Under that the
// old code computed a slot displacement of (ULONG)(0 - Magnitude - Base)
// -- near 4 GB -- and copied from there. Every slot base the ring could
// plausibly be holding is tried, both while its fetches are in flight (the
// park path, which would have stored the same displacement in
// WaiterSlotOffsets and copied on completion) and once they are ready (the
// hit path), because both consume the lookup's answer.
//
TEST_F(PrefetchKernelTest, WrappingOffsetIsNeverServedFromASlot)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    const ULONG magnitude = 4096;
    const ULONG64 hostileOffset = 0ull - magnitude;
    const ULONG largest = (PREFETCH_DEPTH + 2) * PREFETCH_CHUNK + magnitude;

    unsigned char* target = NewBuffer(largest);

    for (int pass = 0; pass < 2; ++pass)
    {
        for (ULONG slot = 0; slot < PREFETCH_DEPTH + 2; ++slot)
        {
            ULONG length = (slot * PREFETCH_CHUNK) + magnitude;
            PIRP irp = MakeRead(target, length);

            NTSTATUS status = Serve(irp, hostileOffset, length);

            ASSERT_EQ(STATUS_NOT_FOUND, status)
                << "wrapping offset admitted by the containment test at slot base "
                << (slot * PREFETCH_CHUNK) << " (pass " << pass << ")";
            ASSERT_EQ(0, PrefetchModelCompletionCount(irp))
                << "a rejected read must be left for the caller's direct fetch";
        }

        PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    }
}

///////////////////////////////////////////////////////////////////////////
// Park path -- delivery and ordering
///////////////////////////////////////////////////////////////////////////

//
// A read that lands on a chunk still in flight parks on it, and the
// fetch completion delivers it. Exactly one completion: the model treats
// a second as a violation, because a double-completed paging IRP is a
// corruption MM will not survive.
//
TEST_F(PrefetchKernelTest, ParkedReadIsDeliveredExactlyOnce)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);
    ASSERT_GT(PrefetchModelFetchesPending(), 0);

    unsigned char* target = NewBuffer(PREFETCH_CHUNK);
    PIRP irp = MakeRead(target, PREFETCH_CHUNK);

    PrefetchModelSetFillByte(0x77);

    NTSTATUS status = Serve(irp, 2 * PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_EQ(STATUS_PENDING, status) << "a read on an in-flight chunk should park";
    EXPECT_EQ(0, PrefetchModelCompletionCount(irp)) << "a parked read must not be completed before its fetch";

    //
    // Returning STATUS_PENDING obliges the dispatch path to have marked
    // the IRP pending, and the mark has to happen before the waiter is
    // published -- once it is visible the completion can finish the IRP on
    // another CPU. A paging read bypasses the FSP queue and the oplock
    // package, so nothing else would have marked it.
    //
    EXPECT_TRUE(irp->PendingReturned)
        << "a parked read returned STATUS_PENDING without IoMarkIrpPending";

    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    ShimDrainWorkItems();

    EXPECT_EQ(1, PrefetchModelCompletionCount(irp)) << "parked read not delivered exactly once";
    EXPECT_TRUE(NT_SUCCESS(PrefetchModelCompletionStatus(irp)));
    EXPECT_EQ(PREFETCH_CHUNK, PrefetchModelCompletionBytes(irp));

    for (SIZE_T i = 0; i < PREFETCH_CHUNK; ++i)
    {
        ASSERT_EQ(0x77, target[i]) << "parked read got bytes from the wrong slot at offset " << i;
    }
}

//
// A fetch that fails must still complete the read parked on it. Leaving
// it would strand the paging IRP forever -- and a later reuse of the slot
// would complete it with data from a different range, which is worse.
//
TEST_F(PrefetchKernelTest, FailedFetchStillCompletesItsParkedRead)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    unsigned char* target = NewBuffer(PREFETCH_CHUNK);
    PIRP irp = MakeRead(target, PREFETCH_CHUNK);

    ASSERT_EQ(STATUS_PENDING, Serve(irp, 2 * PREFETCH_CHUNK, PREFETCH_CHUNK));

    PrefetchModelCompleteAllFetches(STATUS_CONNECTION_DISCONNECTED);
    ShimDrainWorkItems();

    EXPECT_EQ(1, PrefetchModelCompletionCount(irp)) << "a failed fetch stranded its parked read";
    EXPECT_FALSE(NT_SUCCESS(PrefetchModelCompletionStatus(irp)));
}

//
// A second read of a range already parked on falls through to a direct
// fetch rather than displacing the first waiter. One waiter per slot is
// the invariant; overwriting it would strand the first read permanently.
//
TEST_F(PrefetchKernelTest, SecondReaderOfAParkedSlotDoesNotDisplaceTheFirst)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    unsigned char* firstTarget = NewBuffer(PREFETCH_CHUNK);
    PIRP first = MakeRead(firstTarget, PREFETCH_CHUNK);

    ASSERT_EQ(STATUS_PENDING, Serve(first, 2 * PREFETCH_CHUNK, PREFETCH_CHUNK));

    unsigned char* secondTarget = NewBuffer(PREFETCH_CHUNK);
    PIRP second = MakeRead(secondTarget, PREFETCH_CHUNK);

    NTSTATUS status = Serve(second, 2 * PREFETCH_CHUNK, PREFETCH_CHUNK);

    EXPECT_EQ(STATUS_NOT_FOUND, status)
        << "a second reader of an already-parked slot must fall through to a direct fetch";

    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    ShimDrainWorkItems();

    EXPECT_EQ(1, PrefetchModelCompletionCount(first)) << "the original waiter was displaced";
    EXPECT_EQ(0, PrefetchModelCompletionCount(second)) << "a read that was told to fetch directly was completed anyway";
}

///////////////////////////////////////////////////////////////////////////
// Lifetime
///////////////////////////////////////////////////////////////////////////

//
// Detach with fetches still outstanding. The ring must survive until the
// last completion runs -- freeing on detach would have those completions
// write into freed pool. The model's guarded pool is what would catch it.
//
TEST_F(PrefetchKernelTest, DetachWithFetchesInFlightDefersTheFree)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);
    ASSERT_GT(PrefetchModelFetchesPending(), 0) << "test needs fetches in flight to be meaningful";

    BlorgPrefetchDetach(&Fcb);

    EXPECT_EQ(nullptr, Fcb.PrefetchRing) << "detach must unlink the ring from the FCB immediately";

    // The completions now run against a detached ring.
    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    ShimDrainWorkItems();

    // TearDown's quiescence assertion is the other half of this test.
}

//
// A fetch that completes after detach must be discarded, not delivered:
// its FCB is gone. The generation bump on detach is what makes that
// happen, and the model's IRP double-completion check plus the quiescence
// assertion are what would catch it going wrong.
//
TEST_F(PrefetchKernelTest, CompletionAfterDetachIsDiscardedCleanly)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    BlorgPrefetchDetach(&Fcb);

    ULONG64 before = BlorgStatisticsForCurrentProcessor()->PrefetchStaleDiscards;

    int completed = PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    EXPECT_GT(completed, 0);

    ShimDrainWorkItems();

    EXPECT_EQ(0u, ShimPendingWorkItems()) << "a detached ring kept queueing pumps";
    EXPECT_EQ(before + (ULONG64)completed, BlorgStatisticsForCurrentProcessor()->PrefetchStaleDiscards)
        << "every successfully-completed stale fetch must count as a discard";
}

//
// The same discard path, but the fetch itself failed rather than merely
// arriving after detach. A failed fetch was never going to serve data, so
// it must not be counted alongside the ones that fetched successfully into
// a slot nobody wanted anymore -- the two are different events for anyone
// reading the counter.
//
TEST_F(PrefetchKernelTest, DetachedCompletionOnlyCountsAsStaleDiscardOnSuccess)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    BlorgPrefetchDetach(&Fcb);

    ULONG64 before = BlorgStatisticsForCurrentProcessor()->PrefetchStaleDiscards;

    PrefetchModelCompleteAllFetches(STATUS_CONNECTION_DISCONNECTED);
    ShimDrainWorkItems();

    EXPECT_EQ(before, BlorgStatisticsForCurrentProcessor()->PrefetchStaleDiscards)
        << "a failed fetch must not be counted as a stale discard";
}

//
// An issue failure releases the fetch's reference on the spot, because
// the completion callback will never run. Getting that wrong leaks the
// ring; getting it wrong the other way frees it early.
//
TEST_F(PrefetchKernelTest, IssueFailureReleasesItsReference)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);

    PrefetchModelFailNextIssues(64);

    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    PrefetchModelFailNextIssues(0);

    EXPECT_EQ(0, PrefetchModelFetchesPending()) << "no fetch should be outstanding after a total issue failure";

    // TearDown detaches and asserts quiescence: if the failure path leaked
    // or double-released the ring reference, that is where it shows.
}

//
// A stream that has finished its file must not keep starving the ones after
// it. This is the sandbox translation of a measurement taken against the
// live driver, and it fails today.
//
// On a 4 GB guest the budget is 8 rings. Reading eleven files in turn, each
// opened, read and closed before the next, the ninth stream onward was
// refused a ring and fell back to direct fetches at full network RTT --
// 60-90 ms per read on the test path, over a second at worst. The budget
// only came back once the whole workload ended.
//
// The reason is visible in the hit path above: serving a hit sets the slot
// to PrefetchSlotEmpty and then donates the buffer straight back to it, so a
// ring whose file is finished sits on its entire buffer set with every slot
// reporting itself unused. The memory is held by slots that say they are
// holding nothing.
//
// Deliberately NOT detached. That models the real case: an FCB whose handle
// has closed but which the cache manager and MM still hold, because
// SectionObjectPointers lives on the FCB and paging reads can legitimately
// still arrive. Releasing on close is not available as a fix -- and is not
// even sound, since BlorgPrefetchServeRead holds no ring reference for the
// duration of a serve.
//
TEST_F(PrefetchKernelTest, FinishedStreamsStopStarvingLaterOnes)
{
    //
    // Comfortably more than any budget tier, so the test states the property
    // rather than encoding this machine's tier.
    //
    const int kFiles = 16;
    const ULONG64 kSmallFile = 4 * PREFETCH_CHUNK;

    std::vector<FCB> fcbs(kFiles);
    std::vector<std::vector<unsigned char>> buffers;

    const ULONG64 refusedBefore = ShimStatistics.PrefetchRingsRefused;

    for (int i = 0; i < kFiles; ++i)
    {
        memset(&fcbs[i], 0, sizeof(FCB));
        fcbs[i].FullPath.Buffer = PathBuffer;
        fcbs[i].FullPath.Length = Fcb.FullPath.Length;
        fcbs[i].FullPath.MaximumLength = Fcb.FullPath.Length;
        fcbs[i].Header.FileSize.QuadPart = (LONGLONG)kSmallFile;

        buffers.emplace_back(PREFETCH_CHUNK, 0);

        //
        // Two contiguous reads arm the ring, then read the file out to its
        // end and settle every fetch, which is what a finished stream looks
        // like: slots drained, nothing in flight, no reader left.
        //
        for (ULONG64 offset = 0; offset < kSmallFile; offset += PREFETCH_CHUNK)
        {
            BlorgPrefetchServeRead(&fcbs[i], MakeRead(buffers.back().data(), PREFETCH_CHUNK),
                offset, PREFETCH_CHUNK);
        }

        PrefetchModelSettle(STATUS_SUCCESS);
        ShimDrainWorkItems();
    }

    int armed = 0;

    for (int i = 0; i < kFiles; ++i)
    {
        if (fcbs[i].PrefetchRing)
        {
            ++armed;
        }
    }

    EXPECT_EQ(kFiles, armed)
        << "only " << armed << " of " << kFiles << " streams got a ring -- a finished "
           "stream is still holding buffers in slots it has marked empty, so every "
           "later stream falls back to full-RTT direct fetches";

    EXPECT_EQ(refusedBefore, ShimStatistics.PrefetchRingsRefused)
        << "a stream was refused a ring while earlier, finished streams held theirs";

    for (int i = 0; i < kFiles; ++i)
    {
        BlorgPrefetchDetach(&fcbs[i]);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();
}

//
// The driver-wide ring budget. Beyond bounding memory, this is a lifetime
// path: the budget is claimed before the ring is built and must be
// released on every failure exit, or the cap ratchets down until no
// stream can ever arm again.
//
TEST_F(PrefetchKernelTest, RingBudgetIsReleasedOnEveryPath)
{
    const int kFiles = 24;

    std::vector<FCB> fcbs(kFiles);
    std::vector<std::vector<unsigned char>> buffers;

    for (int i = 0; i < kFiles; ++i)
    {
        memset(&fcbs[i], 0, sizeof(FCB));
        fcbs[i].FullPath.Buffer = PathBuffer;
        fcbs[i].FullPath.Length = Fcb.FullPath.Length;
        fcbs[i].FullPath.MaximumLength = Fcb.FullPath.Length;
        fcbs[i].Header.FileSize.QuadPart = (LONGLONG)kFileSize;

        buffers.emplace_back(PREFETCH_CHUNK, 0);

        PIRP first = MakeRead(buffers.back().data(), PREFETCH_CHUNK);
        PIRP second = MakeRead(buffers.back().data(), PREFETCH_CHUNK);

        BlorgPrefetchServeRead(&fcbs[i], first, 0, PREFETCH_CHUNK);
        BlorgPrefetchServeRead(&fcbs[i], second, PREFETCH_CHUNK, PREFETCH_CHUNK);
    }

    PrefetchModelSettle(STATUS_SUCCESS);

    for (int i = 0; i < kFiles; ++i)
    {
        BlorgPrefetchDetach(&fcbs[i]);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    //
    // After everything is detached the budget must be fully restored: a
    // fresh stream should still be able to arm. If any path leaked a
    // budget slot, this is where the cap has silently ratcheted to zero.
    //
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    EXPECT_NE(nullptr, Fcb.PrefetchRing)
        << "the ring budget did not come back -- a slot leaked on some path";
}

///////////////////////////////////////////////////////////////////////////
// Seek and generation
///////////////////////////////////////////////////////////////////////////

//
// A sequential reader that merely outruns an empty pipeline re-aims the
// ring even though the ring was already fetching ahead of it, and that
// re-aim bumps Generation and discards the in-flight chunks.
//
// The re-aim path cannot tell this apart from a real seek: it is gated on
// streak >= PREFETCH_ARM_STREAK, and a seek resets the streak to 1, so
// every re-aim fires on a currently-sequential stream. The only thing
// separating "pipeline points somewhere stale" from "pipeline is fine, the
// reader is just ahead of it" is whether the fetch cursor had already
// passed the offset being served -- which is what PrefetchReaimsInPlace
// records.
//
// Reads here are contiguous with each other (so the streak builds) but sit
// at chunk interiors (so none match a slot and none consume), which is the
// shape the measured workload produces. Fetches are deliberately left
// pending so the slots stay in flight and the discard is real.
//
TEST_F(PrefetchKernelTest, SequentialReaderOutrunningThePipelineIsNotReaimed)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);
    ASSERT_GT(PrefetchModelFetchesPending(), 0) << "ring must be fetching ahead for this to be in-window";

    const ULONG64 reaimsBefore = ShimStatistics.PrefetchReaims;
    const ULONG64 suppressedBefore = ShimStatistics.PrefetchReaimsSuppressed;

    const ULONG shortLength = 4096;
    ULONG64 offset = (2 * PREFETCH_CHUNK) + shortLength;

    for (int i = 0; i < 4; ++i)
    {
        Serve(MakeRead(NewBuffer(shortLength), shortLength), offset, shortLength);
        offset += shortLength;
    }

    EXPECT_EQ(reaimsBefore, ShimStatistics.PrefetchReaims)
        << "the reader is inside the range the ring is fetching, so re-aiming would only "
           "discard the in-flight chunks it is waiting for";

    EXPECT_GT(ShimStatistics.PrefetchReaimsSuppressed, suppressedBefore)
        << "the idle test should have fired and the window test vetoed it";
}

//
// The counterpart, and the reason the veto tests the pipeline WINDOW rather
// than simply asking whether the fetch cursor is ahead of the reader. After
// a backward seek the cursor is also ahead -- far ahead -- but there the
// ring's coverage is genuinely useless and it must be re-pointed. A veto
// written as "cursor is ahead, do nothing" would strand a backward-seeking
// reader on direct fetches forever.
//
TEST_F(PrefetchKernelTest, BackwardSeekStillReaimsEvenThoughTheCursorIsAhead)
{
    const ULONG64 farOffset = 64ull * 1024 * 1024;

    Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), farOffset, PREFETCH_CHUNK);
    Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), farOffset + PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    ShimDrainWorkItems();

    const ULONG64 reaimsBefore = ShimStatistics.PrefetchReaims;

    //
    // Seek back to the start and establish a streak there. The ring's
    // cursor is still tens of megabytes ahead, well outside the window
    // covering these reads.
    //
    ULONG64 offset = 0;

    for (int i = 0; i < 4; ++i)
    {
        Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), offset, PREFETCH_CHUNK);
        offset += PREFETCH_CHUNK;
    }

    EXPECT_GT(ShimStatistics.PrefetchReaims, reaimsBefore)
        << "a backward seek leaves the cursor ahead but the coverage useless -- it must re-aim";
}

//
// A seek away from the stream, then a new streak elsewhere. Data fetched
// for the old region must not be served to the new one; the generation
// counter is what prevents it.
//
TEST_F(PrefetchKernelTest, SeekDoesNotServeStaleData)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    PrefetchModelSetFillByte(0x11);
    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);
    ShimDrainWorkItems();

    // Seek far away and establish a new streak there.
    const ULONG64 farOffset = 32ull * 1024 * 1024;

    unsigned char* c = NewBuffer(PREFETCH_CHUNK);
    unsigned char* d = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(c, PREFETCH_CHUNK), farOffset, PREFETCH_CHUNK);
    Serve(MakeRead(d, PREFETCH_CHUNK), farOffset + PREFETCH_CHUNK, PREFETCH_CHUNK);
    Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK),
          farOffset + 2 * PREFETCH_CHUNK, PREFETCH_CHUNK);

    PrefetchModelSetFillByte(0x22);
    PrefetchModelSettle(STATUS_SUCCESS);

    //
    // A read back in the old region must not be served the 0x11 data that
    // was fetched for it before the seek -- by then the ring has re-aimed
    // and that coverage is gone.
    //
    unsigned char* target = NewBuffer(PREFETCH_CHUNK);
    PIRP irp = MakeRead(target, PREFETCH_CHUNK);

    NTSTATUS status = Serve(irp, 3 * PREFETCH_CHUNK, PREFETCH_CHUNK);

    if (STATUS_SUCCESS == status)
    {
        for (SIZE_T i = 0; i < PREFETCH_CHUNK; ++i)
        {
            ASSERT_NE(0x11, target[i])
                << "stale pre-seek data was served at offset " << i;
        }
    }

    SUCCEED();
}

///////////////////////////////////////////////////////////////////////////
// Concurrency
///////////////////////////////////////////////////////////////////////////

namespace
{
    struct RingStressContext
    {
        FCB* Fcb;
        KM_BARRIER* Barrier;
        int Iterations;
        volatile long Served;
    };

    //
    // Serving and completing from different threads at once. Serve runs at
    // PASSIVE and takes the ring lock; completions run at DISPATCH and take
    // the same lock. That is the exact pairing the ring's design is about,
    // and the model is watching for order inversions, recursion, pool
    // corruption and double completion throughout.
    //
    void RingCompleterThread(void* Parameter)
    {
        RingStressContext* context = (RingStressContext*)Parameter;

        KmBarrierWait(context->Barrier);

        for (int i = 0; i < context->Iterations; ++i)
        {
            PrefetchModelCompleteNextFetch(STATUS_SUCCESS);
            ShimDrainWorkItems();
            KmJitter();
        }
    }
}

//
// A reader walking a stream while completions land underneath it from
// another thread. Nothing here asserts a particular hit rate -- the
// point is that no model rule fires and nothing leaks, under an
// interleaving a single-threaded test cannot produce.
//
TEST_F(PrefetchKernelTest, ServeAndCompleteConcurrentlyStayConsistent)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    const int kReads = 64;

    KM_BARRIER barrier;
    KmInitializeBarrier(&barrier, 2);

    RingStressContext context = {};
    context.Fcb = &Fcb;
    context.Barrier = &barrier;
    context.Iterations = kReads;

    KM_THREAD* completer = KmStartThread(RingCompleterThread, &context);

    KmBarrierWait(&barrier);

    for (int i = 0; i < kReads; ++i)
    {
        unsigned char* target = NewBuffer(PREFETCH_CHUNK);
        PIRP irp = MakeRead(target, PREFETCH_CHUNK);

        ULONG64 offset = (ULONG64)(i + 2) * PREFETCH_CHUNK;

        NTSTATUS status = Serve(irp, offset, PREFETCH_CHUNK);

        //
        // Any of the three outcomes is legal under this interleaving. What
        // is NOT legal is a parked read never being delivered, which the
        // final settle plus teardown quiescence check covers.
        //
        EXPECT_TRUE(STATUS_SUCCESS == status ||
                    STATUS_PENDING == status ||
                    STATUS_NOT_FOUND == status)
            << "unexpected serve status 0x" << std::hex << status;

        InterlockedIncrement(&context.Served);

        KmJitter();
    }

    KmJoinThread(completer);

    PrefetchModelSettle(STATUS_SUCCESS);

    EXPECT_EQ(kReads, context.Served);
}

//
// Teardown racing in-flight completions -- the shape that produced a real
// use-after-free in this ring before. Detach on one thread while another
// is completing fetches: the refcount must let exactly one of them free
// the ring, and no completion may touch it afterwards.
//
TEST_F(PrefetchKernelTest, DetachRacingCompletionsIsSafe)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);
    ASSERT_GT(PrefetchModelFetchesPending(), 0);

    KM_BARRIER barrier;
    KmInitializeBarrier(&barrier, 2);

    struct Local
    {
        static void Completer(void* Parameter)
        {
            KM_BARRIER* barrier = (KM_BARRIER*)Parameter;

            KmBarrierWait(barrier);

            for (int i = 0; i < 16; ++i)
            {
                PrefetchModelCompleteNextFetch(STATUS_SUCCESS);
                KmJitter();
            }
        }
    };

    KM_THREAD* completer = KmStartThread(Local::Completer, &barrier);

    KmBarrierWait(&barrier);
    KmJitter();

    BlorgPrefetchDetach(&Fcb);

    KmJoinThread(completer);

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    EXPECT_EQ(nullptr, Fcb.PrefetchRing);
}

//
// Two properties at once, and they need the same setup: enough streams
// live AT THE SAME TIME that even the largest tier cannot give them all
// PREFETCH_MIN_DEPTH. Nothing is settled or detached inside the arming
// loop, so chunks cannot be recycled between streams and demand is
// genuinely unsatisfiable rather than merely bursty.
//
// That simultaneity is load-bearing for the ceiling assertion. An earlier
// version of this test armed streams one at a time, letting each finish
// before the next began; every stream was then served out of the free list
// and the allocation path -- the only thing the budget gates -- was
// essentially never re-entered. It passed against a driver whose budget
// accounting was deliberately broken, which makes it worth stating plainly:
// a ceiling can only be tested where something is actually pushing on it.
//
// The property this whole design exists for is the other one. Past the
// budget every stream must still arm and still prefetch -- shallower, but
// working. Under the old ring cap this was a cliff: streams past the cap
// got no ring at all and fell back to full-RTT direct fetches for their
// entire lifetime.
//
TEST_F(PrefetchKernelTest, EveryStreamStillArmsWhenTheChunkPoolIsExhausted)
{
    const int kFiles = 48;
    const ULONG64 kFileBytes = 8 * PREFETCH_CHUNK;
    const LONG64 kLargestTier = 64;

    std::vector<FCB> fcbs(kFiles);
    std::vector<unsigned char> buffer(PREFETCH_CHUNK, 0);

    const ULONG64 refusedBefore = ShimStatistics.PrefetchRingsRefused;

    for (int i = 0; i < kFiles; ++i)
    {
        memset(&fcbs[i], 0, sizeof(FCB));
        fcbs[i].FullPath.Buffer = PathBuffer;
        fcbs[i].FullPath.Length = Fcb.FullPath.Length;
        fcbs[i].FullPath.MaximumLength = Fcb.FullPath.Length;
        fcbs[i].Header.FileSize.QuadPart = (LONGLONG)kFileBytes;

        for (ULONG64 offset = 0; offset < 2 * PREFETCH_CHUNK; offset += PREFETCH_CHUNK)
        {
            BlorgPrefetchServeRead(&fcbs[i], MakeRead(buffer.data(), PREFETCH_CHUNK),
                offset, PREFETCH_CHUNK);
        }
    }

    int armed = 0;

    for (int i = 0; i < kFiles; ++i)
    {
        if (fcbs[i].PrefetchRing)
        {
            ++armed;
        }
    }

    EXPECT_EQ(kFiles, armed)
        << "only " << armed << " of " << kFiles << " streams armed under chunk "
           "pressure -- exhausting the pool is supposed to cost depth, not the "
           "whole pipeline";

    EXPECT_EQ(refusedBefore, ShimStatistics.PrefetchRingsRefused)
        << "a ring was refused under memory pressure; only the unload drain may "
           "refuse an arm";

    EXPECT_GT(ShimStatistics.PrefetchChunkStarvations, 0ull)
        << "the pool was never actually exhausted, so this test proved nothing -- "
           "raise kFiles above the largest budget tier";

    EXPECT_LE(BlorgPrefetchChunksLive(), kLargestTier)
        << "chunks live reached " << BlorgPrefetchChunksLive()
        << " with " << kFiles << " streams demanding at once, above the largest "
           "budget tier (" << kLargestTier << ") -- the budget is not bounding "
           "allocation, so concurrency translates directly into non-paged pool";

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    for (int i = 0; i < kFiles; ++i)
    {
        BlorgPrefetchDetach(&fcbs[i]);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();
}

//
// Chunks are recycled between unrelated files now, which is a new way to
// serve the wrong bytes: a chunk released by one stream and handed to
// another still holds the first file's data until the fetch overwrites it.
// A slot that reached Ready without its fetch having actually filled it
// would hand that stale content to the second reader as a hit.
//
// The two streams use distinct fill bytes, so any byte the second reader
// receives carrying the first one's fill is the recycled chunk leaking
// through.
//
// Unlike the two tests above, this one is NOT mutation-controlled, and
// that is worth saying rather than leaving for someone to discover. Every
// mutation tried against it destroyed its own premise -- publishing a slot
// Ready before its fetch runs means no fetch ever writes the first file's
// fill, so there is nothing left to leak and the test passes for the wrong
// reason. It is a guard against a class of regression that chunk recycling
// newly makes possible, not evidence that today's code is right.
//
TEST_F(PrefetchKernelTest, ARecycledChunkNeverServesAnotherFilesBytes)
{
    const ULONG64 kFileBytes = 4 * PREFETCH_CHUNK;
    const unsigned char kFirstFill = 0x11;
    const unsigned char kSecondFill = 0x22;

    FCB first;
    memset(&first, 0, sizeof(first));
    first.FullPath.Buffer = PathBuffer;
    first.FullPath.Length = Fcb.FullPath.Length;
    first.FullPath.MaximumLength = Fcb.FullPath.Length;
    first.Header.FileSize.QuadPart = (LONGLONG)kFileBytes;

    std::vector<unsigned char> buffer(PREFETCH_CHUNK, 0);

    PrefetchModelSetFillByte(kFirstFill);

    for (ULONG64 offset = 0; offset < kFileBytes; offset += PREFETCH_CHUNK)
    {
        BlorgPrefetchServeRead(&first, MakeRead(buffer.data(), PREFETCH_CHUNK),
            offset, PREFETCH_CHUNK);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    //
    // Detaching returns the first file's chunks to the pool, so the second
    // stream is handed the very chunks still holding 0x11.
    //
    BlorgPrefetchDetach(&first);

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    FCB second;
    memset(&second, 0, sizeof(second));
    second.FullPath.Buffer = PathBuffer;
    second.FullPath.Length = Fcb.FullPath.Length;
    second.FullPath.MaximumLength = Fcb.FullPath.Length;
    second.Header.FileSize.QuadPart = (LONGLONG)kFileBytes;

    PrefetchModelSetFillByte(kSecondFill);

    for (ULONG64 offset = 0; offset < kFileBytes; offset += PREFETCH_CHUNK)
    {
        std::vector<unsigned char> received(PREFETCH_CHUNK, 0);

        NTSTATUS status = BlorgPrefetchServeRead(&second,
            MakeRead(received.data(), PREFETCH_CHUNK), offset, PREFETCH_CHUNK);

        PrefetchModelSettle(STATUS_SUCCESS);
        ShimDrainWorkItems();

        if (STATUS_SUCCESS != status)
        {
            continue;
        }

        for (ULONG b = 0; b < PREFETCH_CHUNK; ++b)
        {
            ASSERT_NE(kFirstFill, received[b])
                << "byte " << b << " of the read at offset " << offset
                << " carries the previous file's fill -- a recycled chunk reached a "
                   "reader without its fetch having overwritten it";
        }
    }

    BlorgPrefetchDetach(&second);

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();
}

//
// The property the multi-serve change exists for: a read smaller than
// PREFETCH_CHUNK must leave its slot Ready so the following read, whose
// bytes are in the same chunk, hits instead of missing.
//
// Retiring the slot on the first hit is what the ring used to do, and it
// threw away roughly three quarters of every 512 KB chunk against ~128 KB
// clustered paging reads -- then missed on the very next read.
//
TEST_F(PrefetchKernelTest, APartialReadLeavesTheSlotReadyForTheNextRead)
{
    const ULONG quarter = PREFETCH_CHUNK / 4;

    std::vector<unsigned char> buffer(PREFETCH_CHUNK, 0);

    for (ULONG64 offset = 0; offset < 2 * PREFETCH_CHUNK; offset += PREFETCH_CHUNK)
    {
        BlorgPrefetchServeRead(&Fcb, MakeRead(buffer.data(), PREFETCH_CHUNK), offset, PREFETCH_CHUNK);
    }

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    const ULONG64 hitsBefore = ShimStatistics.PrefetchHits;
    const ULONG64 missesBefore = ShimStatistics.PrefetchMisses;

    //
    // Walk one chunk in quarter-sized reads. The first may hit or miss
    // depending on where the pipeline is aimed; what matters is that the
    // three that follow it are served from the same slot.
    //
    ULONG64 base = 2 * PREFETCH_CHUNK;
    int served = 0;

    for (ULONG q = 0; q < 4; ++q)
    {
        std::vector<unsigned char> got(quarter, 0);

        NTSTATUS status = BlorgPrefetchServeRead(&Fcb, MakeRead(got.data(), quarter),
            base + (ULONG64)q * quarter, quarter);

        if (STATUS_SUCCESS == status)
        {
            ++served;
        }

        PrefetchModelSettle(STATUS_SUCCESS);
        ShimDrainWorkItems();
    }

    const ULONG64 hits = ShimStatistics.PrefetchHits - hitsBefore;
    const ULONG64 misses = ShimStatistics.PrefetchMisses - missesBefore;

    EXPECT_GE(hits, 2u)
        << "only " << hits << " of 4 quarter-chunk reads hit (" << misses
        << " missed), so a slot is still being retired after one partial read "
           "and the rest of each fetched chunk is being thrown away";

    EXPECT_GT(ShimStatistics.PrefetchPartialServes, 0ull)
        << "no partial serve was recorded, so no read left its slot Ready -- "
           "this test is not exercising the path it exists for";

    EXPECT_GT(served, 0)
        << "no quarter-chunk read was served from the ring at all";
}

//
// A re-aim whose new cursor lands just behind the prefetched window must
// keep that window. Generation invalidates in-flight fetches aimed at the
// old position; a slot that has already landed holds file bytes at a known
// offset and is still correct for a reader about to arrive there.
//
// The scenario is a reader that has fallen one chunk behind its own
// pipeline: the ring holds [N, N+2C) Ready, the reader misses at [N-C, N)
// with a streak, and the re-aim cursor becomes N -- exactly where the kept
// run starts.
//
TEST_F(PrefetchKernelTest, AReaimKeepsTheChunksAheadOfItsNewCursor)
{
    const ULONG64 chunk = PREFETCH_CHUNK;

    //
    // Arm and run forward so the ring fetches ahead and holds Ready slots.
    //
    for (ULONG64 offset = 0; offset < 4 * chunk; offset += chunk)
    {
        Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), offset, PREFETCH_CHUNK);
        PrefetchModelSettle(STATUS_SUCCESS);
        ShimDrainWorkItems();
    }

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    //
    // Where the Ready run actually starts, taken from the ring rather than
    // inferred from NextFetchOffset -- Ready slots sit BELOW the cursor, so
    // guessing from it lands inside the window and hits instead of missing.
    //
    ULONG64 readyBase = ~0ull;
    int readyBefore = 0;

    for (ULONG i = 0; i < PREFETCH_DEPTH; ++i)
    {
        if (PrefetchSlotReady == Fcb.PrefetchRing->Hot[i].State)
        {
            ++readyBefore;

            if (Fcb.PrefetchRing->Hot[i].RangeOffset < readyBase)
            {
                readyBase = Fcb.PrefetchRing->Hot[i].RangeOffset;
            }
        }
    }

    ASSERT_GT(readyBefore, 0) << "test needs Ready slots for a re-aim to discard";
    ASSERT_GE(readyBase, 2 * chunk) << "need room below the Ready run to miss in";

    //
    // Idle the ring past the re-aim threshold so a streaked miss re-aims
    // rather than being suppressed as "reader outran its own pipeline".
    //
    for (ULONG i = 0; i < PREFETCH_REAIM_IDLE_SERVES + 2; ++i)
    {
        Fcb.StreamClock++;
    }

    const ULONG64 reaimsBefore = ShimStatistics.PrefetchReaims;
    const ULONG64 discardedBefore = ShimStatistics.PrefetchReaimDiscardedChunks;

    //
    // Two contiguous reads in the already-consumed region below the Ready
    // run, ending exactly where that run begins. Both miss; the second
    // carries a streak, so it re-aims with a cursor of readyBase -- exactly
    // where the kept slots start.
    //
    Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), readyBase - 2 * chunk, PREFETCH_CHUNK);
    Serve(MakeRead(NewBuffer(PREFETCH_CHUNK), PREFETCH_CHUNK), readyBase - chunk, PREFETCH_CHUNK);

    PrefetchModelSettle(STATUS_SUCCESS);
    ShimDrainWorkItems();

    ASSERT_GT(ShimStatistics.PrefetchReaims, reaimsBefore)
        << "no re-aim fired, so this test exercised nothing -- it would pass "
           "against any discard policy at all";

    const ULONG64 discarded = ShimStatistics.PrefetchReaimDiscardedChunks - discardedBefore;

    EXPECT_LT(discarded, static_cast<ULONG64>(readyBefore))
        << "the re-aim discarded " << discarded << " of " << readyBefore
        << " Ready chunks -- fetched bytes thrown away that the reader was "
           "about to arrive at";
}

} // namespace
