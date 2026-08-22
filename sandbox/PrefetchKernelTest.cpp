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
#include "..\Driver.h"

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
// A read whose range is entirely covered by a resident slot, but which
// starts inside that slot rather than exactly on its boundary, is a MISS
// today: the lookup tests Offset == RangeOffset, not containment. The ring
// already holds every byte being asked for and the read still pays a full
// HTTP round trip to fetch them again.
//
// This is pinned deliberately rather than treated as a bug to fix in place.
// PrefetchNearMisses exists to size the problem on real workloads before
// anyone widens the lookup, because containment matching also means copying
// from a slot interior, which the hit path does not do today. If the lookup
// is changed, this test flips to STATUS_SUCCESS -- that is the signal to
// update it, the counter's meaning, and Prefetch.h's lookup contract
// together, not to delete it.
//
TEST_F(PrefetchKernelTest, InteriorOffsetCoveredByASlotIsANearMiss)
{
    unsigned char* a = NewBuffer(PREFETCH_CHUNK);
    unsigned char* b = NewBuffer(PREFETCH_CHUNK);

    Serve(MakeRead(a, PREFETCH_CHUNK), 0, PREFETCH_CHUNK);
    Serve(MakeRead(b, PREFETCH_CHUNK), PREFETCH_CHUNK, PREFETCH_CHUNK);

    ASSERT_NE(nullptr, Fcb.PrefetchRing);

    PrefetchModelCompleteAllFetches(STATUS_SUCCESS);

    const ULONG64 before = ShimStatistics.PrefetchNearMisses;

    const ULONG interiorLength = 4096;
    unsigned char* target = NewBuffer(interiorLength);
    PIRP irp = MakeRead(target, interiorLength);

    NTSTATUS status = Serve(irp, (2 * PREFETCH_CHUNK) + interiorLength, interiorLength);

    EXPECT_EQ(STATUS_NOT_FOUND, status)
        << "exact-offset lookup should still reject an interior read; if this now "
           "succeeds the lookup was widened -- see the comment above";

    EXPECT_EQ(before + 1, ShimStatistics.PrefetchNearMisses)
        << "a miss whose bytes were already in the ring must be counted as a near miss";
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

} // namespace
