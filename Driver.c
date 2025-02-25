#include "Driver.h"

FAST_IO_DISPATCH BlorgFsFastDispatch;

struct GLOBAL global;

// {A6E07401-F24E-443E-A47C-D9BD219B9E68}
static const GUID BLORGFS_VDO_GUID = { 0xa6e07401, 0xf24e, 0x443e, { 0xa4, 0x7c, 0xd9, 0xbd, 0x21, 0x9b, 0x9e, 0x68 } };
// {CC6E9F4D-1968-4D95-91AF-FFD72F35F6DA}
static const GUID BLORGFS_DDO_GUID = { 0xcc6e9f4d, 0x1968, 0x4d95, { 0x91, 0xaf, 0xff, 0xd7, 0x2f, 0x35, 0xf6, 0xda } };

static NTSTATUS CreateBlorgDiskDevice(PDRIVER_OBJECT pDriverObject)
{
	BLORGFS_PRINT("Entering Drive Creation\n");
	UNICODE_STRING uniVDOString = RTL_CONSTANT_STRING(BLORGFS_DDO_STRING);
	UNICODE_STRING uniSDDLString = RTL_CONSTANT_STRING(BLORGFS_DDO_DEVICE_SDDL_STRING);
	PDEVICE_OBJECT pDiskDeviceObject = NULL;
	NTSTATUS       result = STATUS_UNSUCCESSFUL;

	result = WdmlibIoCreateDeviceSecure(pDriverObject,
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
	if (NULL == wchBuffer)
	{
		IoDeleteDevice(pDiskDeviceObject);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	PBLORGFS_DDO_DEVICE_EXTENSION pDevExt = pDiskDeviceObject->DeviceExtension;

	pDevExt->Hdr.Identifier = BLORGFS_DDO_MAGIC;

	global.pDiskDeviceObject = pDiskDeviceObject;

	ClearFlag(global.pDiskDeviceObject->Flags, DO_DEVICE_INITIALIZING);

	RtlInitEmptyUnicodeString(&pDevExt->SymLinkName, wchBuffer, wchBufferSize);

	result = RtlUnicodeStringPrintf(&pDevExt->SymLinkName,
		BLORGFS_DOS_DRIVELETTER_FORMAT_STRING,
		L'B');

    if (!NT_SUCCESS(result))
    {
        IoDeleteDevice(global.pDiskDeviceObject);
        global.pDiskDeviceObject = NULL;
        return result;
    }

	IoCreateSymbolicLink(&pDevExt->SymLinkName, &uniVDOString);

    return result;
}

static NTSTATUS CreateBlorgVolumeDevice(PDRIVER_OBJECT pDriverObject)
{
    BLORGFS_PRINT("Entering Volume Creation\n");
    UNICODE_STRING uniVDOString = RTL_CONSTANT_STRING(BLORGFS_VDO_STRING);
    UNICODE_STRING uniSDDLString = RTL_CONSTANT_STRING(BLORGFS_VDO_DEVICE_SDDL_STRING);
    PDEVICE_OBJECT pVolumeDeviceObject = NULL;
    NTSTATUS       result = STATUS_UNSUCCESSFUL;

    result = WdmlibIoCreateDeviceSecure(pDriverObject,
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
    
    UNICODE_STRING rootDirectoryName = RTL_CONSTANT_STRING(L"\\");

    result = BlorgCreateDCB(&pDevExt->RootDcb, NULL, BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE, &rootDirectoryName);
    if (!NT_SUCCESS(result))
    {
        IoDeleteDevice(global.pVolumeDeviceObject);
        return result;
    }

	global.pVolumeDeviceObject = pVolumeDeviceObject;

    IoRegisterFileSystem(global.pVolumeDeviceObject);

    return result;
}

VOID DriverUnload(PDRIVER_OBJECT pDriverObject)
{
	UNREFERENCED_PARAMETER(pDriverObject);
    if (global.pVolumeDeviceObject)
    {
        IoUnregisterFileSystem(global.pVolumeDeviceObject);
        IoDeleteDevice(global.pVolumeDeviceObject);
    }

    if (global.pDiskDeviceObject)
    {
		IoDeleteSymbolicLink(&GetDiskDeviceExtension(global.pDiskDeviceObject)->SymLinkName);
		ExFreePool(&GetDiskDeviceExtension(global.pDiskDeviceObject)->SymLinkName.Buffer);
        IoDeleteDevice(global.pDiskDeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    UNREFERENCED_PARAMETER(pRegistryPath);
   
    NTSTATUS  result = STATUS_SUCCESS;

	global.pDriverObject = pDriverObject;

    pDriverObject->DriverUnload = DriverUnload;
    pDriverObject->MajorFunction[IRP_MJ_CREATE] = BlorgCreate;
    pDriverObject->MajorFunction[IRP_MJ_CLOSE] = BlorgClose;
    pDriverObject->MajorFunction[IRP_MJ_READ] = BlorgRead;
    pDriverObject->MajorFunction[IRP_MJ_WRITE] = BlorgWrite;
    pDriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] = BlorgQueryInformation;
    pDriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = BlorgSetInformation;
    pDriverObject->MajorFunction[IRP_MJ_QUERY_EA] = BlorgQueryEa;
    pDriverObject->MajorFunction[IRP_MJ_SET_EA] = BlorgSetEa;
    pDriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = BlorgFlushBuffers;
    pDriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = BlorgQueryVolumeInformation;
    pDriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] = BlorgSetVolumeInformation;
    pDriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = BlorgDirectoryControl;
    pDriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = BlorgFileSystemControl;
    pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = BlorgDeviceControl;
    pDriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = BlorgShutdown;
    pDriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL] = BlorgLockControl;
    pDriverObject->MajorFunction[IRP_MJ_CLEANUP] = BlorgCleanup;
    pDriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY] = BlorgQuerySecurity;
    pDriverObject->MajorFunction[IRP_MJ_SET_SECURITY] = BlorgSetSecurity;

	KdBreakPoint();
    
    result = CreateBlorgVolumeDevice(pDriverObject);

    if (!NT_SUCCESS(result))
    {
        return STATUS_FAILED_DRIVER_ENTRY;
    }

	result = CreateBlorgDiskDevice(pDriverObject);

	if (!NT_SUCCESS(result))
	{
		return STATUS_FAILED_DRIVER_ENTRY;
	}

    return STATUS_SUCCESS;

}