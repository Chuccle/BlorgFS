#include "Driver.h"

//
//  IRP_MJ_QUERY_INFORMATION / IRP_MJ_SET_INFORMATION handling. Query side
//  fills the supported FILE_XXX_INFORMATION classes from the in-memory
//  FCB/DCB; set side is currently unimplemented for all device types.
//

//
// Handles IRP_MJ_QUERY_INFORMATION for the volume device, filling the
// supported FILE_XXX_INFORMATION classes from the in-memory FCB/DCB.
// FilePositionInformation in particular must be implemented -- Windows'
// GetVolumeInformation crashes without it.
//
static NTSTATUS BlorgVolumeQueryInformation(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
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
            if (inputLength < sizeof(FILE_NAME_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_NAME_INFORMATION nameInfo = systemBuffer;

            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

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
            standardInfo->EndOfFile = commonContext->Header.FileSize;
            standardInfo->NumberOfLinks = 1;
            standardInfo->DeletePending = FALSE;
            standardInfo->Directory = GET_NODE_TYPE(commonContext) == BLORGFS_DCB_SIGNATURE;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_STANDARD_INFORMATION);
            break;
        }
        case FileEaInformation:
        {
            if (inputLength < sizeof(FILE_EA_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            C_CAST(PFILE_EA_INFORMATION, systemBuffer)->EaSize = 0;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_EA_INFORMATION);
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
            bytesWritten = sizeof(FILE_NETWORK_OPEN_INFORMATION);
            break;
        }
        case FileAllInformation:
        {
            ULONG baseLength = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName);

            if (inputLength < baseLength)
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_ALL_INFORMATION allInfo = systemBuffer;
            PCOMMON_CONTEXT commonContext = fileObject->FsContext;
            BOOLEAN isFile = (GET_NODE_TYPE(commonContext) == BLORGFS_FCB_SIGNATURE);

            RtlZeroMemory(allInfo, baseLength);

            allInfo->BasicInformation.CreationTime.QuadPart = commonContext->CreationTime;
            allInfo->BasicInformation.LastAccessTime.QuadPart = commonContext->LastAccessedTime;
            allInfo->BasicInformation.LastWriteTime.QuadPart = commonContext->LastModifiedTime;
            allInfo->BasicInformation.ChangeTime.QuadPart = commonContext->LastModifiedTime;
            allInfo->BasicInformation.FileAttributes = isFile ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;

            allInfo->StandardInformation.AllocationSize = commonContext->Header.AllocationSize;
            allInfo->StandardInformation.EndOfFile = commonContext->Header.FileSize;
            allInfo->StandardInformation.NumberOfLinks = 1;
            allInfo->StandardInformation.DeletePending = FALSE;
            allInfo->StandardInformation.Directory = !isFile;

            allInfo->PositionInformation.CurrentByteOffset = fileObject->CurrentByteOffset;

            ULONG nameAvail = inputLength - baseLength;
            ULONG nameLength = commonContext->FullPath.Length;
            ULONG nameToCopy = (nameAvail < nameLength) ? nameAvail : nameLength;

            allInfo->NameInformation.FileNameLength = nameLength;
            RtlCopyMemory(allInfo->NameInformation.FileName, commonContext->FullPath.Buffer, nameToCopy);

            bytesWritten = baseLength + nameToCopy;
            result = (nameToCopy < nameLength) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
            break;
        }
        case FileStandardLinkInformation:
        {
            if (inputLength < sizeof(FILE_STANDARD_LINK_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            PFILE_STANDARD_LINK_INFORMATION linkInfo = systemBuffer;
            PCOMMON_CONTEXT commonContext = fileObject->FsContext;

            linkInfo->NumberOfAccessibleLinks = 1;
            linkInfo->TotalNumberOfLinks = 1;
            linkInfo->DeletePending = FALSE;
            linkInfo->Directory = (GET_NODE_TYPE(commonContext) != BLORGFS_FCB_SIGNATURE);

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_STANDARD_LINK_INFORMATION);
            break;
        }
        case FileCaseSensitiveInformation:
        {
            if (inputLength < sizeof(FILE_CASE_SENSITIVE_INFORMATION))
            {
                result = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            C_CAST(PFILE_CASE_SENSITIVE_INFORMATION, systemBuffer)->Flags = 0;

            result = STATUS_SUCCESS;
            bytesWritten = sizeof(FILE_CASE_SENSITIVE_INFORMATION);
            break;
        }
        case FileRemoteProtocolInformation:
        {
            result = STATUS_INVALID_PARAMETER;
            break;
        }
        default:
        {
            BLORGFS_PRINT("Unhandled QueryInformation class %d\n", fileInfoClass);
            result = STATUS_INVALID_PARAMETER;
            bytesWritten = 0;
        }
    }

    Irp->IoStatus.Information = bytesWritten;

    return result;
}

//
// IRP_MJ_QUERY_INFORMATION dispatch entry point: routes to
// BlorgVolumeQueryInformation for the volume device, is a no-op (leaves
// STATUS_INVALID_DEVICE_REQUEST) for the disk/FSDO devices, and always
// completes the IRP synchronously.
//
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


//
// IRP_MJ_SET_INFORMATION dispatch entry point: stubbed out for every
// device type (every branch is a no-op), so this always completes with
// STATUS_INVALID_DEVICE_REQUEST -- the volume is read-only, so file-info
// mutation is not supported.
//
NTSTATUS BlorgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
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
