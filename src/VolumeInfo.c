#include "Driver.h"

//
// FILE_FS_XXX_INFORMATION query/set handlers (IRP_MJ_QUERY/SET_VOLUME_INFORMATION).
// Reports static volume/device/attribute info; size classes report zeroed
// (unknown) capacity. Volume is read-only, so set-volume-information is a
// no-op stub.
//

static NTSTATUS BlorgVolumeQueryVolumeInformation(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    FS_INFORMATION_CLASS fsInformationClass = IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG inputLength = IrpSp->Parameters.QueryVolume.Length;
    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;

    NTSTATUS result = STATUS_INVALID_PARAMETER;
    ULONG bytesWritten = 0;

    switch (fsInformationClass)
    {
        case FileFsVolumeInformation:
        {
            if (inputLength < sizeof(FILE_FS_VOLUME_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_VOLUME_INFORMATION volumeInfo = systemBuffer;
            volumeInfo->VolumeCreationTime.QuadPart = 0;
            volumeInfo->VolumeSerialNumber = 0x12345678;
            volumeInfo->SupportsObjects = FALSE;

            WCHAR volumeLabelBuffer[] = L"BLORGDRIVE";

            volumeInfo->VolumeLabelLength = sizeof(volumeLabelBuffer) - sizeof(WCHAR);

            if (inputLength - FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) >= volumeInfo->VolumeLabelLength)
            {
                RtlCopyMemory(volumeInfo->VolumeLabel, volumeLabelBuffer, volumeInfo->VolumeLabelLength);
            }
            else
            {
                bytesWritten = 0;
                result = STATUS_BUFFER_OVERFLOW;
                break;
            }

            bytesWritten = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + volumeInfo->VolumeLabelLength;
            result = STATUS_SUCCESS;
            break;
        }
        case FileFsSizeInformation:
        {
            if (inputLength < sizeof(FILE_FS_SIZE_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_SIZE_INFORMATION sizeInfo = systemBuffer;
            sizeInfo->TotalAllocationUnits.QuadPart = 0;
            sizeInfo->AvailableAllocationUnits.QuadPart = 0;
            sizeInfo->SectorsPerAllocationUnit = 0;
            sizeInfo->BytesPerSector = 0;

            bytesWritten = sizeof(FILE_FS_SIZE_INFORMATION);
            result = STATUS_SUCCESS;
            break;
        }
        case FileFsDeviceInformation:
        {
            if (inputLength < sizeof(FILE_FS_DEVICE_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_DEVICE_INFORMATION deviceInfo = systemBuffer;
            deviceInfo->DeviceType = global.DiskDeviceObject->DeviceType;
            deviceInfo->Characteristics = global.DiskDeviceObject->Characteristics;

            bytesWritten = sizeof(FILE_FS_DEVICE_INFORMATION);
            result = STATUS_SUCCESS;
            break;
        }
        case FileFsAttributeInformation:
        {
            if (inputLength < sizeof(FILE_FS_ATTRIBUTE_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_ATTRIBUTE_INFORMATION attributeInfo = systemBuffer;
            attributeInfo->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
            attributeInfo->MaximumComponentNameLength = 255;

            SetFlag(attributeInfo->FileSystemAttributes, FILE_READ_ONLY_VOLUME);

            WCHAR fileSystemNameBuffer[] = L"BLORGFS";

            attributeInfo->FileSystemNameLength = sizeof(fileSystemNameBuffer) - sizeof(WCHAR);

            if (inputLength - FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) >= attributeInfo->FileSystemNameLength)
            {
                RtlCopyMemory(attributeInfo->FileSystemName, fileSystemNameBuffer, attributeInfo->FileSystemNameLength);
            }
            else
            {
                bytesWritten = 0;
                result = STATUS_BUFFER_OVERFLOW;
                break;
            }

            bytesWritten = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + attributeInfo->FileSystemNameLength;
            result = STATUS_SUCCESS;
            break;
        }
        case FileFsFullSizeInformation:
        {
            if (inputLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_FULL_SIZE_INFORMATION fullSizeInfo = systemBuffer;
            fullSizeInfo->TotalAllocationUnits.QuadPart = 0;
            fullSizeInfo->CallerAvailableAllocationUnits.QuadPart = 0;
            fullSizeInfo->ActualAvailableAllocationUnits.QuadPart = 0;
            fullSizeInfo->SectorsPerAllocationUnit = 0;
            fullSizeInfo->BytesPerSector = 0;

            bytesWritten = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
            result = STATUS_SUCCESS;
            break;
        }
        case FileFsFullSizeInformationEx:
        {
            if (inputLength < sizeof(FILE_FS_FULL_SIZE_INFORMATION_EX))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_FS_FULL_SIZE_INFORMATION_EX fullSizeInfoEx = systemBuffer;
            fullSizeInfoEx->ActualTotalAllocationUnits = 0;
            fullSizeInfoEx->ActualAvailableAllocationUnits = 0;
            fullSizeInfoEx->ActualPoolUnavailableAllocationUnits = 0;
            fullSizeInfoEx->CallerTotalAllocationUnits = 0;
            fullSizeInfoEx->CallerAvailableAllocationUnits = 0;
            fullSizeInfoEx->CallerPoolUnavailableAllocationUnits = 0;
            fullSizeInfoEx->UsedAllocationUnits = 0;
            fullSizeInfoEx->TotalReservedAllocationUnits = 0;
            fullSizeInfoEx->VolumeStorageReserveAllocationUnits = 0;
            fullSizeInfoEx->AvailableCommittedAllocationUnits = 0;
            fullSizeInfoEx->PoolAvailableAllocationUnits = 0;
            fullSizeInfoEx->SectorsPerAllocationUnit = 0;
            fullSizeInfoEx->BytesPerSector = 0;

            bytesWritten = sizeof(FILE_FS_FULL_SIZE_INFORMATION_EX);
            result = STATUS_SUCCESS;
            break;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeQueryVolumeInformation: FsInformationClass=%d\n", fsInformationClass);
            result = STATUS_INVALID_PARAMETER;
        }
    }

    Irp->IoStatus.Information = bytesWritten;
    return result;
}

//
// IRP_MJ_QUERY_VOLUME_INFORMATION dispatch entry point: routes to the
// per-device-type handler based on the device extension magic, completing
// the IRP unconditionally (disk/FSDO device types currently have no
// query handler and fall through to STATUS_INVALID_DEVICE_REQUEST).
//
NTSTATUS BlorgQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            result = BlorgVolumeQueryVolumeInformation(Irp, irpSp);
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

//
// IRP_MJ_SET_VOLUME_INFORMATION dispatch entry point. All device types are
// currently unimplemented stubs -- the volume is read-only (see
// FILE_READ_ONLY_VOLUME in BlorgVolumeQueryVolumeInformation), so this
// always completes with STATUS_INVALID_DEVICE_REQUEST.
//
NTSTATUS BlorgSetVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
