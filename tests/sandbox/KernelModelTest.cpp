//
// Negative controls for the kernel model itself, plus the multithreaded
// synchronisation tests that need real threads.
//
// A model that enforces nothing passes everything. These tests commit
// each violation deliberately and assert the model reports it -- so a
// green run of the Socket suite means the rules were checked, not that
// the checker was asleep. Without this file the rest of the sandbox is
// unfalsifiable.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
}

namespace
{

class KernelModelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        WskModelReset();
    }

    void TearDown() override
    {
        KmTakeViolation();
    }
};

///////////////////////////////////////////////////////////////////////////
// IRQL enforcement
///////////////////////////////////////////////////////////////////////////

//
// Paged pool cannot be touched above APC_LEVEL. This is the mistake that
// produces an IRQL_NOT_LESS_OR_EQUAL bugcheck far from its cause, so the
// model has to catch it at the allocation.
//
TEST_F(KernelModelTest, PagedAllocationAtDispatchIsRejected)
{
    KmSetIrql(DISPATCH_LEVEL);
    KmExpectViolation(KmViolationIrql);

    PVOID block = ExAllocatePoolZero(PagedPool, 64, 'tseT');

    EXPECT_EQ(KmViolationIrql, KmTakeViolation()) << "the model failed to catch a paged allocation at DISPATCH";

    KmSetIrql(PASSIVE_LEVEL);

    if (block)
    {
        ExFreePool(block);
    }
}

//
// Non-paged pool at DISPATCH is legal and must NOT be flagged -- a model
// that cried wolf here would make the driver's whole completion path
// untestable.
//
TEST_F(KernelModelTest, NonPagedAllocationAtDispatchIsAllowed)
{
    KmSetIrql(DISPATCH_LEVEL);

    PVOID block = ExAllocatePoolZero(NonPagedPoolNx, 64, 'tseT');

    ASSERT_NE(nullptr, block);
    ExFreePool(block);

    KmSetIrql(PASSIVE_LEVEL);
    KmAssertQuiescent("NonPagedAllocationAtDispatchIsAllowed");
}

//
// KeWaitForSingleObject is PASSIVE-only. Blocking at DISPATCH is a
// deadlock of the whole processor, which is exactly the sort of thing a
// drain path could acquire by accident.
//
TEST_F(KernelModelTest, BlockingAtDispatchIsRejected)
{
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, TRUE);

    KmSetIrql(DISPATCH_LEVEL);
    KmExpectViolation(KmViolationIrql);

    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);

    EXPECT_EQ(KmViolationIrql, KmTakeViolation());

    KmSetIrql(PASSIVE_LEVEL);
}

///////////////////////////////////////////////////////////////////////////
// Lock rules
///////////////////////////////////////////////////////////////////////////

//
// Taking a spin lock you already hold is an unconditional self-deadlock
// in the kernel; there is no recursive acquisition.
//
TEST_F(KernelModelTest, RecursiveSpinLockAcquisitionIsRejected)
{
    KM_LOCK lock;
    KmInitializeLock(&lock, "recursion-test");

    unsigned char irql = KmAcquireLock(&lock);

    KmExpectViolation(KmViolationLockRecursion);
    KmAcquireLock(&lock);

    EXPECT_EQ(KmViolationLockRecursion, KmTakeViolation());

    KmReleaseLock(&lock, irql);
}

//
// The one that finds real bugs. Two locks taken in both orders by
// different paths is a latent AB/BA deadlock; it may never actually
// deadlock in testing, so detecting the *inversion* rather than waiting
// for the hang is the only practical way to find it.
//
TEST_F(KernelModelTest, LockOrderInversionIsDetected)
{
    KM_LOCK a;
    KM_LOCK b;

    KmInitializeLock(&a, "lock-A");
    KmInitializeLock(&b, "lock-B");

    // Establish A -> B as the order.
    unsigned char irqlA = KmAcquireLock(&a);
    unsigned char irqlB = KmAcquireLock(&b);
    KmReleaseLock(&b, irqlB);
    KmReleaseLock(&a, irqlA);

    // Now take them B -> A. Same two locks, opposite order.
    irqlB = KmAcquireLock(&b);

    KmExpectViolation(KmViolationLockOrder);
    irqlA = KmAcquireLock(&a);

    EXPECT_EQ(KmViolationLockOrder, KmTakeViolation())
        << "the model failed to detect an AB/BA lock-order inversion";

    KmReleaseLock(&a, irqlA);
    KmReleaseLock(&b, irqlB);
}

//
// A consistent order across many acquisitions must stay silent, or the
// detector is useless in a driver that legitimately nests locks.
//
TEST_F(KernelModelTest, ConsistentLockOrderIsNotFlagged)
{
    KM_LOCK a;
    KM_LOCK b;
    KM_LOCK c;

    KmInitializeLock(&a, "ordered-A");
    KmInitializeLock(&b, "ordered-B");
    KmInitializeLock(&c, "ordered-C");

    for (int i = 0; i < 50; ++i)
    {
        unsigned char irqlA = KmAcquireLock(&a);
        unsigned char irqlB = KmAcquireLock(&b);
        unsigned char irqlC = KmAcquireLock(&c);

        KmReleaseLock(&c, irqlC);
        KmReleaseLock(&b, irqlB);
        KmReleaseLock(&a, irqlA);
    }

    SUCCEED();
}

//
// Holding a spin lock raises to DISPATCH and releasing restores. The
// driver's IRQL reasoning depends on this, and so does the paged-pool
// check above -- if the raise did not happen, an allocation under a lock
// would look legal.
//
TEST_F(KernelModelTest, SpinLockRaisesAndRestoresIrql)
{
    KM_LOCK lock;
    KmInitializeLock(&lock, "irql-test");

    ASSERT_EQ(PASSIVE_LEVEL, KmGetIrql());

    unsigned char old = KmAcquireLock(&lock);

    EXPECT_EQ(DISPATCH_LEVEL, KmGetIrql()) << "a held spin lock must hold IRQL at DISPATCH";

    KmReleaseLock(&lock, old);

    EXPECT_EQ(PASSIVE_LEVEL, KmGetIrql()) << "IRQL was not restored on release";
}

///////////////////////////////////////////////////////////////////////////
// Pool rules
///////////////////////////////////////////////////////////////////////////

TEST_F(KernelModelTest, PoolOverrunIsDetected)
{
    unsigned char* block = (unsigned char*)ExAllocatePoolUninitialized(NonPagedPoolNx, 16, 'tseT');
    ASSERT_NE(nullptr, block);

    // One byte past the end -- the classic off-by-one.
    block[16] = 0x41;

    KmExpectViolation(KmViolationPool);
    ExFreePool(block);

    EXPECT_EQ(KmViolationPool, KmTakeViolation()) << "the guard did not catch a one-byte overrun";
}

TEST_F(KernelModelTest, DoubleFreeIsDetected)
{
    PVOID block = ExAllocatePoolZero(NonPagedPoolNx, 32, 'tseT');
    ASSERT_NE(nullptr, block);

    ExFreePool(block);

    KmExpectViolation(KmViolationPool);
    ExFreePool(block);

    EXPECT_EQ(KmViolationPool, KmTakeViolation());
}

TEST_F(KernelModelTest, LeakIsDetected)
{
    PVOID block = ExAllocatePoolZero(NonPagedPoolNx, 32, 'tseT');
    ASSERT_NE(nullptr, block);

    KmExpectViolation(KmViolationLifetime);
    KmAssertQuiescent("LeakIsDetected");

    EXPECT_EQ(KmViolationLifetime, KmTakeViolation()) << "quiescence check missed a leaked allocation";

    ExFreePool(block);
}

///////////////////////////////////////////////////////////////////////////
// DPC and timer rules
///////////////////////////////////////////////////////////////////////////

namespace
{
    int DpcRunCount = 0;
    unsigned char DpcObservedIrql = 0xFF;

    void CountingDpc(KM_DPC* Dpc, void* Context, void* Arg1, void* Arg2)
    {
        (void)Dpc; (void)Context; (void)Arg1; (void)Arg2;

        DpcRunCount++;
        DpcObservedIrql = KmGetIrql();
    }
}

//
// The kernel queues a given DPC object at most once. Drivers rely on that
// for dedup -- the prefetch pump does exactly this -- so a model that
// queued twice would hide a real double-run bug.
//
TEST_F(KernelModelTest, DpcQueuesAtMostOnce)
{
    DpcRunCount = 0;

    KM_DPC dpc;
    KmInitializeDpc(&dpc, CountingDpc, nullptr);

    EXPECT_TRUE(KmInsertQueueDpc(&dpc, nullptr, nullptr));
    EXPECT_FALSE(KmInsertQueueDpc(&dpc, nullptr, nullptr)) << "a second queue of the same DPC must be refused";

    EXPECT_EQ(1, KmDrainDpcs());
    EXPECT_EQ(1, DpcRunCount);
}

TEST_F(KernelModelTest, DpcRunsAtDispatchLevel)
{
    DpcRunCount = 0;
    DpcObservedIrql = 0xFF;

    KM_DPC dpc;
    KmInitializeDpc(&dpc, CountingDpc, nullptr);

    ASSERT_EQ(PASSIVE_LEVEL, KmGetIrql());

    KmInsertQueueDpc(&dpc, nullptr, nullptr);
    KmDrainDpcs();

    EXPECT_EQ(DISPATCH_LEVEL, DpcObservedIrql);
    EXPECT_EQ(PASSIVE_LEVEL, KmGetIrql()) << "IRQL was not restored after the DPC";
}

//
// The virtual clock is what makes a 30-second watchdog testable at all,
// so its boundary behaviour has to be exact: a timer due at T fires when
// the clock reaches T, not before.
//
TEST_F(KernelModelTest, TimerFiresExactlyAtItsDueTime)
{
    DpcRunCount = 0;

    KM_DPC dpc;
    KM_TIMER timer;

    KmInitializeDpc(&dpc, CountingDpc, nullptr);
    KmInitializeTimer(&timer);

    // Negative is relative, in 100ns units: 1000ms.
    KmSetTimer(&timer, -1000LL * 10000LL, &dpc);

    EXPECT_EQ(0, KmAdvanceTime(999));
    EXPECT_EQ(0, DpcRunCount) << "timer fired early";

    EXPECT_EQ(1, KmAdvanceTime(1));
    EXPECT_EQ(1, DpcRunCount);
}

TEST_F(KernelModelTest, CancelledTimerNeverFires)
{
    DpcRunCount = 0;

    KM_DPC dpc;
    KM_TIMER timer;

    KmInitializeDpc(&dpc, CountingDpc, nullptr);
    KmInitializeTimer(&timer);

    KmSetTimer(&timer, -1000LL * 10000LL, &dpc);

    EXPECT_TRUE(KmCancelTimer(&timer)) << "cancel must report the timer was still armed";
    EXPECT_EQ(0, KmAdvanceTime(10000));
    EXPECT_EQ(0, DpcRunCount);

    // Cancelling again reports it was not armed, which is how the
    // driver's refcount protocol decides who owns the free.
    EXPECT_FALSE(KmCancelTimer(&timer));
}

///////////////////////////////////////////////////////////////////////////
// Concurrency against the real connection pool
///////////////////////////////////////////////////////////////////////////

namespace
{
    struct PoolStressContext
    {
        KM_BARRIER* Barrier;
        int Iterations;
        volatile long Acquired;
        volatile long Failures;
    };

    void AcquireRecordNoop(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID Context)
    {
        (void)Reused;

        PKSOCKET* out = (PKSOCKET*)Context;

        *out = NT_SUCCESS(Status) ? Socket : nullptr;
    }

    //
    // Hammers acquire/release from several threads at once. The pool is a
    // spinlock-guarded list, so this is where a missing lock, a wrong
    // release, or an inconsistent order would show up -- and the model is
    // watching all three while it runs.
    //
    void PoolStressThread(void* Parameter)
    {
        PoolStressContext* context = (PoolStressContext*)Parameter;

        SOCKADDR_IN address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(80);

        KmBarrierWait(context->Barrier);

        for (int i = 0; i < context->Iterations; ++i)
        {
            PKSOCKET socket = nullptr;

            AcquireReusableWskSocketAsync((PSOCKADDR)&address, FALSE, AcquireRecordNoop, &socket);

            if (!socket)
            {
                InterlockedIncrement(&context->Failures);
                continue;
            }

            InterlockedIncrement(&context->Acquired);

            KmJitter();

            ReleaseReusableWskSocket(socket);
        }
    }
}

//
// The pool under real contention. What is being checked is not throughput
// but that nothing in the model fires: no lock-order inversion, no
// recursive acquisition, no release by the wrong thread, no pool
// corruption, and -- via the final drain -- no socket lost or
// double-counted.
//
TEST_F(KernelModelTest, ConnectionPoolSurvivesConcurrentAcquireRelease)
{
    ASSERT_EQ(STATUS_SUCCESS, InitialiseWskClient());

    const int kThreads = 4;
    const int kIterations = 250;

    KM_BARRIER barrier;
    KmInitializeBarrier(&barrier, kThreads);

    PoolStressContext context = {};
    context.Barrier = &barrier;
    context.Iterations = kIterations;

    KM_THREAD* threads[kThreads] = {};

    for (int i = 0; i < kThreads; ++i)
    {
        threads[i] = KmStartThread(PoolStressThread, &context);
    }

    for (int i = 0; i < kThreads; ++i)
    {
        KmJoinThread(threads[i]);
    }

    EXPECT_EQ(kThreads * kIterations, context.Acquired) << "an acquire failed under contention";
    EXPECT_EQ(0, context.Failures);

    CleanupWskClient();

    EXPECT_EQ(0, KmObjectsLive(KmObjectSocket)) << "sockets leaked under concurrent use";

    KmAssertQuiescent("ConnectionPoolSurvivesConcurrentAcquireRelease");
}

//
// Concurrent watchdog arm/disarm. Every thread issues an operation that
// completes normally, so every watchdog must be disarmed by its own
// completion -- with the timer list being mutated from several threads at
// once. A shared or mis-scoped timer shows up as a stray fire afterwards.
//
TEST_F(KernelModelTest, ConcurrentWatchdogArmAndDisarmIsClean)
{
    ASSERT_EQ(STATUS_SUCCESS, InitialiseWskClient());

    struct Local
    {
        static void Completion(NTSTATUS Status, ULONG_PTR Bytes, PVOID Context)
        {
            (void)Status;
            (void)Bytes;
            InterlockedIncrement((volatile long*)Context);
        }

        static void Body(void* Parameter)
        {
            volatile long* completions = (volatile long*)Parameter;

            SOCKADDR_IN address = {};
            address.sin_family = AF_INET;
            address.sin_port = htons(80);

            for (int i = 0; i < 100; ++i)
            {
                PKSOCKET socket = nullptr;

                AcquireReusableWskSocketAsync((PSOCKADDR)&address, TRUE, AcquireRecordNoop, &socket);

                if (!socket)
                {
                    continue;
                }

                unsigned char buffer[16] = {};

                ReceiveWskAsync(socket, buffer, sizeof(buffer), 0, Completion, (PVOID)completions);

                KmJitter();

                CloseWskSocketAsync(socket);
            }
        }
    };

    WSK_MODEL_BEHAVIOUR inlineOk = {};
    inlineOk.Completion = WskModelInline;
    inlineOk.Status = STATUS_SUCCESS;
    WskModelSetReceiveBehaviour(&inlineOk);

    volatile long completions = 0;

    const int kThreads = 4;
    KM_THREAD* threads[kThreads] = {};

    for (int i = 0; i < kThreads; ++i)
    {
        threads[i] = KmStartThread(Local::Body, (void*)&completions);
    }

    for (int i = 0; i < kThreads; ++i)
    {
        KmJoinThread(threads[i]);
    }

    EXPECT_EQ(kThreads * 100, completions);

    //
    // Every watchdog should have been disarmed by its completion. Any
    // timer still armed fires here, on a context that is long gone.
    //
    EXPECT_EQ(0, KmAdvanceTime(120000)) << "a watchdog outlived its operation";

    CleanupWskClient();
    KmAssertQuiescent("ConcurrentWatchdogArmAndDisarmIsClean");
}

} // namespace
