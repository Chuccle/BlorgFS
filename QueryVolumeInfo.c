#include "Driver.h"

static NTSTATUS BlorgVolumeQueryVolumeInformation(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
	KdBreakPoint();
	FS_INFORMATION_CLASS fsInformationClass = pIrpSp->Parameters.QueryVolume.FsInformationClass;
	ULONG inputLength = pIrpSp->Parameters.QueryVolume.Length;
	PVOID systemBuffer = pIrp->AssociatedIrp.SystemBuffer;
	
	NTSTATUS result = STATUS_INVALID_PARAMETER;
	ULONG bytesWritten = 0;
	
	switch (fsInformationClass)
	{
		case FileFsVolumeInformation:
		{
			KdBreakPoint();
			
			PFILE_FS_VOLUME_INFORMATION pVolumeInfo = systemBuffer;
			pVolumeInfo->VolumeCreationTime.QuadPart = 0;
			pVolumeInfo->VolumeSerialNumber = 0x12345678;
			pVolumeInfo->SupportsObjects = FALSE;

			WCHAR volumeLabelBuffer[] = L"BLORGDRIVE";

			ULONG stringBufferBytes = inputLength - FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);

			pVolumeInfo->VolumeLabelLength = sizeof(volumeLabelBuffer) - sizeof(WCHAR);

			// Check if the buffer is large enough to hold the string
			if (stringBufferBytes >= pVolumeInfo->VolumeLabelLength)
			{
				RtlCopyMemory(pVolumeInfo->VolumeLabel, volumeLabelBuffer, pVolumeInfo->VolumeLabelLength);
			}
			else
			{
				bytesWritten = 0;
				result = STATUS_BUFFER_OVERFLOW;
				break;
			}
			
			bytesWritten = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + pVolumeInfo->VolumeLabelLength;
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsSizeInformation:
		{
			KdBreakPoint();
			
			PFILE_FS_SIZE_INFORMATION pSizeInfo = systemBuffer;
			pSizeInfo->TotalAllocationUnits.QuadPart = 0;
			pSizeInfo->AvailableAllocationUnits.QuadPart = 0;
			pSizeInfo->SectorsPerAllocationUnit = 0;
			pSizeInfo->BytesPerSector = 0;
			
			bytesWritten = sizeof(FILE_FS_SIZE_INFORMATION);
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsDeviceInformation:
		{
			KdBreakPoint();
			
			PFILE_FS_DEVICE_INFORMATION pDeviceInfo = systemBuffer;
			pDeviceInfo->DeviceType = global.pDiskDeviceObject->DeviceType;
			pDeviceInfo->Characteristics = global.pDiskDeviceObject->Characteristics;
			
			bytesWritten = sizeof(FILE_FS_SIZE_INFORMATION);
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

			ULONG stringBufferBytes = inputLength - FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);

			pAttributeInfo->FileSystemNameLength = sizeof(fileSystemNameBuffer) - sizeof(WCHAR);

			// Check if the buffer is large enough to hold the string
			if (stringBufferBytes >= pAttributeInfo->FileSystemNameLength)
			{
				RtlCopyMemory(pAttributeInfo->FileSystemName, fileSystemNameBuffer, pAttributeInfo->FileSystemNameLength);
			}
			else
			{
				bytesWritten = 0;
				result = STATUS_BUFFER_OVERFLOW;
				break;
			}

			bytesWritten = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + pAttributeInfo->FileSystemNameLength;
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsFullSizeInformation:
		{
			KdBreakPoint();

			PFILE_FS_FULL_SIZE_INFORMATION pFullSizeInfo = systemBuffer;
			pFullSizeInfo->TotalAllocationUnits.QuadPart = 0;
			pFullSizeInfo->CallerAvailableAllocationUnits.QuadPart = 0;
			pFullSizeInfo->ActualAvailableAllocationUnits.QuadPart = 0;
			pFullSizeInfo->SectorsPerAllocationUnit = 0;
			pFullSizeInfo->BytesPerSector = 0;

			bytesWritten = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
			result = STATUS_SUCCESS;
			break;
		}
		case FileFsFullSizeInformationEx:
		{
			KdBreakPoint();

			PFILE_FS_FULL_SIZE_INFORMATION_EX pFullSizeInfoEx = systemBuffer;
			pFullSizeInfoEx->ActualTotalAllocationUnits = 0;
			pFullSizeInfoEx->ActualAvailableAllocationUnits = 0;
			pFullSizeInfoEx->ActualPoolUnavailableAllocationUnits = 0;
			pFullSizeInfoEx->CallerTotalAllocationUnits = 0;
			pFullSizeInfoEx->CallerAvailableAllocationUnits = 0;
			pFullSizeInfoEx->CallerPoolUnavailableAllocationUnits = 0;
			pFullSizeInfoEx->UsedAllocationUnits = 0;
			pFullSizeInfoEx->TotalReservedAllocationUnits = 0;
			pFullSizeInfoEx->VolumeStorageReserveAllocationUnits = 0;
			pFullSizeInfoEx->AvailableCommittedAllocationUnits = 0;
			pFullSizeInfoEx->PoolAvailableAllocationUnits = 0;
			pFullSizeInfoEx->SectorsPerAllocationUnit = 0;
			pFullSizeInfoEx->BytesPerSector = 0;

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