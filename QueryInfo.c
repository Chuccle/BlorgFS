#include "Driver.h"

NTSTATUS BlorgQueryInformation(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);
    
    PIO_STACK_LOCATION IoStackLocation = IoGetCurrentIrpStackLocation(pIrp);
    FILE_INFORMATION_CLASS FileInfoClass = IoStackLocation->Parameters.QueryFile.FileInformationClass;

    switch (FileInfoClass) 
    {
        case FilePositionInformation:
        {
            PFILE_POSITION_INFORMATION PositionInfo = (PFILE_POSITION_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
            PositionInfo->CurrentByteOffset.QuadPart = 0;
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
            break;
        }
        case FileNameInformation:
        {
            PFILE_NAME_INFORMATION NameInfo = (PFILE_NAME_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
            NameInfo->FileNameLength = 0;
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_NAME_INFORMATION);
            break;
        }
        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION BasicInfo = (PFILE_BASIC_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
            BasicInfo->CreationTime.QuadPart = 0; // Example creation time
            BasicInfo->LastAccessTime.QuadPart = 0;
            BasicInfo->LastWriteTime.QuadPart = 0;
            BasicInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
            break;
        }
        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION StandardInfo = (PFILE_STANDARD_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
            StandardInfo->AllocationSize.QuadPart = 0; // Example creation time
            StandardInfo->EndOfFile.QuadPart = 0;
            StandardInfo->NumberOfLinks = 0;
            StandardInfo->DeletePending = FALSE;
            StandardInfo->Directory = FALSE;

            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
            break;
        }
        case FileAttributeTagInformation:
        {
            PFILE_ATTRIBUTE_TAG_INFORMATION AttributeTagInfo = (PFILE_ATTRIBUTE_TAG_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
            AttributeTagInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;

            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
            break;
        }
        case FileAllInformation:
        {
            //PFILE_ALL_INFORMATION AllInfo = (PFILE_ALL_INFORMATION)pIrp->AssociatedIrp.SystemBuffer;

            // Populate file attributes
           

            pIrp->IoStatus.Status = STATUS_SUCCESS;
            pIrp->IoStatus.Information = sizeof(FILE_ALL_INFORMATION);
            break;
        }
        default:
        {
            pIrp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            pIrp->IoStatus.Information = 0;
            break;
        }
    }

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
}