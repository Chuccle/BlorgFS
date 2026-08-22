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

TEST(DispatchSandbox, LinksEveryDispatchTranslationUnit)
{
    SUCCEED() << "every dispatch .c compiled and linked against the kernel model";
}

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
