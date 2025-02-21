#include "Driver.h"

static NTSTATUS BlorgVolumeQueryVolumeInformation(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
	FS_INFORMATION_CLASS fsInformationClass = pIrpSp->Parameters.QueryVolume.FsInformationClass;
	ULONG inputLength = pIrpSp->Parameters.QueryVolume.Length;
	PVOID systemBuffer = pIrp->AssociatedIrp.SystemBuffer;
	
	NTSTATUS result = STATUS_SUCCESS;
	ULONG bytesWritten = 0;
	
	switch (fsInformationClass)
	{
		case FileFsVolumeInformation:
		{
			PFILE_FS_VOLUME_INFORMATION pVolumeInfo = systemBuffer;
			pVolumeInfo->VolumeCreationTime.QuadPart = 0;
			pVolumeInfo->VolumeSerialNumber = 0x12345678;
			pVolumeInfo->VolumeLabelLength = 0;
			bytesWritten = 0;
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsSizeInformation:
		{
			PFILE_FS_SIZE_INFORMATION pSizeInfo = systemBuffer;
			pSizeInfo->TotalAllocationUnits.QuadPart = 0;
			pSizeInfo->AvailableAllocationUnits.QuadPart = 0;
			pSizeInfo->SectorsPerAllocationUnit = 0;
			pSizeInfo->BytesPerSector = 0;
			bytesWritten = 0;
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsDeviceInformation:
		{
			PFILE_FS_DEVICE_INFORMATION pDeviceInfo = systemBuffer;
			pDeviceInfo->DeviceType = global.pDiskDeviceObject->DeviceType;
			pDeviceInfo->Characteristics = global.pDiskDeviceObject->Characteristics;
			
			bytesWritten = 0;
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsAttributeInformation:
		{
			KdBreakPoint();

			PFILE_FS_ATTRIBUTE_INFORMATION pAttributeInfo = systemBuffer;
			pAttributeInfo->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
			pAttributeInfo->MaximumComponentNameLength = 255;

			// READ ONLY VOLUME SET FOR NOW - WILL CHANGE LATER
			SetFlag(pAttributeInfo->FileSystemAttributes, FILE_READ_ONLY_VOLUME);
			
			WCHAR fileSystemNameBuffer[] = L"BLORGFS";

			int stringBufferBytes = inputLength - FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);

			pAttributeInfo->FileSystemNameLength = sizeof(fileSystemNameBuffer);

			// Check if the buffer is large enough to hold the string
			if (stringBufferBytes >= sizeof(fileSystemNameBuffer))
			{
				RtlCopyMemory(pAttributeInfo->FileSystemName, fileSystemNameBuffer, sizeof(fileSystemNameBuffer));
			}
			else
			{
				bytesWritten = 0;
				result = STATUS_BUFFER_OVERFLOW;
				break;
			}

			bytesWritten = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + sizeof(fileSystemNameBuffer);
			result = STATUS_SUCCESS;
			break;
		}
		default:
		{
			bytesWritten = 0;
			result = STATUS_INVALID_PARAMETER;
			break;
		}
	}

	pIrp->IoStatus.Information = bytesWritten;
	return result;
}

NTSTATUS BlorgQueryVolumeInformation(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	UNREFERENCED_PARAMETER(pDeviceObject);

	PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	switch (GetDeviceExtensionMagic(pDeviceObject))
	{
		case BLORGFS_VDO_MAGIC:
		{
			result = BlorgVolumeQueryVolumeInformation(pIrp, pIrpSp);
			break;
		}
		case BLORGFS_DDO_MAGIC:
		{
			// result = BlorgDiskFlushBuffers(pIrp);
			break;
		}
	}

	pIrp->IoStatus.Status = result;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}