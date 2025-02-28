#include "Driver.h"

static NTSTATUS BlorgVolumeQueryInformation(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    // Likely the key to GetVolumeInformation exceptions
    FILE_INFORMATION_CLASS fileInfoClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    ULONG inputLength = IrpSp->Parameters.QueryFile.Length;
    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_OBJECT pFileObject = IrpSp->FileObject;

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
    ULONG bytesWritten = 0;

    switch (fileInfoClass)
    {
        case FilePositionInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_POSITION_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_POSITION_INFORMATION PositionInfo = systemBuffer;

            PositionInfo->CurrentByteOffset = pFileObject->CurrentByteOffset;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_POSITION_INFORMATION);
            break;
        }
        case FileNameInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_NAME_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_NAME_INFORMATION NameInfo = systemBuffer;

            PCOMMON_CONTEXT pCommonContext = pFileObject->FsContext;

            // Check if the buffer is large enough to hold the string
            if (inputLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) >= pCommonContext->Name.Length)
            {
                NameInfo->FileNameLength = pCommonContext->Name.Length;
                RtlCopyMemory(NameInfo->FileName, pCommonContext->Name.Buffer, NameInfo->FileNameLength);
            }
            else
            {
                bytesWritten = 0;
                result = STATUS_BUFFER_OVERFLOW;
                break;
            }

            bytesWritten = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + NameInfo->FileNameLength;

            result = STATUS_SUCCESS;
            break;
        }
        case FileBasicInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_BASIC_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_BASIC_INFORMATION BasicInfo = systemBuffer;

            BasicInfo->CreationTime.QuadPart = 0;
            BasicInfo->LastAccessTime.QuadPart = 0;
            BasicInfo->LastWriteTime.QuadPart = 0;
            BasicInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_BASIC_INFORMATION);
            break;
        }
        case FileStandardInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_STANDARD_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_STANDARD_INFORMATION StandardInfo = systemBuffer;

            StandardInfo->AllocationSize.QuadPart = 0;
            StandardInfo->EndOfFile.QuadPart = 0;
            StandardInfo->NumberOfLinks = 0;
            StandardInfo->DeletePending = FALSE;
            StandardInfo->Directory = FALSE;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_STANDARD_INFORMATION);
            break;
        }
        case FileAttributeTagInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_ATTRIBUTE_TAG_INFORMATION AttributeTagInfo = systemBuffer;

            AttributeTagInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        }
        case FileAllInformation:
        {
            KdBreakPoint();

            if (inputLength < sizeof(FILE_ALL_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            //PFILE_ALL_INFORMATION AllInfo = (PFILE_ALL_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            result = STATUS_INVALID_DEVICE_REQUEST;
            bytesWritten = 0;
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            bytesWritten = 0;
        }
    }

    Irp->IoStatus.Information = bytesWritten;

    return result;
}



NTSTATUS BlorgQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeQueryInformation(Irp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskQueryInformation(pIrp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}


NTSTATUS BlorgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KdBreakPoint();
    UNREFERENCED_PARAMETER(DeviceObject);

    // PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            // result = BlorgVolumeSetInformation(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskSetInformation(pIrp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}