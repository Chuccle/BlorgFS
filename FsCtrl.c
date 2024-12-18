#include "Driver.h"

static inline void HandleUserFSRequest(ULONG FsctlCode, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    UNREFERENCED_PARAMETER(IrpSp);

    switch (FsctlCode)
    {
        case FSCTL_IS_VOLUME_MOUNTED:
        {
            Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
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

static NTSTATUS BlorgVolumeFileSystemControl(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_OBJECT targetDeviceObject = NULL;
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;


    switch (IrpSp->MinorFunction)
    {
        case IRP_MN_USER_FS_REQUEST:
        {
            HandleUserFSRequest(IrpSp->Parameters.FileSystemControl.FsControlCode, Irp, IrpSp);
            break;
        }
        case IRP_MN_MOUNT_VOLUME:
        {
            result = STATUS_UNRECOGNIZED_VOLUME;
            targetDeviceObject = IrpSp->Parameters.MountVolume.DeviceObject;

            if (targetDeviceObject && GetDeviceExtensionMagic(targetDeviceObject) == BLORGFS_DDO_MAGIC)
            {
                PDEVICE_OBJECT volumeDeviceObject;
                result = CreateBlorgVolumeDeviceObject(global.DriverObject, &volumeDeviceObject);

                if (!NT_SUCCESS(result))
                {
                    return result;
                }

                ObReferenceObject(volumeDeviceObject);
                GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject = volumeDeviceObject;

                PVPB vpb = IrpSp->Parameters.MountVolume.Vpb;
                KIRQL irql;

                IoAcquireVpbSpinLock(&irql);
                vpb->DeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
                vpb->VolumeLabelLength = 0;
                SetFlag(vpb->Flags, VPB_MOUNTED);
                IoReleaseVpbSpinLock(irql);

                // Apparently this needs to be cleaned up by us
                ObDereferenceObject(targetDeviceObject);

                Irp->IoStatus.Information = 0;
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

NTSTATUS BlorgFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskFileSystemControl(Irp);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            result = BlorgVolumeFileSystemControl(Irp, irpSp);
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
