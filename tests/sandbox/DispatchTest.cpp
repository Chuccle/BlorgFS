//
// IRP dispatch tests over the real dispatch translation units.
//
// The point of this target is coverage of the files nothing else compiles:
// every IRP_MJ handler, the FSP work queue, the path cache, the statistics
// counters and the cache-manager callbacks. Getting them to build and link
// against the kernel model is the prerequisite -- a handler that does not
// compile here cannot be tested here -- and is worth having on its own,
// because it is what stops these files drifting away from the shim.
//
#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
}

#include "DeviceKindScope.h"

//
// Proves the three shim hooks actually flip the branch they claim to,
// rather than existing as decoration. This does not yet drive them
// through a real IRP_MJ_CREATE/IRP_MJ_READ dispatch -- OpenExistingFcb
// and the read cache-miss path are static inline, reachable only from
// inside Create.c/Read.c's own translation units -- so it checks the
// model surface those dispatch paths call directly. Wiring a full
// end-to-end dispatch test through BlorgCreate/BlorgRead is follow-on
// work, not done here.
//
TEST(DispatchSandbox, ShimHooksActuallyFlipTheBranchTheyClaimTo)
{
    ShimForceNextOplockCheck(STATUS_PENDING);
    EXPECT_EQ(STATUS_PENDING, FsRtlCheckOplock(nullptr, nullptr, nullptr, nullptr, nullptr))
        << "the forced status did not come back out -- the oplock-pending "
           "path is still structurally unreachable";
    EXPECT_EQ(STATUS_SUCCESS, FsRtlCheckOplock(nullptr, nullptr, nullptr, nullptr, nullptr))
        << "the force did not reset -- it would corrupt every later call in the same run";

    ShimForceNextCcCopyReadMiss();
    IO_STATUS_BLOCK ios = {};
    EXPECT_FALSE(CcCopyReadEx(nullptr, nullptr, 0, TRUE, nullptr, &ios, nullptr))
        << "the forced miss did not come back out -- the repost-to-worker "
           "path is still structurally unreachable";
    EXPECT_TRUE(CcCopyReadEx(nullptr, nullptr, 0, TRUE, nullptr, &ios, nullptr))
        << "the force did not reset";

    //
    // KeWaitForMultipleObjects is a real wait on real events (see
    // FspWorkQueueStressTest.cpp), so proving it distinguishes its two
    // objects means real KEVENTs, not placeholder nullptrs: index 0 stays
    // unsignalled, index 1 gets set, and the wait must report index 1.
    //
    KEVENT workEvent;
    KEVENT terminationEvent;
    KeInitializeEvent(&workEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&terminationEvent, NotificationEvent, FALSE);

    KeSetEvent(&terminationEvent, EVENT_INCREMENT, FALSE);

    PVOID objects[2] = { &workEvent, &terminationEvent };
    EXPECT_EQ(STATUS_WAIT_1,
        KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, nullptr, nullptr))
        << "the termination event's signal did not come back out as index 1";

    KeSetEvent(&workEvent, EVENT_INCREMENT, FALSE);
    EXPECT_EQ(STATUS_WAIT_0,
        KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, nullptr, nullptr))
        << "the work event's signal (index 0) was not reported now that it is set too -- "
           "WaitAny must return the lowest-indexed signalled object";
}

//
// A flush on a read-only volume has nothing to write back, so the honest
// answer is success. It used to return STATUS_INVALID_DEVICE_REQUEST, which
// reaches an application as "Incorrect function" -- the same misleading
// error that hid the statistics IOCTL routing bug -- and some applications
// treat a failed flush as fatal rather than as "this volume needs no
// flushing".
//
// All three of the driver's device objects, because BlorgFlushBuffers is
// reached on any of them and the old stub answered identically for each.
// The unknown device is the control: a flush arriving on a device this
// driver does not own is a routing error, and turning that into success
// would hide it.
//
TEST(DispatchSandbox, FlushBuffersSucceedsOnEveryDeviceThisDriverOwns)
{
    DEVICE_OBJECT device;
    memset(&device, 0, sizeof(device));

    PDEVICE_OBJECT* const slots[] =
    {
        &global.VolumeDeviceObject,
        &global.DiskDeviceObject,
        &global.FileSystemDeviceObject
    };

    for (PDEVICE_OBJECT* slot : slots)
    {
        ScopedDeviceKind asOurs(slot, &device);

        IRP irp;
        memset(&irp, 0, sizeof(irp));

        EXPECT_EQ(STATUS_SUCCESS, BlorgFlushBuffers(&device, &irp));
        EXPECT_EQ(STATUS_SUCCESS, irp.IoStatus.Status);
        EXPECT_EQ(1u, irp.CompletionCount) << "a flush must be completed exactly once";
    }

    IRP foreign;
    memset(&foreign, 0, sizeof(foreign));

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgFlushBuffers(&device, &foreign))
        << "a flush on a device this driver does not own is a routing error, not a no-op";
}

//
// BlorgPrePostIrp must leave every posted IRP describing its user buffer
// with a locked MDL, because the worker that picks the IRP up runs in a
// different process context and a raw user VA means nothing there.
//
// This is the only test that drives BlorgLockUserBuffer into actually
// allocating. Everything else either pre-sets MdlAddress so it no-ops
// (FspWorkQueueStressTest) or never posts at all, which is how the shim's
// IoAllocateMdl came to ignore its Irp parameter unnoticed: the real one
// attaches the MDL to Irp->MdlAddress and BlorgLockUserBuffer relies on
// that, so with the parameter dropped every posted request in the sandbox
// silently took the unlocked branch and leaked the MDL it had just built.
// Reverting that one line must fail this test.
//
TEST(DispatchSandbox, PrePostIrpLocksAnUnlockedUserBufferBeforeItIsQueued)
{
    unsigned char buffer[512] = {};

    FILE_OBJECT fileObject;
    memset(&fileObject, 0, sizeof(fileObject));

    IO_STACK_LOCATION stack;
    memset(&stack, 0, sizeof(stack));
    stack.MajorFunction = IRP_MJ_READ;
    stack.FileObject = &fileObject;
    stack.Parameters.Read.Length = sizeof(buffer);

    IRP irp;
    memset(&irp, 0, sizeof(irp));
    irp.StackLocation = &stack;
    irp.UserBuffer = buffer;
    irp.RequestorMode = KernelMode;

    ASSERT_EQ(nullptr, irp.MdlAddress) << "the point of the test is the NULL case";

    EXPECT_EQ(STATUS_SUCCESS, BlorgPrePostIrp(nullptr, &irp));

    ASSERT_NE(nullptr, irp.MdlAddress)
        << "the buffer was not locked, so a worker in another process context "
           "would dereference a raw user VA";
    EXPECT_EQ(buffer, irp.MdlAddress->Base);
    EXPECT_EQ(sizeof(buffer), irp.MdlAddress->Length);

    ShimReleaseIrpMdl(&irp);

    EXPECT_EQ(0, KmObjectsLive(KmObjectMdl)) << "the MDL outlived the request";
}

//
// A zero-length buffer has nothing to lock, and an IRP that already carries
// an MDL -- paging I/O, or a second post of the same request -- must not
// have a second one built over the top of it.
//
TEST(DispatchSandbox, PrePostIrpBuildsNoMdlWhenThereIsNothingToLock)
{
    FILE_OBJECT fileObject;
    memset(&fileObject, 0, sizeof(fileObject));

    IO_STACK_LOCATION stack;
    memset(&stack, 0, sizeof(stack));
    stack.MajorFunction = IRP_MJ_READ;
    stack.FileObject = &fileObject;
    stack.Parameters.Read.Length = 0;

    IRP irp;
    memset(&irp, 0, sizeof(irp));
    irp.StackLocation = &stack;
    irp.RequestorMode = KernelMode;

    EXPECT_EQ(STATUS_SUCCESS, BlorgPrePostIrp(nullptr, &irp));
    EXPECT_EQ(nullptr, irp.MdlAddress);

    unsigned char buffer[64] = {};
    PMDL existing = IoAllocateMdl(buffer, sizeof(buffer), FALSE, FALSE, nullptr);
    ASSERT_NE(nullptr, existing);

    IRP paging;
    memset(&paging, 0, sizeof(paging));
    paging.StackLocation = &stack;
    paging.MdlAddress = existing;
    paging.RequestorMode = KernelMode;
    stack.Parameters.Read.Length = sizeof(buffer);

    EXPECT_EQ(STATUS_SUCCESS, BlorgPrePostIrp(nullptr, &paging));
    EXPECT_EQ(existing, paging.MdlAddress) << "an existing MDL was replaced";

    ShimReleaseIrpMdl(&paging);

    EXPECT_EQ(0, KmObjectsLive(KmObjectMdl));
}
