#include "Driver.h"

static NTSTATUS BlorgVolumeCleanup(PIO_STACK_LOCATION pIrpSp)
{
	KdBreakPoint();

	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	PFILE_OBJECT pFileObject = pIrpSp->FileObject;

	switch GetNodeType(pFileObject->FsContext)
	{
		case BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE:
		{
			PDCB pRootDcb = pFileObject->FsContext;

			KeAcquireGuardedMutex(&pRootDcb->Lock);

			pRootDcb->RefCount -= 1;

			ASSERT(pRootDcb->RefCount >= 1);

			KeReleaseGuardedMutex(&pRootDcb->Lock);

			result = STATUS_SUCCESS;
			break;
		}
		case BLORGFS_DIRECTORY_NODE_SIGNATURE:
		{
			result = STATUS_INVALID_DEVICE_REQUEST;
			break;
		}
		case BLORGFS_FILE_NODE_SIGNATURE:
		{
			result = STATUS_INVALID_DEVICE_REQUEST;
			break;
		}
		case BLORGFS_VOLUME_NODE_SIGNATURE:
		{
			PDCB pRootDcb = pFileObject->FsContext;

			KeAcquireGuardedMutex(&pRootDcb->Lock);

			pRootDcb->RefCount -= 1;

			ASSERT(pRootDcb->RefCount >= 1);

			KeReleaseGuardedMutex(&pRootDcb->Lock);

			result = STATUS_SUCCESS;
			break;
		}
		default:
		{
			result = STATUS_INVALID_DEVICE_REQUEST;
			BLORGFS_PRINT("BlorgVolumeCleanup: Unknown node type\n");
		}
	}

	return result;
}

NTSTATUS BlorgCleanup(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	UNREFERENCED_PARAMETER(pDeviceObject);

	PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	switch (GetDeviceExtensionMagic(pDeviceObject))
	{
		case BLORGFS_VDO_MAGIC:
		{
			result = BlorgVolumeCleanup(pIrpSp);
			break;
		}
		case BLORGFS_DDO_MAGIC:
		{
			// result = BlorgDiskCleanup(pIrp);
			break;
		}
	}

	pIrp->IoStatus.Status = result;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}