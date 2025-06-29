#include "Driver.h"

static NTSTATUS BlorgVolumeQueryInformation(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    // GetVolumeInformation crashes without this being implemented, come on Windows!
    FILE_INFORMATION_CLASS fileInfoClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    ULONG inputLength = IrpSp->Parameters.QueryFile.Length;
    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_OBJECT fileObject = IrpSp->FileObject;

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
    ULONG bytesWritten = 0;

    switch (fileInfoClass)
    {
        case FilePositionInformation:
        {
            if (inputLength < sizeof(FILE_POSITION_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_POSITION_INFORMATION positionInfo = systemBuffer;

            positionInfo->CurrentByteOffset = fileObject->CurrentByteOffset;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_POSITION_INFORMATION);
            break;
        }
        case FileNormalizedNameInformation:
        case FileNameInformation:
        {
            // REVIEW!!!!
            if (inputLength < sizeof(FILE_NAME_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_NAME_INFORMATION nameInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            // Check if the buffer is large enough to hold the string
            if (inputLength - UFIELD_OFFSET(FILE_NAME_INFORMATION, FileName) >= commonContext->FullPath.Length)
            {
                nameInfo->FileNameLength = commonContext->FullPath.Length;
                RtlCopyMemory(nameInfo->FileName, commonContext->FullPath.Buffer, nameInfo->FileNameLength);
            }
            else
            {
                bytesWritten = 0;
                result = STATUS_BUFFER_OVERFLOW;
                break;
            }

            bytesWritten = UFIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + nameInfo->FileNameLength;

            result = STATUS_SUCCESS;
            break;
        }
        case FileBasicInformation:
        {
            if (inputLength < sizeof(FILE_BASIC_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_BASIC_INFORMATION basicInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            basicInfo->CreationTime.QuadPart = commonContext->CreationTime;
            basicInfo->LastAccessTime.QuadPart = commonContext->LastAccessedTime;
            basicInfo->LastWriteTime.QuadPart = commonContext->LastModifiedTime;
            basicInfo->ChangeTime.QuadPart = commonContext->LastModifiedTime;
            basicInfo->FileAttributes = (GET_NODE_TYPE(commonContext) == BLORGFS_FCB_SIGNATURE) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_BASIC_INFORMATION);
            break;
        }
        case FileStandardInformation:
        {
            if (inputLength < sizeof(FILE_STANDARD_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_STANDARD_INFORMATION standardInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            standardInfo->AllocationSize = commonContext->Header.AllocationSize;
            standardInfo->EndOfFile = commonContext->Header.AllocationSize;
            standardInfo->NumberOfLinks = 0;
            standardInfo->DeletePending = FALSE;
            standardInfo->Directory = (GET_NODE_TYPE(commonContext) == BLORGFS_DCB_SIGNATURE);

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_STANDARD_INFORMATION);
            break;
        }
        case FileAttributeTagInformation:
        {
            if (inputLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_ATTRIBUTE_TAG_INFORMATION attributeTagInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            attributeTagInfo->FileAttributes = (GET_NODE_TYPE(commonContext) == BLORGFS_FCB_SIGNATURE) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        }
        case FileNetworkOpenInformation:
        {
            if (inputLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_NETWORK_OPEN_INFORMATION networkOpenInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            networkOpenInfo->AllocationSize = commonContext->Header.AllocationSize;
            networkOpenInfo->EndOfFile = commonContext->Header.AllocationSize;
            networkOpenInfo->CreationTime.QuadPart = commonContext->CreationTime;
            networkOpenInfo->LastAccessTime.QuadPart = commonContext->LastAccessedTime;
            networkOpenInfo->LastWriteTime.QuadPart = commonContext->LastModifiedTime;
            networkOpenInfo->ChangeTime.QuadPart = commonContext->LastModifiedTime;
            networkOpenInfo->FileAttributes = (GET_NODE_TYPE(commonContext) == BLORGFS_FCB_SIGNATURE) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;
            
            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        }
        case FileAllInformation:
        {
            if (inputLength < sizeof(FILE_ALL_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            //PFILE_ALL_INFORMATION allInfo = (PFILE_ALL_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

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

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeQueryInformation(Irp, irpSp);
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
    UNREFERENCED_PARAMETER(DeviceObject);

    // PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            // result = BlorgVolumeSetInformation(Irp, irpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskSetInformation(Irp);
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