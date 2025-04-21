#include "Driver.h"

FAST_IO_DISPATCH  BlorgFsFastDispatch;
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     DriverUnload;

struct GLOBAL global;

// {02EF343C-413D-4932-BBCE-15624AACE5D9}
static const GUID BLORGFS_FSDO_GUID = { 0x2ef343c, 0x413d, 0x4932, { 0xbb, 0xce, 0x15, 0x62, 0x4a, 0xac, 0xe5, 0xd9 } };
// {A6E07401-F24E-443E-A47C-D9BD219B9E68}
static const GUID BLORGFS_VDO_GUID = { 0xa6e07401, 0xf24e, 0x443e, { 0xa4, 0x7c, 0xd9, 0xbd, 0x21, 0x9b, 0x9e, 0x68 } };
// {CC6E9F4D-1968-4D95-91AF-FFD72F35F6DA}
static const GUID BLORGFS_DDO_GUID = { 0xcc6e9f4d, 0x1968, 0x4d95, { 0x91, 0xaf, 0xff, 0xd7, 0x2f, 0x35, 0xf6, 0xda } };

static NTSTATUS CreateBlorgDiskDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* DiskDeviceObject)
{
    BLORGFS_PRINT("Entering Drive Creation\n");
    *DiskDeviceObject = NULL;
    UNICODE_STRING vdoString = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
    UNICODE_STRING sddlString = RTL_CONSTANT_STRING(BLORGFS_DDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT diskDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_DDO_DEVICE_EXTENSION),
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

    USHORT nameBufferSize = 128 * sizeof(WCHAR);

    PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, nameBufferSize, 'BFS');

    if (!nameBuffer)
    {
        IoDeleteDevice(diskDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PBLORGFS_DDO_DEVICE_EXTENSION devExt = diskDeviceObject->DeviceExtension;

    devExt->Hdr.Identifier = BLORGFS_DDO_MAGIC;

    RtlInitEmptyUnicodeString(&devExt->SymLinkName, nameBuffer, nameBufferSize);

    result = RtlUnicodeStringPrintf(&devExt->SymLinkName,
        BLORGFS_DOS_DRIVELETTER_FORMAT_STRING,
        L'B');

    if (!NT_SUCCESS(result))
    {
        ExFreePool(GetDiskDeviceExtension(diskDeviceObject)->SymLinkName.Buffer);
        IoDeleteDevice(diskDeviceObject);
        return result;
    }

    IoCreateSymbolicLink(&devExt->SymLinkName, &vdoString);

    *DiskDeviceObject = diskDeviceObject;

    return result;
}

static void DeleteBlorgDiskDeviceObject(PDEVICE_OBJECT DiskDeviceObject)
{
    if (DiskDeviceObject)
    {
        IoDeleteSymbolicLink(&GetDiskDeviceExtension(DiskDeviceObject)->SymLinkName);
        ExFreePool(GetDiskDeviceExtension(DiskDeviceObject)->SymLinkName.Buffer);
        IoDeleteDevice(DiskDeviceObject);
    }
}

NTSTATUS CreateBlorgVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject)
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

    devExt->Hdr.Identifier = BLORGFS_VDO_MAGIC;

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

    InitializeListHead(&devExt->OverflowQueue);

    KeInitializeSpinLock(&devExt->OverflowQueueSpinLock);

    ClearFlag(volumeDeviceObject->Flags, DO_DEVICE_INITIALIZING);

    *VolumeDeviceObject = volumeDeviceObject;
    return STATUS_SUCCESS;
}

static void DeleteBlorgVolumeDeviceObject(PDEVICE_OBJECT VolumeDeviceObject)
{
    if (VolumeDeviceObject)
    {
        PBLORGFS_VDO_DEVICE_EXTENSION pDevExt = GetVolumeDeviceExtension(VolumeDeviceObject);
        BlorgFreeFileContext(pDevExt->Vcb, VolumeDeviceObject);
        BlorgFreeFileContext(pDevExt->RootDcb, VolumeDeviceObject);
        ExDeleteNPagedLookasideList(&pDevExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&pDevExt->FcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->DcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->CcbLookasideList);
        IoDeleteDevice(VolumeDeviceObject);
    }
}

static NTSTATUS CreateBlorgFileSystemDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* FileSystemDeviceObject)
{
    BLORGFS_PRINT("Entering Filesystem Creation\n");
    *FileSystemDeviceObject = NULL;

    UNICODE_STRING fsdoString = RTL_CONSTANT_STRING(BLORGFS_FSDO_STRING);
    UNICODE_STRING sddlString = RTL_CONSTANT_STRING(BLORGFS_FSDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT fileSystemDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_FSDO_DEVICE_EXTENSION),
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

    PBLORGFS_FSDO_DEVICE_EXTENSION devExt = fileSystemDeviceObject->DeviceExtension;

    devExt->Hdr.Identifier = BLORGFS_FSDO_MAGIC;

    IoRegisterFileSystem(fileSystemDeviceObject);

    *FileSystemDeviceObject = fileSystemDeviceObject;

    return result;
}

static void DeleteBlorgFileSystemDeviceObject(PDEVICE_OBJECT FileSystemDeviceObject)
{
    if (FileSystemDeviceObject)
    {
        PDEVICE_OBJECT volumeDeviceObject = GetFileSystemDeviceExtension(FileSystemDeviceObject)->VolumeDeviceObject;

        if (volumeDeviceObject)
        {
            ObDereferenceObject(volumeDeviceObject);
            DeleteBlorgVolumeDeviceObject(volumeDeviceObject);
        }

        IoUnregisterFileSystem(FileSystemDeviceObject);
        IoDeleteDevice(FileSystemDeviceObject);
    }
}

void DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    ObDereferenceObject(global.FileSystemDeviceObject);
    DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
    global.FileSystemDeviceObject = NULL;

    ObDereferenceObject(global.DiskDeviceObject);
    DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
    global.DiskDeviceObject = NULL;

    ExDeleteNPagedLookasideList(&global.IrpContextLookasideList);

    FreeHttpAddrInfo(global.RemoteAddressInfo);

    CleanupHttpClient();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

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

#pragma warning(suppress: 28175) // "We are a filesystem. Touching FastIoDispatch is allowed"
    DriverObject->FastIoDispatch = &BlorgFsFastDispatch;

    RtlZeroMemory(&BlorgFsFastDispatch, sizeof(FAST_IO_DISPATCH));

    global.CacheManagerCallbacks.AcquireForLazyWrite = BlorgAcquireNodeForLazyWrite;
    global.CacheManagerCallbacks.ReleaseFromLazyWrite = BlorgReleaseNodeFromLazyWrite;
    global.CacheManagerCallbacks.AcquireForReadAhead = BlorgAcquireNodeForReadAhead;
    global.CacheManagerCallbacks.ReleaseFromReadAhead = BlorgReleaseNodeFromReadAhead;

    BlorgFsFastDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    BlorgFsFastDispatch.FastIoCheckIfPossible = FastIoCheckIfPossible;
    BlorgFsFastDispatch.FastIoRead = FsRtlCopyRead;
    BlorgFsFastDispatch.MdlRead = FsRtlMdlReadDev;
    BlorgFsFastDispatch.MdlReadComplete = FsRtlMdlReadCompleteDev;
    
    PDEVICE_OBJECT fileSystemDeviceObject;
    NTSTATUS result = CreateBlorgFileSystemDeviceObject(DriverObject, &fileSystemDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(fileSystemDeviceObject);
    global.FileSystemDeviceObject = fileSystemDeviceObject;

    PDEVICE_OBJECT diskDeviceObject;
    result = CreateBlorgDiskDeviceObject(DriverObject, &diskDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(diskDeviceObject);
    global.DiskDeviceObject = diskDeviceObject;

    ExInitializeNPagedLookasideList(&global.IrpContextLookasideList, NULL, NULL, POOL_NX_ALLOCATION, sizeof(IRP_CONTEXT) + IoSizeofWorkItem(), 'ICTX', 0);

    result = InitialiseHttpClient();

    if (!NT_SUCCESS(result))
    {
        ExDeleteNPagedLookasideList(&global.IrpContextLookasideList);
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ADDRINFOEXW hints = { .ai_flags = AI_CANONNAME, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    UNICODE_STRING nodeString = RTL_CONSTANT_STRING(L"blorgfs-server.blorg.lan");
    UNICODE_STRING portString = RTL_CONSTANT_STRING(L"8080");

    result = GetHttpAddrInfo(&nodeString, &portString, &hints, &global.RemoteAddressInfo);

    if (!NT_SUCCESS(result))
    {
        CleanupHttpClient();
        ExDeleteNPagedLookasideList(&global.IrpContextLookasideList);
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    return STATUS_SUCCESS;
}