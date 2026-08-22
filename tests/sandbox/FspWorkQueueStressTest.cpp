//
// Real-thread stress coverage for the real FspWorkQueue.c: the cancel-safe
// IRP queue (IO_CSQ) and its worker dispatch loop, under genuine concurrent
// producers and consumers -- previously 3.3% covered and, unlike every
// other file touched this session, entirely untested for concurrency.
//
// This is deliberately NOT a KmExploreInterleavings proof. FspDispatch
// blocks on KeWaitForMultipleObjects, and that shim does a real
// WaitForSingleObject/SetEvent (see NtShim.c's own comment: "a real wait,
// because the drain paths under test are genuinely cross-thread") --
// exactly the kind of OS-level block the cooperative scheduler cannot
// resolve (Scheduler.h: "an OS-blocking acquire would deadlock instantly
// here, because the thread holding the lock is suspended and only the
// scheduler can wake it"). Making KeWaitForMultipleObjects
// scheduler-cooperative would fix that, but it is shared infrastructure
// SocketKernelTest.cpp depends on for the opposite reason -- real
// cross-thread waits -- so that change is out of scope here.
//
// What this proves instead, matching DispatchStressTest.cpp's acknowledged
// tradeoff: real OS threads under real preemption, not exhaustive, not
// deterministic, but able to interrupt between any two instructions,
// lock or no lock -- which is exactly what a stress test can catch that
// hand-written sequential tests cannot.
//
// PsCreateSystemThread is a no-op in this sandbox (DispatchModel.c), so
// CreateWorkQueue() initializes the real CSQ/events/spinlock but starts no
// workers of its own -- the worker threads below are spawned directly
// against the real (non-static) FspDispatch, which is the actual
// dequeue-and-dispatch loop under test.
//

#include <gtest/gtest.h>

#include <vector>
#include <memory>

extern "C" {
#include "SandboxSocket.h"

// Not declared in any header -- FspWorkQueue.c's only other caller is
// PsCreateSystemThread, which is a no-op in this sandbox.
VOID FspDispatch(PVOID StartContext);
}

#include "DeviceKindScope.h"

namespace
{

struct StressIrpSlot
{
    FILE_OBJECT FileObject;
    IO_STACK_LOCATION Stack;
    IRP Irp;
};

struct StressState
{
    volatile long Ready;
    volatile long Barrier;
};

struct ProducerArg
{
    StressState* State;
    int Iterations;
    std::vector<std::unique_ptr<StressIrpSlot>> Slots;
};

DWORD WINAPI WorkerThreadMain(LPVOID)
{
    FspDispatch(NULL);
    return 0;
}

DWORD WINAPI ProducerThreadMain(LPVOID param)
{
    ProducerArg* arg = (ProducerArg*)param;
    StressState* state = arg->State;

    arg->Slots.reserve(arg->Iterations);

    InterlockedIncrement(&state->Ready);

    while (!ReadNoFence(&state->Barrier))
    {
        SwitchToThread();
    }

    for (int i = 0; i < arg->Iterations; ++i)
    {
        arg->Slots.push_back(std::make_unique<StressIrpSlot>());
        StressIrpSlot* slot = arg->Slots.back().get();
        memset(slot, 0, sizeof(*slot));

        slot->Stack.MajorFunction = IRP_MJ_READ;
        slot->Stack.FileObject = &slot->FileObject;
        slot->Stack.Parameters.Read.Length = 0;
        slot->Irp.StackLocation = &slot->Stack;

        //
        // A zero-length read short-circuits at the very top of
        // BlorgVolumeRead, before it ever touches FsContext -- isolating
        // the queue's own insert/dispatch/complete mechanics from Read.c's
        // logic, which already has its own coverage.
        //
        NTSTATUS status = FsdPostRequest(&slot->Irp, &slot->Stack);

        if (STATUS_PENDING != status)
        {
            ADD_FAILURE() << "FsdPostRequest returned " << std::hex << status
                          << " instead of STATUS_PENDING on iteration " << i;
        }

        if (0 == (i % 37))
        {
            SwitchToThread();
        }
    }

    return 0;
}

class FspWorkQueueStressTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("FspWorkQueueStressTest teardown");
    }
};

//
// Many real producer threads posting concurrently, many real worker
// threads pulling from the same IO_CSQ concurrently. The invariant: every
// posted IRP is completed exactly once -- not lost (queue corruption
// dropping an entry), not delivered to two workers at once (the CSQ lock
// failing to serialize insert/remove), and not double-completed (two
// workers picking up the same IRP). IoCompleteRequest's own shim already
// treats a double-completion as a hard violation
// (KmReportViolation(KmViolationLifetime, ...)), so that failure mode
// surfaces on its own; this test's own assertions cover the other two.
//
TEST_F(FspWorkQueueStressTest, ConcurrentPostersAndWorkersCompleteEveryIrpExactlyOnce)
{
    ShimReset();

    ASSERT_EQ(STATUS_SUCCESS, CreateWorkQueue());

    const int kWorkers = 8;
    const int kProducers = 8;
    const int kIterationsPerProducer = 500;

    HANDLE workerHandles[kWorkers];

    for (int i = 0; i < kWorkers; ++i)
    {
        workerHandles[i] = CreateThread(NULL, 0, WorkerThreadMain, NULL, 0, NULL);
        ASSERT_NE((HANDLE)NULL, workerHandles[i]);
    }

    StressState state = {};

    std::vector<std::unique_ptr<ProducerArg>> producerArgs;
    HANDLE producerHandles[kProducers];

    for (int i = 0; i < kProducers; ++i)
    {
        producerArgs.push_back(std::make_unique<ProducerArg>());
        producerArgs.back()->State = &state;
        producerArgs.back()->Iterations = kIterationsPerProducer;
        producerHandles[i] = CreateThread(NULL, 0, ProducerThreadMain, producerArgs.back().get(), 0, NULL);
        ASSERT_NE((HANDLE)NULL, producerHandles[i]);
    }

    while (ReadNoFence(&state.Ready) < kProducers)
    {
        SwitchToThread();
    }

    InterlockedExchange(&state.Barrier, 1);

    ASSERT_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(kProducers, producerHandles, TRUE, 30000))
        << "producer threads did not finish posting within 30s";

    for (int i = 0; i < kProducers; ++i)
    {
        CloseHandle(producerHandles[i]);
    }

    //
    // Poll for every posted IRP to complete before tearing the queue down,
    // rather than racing DestroyWorkQueue's own drain-and-cancel against
    // still-active workers -- that race is real but a different, separate
    // question (graceful vs. torn-down shutdown) from the one this test
    // asks (does concurrent post/dispatch lose or duplicate work).
    //
    int totalPosted = kProducers * kIterationsPerProducer;
    DWORD start = GetTickCount();

    for (;;)
    {
        int completed = 0;

        for (auto& arg : producerArgs)
        {
            for (auto& slot : arg->Slots)
            {
                if (slot->Irp.CompletionCount > 0)
                {
                    ++completed;
                }
            }
        }

        if (completed >= totalPosted)
        {
            break;
        }

        ASSERT_LT(GetTickCount() - start, 30000u)
            << "not every posted IRP completed within 30s -- " << completed
            << "/" << totalPosted << " completed, queue or dispatch lost work";

        SwitchToThread();
    }

    DestroyWorkQueue();

    ASSERT_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(kWorkers, workerHandles, TRUE, 30000))
        << "worker threads did not exit within 30s of DestroyWorkQueue";

    for (int i = 0; i < kWorkers; ++i)
    {
        CloseHandle(workerHandles[i]);
    }

    int checked = 0;

    for (auto& arg : producerArgs)
    {
        for (auto& slot : arg->Slots)
        {
            EXPECT_EQ(1, slot->Irp.CompletionCount)
                << "an IRP completed " << slot->Irp.CompletionCount
                << " times instead of exactly once";
            EXPECT_EQ(STATUS_SUCCESS, slot->Irp.IoStatus.Status)
                << "a zero-length read must succeed synchronously once dispatched, "
                   "not race into a cancellation";
            EXPECT_EQ(0u, slot->Irp.IoStatus.Information);
            ++checked;
        }
    }

    ASSERT_EQ(totalPosted, checked);
}

///////////////////////////////////////////////////////////////////////////
// Per-IRP result isolation within one worker batch
///////////////////////////////////////////////////////////////////////////

//
// FspDispatch declares `result` outside its inner dequeue loop, and the
// switch's default arm does not assign it. So an IRP whose major function
// has no case inherits whatever the *previous* IRP in the same batch
// returned -- and if that was STATUS_PENDING, the
// `if (STATUS_PENDING != result)` guard skips completion entirely and the
// IRP is stranded: already removed from the CSQ, so not even
// DestroyWorkQueue's drain-and-cancel can find it. The caller waits
// forever.
//
// Not reachable today: every current poster (Create.c, Read.c, DirCtrl.c,
// and OplockComplete, which only ever carries CREATE and READ -- FsCtrl.c
// uses the 3-arg FsRtlOplockFsctrl, which takes no completion routine) is
// one of the three majors the switch handles. It is reachable the moment a
// fourth major is posted without a matching case, which the post path is
// already built for: PrePostIrp handles IRP_MJ_WRITE, IRP_MJ_QUERY_EA and
// IRP_MJ_SET_EA, none of which FspDispatch's switch knows about, and the
// write path is tracked work rather than a hypothetical.
//
// Both IRPs are posted before any worker exists (PsCreateSystemThread is a
// no-op here), so the single worker started afterwards drains both in one
// pass of the inner loop -- which is the precondition for the leak.
//
TEST_F(FspWorkQueueStressTest, UnhandledMajorFunctionIsCompletedRatherThanStranded)
{
    SandboxInitialize();

    static const SANDBOX_STEP script[] =
    {
        { SandboxStepDeliver,
          (const unsigned char*)"HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ",
          sizeof("HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\n\r\nWXYZ") - 1,
          STATUS_SUCCESS, TRUE }
    };
    SandboxSetPeerScript(script, RTL_NUMBER_OF(script));

    PDEVICE_OBJECT volume = StructsModelCreateVolume();
    ASSERT_NE(nullptr, volume);
    ScopedDeviceKind asVolume(&global.VolumeDeviceObject, volume);

    wchar_t pathBuffer[] = L"\\media\\batch.bin";
    UNICODE_STRING path;
    path.Buffer = pathBuffer;
    path.Length = (USHORT)(wcslen(pathBuffer) * sizeof(wchar_t));
    path.MaximumLength = path.Length;

    PFCB fcb = nullptr;
    ASSERT_EQ(STATUS_SUCCESS,
        BlorgCreateFCB(&fcb, (CSHORT)BLORGFS_FCB_SIGNATURE, &path, volume, 4096));
    InitializeListHead(&fcb->Links);

    ASSERT_EQ(STATUS_SUCCESS, CreateWorkQueue());

    //
    // A non-cached read the worker will drive to a real (inline-scripted)
    // fetch. MdlAddress is set up front so PrePostIrp's LockUserBuffer
    // no-ops on it and this test owns the MDL's lifetime outright.
    //
    unsigned char readBuffer[4] = {};
    StressIrpSlot readSlot{};
    SECTION_OBJECT_POINTERS sectionObject{};
    readSlot.FileObject.FsContext = fcb;
    readSlot.FileObject.DeviceObject = volume;
    readSlot.FileObject.SectionObjectPointer = &sectionObject;
    readSlot.Stack.MajorFunction = IRP_MJ_READ;
    readSlot.Stack.FileObject = &readSlot.FileObject;
    readSlot.Stack.DeviceObject = volume;
    readSlot.Stack.Parameters.Read.Length = sizeof(readBuffer);
    readSlot.Stack.Parameters.Read.ByteOffset.QuadPart = 0;
    readSlot.Irp.StackLocation = &readSlot.Stack;
    readSlot.Irp.Flags = IRP_NOCACHE;
    readSlot.Irp.MdlAddress = IoAllocateMdl(readBuffer, sizeof(readBuffer), FALSE, FALSE, nullptr);
    ASSERT_NE(nullptr, readSlot.Irp.MdlAddress);

    // A major the dispatch switch has no case for.
    StressIrpSlot writeSlot{};
    writeSlot.FileObject.DeviceObject = volume;
    writeSlot.Stack.MajorFunction = IRP_MJ_WRITE;
    writeSlot.Stack.FileObject = &writeSlot.FileObject;
    writeSlot.Stack.DeviceObject = volume;
    writeSlot.Stack.Parameters.Write.Length = 0;
    writeSlot.Irp.StackLocation = &writeSlot.Stack;

    ASSERT_EQ(STATUS_PENDING, FsdPostRequest(&readSlot.Irp, &readSlot.Stack));
    ASSERT_EQ(STATUS_PENDING, FsdPostRequest(&writeSlot.Irp, &writeSlot.Stack));

    HANDLE worker = CreateThread(NULL, 0, WorkerThreadMain, NULL, 0, NULL);
    ASSERT_NE((HANDLE)NULL, worker);

    DWORD start = GetTickCount();

    while (0 == writeSlot.Irp.CompletionCount && (GetTickCount() - start) < 5000)
    {
        SwitchToThread();
    }

    EXPECT_EQ(1, writeSlot.Irp.CompletionCount)
        << "an IRP whose major function the dispatch switch does not handle must be "
           "completed with a failure status, not silently stranded because the previous "
           "IRP in the same batch returned STATUS_PENDING";
    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, writeSlot.Irp.IoStatus.Status)
        << "the unhandled-major contract is STATUS_INVALID_DEVICE_REQUEST";

    EXPECT_EQ(1, readSlot.Irp.CompletionCount)
        << "the preceding read must still complete exactly once";

    DestroyWorkQueue();

    ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(worker, 30000));
    CloseHandle(worker);

    SandboxDrainCompletions();
    ShimDrainWorkItems();
    CleanupWskClient();

    IoFreeMdl(readSlot.Irp.MdlAddress);
    BlorgFreeFileContext(fcb, volume);
    StructsModelDestroyVolume(volume);
}

//
// The teardown drain and the cancel callback are the only two places that
// complete a posted IRP without running its handler, and an IRP that an
// async completion re-queued is carrying that completion's result in
// DriverContext[1] -- allocated by BlorgCreateComplete, and normally
// consumed at the top of BlorgVolumeCreate's next pass. When that pass
// never happens the block has no other owner.
//
// Driven through FsdRequeueRequest with the same stash-then-flag-then-
// requeue sequence BlorgCreateComplete uses, so the IRP arrives in the
// queue in exactly the shape production leaves it in. No worker runs here
// (PsCreateSystemThread is a no-op in this sandbox), so the IRP is still
// queued when DestroyWorkQueue drains it -- which is the scenario: an
// unload landing between the network result and the pass that would have
// used it.
//
TEST_F(FspWorkQueueStressTest, TeardownDrainFreesTheStashOnAnIrpItCancels)
{
    ASSERT_EQ(STATUS_SUCCESS, CreateWorkQueue());

    StressIrpSlot slot{};
    slot.Stack.MajorFunction = IRP_MJ_CREATE;
    slot.Stack.FileObject = &slot.FileObject;
    slot.Irp.StackLocation = &slot.Stack;

    const SIZE_T before = ShimPoolOutstanding();

    PVOID stash = ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(DIRECTORY_ENTRY_METADATA), 'CRET');
    ASSERT_NE(nullptr, stash);

    slot.Irp.Tail.Overlay.DriverContext[1] = stash;
    SetIrpContextFlag(&slot.Irp, IRP_CONTEXT_FLAG_NET_DONE);

    ASSERT_EQ(STATUS_PENDING, FsdRequeueRequest(&slot.Irp));

    DestroyWorkQueue();

    EXPECT_EQ(1u, slot.Irp.CompletionCount) << "a drained IRP must still be completed exactly once";
    EXPECT_EQ(STATUS_CANCELLED, slot.Irp.IoStatus.Status);

    EXPECT_EQ(before, ShimPoolOutstanding())
        << "the stash the re-queue attached was orphaned when the drain cancelled the IRP";
}

} // namespace
