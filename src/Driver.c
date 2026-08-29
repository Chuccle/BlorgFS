//
// Driver load/unload: device object creation/teardown, FastIO and cache
// manager callback wiring, mount-manager volume-arrival notification, and
// registry-based TLS config (TlsEnabled/TlsPin/RemotePort) read at
// DriverEntry.
//

#include "Driver.h"
#include "Socket.h"
#include "TlsHandshake.h"
#include <mountmgr.h>

FAST_IO_DISPATCH  BlorgFsFastDispatch;
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     DriverUnload;

struct GLOBAL global;

//
// global.LogLevel (DBG builds only): trace verbosity for BLORGFS_PRINT.
// 0 = silent (BLORGFS_LOG still prints); raise from the debugger
// (ed blorgfs!global.LogLevel 1) for the full per-IRP firehose. Zero-
// initialized along with the rest of `global` (static storage duration).
//

// {02EF343C-413D-4932-BBCE-15624AACE5D9}
static const GUID BLORGFS_FSDO_GUID = { 0x2ef343c, 0x413d, 0x4932, { 0xbb, 0xce, 0x15, 0x62, 0x4a, 0xac, 0xe5, 0xd9 } };
// {A6E07401-F24E-443E-A47C-D9BD219B9E68}
static const GUID BLORGFS_VDO_GUID = { 0xa6e07401, 0xf24e, 0x443e, { 0xa4, 0x7c, 0xd9, 0xbd, 0x21, 0x9b, 0x9e, 0x68 } };
// {CC6E9F4D-1968-4D95-91AF-FFD72F35F6DA}
static const GUID BLORGFS_DDO_GUID = { 0xcc6e9f4d, 0x1968, 0x4d95, { 0x91, 0xaf, 0xff, 0xd7, 0x2f, 0x35, 0xf6, 0xda } };

//
//  Tell the mount manager our volume has arrived. Until it does, the volume
//  has no entry in the mount-manager database / no \??\Volume{GUID} name, so
//  process creation can't resolve the image's backing volume and fails with
//  STATUS_OBJECT_NAME_NOT_FOUND -- even though every file operation works
//  through the raw B: symlink. On arrival the manager queries the device's
//  MOUNTDEV identity (unique id / device name / suggested link name,
//  answered in DevIoCtrl.c) and registers the volume.
//
//  DeviceName is the DDO's name (\Device\BlorgDrive) -- the device the B:
//  symlink targets, i.e. the volume from the manager's point of view. Runs at
//  PASSIVE_LEVEL from DriverEntry; failure is non-fatal (the manual symlink
//  still gives working file I/O), but image activation needs it.
//
static NTSTATUS DriverNotifyMountManagerVolumeArrival(PCUNICODE_STRING DeviceName)
{
    UNICODE_STRING mountMgrName = RTL_CONSTANT_STRING(MOUNTMGR_DEVICE_NAME);
    PFILE_OBJECT mountMgrFileObject = NULL;
    PDEVICE_OBJECT mountMgrDeviceObject = NULL;

    NTSTATUS status = IoGetDeviceObjectPointer(
        &mountMgrName,
        FILE_READ_ATTRIBUTES,
        &mountMgrFileObject,
        &mountMgrDeviceObject);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ULONG targetSize = UFIELD_OFFSET(MOUNTMGR_TARGET_NAME, DeviceName) + DeviceName->Length;
    PMOUNTMGR_TARGET_NAME target = ExAllocatePoolZero(NonPagedPoolNx, targetSize, 'MMlB');

    if (!target)
    {
        ObDereferenceObject(mountMgrFileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    target->DeviceNameLength = DeviceName->Length;
    RtlCopyMemory(target->DeviceName, DeviceName->Buffer, DeviceName->Length);

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IO_STATUS_BLOCK iosb = { 0 };
    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION,
        mountMgrDeviceObject,
        target,
        targetSize,
        NULL,
        0,
        FALSE,
        &event,
        &iosb);

    if (!irp)
    {
        ExFreePool(target);
        ObDereferenceObject(mountMgrFileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(mountMgrDeviceObject, irp);

    if (STATUS_PENDING == status)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    ExFreePool(target);
    ObDereferenceObject(mountMgrFileObject);

    return status;
}

//
// Creates the disk device object (the DDO the B: symlink targets) via
// WdmlibIoCreateDeviceSecure and clears DO_DEVICE_INITIALIZING so I/O can
// reach it. No device extension: this object is identified by its pointer
// (BlorgDeviceKind, Driver.h) and carries no per-device state.
//
static NTSTATUS DriverCreateDiskDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* DiskDeviceObject)
{
    BLORGFS_PRINT("Entering Drive Creation\n");
    *DiskDeviceObject = NULL;
    UNICODE_STRING vdoString = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
    UNICODE_STRING sddlString = RTL_CONSTANT_STRING(BLORGFS_DDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT diskDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        0,
        &vdoString,
        FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddlString,
        &BLORGFS_DDO_GUID,
        &diskDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    ClearFlag(diskDeviceObject->Flags, DO_DEVICE_INITIALIZING);

    *DiskDeviceObject = diskDeviceObject;

    return result;
}

// Tears down the disk device object created by DriverCreateDiskDeviceObject.
static VOID DriverDeleteDiskDeviceObject(PDEVICE_OBJECT DiskDeviceObject)
{
    if (DiskDeviceObject)
    {
        IoDeleteDevice(DiskDeviceObject);
    }
}

//
// Creates the volume device object (VDO) at mount time: allocates the
// device, initializes its lookaside lists, root DCB, VCB, work queue, and
// directory-notify package, unwinding each already-initialized piece in
// reverse order if a later step fails so no resource leaks on a partial
// mount failure. The notify IRPs registered against the directory-notify
// package are completed in DriverDeleteVolumeDeviceObject (via
// FsRtlNotifyUninitializeSync) and at handle cleanup (FsRtlNotifyCleanup).
// FsRtlNotifyInitializeSync raises on allocation failure, which is
// acceptable here since it only runs at driver/volume init as a load
// failure.
//
NTSTATUS BlorgCreateVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject)
{
    BLORGFS_PRINT("Entering Volume Creation\n");
    *VolumeDeviceObject = NULL;
    UNICODE_STRING vdoString = RTL_CONSTANT_STRING(BLORGFS_VDO_STRING);
    UNICODE_STRING sddlString = RTL_CONSTANT_STRING(BLORGFS_VDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT volumeDeviceObject = NULL;

    NTSTATUS result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_VDO_DEVICE_EXTENSION),
        &vdoString,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddlString,
        &BLORGFS_VDO_GUID,
        &volumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    PBLORGFS_VDO_DEVICE_EXTENSION devExt = volumeDeviceObject->DeviceExtension;

    RtlZeroMemory(devExt, sizeof(BLORGFS_VDO_DEVICE_EXTENSION));

    ExInitializeNPagedLookasideList(&devExt->NonPagedNodeLookasideList, NULL, NULL, POOL_NX_ALLOCATION, sizeof(NON_PAGED_NODE), 'NPN', 0);
    ExInitializePagedLookasideList(&devExt->FcbLookasideList, NULL, NULL, 0, sizeof(FCB), 'FCB', 0);
    ExInitializePagedLookasideList(&devExt->DcbLookasideList, NULL, NULL, 0, sizeof(DCB), 'DCB', 0);
    ExInitializePagedLookasideList(&devExt->CcbLookasideList, NULL, NULL, 0, sizeof(CCB), 'CCB', 0);

    UNICODE_STRING rootDcbPath = RTL_CONSTANT_STRING(L"\\");

    result = BlorgCreateDCB(&devExt->RootDcb, BLORGFS_ROOT_DCB_SIGNATURE, &rootDcbPath, volumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ExDeleteNPagedLookasideList(&devExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&devExt->FcbLookasideList);
        ExDeletePagedLookasideList(&devExt->DcbLookasideList);
        ExDeletePagedLookasideList(&devExt->CcbLookasideList);
        IoDeleteDevice(volumeDeviceObject);
        return result;
    }

    result = BlorgCreateFCB(&devExt->Vcb, BLORGFS_VCB_SIGNATURE, NULL, volumeDeviceObject, 0);

    if (!NT_SUCCESS(result))
    {
        BlorgFreeFileContext(devExt->RootDcb, volumeDeviceObject);
        ExDeleteNPagedLookasideList(&devExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&devExt->FcbLookasideList);
        ExDeletePagedLookasideList(&devExt->DcbLookasideList);
        ExDeletePagedLookasideList(&devExt->CcbLookasideList);
        IoDeleteDevice(volumeDeviceObject);
        return result;
    }

    result = BlorgNodeTableInit(volumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        BlorgFreeFileContext(devExt->Vcb, volumeDeviceObject);
        BlorgFreeFileContext(devExt->RootDcb, volumeDeviceObject);
        ExDeleteNPagedLookasideList(&devExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&devExt->FcbLookasideList);
        ExDeletePagedLookasideList(&devExt->DcbLookasideList);
        ExDeletePagedLookasideList(&devExt->CcbLookasideList);
        IoDeleteDevice(volumeDeviceObject);
        return result;
    }

    result = BlorgCreateWorkQueue();

    if (!NT_SUCCESS(result))
    {
        BlorgNodeTableTeardown();
        BlorgFreeFileContext(devExt->Vcb, volumeDeviceObject);
        BlorgFreeFileContext(devExt->RootDcb, volumeDeviceObject);
        ExDeleteNPagedLookasideList(&devExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&devExt->FcbLookasideList);
        ExDeletePagedLookasideList(&devExt->DcbLookasideList);
        ExDeletePagedLookasideList(&devExt->CcbLookasideList);
        IoDeleteDevice(volumeDeviceObject);
        return result;
    }

    FsRtlNotifyInitializeSync(&devExt->NotifySync);
    InitializeListHead(&devExt->NotifyList);

    ClearFlag(volumeDeviceObject->Flags, DO_DEVICE_INITIALIZING);

    *VolumeDeviceObject = volumeDeviceObject;
    return STATUS_SUCCESS;
}

//
// Frees every node remaining in the FCB/DCB tree under RootDcb, leaf-first:
// descend to a leaf-most node (an FCB, or a DCB with no children), free it
// (BlorgFreeFileContext unlinks it from its parent's ChildrenList), and
// restart from the root until the tree is empty. Runs only at volume
// teardown, after the FSP queue is drained and no handles remain, so no
// locking is needed. Without this walk, any nodes still cached in the
// tree would outlive the lookaside lists they were allocated from.
//
static VOID FreeFileContextTree(PDCB RootDcb, PDEVICE_OBJECT VolumeDeviceObject)
{
    while (!IsListEmpty(&RootDcb->ChildrenList))
    {
        PCOMMON_CONTEXT node = CONTAINING_RECORD(RootDcb->ChildrenList.Flink, COMMON_CONTEXT, Links);

        while ((BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(node)) &&
               !IsListEmpty(&C_CAST(PDCB, node)->ChildrenList))
        {
            node = CONTAINING_RECORD(C_CAST(PDCB, node)->ChildrenList.Flink, COMMON_CONTEXT, Links);
        }

        BlorgFreeFileContext(node, VolumeDeviceObject);
    }
}

//
// Tears down the volume device object: completes/frees the notify sync
// object, destroys the work queue, retires the node table and its reap
// worker (before the tree walk, so no work item can race the frees),
// releases the VCB, the remaining node tree, and the root DCB, deletes
// the lookaside lists, and deletes the device -- mirrors the init order
// in BlorgCreateVolumeDeviceObject in reverse. FsRtlNotifyUninitializeSync
// completes any still-pending NOTIFY_CHANGE_DIRECTORY IRPs and frees the
// notify sync object; safe even if no notifies were ever registered.
//
// The work queue is destroyed before the node frees below, and that order
// is not free: freeing a node runs FsRtlUninitializeOplock, which hands
// every IRP the oplock package still holds to BlorgOplockComplete -- after
// the queue has been stopped and drained. BlorgOplockComplete checks the
// ThreadsActive gate for that reason and completes those IRPs rather than
// queueing them where nothing would ever pick them up. Reversing the order
// here instead would run oplock-released IRPs through live workers against
// a volume already mid-teardown, which is worse; the gate keeps the
// teardown sequence as it is and makes the late completions safe.
//
static VOID DriverDeleteVolumeDeviceObject(PDEVICE_OBJECT VolumeDeviceObject)
{
    if (VolumeDeviceObject)
    {
        PBLORGFS_VDO_DEVICE_EXTENSION pDevExt = BlorgGetVolumeDeviceExtension(VolumeDeviceObject);

        FsRtlNotifyUninitializeSync(&pDevExt->NotifySync);

        BlorgDestroyWorkQueue();
        BlorgNodeTableTeardown();
        BlorgFreeFileContext(pDevExt->Vcb, VolumeDeviceObject);
        FreeFileContextTree(pDevExt->RootDcb, VolumeDeviceObject);
        BlorgFreeFileContext(pDevExt->RootDcb, VolumeDeviceObject);
        ExDeleteNPagedLookasideList(&pDevExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&pDevExt->FcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->DcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->CcbLookasideList);
        IoDeleteDevice(VolumeDeviceObject);
    }
}

//
// Creates the file system device object (FSDO) and registers it with the
// I/O manager via IoRegisterFileSystem, making it visible to mount
// requests and the FS recognizer.
//
// The symbolic link is what lets usermode name the FSDO at all: the device
// lives at BLORGFS_FSDO_STRING in the object-namespace root, which
// CreateFile has no syntax for, so the vendor IOCTLs (the TLS pin update,
// the statistics query) are unreachable from usermode without it. Creating
// it is deliberately non-fatal -- a failure costs those IOCTLs, not the
// filesystem, and the device SDDL still governs who may open it.
//
static NTSTATUS DriverCreateFileSystemDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* FileSystemDeviceObject)
{
    BLORGFS_PRINT("Entering Filesystem Creation\n");
    *FileSystemDeviceObject = NULL;

    UNICODE_STRING fsdoString = RTL_CONSTANT_STRING(BLORGFS_FSDO_STRING);
    UNICODE_STRING sddlString = RTL_CONSTANT_STRING(BLORGFS_FSDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT fileSystemDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        0,
        &fsdoString,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddlString,
        &BLORGFS_FSDO_GUID,
        &fileSystemDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    UNICODE_STRING symlinkString = RTL_CONSTANT_STRING(BLORGFS_FSDO_SYMLINK_STRING);

    (VOID)IoCreateSymbolicLink(&symlinkString, &fsdoString);

    IoRegisterFileSystem(fileSystemDeviceObject);

    ClearFlag(fileSystemDeviceObject->Flags, DO_DEVICE_INITIALIZING);

    *FileSystemDeviceObject = fileSystemDeviceObject;

    return result;
}

//
// Tears down the file system device object: drops the usermode symbolic
// link, then, if a volume is still mounted on it, dereferences and
// deletes that volume device object, then unregisters and deletes the
// FSDO itself.
//
static VOID DriverDeleteFileSystemDeviceObject(PDEVICE_OBJECT FileSystemDeviceObject)
{
    if (FileSystemDeviceObject)
    {
        UNICODE_STRING symlinkString = RTL_CONSTANT_STRING(BLORGFS_FSDO_SYMLINK_STRING);

        (VOID)IoDeleteSymbolicLink(&symlinkString);

        PDEVICE_OBJECT volumeDeviceObject = global.VolumeDeviceObject;

        if (volumeDeviceObject)
        {
            ObDereferenceObject(volumeDeviceObject);
            DriverDeleteVolumeDeviceObject(volumeDeviceObject);
            global.VolumeDeviceObject = NULL;
        }

        IoUnregisterFileSystem(FileSystemDeviceObject);
        IoDeleteDevice(FileSystemDeviceObject);
    }
}

//
// Driver unload callback: releases the FSDO and DDO (dereference then
// delete, mirroring the reference taken in DriverEntry), then tears down
// the HTTP client, path cache, TLS globals, and security descriptor.
// Mostly the reverse of DriverEntry init order, with two deliberate
// exceptions around the HTTP client's inputs: RemoteAddressInfo must be
// freed while WSK is still registered (BlorgFreeHttpAddrInfo goes through
// WskFreeAddressInfo, so it has to precede BlorgCleanupHttpClient), while
// RemoteHostAnsi -- read by HttpBuildRequest on every request-issue
// path -- and RemoteHostSniAnsi -- read by BlorgTlsStartHandshakeAsync on
// every fresh TLS connection -- are only freed after BlorgCleanupHttpClient
// has drained the client.
//
// The drain runs before anything is torn down. It refuses new work and
// waits for what is already outstanding, and must complete while the
// filesystem device object still exists, because an in-flight request holds
// an IO work item queued against it.
//
// This used to be two drains in a fixed order, rings before requests, since
// a live prefetch ring would otherwise keep issuing into a drained client.
// With the ring gone there is one issuer and one gate.
//
VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    BlorgDrainHttpClient();

    ObDereferenceObject(global.FileSystemDeviceObject);
    DriverDeleteFileSystemDeviceObject(global.FileSystemDeviceObject);
    global.FileSystemDeviceObject = NULL;

    ObDereferenceObject(global.DiskDeviceObject);
    DriverDeleteDiskDeviceObject(global.DiskDeviceObject);
    global.DiskDeviceObject = NULL;

    BlorgFreeHttpAddrInfo(global.RemoteAddressInfo);

    BlorgCleanupHttpClient();

    if (global.RemoteHostAnsi)
    {
        ExFreePool(global.RemoteHostAnsi);
        global.RemoteHostAnsi = NULL;
    }

    if (global.RemoteHostSniAnsi)
    {
        ExFreePool(global.RemoteHostSniAnsi);
        global.RemoteHostSniAnsi = NULL;
    }

    BlorgPathCacheCleanup();

    BlorgTlsGlobalCleanup();

    BlorgStatisticsCleanup();

    BlorgFreeSecurityDescriptor();
}

//
//  Reads TLS config from <services key>\Parameters at
//  DriverEntry, so the backend's cert pin (and, if the operator wants,
//  the remote port) never needs a rebuild to update -- only PortOut is
//  actually mutated by a missing/absent RemotePort value; TlsEnabledOut
//  and the pin (via BlorgTlsSetPin) simply keep their existing defaults
//  (FALSE / unconfigured) when their registry values are absent.
//
//  Every failure path here is silently tolerated (missing Parameters
//  key entirely, individual values missing or the wrong type/size) --
//  this must never fail driver load, matching BlorgTlsGlobalInit's own
//  "TLS is opt-in" policy elsewhere in this same function.
//
#define BLORGFS_REG_TAG 'GRBT'
#define BLORGFS_REG_PORT_MAX_CHARS 8 // "65535" + NUL, with headroom
//
//  Kept identical to DefaultRemoteHost in BlorgFS.inf, which seeds
//  Parameters\RemoteHost with the same string at install time. If the two
//  drift, an INF install and a bare driver load reach different backends
//  and only one of them is the one anybody tested.
//
#define BLORGFS_DEFAULT_REMOTE_HOST L"10.0.50.17"

//
// BuildRemoteHostAnsiString's worst case is exactly these two caps
// combined: BLORGFS_REG_HOST_MAX_CHARS covers the host's characters plus
// the trailing NUL, and BLORGFS_REG_PORT_MAX_CHARS covers the ':'
// separator plus the port's characters. Client.c sizes its Host-header
// reads against BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES (Driver.h), so a bump
// to either cap here must be reflected there -- this assert is what makes
// that drift a build break instead of a free-build NT_ASSERT no-op.
//
static_assert(
    BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES == BLORGFS_REG_HOST_MAX_CHARS + BLORGFS_REG_PORT_MAX_CHARS,
    "BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES must track the registry host and port caps");

//
// Reads a single registry value of the expected type into Buffer, failing
// if the stored value doesn't match ExpectedType or exceeds BufferSize.
// infoBuffer's fixed 256-byte headroom over KEY_VALUE_PARTIAL_INFORMATION's
// own header covers every value this driver actually reads (a DWORD, a
// 32-byte pin, a short port string, or a BLORGFS_REG_HOST_MAX_CHARS
// hostname) -- not a general-purpose arbitrarily-sized read.
//
static NTSTATUS DriverReadRegistryValue(
    HANDLE ParametersKey,
    PCWSTR ValueName,
    ULONG ExpectedType,
    PVOID Buffer,
    ULONG BufferSize,
    PULONG ActualSize)
{
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, ValueName);

    UCHAR infoBuffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 256];
    ULONG resultLength = 0;

    NTSTATUS status = ZwQueryValueKey(
        ParametersKey,
        &valueName,
        KeyValuePartialInformation,
        infoBuffer,
        sizeof(infoBuffer),
        &resultLength);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    PKEY_VALUE_PARTIAL_INFORMATION info = C_CAST(PKEY_VALUE_PARTIAL_INFORMATION, infoBuffer);

    if (info->Type != ExpectedType || info->DataLength > BufferSize)
    {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }

    RtlCopyMemory(Buffer, info->Data, info->DataLength);
    *ActualSize = info->DataLength;

    return STATUS_SUCCESS;
}

//
// Accepts a registry-supplied RemotePort only if it is all digits and
// parses to 1..65535. Anything else -- empty, non-numeric, too long, out
// of range -- is rejected so the caller keeps the scheme-default port: a
// garbage port fed to BlorgGetHttpAddrInfo would otherwise surface only as an
// opaque resolve/connect failure at driver load. The 5-character cap is
// "65535"'s length, which also keeps the accumulator far from overflow.
//
static BOOLEAN IsValidPortString(const WCHAR* Port, USHORT PortChars)
{
    if (0 == PortChars || PortChars > 5)
    {
        return FALSE;
    }

    ULONG value = 0;

    for (USHORT i = 0; i < PortChars; ++i)
    {
        if (Port[i] < L'0' || Port[i] > L'9')
        {
            return FALSE;
        }

        value = (value * 10) + (Port[i] - L'0');
    }

    return (value >= 1) && (value <= 65535);
}

//
// Opens <ServiceRegistryPath>\Parameters and reads TlsEnabled, TlsPin,
// RemotePort, and RemoteHost into the corresponding globals/PortOut/HostOut,
// tolerating any missing key or value per the policy described above.
// PortOut->Length/HostOut->Length are set to actualSize minus one WCHAR to
// exclude the registry REG_SZ value's terminating NUL. RemotePort is
// additionally validated as a real port number (IsValidPortString); an
// invalid value is logged and ignored, keeping the scheme default.
//
// ReadAheadGranularityKb is read here too, in kilobytes, so Cc's read-ahead
// granularity can be swept without a rebuild and redeploy per point. The
// committed default was measured on eight concurrent streams -- a
// throughput workload -- and never against a latency-shaped one, and
// nothing below 256 KB was ever tried, where FastFat uses 64 KB and Cc's
// own default is a page. Zero means do not call CcSetReadAheadGranularity
// at all, which is the "leave it default" case and cannot be expressed by
// any other value.
//
static VOID DriverReadRegistryConfig(PUNICODE_STRING ServiceRegistryPath, PUNICODE_STRING PortOut, PUNICODE_STRING HostOut)
{
    UNICODE_STRING parametersSuffix = RTL_CONSTANT_STRING(L"\\Parameters");

    UNICODE_STRING parametersPath;
    parametersPath.Length = 0;
    parametersPath.MaximumLength = ServiceRegistryPath->Length + parametersSuffix.Length + sizeof(WCHAR);
    parametersPath.Buffer = ExAllocatePoolZero(NonPagedPoolNx, parametersPath.MaximumLength, BLORGFS_REG_TAG);

    if (!parametersPath.Buffer)
    {
        return;
    }

    RtlAppendUnicodeStringToString(&parametersPath, ServiceRegistryPath);
    RtlAppendUnicodeStringToString(&parametersPath, &parametersSuffix);

    OBJECT_ATTRIBUTES objectAttributes;
    InitializeObjectAttributes(&objectAttributes, &parametersPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    HANDLE parametersKey;
    NTSTATUS status = ZwOpenKey(&parametersKey, KEY_READ, &objectAttributes);

    ExFreePool(parametersPath.Buffer);

    if (!NT_SUCCESS(status))
    {
        return;
    }

    ULONG tlsEnabledValue = 0;
    ULONG actualSize = 0;

    if (NT_SUCCESS(DriverReadRegistryValue(parametersKey, L"TlsEnabled", REG_DWORD, &tlsEnabledValue, sizeof(tlsEnabledValue), &actualSize)))
    {
        global.TlsEnabled = (0 != tlsEnabledValue);
    }

    ULONG granularityKb = 0;

    if (NT_SUCCESS(DriverReadRegistryValue(parametersKey, L"ReadAheadGranularityKb", REG_DWORD, &granularityKb, sizeof(granularityKb), &actualSize)))
    {
        global.ReadAheadGranularity = granularityKb * 1024;
        BLORGFS_LOG("DriverReadRegistryConfig() - read-ahead granularity override: %lu KB\n", granularityKb);
    }

    UCHAR pinValue[TLS_HASH_LEN];

    if (NT_SUCCESS(DriverReadRegistryValue(parametersKey, L"TlsPin", REG_BINARY, pinValue, sizeof(pinValue), &actualSize))
        && TLS_HASH_LEN == actualSize)
    {
        BlorgTlsSetPin(pinValue);
    }

    WCHAR portValue[BLORGFS_REG_PORT_MAX_CHARS];

    if (NT_SUCCESS(DriverReadRegistryValue(parametersKey, L"RemotePort", REG_SZ, portValue, sizeof(portValue), &actualSize))
        && actualSize >= sizeof(WCHAR))
    {
        USHORT portChars = C_CAST(USHORT, (actualSize - sizeof(WCHAR)) / sizeof(WCHAR));

        if (IsValidPortString(portValue, portChars))
        {
            PortOut->Length = portChars * sizeof(WCHAR);
            RtlCopyMemory(PortOut->Buffer, portValue, PortOut->Length);
        }
        else
        {
            BLORGFS_LOG("DriverReadRegistryConfig() - ignoring invalid RemotePort registry value, using scheme default\n");
        }
    }

    WCHAR hostValue[BLORGFS_REG_HOST_MAX_CHARS];

    if (NT_SUCCESS(DriverReadRegistryValue(parametersKey, L"RemoteHost", REG_SZ, hostValue, sizeof(hostValue), &actualSize))
        && actualSize >= sizeof(WCHAR))
    {
        HostOut->Length = C_CAST(USHORT, actualSize) - sizeof(WCHAR);
        RtlCopyMemory(HostOut->Buffer, hostValue, HostOut->Length);
    }

    ZwClose(parametersKey);
}

//
// Builds "global.RemoteHostAnsi" -- the ANSI "host" or "host:port"
// authority used in every outgoing request's "Host:" header (Client.c's
// HttpBuildRequest). PortString is NULL when the resolved port is the
// scheme default (80 plaintext / 443 TLS), where RFC 9112 wants the port
// omitted; otherwise it is appended so any name-based routing in front of
// the backend sees the same authority the driver actually connected to --
// with the plaintext default of 8080 the port is therefore normally
// present. Truncating WCHAR->CHAR is safe here: both parts are either
// compiled-in defaults or registry-supplied DNS names/port numbers, which
// are ASCII-only by definition. Total size is bounded by
// BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES (Driver.h), which Client.c relies on.
//
static PSTR BuildRemoteHostAnsiString(PCUNICODE_STRING HostString, PCUNICODE_STRING PortString)
{
    USHORT hostChars = HostString->Length / sizeof(WCHAR);
    USHORT portChars = PortString ? PortString->Length / sizeof(WCHAR) : 0;
    SIZE_T ansiSize = C_CAST(SIZE_T, hostChars) + (PortString ? C_CAST(SIZE_T, portChars) + 1 : 0) + 1;

    NT_ASSERT(ansiSize <= BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES);

    PSTR ansiHost = ExAllocatePoolZero(NonPagedPoolNx, ansiSize, BLORGFS_REG_TAG);

    if (!ansiHost)
    {
        return NULL;
    }

    PSTR dst = ansiHost;
    PWCH src = HostString->Buffer;

    for (USHORT i = 0; i < hostChars; ++i)
    {
        *dst++ = C_CAST(CHAR, *src++);
    }

    if (PortString)
    {
        *dst++ = ':';
        src = PortString->Buffer;

        for (USHORT i = 0; i < portChars; ++i)
        {
            *dst++ = C_CAST(CHAR, *src++);
        }
    }

    return ansiHost;
}

//
// RFC 6066 3: the SNI HostName must be a DNS hostname -- IPv4 and IPv6
// literals are explicitly not permitted. This is a shape test, not full
// address validation: any ':' means an IPv6 literal (impossible in a
// hostname), and a string of only digits and dots is taken as an IPv4
// literal. It deliberately errs toward calling something a literal,
// since the failure modes are asymmetric -- omitting SNI is always legal,
// sending a literal in it never is.
//
static BOOLEAN HostStringIsIpLiteral(PCUNICODE_STRING HostString)
{
    BOOLEAN digitsAndDotsOnly = TRUE;

    for (USHORT i = 0; i < HostString->Length / sizeof(WCHAR); ++i)
    {
        WCHAR c = HostString->Buffer[i];

        if (L':' == c)
        {
            return TRUE;
        }

        if ((c < L'0' || c > L'9') && (L'.' != c))
        {
            digitsAndDotsOnly = FALSE;
        }
    }

    return digitsAndDotsOnly;
}

//
// Driver load entry point: initializes the path cache and TLS globals,
// wires up the major function table, FastIO dispatch, and cache manager
// callbacks, then creates the FSDO and DDO device objects and the HTTP
// client in order, reads TLS/port config from the registry, resolves the
// backend address (defaulting the port by TlsEnabled), and finally
// announces the volume to the mount manager. Any device/client/address
// creation step failing unwinds everything created so far and returns
// STATUS_FAILED_DRIVER_ENTRY; the final mount-manager notification is
// best-effort and does not fail the load.
//
// BlorgTlsGlobalInit is non-fatal: TLS is opt-in (global.TlsEnabled, off by
// default), so a failure here just means it stays unusable if later
// enabled -- it must not fail the whole driver load, since the plaintext
// HTTP path doesn't touch this at all. See Tls.h's BlorgTlsGlobalInit comment
// for why this one-time, driver-lifetime provider handle exists.
//
// TLS/port config (TlsEnabled, TlsPin, an optional RemotePort override)
// is read from the registry before resolving the backend address, so the
// default port can depend on whatever TlsEnabled ends up being -- see
// DriverReadRegistryConfig. With no explicit RemotePort override, the
// default port tracks TlsEnabled exactly (a plaintext server can't parse
// a ClientHello, and a TLS-speaking one won't understand plaintext HTTP):
// 443 if TRUE, 8080 if FALSE. RTL_CONSTANT_STRING only expands to a valid
// initializer, not a general expression, hence if/else rather than a
// ternary for picking the default. RemoteHost works the same way --
// Parameters\RemoteHost if present, else BLORGFS_DEFAULT_REMOTE_HOST --
// and the same resolved UNICODE_STRING both drives BlorgGetHttpAddrInfo and
// (via BuildRemoteHostAnsiString) becomes global.RemoteHostAnsi, so the
// Host header always names whatever address the driver actually
// resolved/connected to, never a stale literal. The Host header carries
// an explicit :port whenever the resolved port isn't the scheme default
// (80 plaintext / 443 TLS) -- see BuildRemoteHostAnsiString.
//
// The mount-manager volume-arrival notification runs last, only once the
// FS is registered, the disk device + B: symlink are up, and the HTTP
// client is ready -- processing the arrival can make the manager open the
// volume, which triggers a mount, which needs the HTTP client live. It is
// best-effort: the manual B: symlink already provides working file I/O,
// so a failure here is logged but does not abort the load -- it only
// costs executable launch from the volume (the kernel needs the
// mount-manager volume identity to resolve B: during image activation).
//
// The HTTP/WSK client comes up before the filesystem is registered, and that
// order is load bearing rather than tidy.
//
// Not because of DO_DEVICE_INITIALIZING -- an ordinary device object
// receives nothing until the load completes. IoRegisterFileSystem is the one
// that matters: it puts this FSD on the I/O manager's registration list there
// and then, and every arriving volume is offered to every registered
// filesystem from another thread, so a mount can land while DriverEntry is
// still running. FsCtrlMountVolume already says so and is written to survive
// it (FsCtrl.c: "the window DriverEntry opens between IoRegisterFileSystem
// and the DDO existing").
//
// A mount reaches BlorgCreateVolumeDeviceObject and from there the read path,
// so initialising the client afterwards left a window in which SocketPool and
// WskProviderNpi were nothing but their zero-initialised storage. Zeroed is
// not harmless: IsListEmpty tests Flink against the list head, and a zeroed
// head has Flink NULL, so the pool reads as NOT empty and the next acquire
// runs RemoveHeadList on a NULL Flink. WskProviderNpi.Dispatch is NULL over
// the same window, and every early return from there on therefore tears the
// client back down.
//
//
// The fast-I/O table is zeroed before it is published, not after: nothing
// can dispatch yet, since no device object exists, but publishing a pointer
// to uninitialised storage and filling it lines later is an ordering one
// future edit away from mattering.
//
// Every load-failure exit frees what DriverEntry allocated ahead of the
// failing step, including the two allocations whose own initialisation is
// tolerant of failure and are therefore the easy ones to forget: the
// per-processor statistics table and the TLS provider handles.
//
// The keep-alive pool is filled last, before anything asks for it. The
// address is resolved by that point and the filesystem is already
// registered, so the first reads may arrive at any moment -- and without it
// they would each open their own connection while a reader waits, at a
// measured ~1.02 seconds per connect. It is fire-and-forget by
// construction: BlorgPrewarmSocketPool returns immediately, fills one
// connection at a time behind the caller, and a failure leaves the driver
// exactly as it behaved before. DriverEntry must not wait on the network,
// and does not.
//
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ExInitializeDriverRuntime(0);

    BlorgPathCacheInit();

    NTSTATUS statisticsInitStatus = BlorgStatisticsInitialize();
    if (!NT_SUCCESS(statisticsInitStatus))
    {
        BLORGFS_LOG("DriverEntry() - BlorgStatisticsInitialize failed: 0x%X (counters unavailable)\n", statisticsInitStatus);
    }

    NTSTATUS tlsInitStatus = BlorgTlsGlobalInit();
    if (!NT_SUCCESS(tlsInitStatus))
    {
        BLORGFS_LOG("DriverEntry() - BlorgTlsGlobalInit failed: 0x%X (TLS unavailable if enabled)\n", tlsInitStatus);
    }

    BlorgTlsHandshakeGlobalInit();

    global.DriverObject = DriverObject;

    DriverObject->DriverUnload = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = BlorgCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = BlorgClose;
    DriverObject->MajorFunction[IRP_MJ_READ] = BlorgRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = BlorgWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] = BlorgQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = BlorgSetInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_EA] = BlorgQueryEa;
    DriverObject->MajorFunction[IRP_MJ_SET_EA] = BlorgSetEa;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = BlorgFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = BlorgQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] = BlorgSetVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = BlorgDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = BlorgFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = BlorgDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = BlorgShutdown;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL] = BlorgLockControl;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = BlorgCleanup;
    DriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY] = BlorgQuerySecurity;
    DriverObject->MajorFunction[IRP_MJ_SET_SECURITY] = BlorgSetSecurity;

    RtlZeroMemory(&BlorgFsFastDispatch, sizeof(FAST_IO_DISPATCH));

#pragma warning(suppress: 28175)
    DriverObject->FastIoDispatch = &BlorgFsFastDispatch;

    global.CacheManagerCallbacks.AcquireForLazyWrite = BlorgAcquireNodeForLazyWrite;
    global.CacheManagerCallbacks.ReleaseFromLazyWrite = BlorgReleaseNodeFromLazyWrite;
    global.CacheManagerCallbacks.AcquireForReadAhead = BlorgAcquireNodeForReadAhead;
    global.CacheManagerCallbacks.ReleaseFromReadAhead = BlorgReleaseNodeFromReadAhead;

    BlorgFsFastDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    BlorgFsFastDispatch.FastIoCheckIfPossible = FastIoCheckIfPossible;
    BlorgFsFastDispatch.FastIoRead = BlorgFastIoRead;
    BlorgFsFastDispatch.MdlRead = FsRtlMdlReadDev;
    BlorgFsFastDispatch.MdlReadComplete = FsRtlMdlReadCompleteDev;
    

    NTSTATUS result = BlorgInitialiseHttpClient();

    if (!NT_SUCCESS(result))
    {
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    result = BlorgInitializeSecurityDescriptor();

    if (!NT_SUCCESS(result))
    {
        BlorgCleanupHttpClient();
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    PDEVICE_OBJECT fileSystemDeviceObject;
    result = DriverCreateFileSystemDeviceObject(DriverObject, &fileSystemDeviceObject);

    if (!NT_SUCCESS(result))
    {
        BlorgFreeSecurityDescriptor();
        BlorgCleanupHttpClient();
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(fileSystemDeviceObject);
    global.FileSystemDeviceObject = fileSystemDeviceObject;

    PDEVICE_OBJECT diskDeviceObject;
    result = DriverCreateDiskDeviceObject(DriverObject, &diskDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ObDereferenceObject(global.FileSystemDeviceObject);
        DriverDeleteFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        BlorgFreeSecurityDescriptor();
        BlorgCleanupHttpClient();
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(diskDeviceObject);
    global.DiskDeviceObject = diskDeviceObject;

    WCHAR portBuffer[BLORGFS_REG_PORT_MAX_CHARS];
    UNICODE_STRING portString;
    portString.Length = 0;
    portString.MaximumLength = sizeof(portBuffer);
    portString.Buffer = portBuffer;

    WCHAR hostBuffer[BLORGFS_REG_HOST_MAX_CHARS];
    UNICODE_STRING hostString;
    hostString.Length = 0;
    hostString.MaximumLength = sizeof(hostBuffer);
    hostString.Buffer = hostBuffer;

    global.ReadAheadGranularity = READ_AHEAD_GRANULARITY;

    DriverReadRegistryConfig(RegistryPath, &portString, &hostString);

    if (0 == portString.Length)
    {
        if (global.TlsEnabled)
        {
            UNICODE_STRING defaultPort = RTL_CONSTANT_STRING(L"443");
            RtlCopyUnicodeString(&portString, &defaultPort);
        }
        else
        {
            UNICODE_STRING defaultPort = RTL_CONSTANT_STRING(L"8080");
            RtlCopyUnicodeString(&portString, &defaultPort);
        }
    }

    if (0 == hostString.Length)
    {
        UNICODE_STRING defaultHost = RTL_CONSTANT_STRING(BLORGFS_DEFAULT_REMOTE_HOST);
        RtlCopyUnicodeString(&hostString, &defaultHost);
    }

    ADDRINFOEXW hints = { .ai_flags = AI_CANONNAME, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };

    result = BlorgGetHttpAddrInfo(&hostString, &portString, &hints, &global.RemoteAddressInfo);

    if (!NT_SUCCESS(result))
    {
        BlorgCleanupHttpClient();
        ObDereferenceObject(global.FileSystemDeviceObject);
        DriverDeleteFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DriverDeleteDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        BlorgFreeSecurityDescriptor();
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    UNICODE_STRING schemeDefaultPort;

    if (global.TlsEnabled)
    {
        UNICODE_STRING tlsDefaultPort = RTL_CONSTANT_STRING(L"443");
        schemeDefaultPort = tlsDefaultPort;
    }
    else
    {
        UNICODE_STRING plaintextDefaultPort = RTL_CONSTANT_STRING(L"80");
        schemeDefaultPort = plaintextDefaultPort;
    }

    BOOLEAN portIsSchemeDefault = RtlEqualUnicodeString(&portString, &schemeDefaultPort, FALSE);

    global.RemoteHostAnsi = BuildRemoteHostAnsiString(&hostString, portIsSchemeDefault ? NULL : &portString);

    if (!global.RemoteHostAnsi)
    {
        BlorgFreeHttpAddrInfo(global.RemoteAddressInfo);
        BlorgCleanupHttpClient();
        ObDereferenceObject(global.FileSystemDeviceObject);
        DriverDeleteFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DriverDeleteDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        BlorgFreeSecurityDescriptor();
        BlorgStatisticsCleanup();
        BlorgTlsGlobalCleanup();
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    if (global.TlsEnabled && !HostStringIsIpLiteral(&hostString))
    {
        global.RemoteHostSniAnsi = BuildRemoteHostAnsiString(&hostString, NULL);

        if (!global.RemoteHostSniAnsi)
        {
            BLORGFS_LOG("DriverEntry() - SNI host string allocation failed; ClientHello will omit SNI\n");
        }
    }

    BlorgPrewarmSocketPool(
        C_CAST(const SOCKADDR*, global.RemoteAddressInfo->ai_addr),
        BLORGFS_SOCKET_PREWARM_COUNT);

    {
        UNICODE_STRING diskDeviceName = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
        NTSTATUS mountMgrStatus = DriverNotifyMountManagerVolumeArrival(&diskDeviceName);

        if (!NT_SUCCESS(mountMgrStatus))
        {
            BLORGFS_LOG("DriverEntry() - mount manager volume-arrival notification failed: 0x%X\n", mountMgrStatus);
        }
    }

    return STATUS_SUCCESS;
}
