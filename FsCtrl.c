#include "Driver.h"


static inline void HandleUserFSRequest(ULONG ulFsCtrlCode, PDEVICE_OBJECT pDeviceObject, PIRP pIrp, PIO_STACK_LOCATION pIrpSp) 
{

    UNREFERENCED_PARAMETER(pIrpSp);
    UNREFERENCED_PARAMETER(pDeviceObject);
    
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
            //pIrp->IoStatus.Status = FileSystemControlReparsePoint(pDeviceObject, pIrp, pIrpSp, FALSE);
            break;
        }
        case FSCTL_SET_REPARSE_POINT:
        case FSCTL_DELETE_REPARSE_POINT:
        {
           //pIrp->IoStatus.Status = FileSystemControlReparsePoint(pDeviceObject, pIrp, pIrpSp, TRUE);
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
            //pIrp->IoStatus.Status = FileSystemControlOplock(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_QUERY_PERSISTENT_VOLUME_STATE:
        {
           //pIrp->IoStatus.Status = FileSystemControlQueryPersistentVolumeState(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_FILESYSTEM_GET_STATISTICS:
        {
            //pIrp->IoStatus.Status = FileSystemControlGetStatistics(pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case FSCTL_GET_RETRIEVAL_POINTERS:
        {
            //pIrp->IoStatus.Status = FileSystemControlGetRetrievalPointers(pDeviceObject, pIrp, pIrpSp);
            break;
        }
    }
}


NTSTATUS BlorgFileSystemControl(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{

    UNREFERENCED_PARAMETER(pDeviceObject);
    
    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	PDEVICE_OBJECT pTargetDeviceObject = NULL;

    switch (pIrpSp->MinorFunction) 
    {
        case IRP_MN_USER_FS_REQUEST:
        {
            HandleUserFSRequest(pIrpSp->Parameters.FileSystemControl.FsControlCode, pDeviceObject, pIrp, pIrpSp);
            break;
        }
        case IRP_MN_MOUNT_VOLUME:
        {
			pTargetDeviceObject = pIrpSp->Parameters.MountVolume.DeviceObject;
			if (pTargetDeviceObject && GetDeviceExtensionMagic(pTargetDeviceObject) == BLORGFS_VDO_MAGIC)
			{
				KdBreakPoint();
				pIrp->IoStatus.Status = STATUS_SUCCESS;
			}
            break;
        }
        case IRP_MN_VERIFY_VOLUME:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        case IRP_MN_LOAD_FILE_SYSTEM:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        case IRP_MN_KERNEL_CALL:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        default:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
    }

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}
