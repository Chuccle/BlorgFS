//
// Real-thread stress counterpart to DispatchSchedTest, for exactly the gap
// DispatchSchedTest documents it cannot close.
//
// The systematic scheduler proves "no interleaving of the synchronization
// primitives the code actually calls corrupts state" -- and, by mutation,
// proved it does NOT prove "the code calls the primitives it needs to":
// deleting Header.Resource's acquire/release from OpenExistingFcb passed
// the scheduler proof at 252 schedules, because a missing lock leaves no
// scheduling point for the explorer to preempt at.
//
// Real OS threads under real preemption do not have that limitation --
// the CPU can interrupt between any two instructions, lock or no lock. It
// is not exhaustive and it is not deterministic: whether an actual missing
// lock manifests as a wrong count on any given run depends on scheduler
// luck. This is the acknowledged, honest fallback for that gap, not a
// replacement for it -- run under ASan and with enough iterations and
// threads to make the adversarial window as wide as practical.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "Scheduler.h"
}

#include "DeviceKindScope.h"

namespace
{

struct StressOpener
{
    FILE_OBJECT FileObject;
    IO_SECURITY_CONTEXT SecurityContext;
    IO_STACK_LOCATION CreateStack;
    IRP CreateIrp;
    IO_STACK_LOCATION CleanupStack;
    IRP CleanupIrp;
    IO_STACK_LOCATION CloseStack;
    IRP CloseIrp;
};

struct StressState
{
    PDEVICE_OBJECT Volume;
    UNICODE_STRING Path;

    volatile long Barrier;
    volatile long Ready;
};

void PrepareStressOpener(StressOpener* opener, StressState* state)
{
    memset(opener, 0, sizeof(*opener));

    opener->FileObject.FileName = state->Path;
    opener->FileObject.DeviceObject = state->Volume;
    opener->SecurityContext.DesiredAccess = FILE_READ_DATA;

    opener->CreateStack.MajorFunction = IRP_MJ_CREATE;
    opener->CreateStack.FileObject = &opener->FileObject;
    opener->CreateStack.DeviceObject = state->Volume;
    opener->CreateStack.Parameters.Create.Options = (ULONG)FILE_OPEN << 24;
    opener->CreateStack.Parameters.Create.ShareAccess =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    opener->CreateStack.Parameters.Create.SecurityContext = &opener->SecurityContext;
    opener->CreateIrp.StackLocation = &opener->CreateStack;

    opener->CleanupStack.MajorFunction = IRP_MJ_CLEANUP;
    opener->CleanupStack.FileObject = &opener->FileObject;
    opener->CleanupStack.DeviceObject = state->Volume;
    opener->CleanupIrp.StackLocation = &opener->CleanupStack;

    opener->CloseStack.MajorFunction = IRP_MJ_CLOSE;
    opener->CloseStack.FileObject = &opener->FileObject;
    opener->CloseStack.DeviceObject = state->Volume;
    opener->CloseIrp.StackLocation = &opener->CloseStack;
}

struct ThreadArg
{
    StressState* State;
    int ThreadIndex;
    int Iterations;
    volatile long* SeenBadCount;
};

DWORD WINAPI StressOpenCloseThread(LPVOID Param)
{
    ThreadArg* arg = (ThreadArg*)Param;
    StressState* state = arg->State;

    InterlockedIncrement(&state->Ready);

    while (!ReadNoFence(&state->Barrier))
    {
        SwitchToThread();
    }

    for (int i = 0; i < arg->Iterations; ++i)
    {
        StressOpener opener;
        PrepareStressOpener(&opener, state);

        BlorgCreate(state->Volume, &opener.CreateIrp);

        if (NT_SUCCESS(opener.CreateIrp.IoStatus.Status))
        {
            //
            // A few instructions of real, uncontrolled work between open
            // and close -- widening exactly the window a lost update would
            // need, on real hardware rather than at a cooperative
            // scheduling point.
            //
            SwitchToThread();

            BlorgCleanup(state->Volume, &opener.CleanupIrp);
            BlorgClose(state->Volume, &opener.CloseIrp);
        }
    }

    return 0;
}

class DispatchStressTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        KmAssertQuiescent("DispatchStressTest teardown");
    }
};

//
// Many real threads, many iterations, opening and closing the same file
// concurrently through the real dispatch entries. The invariant checked
// is the same as the scheduler proof's: at the end, with every handle
// closed, ShareAccess.OpenCount and RefCount must both be back to zero --
// a lost update anywhere in the open/close sequence leaves one of them
// permanently offset, which a single run surfaces just as reliably as a
// crash would.
//
TEST_F(DispatchStressTest, ManyThreadsOpenAndCloseWithoutLosingCounts)
{
    ShimReset();

    PDEVICE_OBJECT volume = StructsModelCreateVolume();
    ASSERT_NE(nullptr, volume);

    ScopedDeviceKind asVolume(&global.VolumeDeviceObject, volume);

    DEVICE_OBJECT diskDevice = {};
    VPB diskVpb = {};
    diskDevice.Vpb = &diskVpb;
    global.DiskDeviceObject = &diskDevice;

    ASSERT_EQ(STATUS_SUCCESS, BlorgNodeTableInit(volume));

    UNICODE_STRING rootName = { sizeof(WCHAR), sizeof(WCHAR), const_cast<PWSTR>(L"\\") };

    PDCB root = nullptr;
    PFCB vcb = nullptr;
    ASSERT_EQ(STATUS_SUCCESS, BlorgCreateDCB(&root, (CSHORT)BLORGFS_ROOT_DCB_SIGNATURE, &rootName, volume));
    ASSERT_EQ(STATUS_SUCCESS, BlorgCreateFCB(&vcb, (CSHORT)BLORGFS_VCB_SIGNATURE, nullptr, volume, 0));

    GetVolumeDeviceExtension(volume)->RootDcb = root;
    GetVolumeDeviceExtension(volume)->Vcb = vcb;

    wchar_t pathBuffer[] = L"\\media\\stress.bin";
    UNICODE_STRING path;
    path.Buffer = pathBuffer;
    path.Length = (USHORT)(wcslen(pathBuffer) * sizeof(wchar_t));
    path.MaximumLength = path.Length;

    DIRECTORY_ENTRY_METADATA meta = {};
    meta.Size = 4096;

    PCOMMON_CONTEXT node = nullptr;
    ASSERT_EQ(STATUS_SUCCESS, InsertByPath(root, &path, &meta, volume, &node));
    BlorgNodeTablePublish(node);

    StressState state = {};
    state.Volume = volume;
    state.Path = path;

    const int kThreads = 64;
    const int kIterationsPerThread = 2000;

    volatile long badCount = 0;
    ThreadArg args[kThreads];
    HANDLE handles[kThreads];

    for (int i = 0; i < kThreads; ++i)
    {
        args[i].State = &state;
        args[i].ThreadIndex = i;
        args[i].Iterations = kIterationsPerThread;
        args[i].SeenBadCount = &badCount;
        handles[i] = CreateThread(NULL, 0, StressOpenCloseThread, &args[i], 0, NULL);
    }

    while (ReadNoFence(&state.Ready) < kThreads)
    {
        SwitchToThread();
    }

    InterlockedExchange(&state.Barrier, 1);

    WaitForMultipleObjects(kThreads, handles, TRUE, INFINITE);

    for (int i = 0; i < kThreads; ++i)
    {
        CloseHandle(handles[i]);
    }

    PFCB fcb = (PFCB)node;

    EXPECT_EQ(0u, fcb->ShareAccess.OpenCount)
        << "ShareAccess.OpenCount did not return to zero after "
        << (kThreads * kIterationsPerThread) << " open/close cycles across "
        << kThreads << " real threads -- a lost update in the open or "
           "close path";

    EXPECT_EQ(0, ReadNoFence64(&node->RefCount))
        << "RefCount did not return to zero -- same class of lost update";

    BlorgNodeTableTeardown();
    BlorgFreeFileContext(root, volume);
    BlorgFreeFileContext(vcb, volume);
    StructsModelDestroyVolume(volume);
}

} // namespace
