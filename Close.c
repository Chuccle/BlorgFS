#include "Driver.h"

static NTSTATUS BlorgVolumeClose(PIO_STACK_LOCATION pIrpSp)
{
	KdBreakPoint();

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    PFILE_OBJECT pFileObject = pIrpSp->FileObject;

	switch GET_NODE_TYPE(pFileObject->FsContext)
	{
		case BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE:
		{		
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
			result = STATUS_SUCCESS;
			break;
		}
		default:
		{
			result = STATUS_INVALID_DEVICE_REQUEST;
			BLORGFS_PRINT("BlorgVolumeCleanup: Unknown FCB type\n");
		}
	}

    return result;
}

NTSTATUS BlorgClose(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeClose(pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskClose(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}