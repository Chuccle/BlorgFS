#include "Driver.h"

FAST_IO_DISPATCH BlorgFsFastDispatch;
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
    UNICODE_STRING uniVDOString = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
    UNICODE_STRING uniSDDLString = RTL_CONSTANT_STRING(BLORGFS_DDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT pDiskDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_DDO_DEVICE_EXTENSION),
        &uniVDOString,
        FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &uniSDDLString,
        &BLORGFS_DDO_GUID,
        &pDiskDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    USHORT wchBufferSize = 128 * sizeof(WCHAR);

    PWCHAR wchBuffer = ExAllocatePoolZero(PagedPool, wchBufferSize, 'BFS');

    if (!wchBuffer)
    {
        IoDeleteDevice(pDiskDeviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PBLORGFS_DDO_DEVICE_EXTENSION pDevExt = pDiskDeviceObject->DeviceExtension;

    pDevExt->Hdr.Identifier = BLORGFS_DDO_MAGIC;

    RtlInitEmptyUnicodeString(&pDevExt->SymLinkName, wchBuffer, wchBufferSize);

    result = RtlUnicodeStringPrintf(&pDevExt->SymLinkName,
        BLORGFS_DOS_DRIVELETTER_FORMAT_STRING,
        L'B');

    if (!NT_SUCCESS(result))
    {
        ExFreePool(GetDiskDeviceExtension(pDiskDeviceObject)->SymLinkName.Buffer);
        IoDeleteDevice(pDiskDeviceObject);
        return result;
    }

    IoCreateSymbolicLink(&pDevExt->SymLinkName, &uniVDOString);

    *DiskDeviceObject = pDiskDeviceObject;

    return result;
}

static void DeleteBlorgDiskDeviceObject(PDEVICE_OBJECT pDiskDeviceObject)
{
    if (pDiskDeviceObject)
    {
        IoDeleteSymbolicLink(&GetDiskDeviceExtension(pDiskDeviceObject)->SymLinkName);
        ExFreePool(GetDiskDeviceExtension(pDiskDeviceObject)->SymLinkName.Buffer);
        IoDeleteDevice(pDiskDeviceObject);
    }
}

NTSTATUS CreateBlorgVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject)
{
    BLORGFS_PRINT("Entering Volume Creation\n");
    *VolumeDeviceObject = NULL;
    UNICODE_STRING uniVDOString = RTL_CONSTANT_STRING(BLORGFS_VDO_STRING);
    UNICODE_STRING uniSDDLString = RTL_CONSTANT_STRING(BLORGFS_VDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT pVolumeDeviceObject = NULL;

    NTSTATUS result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_VDO_DEVICE_EXTENSION),
        &uniVDOString,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &uniSDDLString,
        &BLORGFS_VDO_GUID,
        &pVolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    PBLORGFS_VDO_DEVICE_EXTENSION pDevExt = pVolumeDeviceObject->DeviceExtension;

    pDevExt->Hdr.Identifier = BLORGFS_VDO_MAGIC;

    result = ExInitializeResourceLite(&pDevExt->NodeResource);

    if (!NT_SUCCESS(result))
    {
        IoDeleteDevice(pVolumeDeviceObject);
        return result;
    }

    UNICODE_STRING rootDirectoryName = RTL_CONSTANT_STRING(L"\\");

    ExInitializeNPagedLookasideList(&pDevExt->NonPagedNodeLookasideList, NULL, NULL, 0, sizeof(NON_PAGED_NODE), 'NPN', 0);
    ExInitializePagedLookasideList(&pDevExt->FcbLookasideList, NULL, NULL, 0, sizeof(FCB), 'FCB', 0);
    ExInitializePagedLookasideList(&pDevExt->DcbLookasideList, NULL, NULL, 0, sizeof(DCB), 'DCB', 0);
    ExInitializePagedLookasideList(&pDevExt->CcbLookasideList, NULL, NULL, 0, sizeof(CCB), 'CCB', 0);


    result = BlorgCreateDCB(&pDevExt->RootDcb, NULL, BLORGFS_ROOT_DCB_SIGNATURE, &rootDirectoryName, pVolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&pDevExt->NodeResource);
        ExDeleteNPagedLookasideList(&pDevExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&pDevExt->FcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->DcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->CcbLookasideList);
        IoDeleteDevice(pVolumeDeviceObject);
        return result;
    }

    result = BlorgCreateFCB(&pDevExt->Vcb, NULL, BLORGFS_VCB_SIGNATURE, NULL, pVolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        BlorgFreeFileContext(pDevExt->RootDcb);
        ExDeleteResourceLite(&pDevExt->NodeResource);
        ExDeleteNPagedLookasideList(&pDevExt->NonPagedNodeLookasideList);
        ExDeletePagedLookasideList(&pDevExt->FcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->DcbLookasideList);
        ExDeletePagedLookasideList(&pDevExt->CcbLookasideList);
        IoDeleteDevice(pVolumeDeviceObject);
        return result;
    }

    *VolumeDeviceObject = pVolumeDeviceObject;
    return STATUS_SUCCESS;
}

static void DeleteBlorgVolumeDeviceObject(PDEVICE_OBJECT VolumeDeviceObject)
{
    if (VolumeDeviceObject)
    {
        PBLORGFS_VDO_DEVICE_EXTENSION pDevExt = GetVolumeDeviceExtension(VolumeDeviceObject);
        BlorgFreeFileContext(pDevExt->Vcb);
        BlorgFreeFileContext(pDevExt->RootDcb);
        ExDeleteResourceLite(&pDevExt->NodeResource);
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

    UNICODE_STRING uniFSDOString = RTL_CONSTANT_STRING(BLORGFS_FSDO_STRING);
    UNICODE_STRING uniSDDLString = RTL_CONSTANT_STRING(BLORGFS_FSDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT pFileSystemDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(DriverObject,
        sizeof(BLORGFS_FSDO_DEVICE_EXTENSION),
        &uniFSDOString,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &uniSDDLString,
        &BLORGFS_FSDO_GUID,
        &pFileSystemDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    PBLORGFS_FSDO_DEVICE_EXTENSION pDevExt = pFileSystemDeviceObject->DeviceExtension;

    pDevExt->Hdr.Identifier = BLORGFS_FSDO_MAGIC;

    IoRegisterFileSystem(pFileSystemDeviceObject);

    *FileSystemDeviceObject = pFileSystemDeviceObject;

    return result;
}

static void DeleteBlorgFileSystemDeviceObject(PDEVICE_OBJECT FileSystemDeviceObject)
{
    if (FileSystemDeviceObject)
    {
        PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(FileSystemDeviceObject)->VolumeDeviceObject;

        if (pVolumeDeviceObject)
        {
            ObDereferenceObject(pVolumeDeviceObject);
            DeleteBlorgVolumeDeviceObject(pVolumeDeviceObject);
        }

        IoUnregisterFileSystem(FileSystemDeviceObject);
        IoDeleteDevice(FileSystemDeviceObject);
    }
}

void DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    KdBreakPoint();

    ObDereferenceObject(global.FileSystemDeviceObject);
    DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
    global.FileSystemDeviceObject = NULL;

    ObDereferenceObject(global.DiskDeviceObject);
    DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
    global.DiskDeviceObject = NULL;

    FreeHttpAddrInfo(global.RemoteAddressInfo);

    CleanupHttpClient();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS  result = STATUS_SUCCESS;

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

    PDEVICE_OBJECT pFileSystemDeviceObject;
    result = CreateBlorgFileSystemDeviceObject(DriverObject, &pFileSystemDeviceObject);

    if (!NT_SUCCESS(result))
    {
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(pFileSystemDeviceObject);
    global.FileSystemDeviceObject = pFileSystemDeviceObject;

    PDEVICE_OBJECT pDiskDeviceObject;
    result = CreateBlorgDiskDeviceObject(DriverObject, &pDiskDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ObReferenceObject(pDiskDeviceObject);
    global.DiskDeviceObject = pDiskDeviceObject;

    result = InitialiseHttpClient();

    if (!NT_SUCCESS(result))
    {
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ADDRINFOEXW hints = { .ai_flags = AI_CANONNAME, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    UNICODE_STRING uniNodeString = RTL_CONSTANT_STRING(L"blorgfs-server.blorg.lan");
    UNICODE_STRING uniPortString = RTL_CONSTANT_STRING(L"8080");

    result = GetHttpAddrInfo(&uniNodeString, &uniPortString, &hints, &global.RemoteAddressInfo);

    if (!NT_SUCCESS(result))
    {
        CleanupHttpClient();
        ObDereferenceObject(global.FileSystemDeviceObject);
        DeleteBlorgFileSystemDeviceObject(global.FileSystemDeviceObject);
        global.FileSystemDeviceObject = NULL;
        ObDereferenceObject(global.DiskDeviceObject);
        DeleteBlorgDiskDeviceObject(global.DiskDeviceObject);
        global.DiskDeviceObject = NULL;
        return STATUS_FAILED_DRIVER_ENTRY;
    }

    ANSI_STRING path = RTL_CONSTANT_STRING("\\");

    result = GetHttpDirectoryInfo(&path);

    if (!NT_SUCCESS(result))
    {
        FreeHttpAddrInfo(global.RemoteAddressInfo);
        CleanupHttpClient();
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