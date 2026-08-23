//
// Coverage for the real DevIoCtrl.c: the FSDO vendor IOCTLs (TLS pin
// update, statistics query/reset) and the DDO's MOUNTDEV identity and
// synthetic disk-geometry IOCTLs. Previously 0% -- and the only surface in
// this driver that takes buffers straight from usermode, which is what
// makes output-buffer bounds and input-length validation worth pinning
// down here rather than inferring them from review.
//
// METHOD_BUFFERED throughout, so every handler reads and writes
// Irp->AssociatedIrp.SystemBuffer, which the I/O manager sizes at
// max(InputBufferLength, OutputBufferLength) and pre-fills with only the
// first InputBufferLength bytes. The fixture models that faithfully --
// including leaving the un-filled tail as garbage rather than zeroes,
// since a test that zeroed it would hide exactly the class of bug that
// reads past the caller's input.
//

#include <gtest/gtest.h>

#include <memory>
#include <vector>

extern "C" {
#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
#include "..\..\src\TlsHandshake.h"
#include <mountdev.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntddvol.h>
}

#include "DeviceKindScope.h"

namespace
{

//
// IOCTL_BLORGFS_SET_TLS_PIN is defined inside DevIoCtrl.c rather than a
// shared header (unlike the statistics pair in Statistics.h, which
// PerfHarness also needs). Mirrored here deliberately, the same way
// SocketKernelTest.cpp mirrors the socket timeouts: if the real
// definition changes, this stops matching and the tests below start
// exercising the default arm instead -- which the unknown-IOCTL test
// would then contradict, rather than everything silently still passing.
//
#define BLORGFS_TEST_IOCTL_SET_TLS_PIN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

//
// TlsPin.Lock is initialized by BlorgTlsHandshakeGlobalInit from DriverEntry,
// which no sandbox target runs. Once per process, not per fixture: it
// mints a lock identity, and re-initializing the same lock every SetUp
// would both burn the model's fixed lock-id budget and look like a
// double-init.
//
void EnsureTlsPinLockInitialized()
{
    static bool initialized = false;

    if (!initialized)
    {
        BlorgTlsHandshakeGlobalInit();
        initialized = true;
    }
}

class DevIoCtrlTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        EnsureTlsPinLockInitialized();

        Fsdo = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Fsdo);
        global.FileSystemDeviceObject = Fsdo;

        Ddo = StructsModelCreateVolume();
        ASSERT_NE(nullptr, Ddo);
        global.DiskDeviceObject = Ddo;
    }

    void TearDown() override
    {
        StructsModelDestroyVolume(Fsdo);
        StructsModelDestroyVolume(Ddo);

        KmAssertQuiescent("DevIoCtrlTest teardown");
    }

    struct IoctlRequest
    {
        IO_STACK_LOCATION Stack;
        IRP Irp;
        std::vector<unsigned char> SystemBuffer;
    };

    //
    // Models the I/O manager's METHOD_BUFFERED allocation: one buffer of
    // max(in, out) bytes, poisoned with 0xCD so anything the handler reads
    // beyond InputBufferLength is visibly garbage rather than an
    // accidentally-plausible zero -- the same reason NtShim.c poisons
    // uninitialized pool.
    //
    IoctlRequest* PrepareIoctl(PDEVICE_OBJECT device, ULONG code,
        ULONG inputLength, ULONG outputLength)
    {
        Requests.push_back(std::make_unique<IoctlRequest>());
        IoctlRequest* req = Requests.back().get();

        ULONG bufferSize = (inputLength > outputLength) ? inputLength : outputLength;
        req->SystemBuffer.assign(bufferSize ? bufferSize : 1, 0xCD);

        memset(&req->Stack, 0, sizeof(req->Stack));
        memset(&req->Irp, 0, sizeof(req->Irp));

        req->Stack.MajorFunction = IRP_MJ_DEVICE_CONTROL;
        req->Stack.DeviceObject = device;
        req->Stack.Parameters.DeviceIoControl.IoControlCode = code;
        req->Stack.Parameters.DeviceIoControl.InputBufferLength = inputLength;
        req->Stack.Parameters.DeviceIoControl.OutputBufferLength = outputLength;

        req->Irp.StackLocation = &req->Stack;
        req->Irp.AssociatedIrp.SystemBuffer = req->SystemBuffer.data();

        return req;
    }

    PDEVICE_OBJECT Fsdo = nullptr;
    PDEVICE_OBJECT Ddo = nullptr;
    std::vector<std::unique_ptr<IoctlRequest>> Requests;
};

///////////////////////////////////////////////////////////////////////////
// Statistics query: input-length validation
///////////////////////////////////////////////////////////////////////////

//
// The normal call, matching PerfHarness: the same buffer in and out, with
// Version/SizeOfStruct filled in by the caller as input.
//
TEST_F(DevIoCtrlTest, StatisticsQueryWithAMatchingRevisionSucceeds)
{
    const ULONG size = (ULONG)sizeof(BLORGFS_STATISTICS_RESPONSE);
    IoctlRequest* req = PrepareIoctl(Fsdo, IOCTL_BLORGFS_QUERY_STATISTICS, size, size);

    auto* request = reinterpret_cast<PBLORGFS_STATISTICS_RESPONSE>(req->SystemBuffer.data());
    memset(request, 0, size);
    request->Version = BLORGFS_STATISTICS_VERSION;
    request->SizeOfStruct = size;

    EXPECT_EQ(STATUS_SUCCESS, BlorgDeviceControl(Fsdo, &req->Irp));
    EXPECT_EQ((ULONG_PTR)size, req->Irp.IoStatus.Information);
}

TEST_F(DevIoCtrlTest, StatisticsQueryWithAWrongRevisionIsRejected)
{
    const ULONG size = (ULONG)sizeof(BLORGFS_STATISTICS_RESPONSE);
    IoctlRequest* req = PrepareIoctl(Fsdo, IOCTL_BLORGFS_QUERY_STATISTICS, size, size);

    auto* request = reinterpret_cast<PBLORGFS_STATISTICS_RESPONSE>(req->SystemBuffer.data());
    memset(request, 0, size);
    request->Version = BLORGFS_STATISTICS_VERSION + 1;
    request->SizeOfStruct = size;

    EXPECT_EQ(STATUS_REVISION_MISMATCH, BlorgDeviceControl(Fsdo, &req->Irp));
}

TEST_F(DevIoCtrlTest, StatisticsQueryWithATooSmallOutputBufferIsRejected)
{
    IoctlRequest* req = PrepareIoctl(Fsdo, IOCTL_BLORGFS_QUERY_STATISTICS, 0, 4);

    EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, BlorgDeviceControl(Fsdo, &req->Irp));
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
}

//
// A query-only call: an output buffer, no input. Legal at the Win32 layer
// (DeviceIoControl takes nullptr/0 for the input side), and the obvious
// way to call something named "query" -- but the handler reads Version and
// SizeOfStruct, which are *input* fields, while validating only
// OutputBufferLength. With no input supplied, those two reads land on the
// part of the METHOD_BUFFERED system buffer the I/O manager never filled,
// so the revision gate decides on uninitialized pool contents.
//
// Not a memory-safety bug -- OutputBufferLength was validated, so the
// buffer really is that large -- and not a disclosure, since
// BlorgStatisticsQuery zeroes the whole response before filling it. But
// the outcome is whatever the pool happens to hold: usually a spurious
// STATUS_REVISION_MISMATCH, occasionally a pass. What it must not be is
// non-deterministic, so the contract is enforced explicitly: no input
// means the caller's revision cannot be checked, which is
// STATUS_INVALID_PARAMETER.
//
TEST_F(DevIoCtrlTest, StatisticsQueryWithNoInputIsRejectedDeterministically)
{
    const ULONG size = (ULONG)sizeof(BLORGFS_STATISTICS_RESPONSE);
    IoctlRequest* req = PrepareIoctl(Fsdo, IOCTL_BLORGFS_QUERY_STATISTICS, 0, size);

    NTSTATUS status = BlorgDeviceControl(Fsdo, &req->Irp);

    EXPECT_EQ(STATUS_INVALID_PARAMETER, status)
        << "a query with no input buffer must be rejected on the input length, not "
           "decided by reading Version/SizeOfStruct out of the uninitialized tail of "
           "the METHOD_BUFFERED system buffer";
    EXPECT_EQ(0u, req->Irp.IoStatus.Information);
}

TEST_F(DevIoCtrlTest, StatisticsQueryWithATruncatedInputIsRejectedDeterministically)
{
    const ULONG size = (ULONG)sizeof(BLORGFS_STATISTICS_RESPONSE);
    IoctlRequest* req = PrepareIoctl(Fsdo, IOCTL_BLORGFS_QUERY_STATISTICS, 4, size);

    EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgDeviceControl(Fsdo, &req->Irp))
        << "an input too short to contain both gate fields must be rejected the same way";
}

///////////////////////////////////////////////////////////////////////////
// TLS pin update
///////////////////////////////////////////////////////////////////////////

TEST_F(DevIoCtrlTest, SetTlsPinAcceptsExactlyAHashLengthInput)
{
    IoctlRequest* req = PrepareIoctl(Fsdo, BLORGFS_TEST_IOCTL_SET_TLS_PIN, TLS_HASH_LEN, 0);

    EXPECT_EQ(STATUS_SUCCESS, BlorgDeviceControl(Fsdo, &req->Irp));
}

TEST_F(DevIoCtrlTest, SetTlsPinRejectsAnyOtherInputLength)
{
    const ULONG lengths[] = { 0u, (ULONG)TLS_HASH_LEN - 1u, (ULONG)TLS_HASH_LEN + 1u, 4096u };

    for (ULONG length : lengths)
    {
        IoctlRequest* req = PrepareIoctl(Fsdo, BLORGFS_TEST_IOCTL_SET_TLS_PIN, length, 0);

        EXPECT_EQ(STATUS_INVALID_PARAMETER, BlorgDeviceControl(Fsdo, &req->Irp))
            << "input length " << length << " must not be accepted as a pin";
    }
}

///////////////////////////////////////////////////////////////////////////
// MOUNTDEV / geometry output-buffer bounds
///////////////////////////////////////////////////////////////////////////

//
// Every variable-length MOUNTDEV reply writes a fixed header, then copies
// a name past it. Walking the output length across the whole boundary --
// one byte at a time from nothing to comfortably past the exact fit --
// is what proves the header write and the name copy are each guarded by
// their own length check rather than by one that happens to cover both at
// the sizes a real caller uses. The guarded pool (NtShim.c) turns any
// overrun into a failure here rather than a corrupted heap later.
//
TEST_F(DevIoCtrlTest, MountdevQueryDeviceNameRespectsEveryOutputLength)
{
    for (ULONG outLength = 0; outLength < 128; ++outLength)
    {
        IoctlRequest* req = PrepareIoctl(Ddo, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, 0, outLength);

        NTSTATUS status = BlorgDeviceControl(Ddo, &req->Irp);

        if (NT_SUCCESS(status))
        {
            auto* name = reinterpret_cast<PMOUNTDEV_NAME>(req->SystemBuffer.data());
            EXPECT_LE(UFIELD_OFFSET(MOUNTDEV_NAME, Name) + name->NameLength, outLength)
                << "reported a name that does not fit the buffer it was given";
            EXPECT_EQ((ULONG_PTR)(UFIELD_OFFSET(MOUNTDEV_NAME, Name) + name->NameLength),
                      req->Irp.IoStatus.Information);
        }
        else
        {
            EXPECT_TRUE(STATUS_BUFFER_TOO_SMALL == status || STATUS_BUFFER_OVERFLOW == status)
                << "unexpected status " << std::hex << status << " at outLength " << outLength;
        }
    }
}

TEST_F(DevIoCtrlTest, MountdevQueryUniqueIdRespectsEveryOutputLength)
{
    for (ULONG outLength = 0; outLength < 128; ++outLength)
    {
        IoctlRequest* req = PrepareIoctl(Ddo, IOCTL_MOUNTDEV_QUERY_UNIQUE_ID, 0, outLength);

        NTSTATUS status = BlorgDeviceControl(Ddo, &req->Irp);

        if (NT_SUCCESS(status))
        {
            auto* id = reinterpret_cast<PMOUNTDEV_UNIQUE_ID>(req->SystemBuffer.data());
            EXPECT_LE(UFIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + id->UniqueIdLength, outLength);
        }
        else
        {
            EXPECT_TRUE(STATUS_BUFFER_TOO_SMALL == status || STATUS_BUFFER_OVERFLOW == status);
        }
    }
}

TEST_F(DevIoCtrlTest, MountdevSuggestedLinkNameRespectsEveryOutputLength)
{
    for (ULONG outLength = 0; outLength < 128; ++outLength)
    {
        IoctlRequest* req = PrepareIoctl(Ddo, IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME, 0, outLength);

        NTSTATUS status = BlorgDeviceControl(Ddo, &req->Irp);

        if (NT_SUCCESS(status))
        {
            auto* suggested = reinterpret_cast<PMOUNTDEV_SUGGESTED_LINK_NAME>(req->SystemBuffer.data());
            EXPECT_LE(UFIELD_OFFSET(MOUNTDEV_SUGGESTED_LINK_NAME, Name) + suggested->NameLength, outLength);
        }
        else
        {
            EXPECT_TRUE(STATUS_BUFFER_TOO_SMALL == status || STATUS_BUFFER_OVERFLOW == status);
        }
    }
}

TEST_F(DevIoCtrlTest, VolumeDiskExtentsRespectsEveryOutputLength)
{
    for (ULONG outLength = 0; outLength < 128; ++outLength)
    {
        IoctlRequest* req = PrepareIoctl(Ddo, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0, outLength);

        NTSTATUS status = BlorgDeviceControl(Ddo, &req->Irp);

        EXPECT_TRUE(NT_SUCCESS(status) ||
                    STATUS_BUFFER_TOO_SMALL == status ||
                    STATUS_BUFFER_OVERFLOW == status)
            << "unexpected status " << std::hex << status << " at outLength " << outLength;
    }
}

TEST_F(DevIoCtrlTest, DiskGeometryIsSelfConsistentWithTheReportedExtent)
{
    IoctlRequest* geometryReq =
        PrepareIoctl(Ddo, IOCTL_DISK_GET_DRIVE_GEOMETRY, 0, (ULONG)sizeof(DISK_GEOMETRY));
    ASSERT_EQ(STATUS_SUCCESS, BlorgDeviceControl(Ddo, &geometryReq->Irp));

    auto* geometry = reinterpret_cast<PDISK_GEOMETRY>(geometryReq->SystemBuffer.data());
    ULONG64 geometrySize =
        (ULONG64)geometry->Cylinders.QuadPart *
        geometry->TracksPerCylinder *
        geometry->SectorsPerTrack *
        geometry->BytesPerSector;

    IoctlRequest* extentsReq =
        PrepareIoctl(Ddo, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, 0, (ULONG)sizeof(VOLUME_DISK_EXTENTS));
    ASSERT_EQ(STATUS_SUCCESS, BlorgDeviceControl(Ddo, &extentsReq->Irp));

    auto* extents = reinterpret_cast<PVOLUME_DISK_EXTENTS>(extentsReq->SystemBuffer.data());

    EXPECT_EQ(geometrySize, (ULONG64)extents->Extents[0].ExtentLength.QuadPart)
        << "the two IOCTLs a loader cross-checks during image activation must agree "
           "on the synthetic disk's size";

    IoctlRequest* numberReq =
        PrepareIoctl(Ddo, IOCTL_STORAGE_GET_DEVICE_NUMBER, 0, (ULONG)sizeof(STORAGE_DEVICE_NUMBER));
    ASSERT_EQ(STATUS_SUCCESS, BlorgDeviceControl(Ddo, &numberReq->Irp));

    auto* number = reinterpret_cast<PSTORAGE_DEVICE_NUMBER>(numberReq->SystemBuffer.data());

    EXPECT_EQ(number->DeviceNumber, extents->Extents[0].DiskNumber)
        << "the synthetic disk number must match between the two IOCTLs that report it";
}

///////////////////////////////////////////////////////////////////////////
// Routing
///////////////////////////////////////////////////////////////////////////

TEST_F(DevIoCtrlTest, VolumeDeviceHasNoIoctlsOfItsOwn)
{
    PDEVICE_OBJECT vdo = StructsModelCreateVolume();
    ASSERT_NE(nullptr, vdo);
    ScopedDeviceKind asVolume(&global.VolumeDeviceObject, vdo);

    IoctlRequest* req = PrepareIoctl(vdo, IOCTL_DISK_GET_DRIVE_GEOMETRY, 0, (ULONG)sizeof(DISK_GEOMETRY));

    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgDeviceControl(vdo, &req->Irp));

    StructsModelDestroyVolume(vdo);
}

//
// The I/O manager routes an IOCTL by the device type baked into its
// CTL_CODE: FILE_DEVICE_FILE_SYSTEM means IRP_MJ_FILE_SYSTEM_CONTROL,
// anything else means IRP_MJ_DEVICE_CONTROL. Every vendor IOCTL here is
// implemented under IRP_MJ_DEVICE_CONTROL, so declaring one with the
// filesystem device type sends it to the FSCTL path instead and it comes
// back STATUS_INVALID_DEVICE_REQUEST from usermode -- while every test in
// this file still passes, since they all invoke BlorgDeviceControl
// directly and never exercise the routing decision. Only asserting on the
// device type catches it, which is why this test exists.
//
TEST_F(DevIoCtrlTest, VendorIoctlsAreNotRoutedAsFsctls)
{
    const ULONG fileSystemType = C_CAST(ULONG, FILE_DEVICE_FILE_SYSTEM);

    EXPECT_NE(fileSystemType, C_CAST(ULONG, IOCTL_BLORGFS_QUERY_STATISTICS >> 16));
    EXPECT_NE(fileSystemType, C_CAST(ULONG, IOCTL_BLORGFS_RESET_STATISTICS >> 16));
    EXPECT_NE(fileSystemType, C_CAST(ULONG, BLORGFS_TEST_IOCTL_SET_TLS_PIN >> 16));
}

TEST_F(DevIoCtrlTest, UnknownIoctlIsRejectedOnBothDevices)
{
    const ULONG bogus = CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0xBAD, METHOD_BUFFERED, FILE_ANY_ACCESS);

    IoctlRequest* fsdoReq = PrepareIoctl(Fsdo, bogus, 0, 0);
    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgDeviceControl(Fsdo, &fsdoReq->Irp));

    IoctlRequest* ddoReq = PrepareIoctl(Ddo, bogus, 0, 0);
    EXPECT_EQ(STATUS_INVALID_DEVICE_REQUEST, BlorgDeviceControl(Ddo, &ddoReq->Irp));
}

///////////////////////////////////////////////////////////////////////////
// FSCTLs aimed at the control device
///////////////////////////////////////////////////////////////////////////

//
// The control device (\.\BlorgFS) shares BlorgFileSystemControl's
// IRP_MN_USER_FS_REQUEST arm with the volume, but a handle on it has no
// node behind it: BlorgFileSystemCreate reports FILE_OPENED and leaves
// FsContext NULL. The oplock FSCTLs read the node's type as their first
// act, and every one of them is FILE_ANY_ACCESS, so a caller needs only
// the GENERIC_READ the FSDO's SDDL grants World.
//
// Both shapes are covered because they fail differently: the legacy
// codes go straight to a type check, while FSCTL_REQUEST_OPLOCK reads
// the type and then its input buffer. Neither may dereference the node
// before establishing there is one.
//
class ControlDeviceFsctlTest : public DevIoCtrlTest
{
protected:
    struct FsctlRequest
    {
        FILE_OBJECT FileObject;
        IO_STACK_LOCATION Stack;
        IRP Irp;
        std::vector<unsigned char> SystemBuffer;
    };

    //
    // A handle on the control device, exactly as BlorgFileSystemCreate
    // leaves one: no FsContext, no CCB.
    //
    FsctlRequest* PrepareControlDeviceFsctl(ULONG code, ULONG inputLength, ULONG outputLength)
    {
        Fsctls.push_back(std::make_unique<FsctlRequest>());
        FsctlRequest* req = Fsctls.back().get();

        memset(&req->FileObject, 0, sizeof(req->FileObject));
        memset(&req->Stack, 0, sizeof(req->Stack));
        memset(&req->Irp, 0, sizeof(req->Irp));

        ULONG bufferSize = (inputLength > outputLength) ? inputLength : outputLength;
        req->SystemBuffer.assign(bufferSize ? bufferSize : 1, 0xCD);

        req->Stack.MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
        req->Stack.MinorFunction = IRP_MN_USER_FS_REQUEST;
        req->Stack.DeviceObject = Fsdo;
        req->Stack.FileObject = &req->FileObject;
        req->Stack.Parameters.FileSystemControl.FsControlCode = code;
        req->Stack.Parameters.FileSystemControl.InputBufferLength = inputLength;
        req->Stack.Parameters.FileSystemControl.OutputBufferLength = outputLength;

        req->Irp.StackLocation = &req->Stack;
        req->Irp.AssociatedIrp.SystemBuffer = req->SystemBuffer.data();

        return req;
    }

    std::vector<std::unique_ptr<FsctlRequest>> Fsctls;
};

TEST_F(ControlDeviceFsctlTest, LegacyOplockRequestOnTheControlDeviceIsRejected)
{
    FsctlRequest* req = PrepareControlDeviceFsctl(FSCTL_REQUEST_OPLOCK_LEVEL_1, 0, 0);

    ASSERT_EQ(nullptr, req->FileObject.FsContext)
        << "this test is only meaningful against a handle with no node behind it";

    NTSTATUS status = BlorgFileSystemControl(Fsdo, &req->Irp);

    EXPECT_FALSE(NT_SUCCESS(status))
        << "an oplock FSCTL on a nodeless handle must be refused, not acted on";
    EXPECT_EQ(1u, req->Irp.CompletionCount)
        << "a refused FSCTL is completed by the dispatcher, exactly once";
}

TEST_F(ControlDeviceFsctlTest, UnifiedOplockRequestOnTheControlDeviceIsRejected)
{
    FsctlRequest* req = PrepareControlDeviceFsctl(
        FSCTL_REQUEST_OPLOCK,
        (ULONG)sizeof(REQUEST_OPLOCK_INPUT_BUFFER),
        (ULONG)sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER));

    auto* input = reinterpret_cast<PREQUEST_OPLOCK_INPUT_BUFFER>(req->SystemBuffer.data());
    memset(input, 0, sizeof(*input));
    input->StructureVersion = 1;
    input->StructureLength = sizeof(REQUEST_OPLOCK_INPUT_BUFFER);
    input->Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;

    NTSTATUS status = BlorgFileSystemControl(Fsdo, &req->Irp);

    EXPECT_FALSE(NT_SUCCESS(status));
    EXPECT_EQ(1u, req->Irp.CompletionCount);
}

//
// The statistics FSCTLs are volume-wide and deliberately answered before
// any node check, so they must keep reaching their handler on the same
// nodeless handle -- proving the guard added for the oplock codes did not
// sweep them up with it.
//
// The assertion is on what the status is NOT, because what it IS depends
// on state this fixture does not own: BlorgStatisticsInitialize may or
// may not have run (StatisticsTest.cpp owns that), so the filler can
// legitimately answer STATUS_BUFFER_TOO_SMALL, STATUS_DEVICE_NOT_READY,
// or success. What it must never answer is the dispatcher's
// "no node behind this handle" rejection, which is the regression.
//
TEST_F(ControlDeviceFsctlTest, StatisticsFsctlStillReachesItsHandlerOnTheControlDevice)
{
    FsctlRequest* req = PrepareControlDeviceFsctl(FSCTL_FILESYSTEM_GET_STATISTICS, 0, 4096);

    NTSTATUS status = BlorgFileSystemControl(Fsdo, &req->Irp);

    EXPECT_NE(STATUS_INVALID_DEVICE_REQUEST, status)
        << "a volume-wide query needs no node and must not be refused for lacking one";
}

///////////////////////////////////////////////////////////////////////////
// Mount requests naming a device this driver does not own
///////////////////////////////////////////////////////////////////////////

//
// IRP_MN_MOUNT_VOLUME is the one place a device object reaches this driver
// that it did not create. Once IoRegisterFileSystem has run, the I/O
// manager offers every arriving volume to every registered file system in
// turn until one claims it, and Parameters.MountVolume.DeviceObject is
// that volume's real storage device -- owned by volmgr, cdrom, a USB
// stack, whatever. A USB stick or a disc going in is enough to send one.
//
// So the "is this mine?" test may not read that device's extension. It is
// another driver's private memory: it can be NULL outright (IoCreateDevice
// with DeviceExtensionSize 0), or shorter than the magic being read out of
// it. Identity has to come from a pointer comparison against the device
// this driver actually created.
//
class ForeignMountTest : public DevIoCtrlTest
{
protected:
    struct MountRequest
    {
        IO_STACK_LOCATION Stack;
        IRP Irp;
        VPB Vpb;
    };

    MountRequest* PrepareMount(PDEVICE_OBJECT target)
    {
        Mounts.push_back(std::make_unique<MountRequest>());
        MountRequest* req = Mounts.back().get();

        memset(&req->Stack, 0, sizeof(req->Stack));
        memset(&req->Irp, 0, sizeof(req->Irp));
        memset(&req->Vpb, 0, sizeof(req->Vpb));

        req->Stack.MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
        req->Stack.MinorFunction = IRP_MN_MOUNT_VOLUME;
        req->Stack.DeviceObject = Fsdo;
        req->Stack.Parameters.MountVolume.DeviceObject = target;
        req->Stack.Parameters.MountVolume.Vpb = &req->Vpb;

        req->Irp.StackLocation = &req->Stack;

        return req;
    }

    std::vector<std::unique_ptr<MountRequest>> Mounts;
};

//
// The crashing shape: a device whose driver asked for no extension at all.
//
TEST_F(ForeignMountTest, MountOfADeviceWithNoExtensionIsDeclined)
{
    DEVICE_OBJECT foreign;
    memset(&foreign, 0, sizeof(foreign));

    ASSERT_EQ(nullptr, foreign.DeviceExtension)
        << "this test is about a device object that carries no extension at all";

    MountRequest* req = PrepareMount(&foreign);

    EXPECT_EQ(STATUS_UNRECOGNIZED_VOLUME, BlorgFileSystemControl(Fsdo, &req->Irp))
        << "a volume this driver does not own must be declined without reading its extension";
}

//
// The other half: a foreign device that DOES carry an extension must be
// declined just the same. There is deliberately no value it could hold
// that would change the answer -- ownership is decided before the
// extension is ever looked at -- so this pins down that the check reads
// nothing out of a device this driver did not create, rather than reading
// it and happening not to match.
//
TEST_F(ForeignMountTest, MountOfADeviceCarryingAnExtensionIsStillDeclined)
{
    ULONG64 foreignPrivateState[8];
    memset(foreignPrivateState, 0xA5, sizeof(foreignPrivateState));

    DEVICE_OBJECT foreign;
    memset(&foreign, 0, sizeof(foreign));
    foreign.DeviceExtension = foreignPrivateState;

    MountRequest* req = PrepareMount(&foreign);

    EXPECT_EQ(STATUS_UNRECOGNIZED_VOLUME, BlorgFileSystemControl(Fsdo, &req->Irp))
        << "a device this driver never created must not claim the volume, whatever its extension holds";
}

//
// The positive control, and the reason the two above are not enough on
// their own: a check that declined everything would satisfy both. The
// device this driver did create must still get past the ownership test.
//
// Asserted as "not declined" rather than success because what follows the
// check -- BlorgCreateVolumeDeviceObject against global.DriverObject --
// is not something this fixture stands up. Reaching it at all is the
// claim; the mount itself belongs to a target that models the volume.
//
TEST_F(ForeignMountTest, MountOfThisDriversOwnDiskDeviceIsAccepted)
{
    PDEVICE_OBJECT savedDiskDeviceObject = global.DiskDeviceObject;
    global.DiskDeviceObject = Ddo;

    MountRequest* req = PrepareMount(Ddo);

    NTSTATUS status = BlorgFileSystemControl(Fsdo, &req->Irp);

    global.DiskDeviceObject = savedDiskDeviceObject;

    EXPECT_NE(STATUS_UNRECOGNIZED_VOLUME, status)
        << "the driver's own disk device must still be recognised as its own";
}

} // namespace
