#include "Driver.h"

//
//  IRP_MJ_CLEANUP handling: removes share access, finishes pending directory
//  notify IRPs for the handle, and settles oplock state before the handle's
//  final close.
//

//
// Completes any NOTIFY_CHANGE_DIRECTORY IRPs this handle left pending in the
// notify package (see BlorgVolumeDirectoryControl) -- keyed on the CCB we
// registered with, so it is a no-op and safe to call unconditionally for
// handles that never registered a watch (e.g. plain files). Also coordinates
// cleanup with the oplock state under the node resource (as fastfat holds
// the FCB here): a cleanup always proceeds immediately (NULL callbacks,
// never pends), releasing any oplock this handle held and advancing a break
// that was waiting on this handle to close, then refreshes Fast I/O.
// For files, every byte-range lock the closing handle still holds is
// released first (FsRtlFastUnlockAll, the fastfat cleanup order) -- a
// no-op while lock control is unimplemented, but the read path and
// shared-oplock grants already consult FileLock, so a dead handle's
// locks must not outlive it once IRP_MJ_LOCK_CONTROL lands.
//
static NTSTATUS BlorgVolumeCleanup(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PCOMMON_CONTEXT node = fileObject->FsContext;
    PBLORGFS_VDO_DEVICE_EXTENSION devExt = GetVolumeDeviceExtension(VolumeDeviceObject);

    FsRtlNotifyCleanup(devExt->NotifySync, &devExt->NotifyList, fileObject->FsContext2);

    switch (GET_NODE_TYPE(node))
    {
        case BLORGFS_FCB_SIGNATURE:
        {
            if (fileObject->PrivateCacheMap)
            {
                CcUninitializeCacheMap(fileObject, NULL, NULL);
            }

            __fallthrough;
        }
        case BLORGFS_DCB_SIGNATURE:
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            ExAcquireResourceExclusiveLite(node->Header.Resource, TRUE);
            IoRemoveShareAccess(fileObject, &node->ShareAccess);

            if (BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(node))
            {
                FsRtlFastUnlockAll(&C_CAST(PFCB, node)->FileLock, fileObject, IoGetRequestorProcess(Irp), NULL);
            }

            FsRtlCheckOplock(&node->Header.Oplock, Irp, NULL, NULL, NULL);
            node->Header.IsFastIoPossible =
                FsRtlOplockIsFastIoPossible(&node->Header.Oplock) ? FastIoIsPossible : FastIoIsNotPossible;

            ExReleaseResourceLite(node->Header.Resource);

            return STATUS_SUCCESS;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            ExAcquireResourceExclusiveLite(node->Header.Resource, TRUE);
            IoRemoveShareAccess(fileObject, &node->ShareAccess);
            ExReleaseResourceLite(node->Header.Resource);
            return STATUS_SUCCESS;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeCleanup: Unknown node type\n");
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
}

//
//  IRP_MJ_CLEANUP dispatch entry: dispatches by device type to the
//  per-node cleanup handler, then completes the IRP.
//
NTSTATUS BlorgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeCleanup(Irp, irpSp, DeviceObject);
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
    FsRtlExitFileSystem();

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
