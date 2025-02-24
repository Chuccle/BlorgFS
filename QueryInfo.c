#include "Driver.h"

static NTSTATUS BlorgVolumeQueryInformation(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
    FILE_INFORMATION_CLASS FileInfoClass = pIrpSp->Parameters.QueryFile.FileInformationClass;
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (FileInfoClass) 
    {
        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION PositionInfo = (PFILE_POSITION_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            PositionInfo->CurrentByteOffset.QuadPart = 0;
            result = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
            break;
        }
        case FileNameInformation:
        {
            PFILE_NAME_INFORMATION NameInfo = (PFILE_NAME_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            NameInfo->FileNameLength = 0;
            result = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_NAME_INFORMATION);
            break;
        }
        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION BasicInfo = (PFILE_BASIC_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            BasicInfo->CreationTime.QuadPart = 0;
            BasicInfo->LastAccessTime.QuadPart = 0;
            BasicInfo->LastWriteTime.QuadPart = 0;
            BasicInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            result = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
            break;
        }
        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION StandardInfo = (PFILE_STANDARD_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            StandardInfo->AllocationSize.QuadPart = 0;
            StandardInfo->EndOfFile.QuadPart = 0;
            StandardInfo->NumberOfLinks = 0;
            StandardInfo->DeletePending = FALSE;
            StandardInfo->Directory = FALSE;

            result = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
            break;
        }
        case FileAttributeTagInformation:
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION AttributeTagInfo = (PFILE_ATTRIBUTE_TAG_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            AttributeTagInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            result = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        }
        case FileAllInformation:
        {
            //PFILE_ALL_INFORMATION AllInfo = (PFILE_ALL_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;
           
            result = STATUS_INVALID_DEVICE_REQUEST;
            pIrp->IoStatus.Information = 0;
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            pIrp->IoStatus.Information = 0;
        }
    }

    return result;
}



NTSTATUS BlorgQueryInformation(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeQueryInformation(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskQueryInformation(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}