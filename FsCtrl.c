#include "Driver.h"

static inline void HandleUserFSRequest(ULONG ulFsCtrlCode, PIRP pIrp, PIO_STACK_LOCATION pIrpSp) 
{
    UNREFERENCED_PARAMETER(pIrpSp);
    
    switch (ulFsCtrlCode)
    {
        case FSCTL_IS_VOLUME_MOUNTED:
        {
            pIrp->IoStatus.Information = 0;
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        case FSCTL_GET_REPARSE_POINT:
        {
            // FileSystemControlReparsePoint(pDeviceObject, pIrp, pIrpSp, FALSE);
            break;
        }
        case FSCTL_SET_REPARSE_POINT:
        case FSCTL_DELETE_REPARSE_POINT:
        {
           // FileSystemControlReparsePoint(pDeviceObject, pIrp, pIrpSp, TRUE);
            break;
        }
        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:
        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        case FSCTL_REQUEST_FILTER_OPLOCK:
        case FSCTL_REQUEST_OPLOCK:
        {
            // FileSystemControlOplock(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_QUERY_PERSISTENT_VOLUME_STATE:
        {
           // FileSystemControlQueryPersistentVolumeState(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_FILESYSTEM_GET_STATISTICS:
        {
            // FileSystemControlGetStatistics(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_GET_RETRIEVAL_POINTERS:
        {
            // FileSystemControlGetRetrievalPointers(pDeviceObject, pIrp, pIrpSp);
            break;
        }
    }
}


static NTSTATUS BlorgVolumeFileSystemControl(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
	PDEVICE_OBJECT pTargetDeviceObject = NULL;
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (pIrpSp->MinorFunction) 
    {
        case IRP_MN_USER_FS_REQUEST:
        {
            HandleUserFSRequest(pIrpSp->Parameters.FileSystemControl.FsControlCode, pIrp, pIrpSp);
            break;
        }
        case IRP_MN_MOUNT_VOLUME:
        {
            result = STATUS_UNRECOGNIZED_VOLUME;
			pTargetDeviceObject = pIrpSp->Parameters.MountVolume.DeviceObject;
			if (pTargetDeviceObject && GetDeviceExtensionMagic(pTargetDeviceObject) == BLORGFS_DDO_MAGIC)
			{
                KdBreakPoint();
                PVPB pVpb = pIrpSp->Parameters.MountVolume.Vpb;
                KIRQL Irql;
                /*
                 * We will increment the VPB's ReferenceCount so that we can do a delayed delete
                 * of the volume device later on.
                 */
                IoAcquireVpbSpinLock(&Irql);
                pVpb->DeviceObject = global.pVolumeDeviceObject;
                IoReleaseVpbSpinLock(Irql);

                // Apparently this needs to be cleaned up by us
                ObDereferenceObject(pTargetDeviceObject);

				pIrp->IoStatus.Information = 0;
                result = STATUS_SUCCESS;

            }
            break;
        }
        default:
        {
            result = STATUS_NOT_IMPLEMENTED;
        }
    }

    return result;
}

NTSTATUS BlorgFileSystemControl(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeFileSystemControl(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskSetVolumeInformation(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}
