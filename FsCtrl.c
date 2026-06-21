#include "Driver.h"

//
// IRP_MJ_FILE_SYSTEM_CONTROL dispatch: user FSCTLs (oplock request/
// acknowledge, ported from fastfat's FatOplockRequest) and volume mount
// handling.
//

//
//  Handles every user FSCTL -- in practice the oplock request/acknowledge
//  family, adapted from fastfat's FatOplockRequest. A single switch on the
//  control code routes and validates in one pass: each oplock code states the
//  node types it is legal on (the legacy codes are file-only; a directory
//  takes its Read/Read-Handle oplock solely through the unified, METHOD_BUFFERED
//  FSCTL_REQUEST_OPLOCK), then falls through to the common handoff. The node is
//  only dereferenced on those oplock paths, so an unrelated FSCTL on the
//  control device (FsContext NULL) safely reaches default. Returns
//  STATUS_PENDING once the IRP has been handed to the FsRtl oplock package
//  (which then owns it -- held pending a break, or already completed), telling
//  BlorgFileSystemControl to leave the IRP alone; validation failures return
//  the error and let the dispatcher complete the IRP with it.
//
//  The handoff takes only the node resource exclusive: the handle behind
//  this FSCTL keeps RefCount nonzero (so the node cannot be reaped), and
//  opens mutate RefCount under this same node resource, so no concurrent
//  open can slip the count upward during the grant. A concurrent close's
//  lock-free decrement can only lower the count, making a stale read err
//  toward denying an exclusive grant -- the safe direction. OpenCount only
//  steers a grant -- FsRtl ignores it on an acknowledge: a shared (Read)
//  grant is denied by a conflicting byte-range lock (files only), while an
//  exclusive grant needs the sole opener, for which RefCount is our analog
//  of fastfat's UncleanCount. After FsRtlOplockFsctrl returns, the IRP
//  belongs to FsRtl (held pending a break, or already completed) and must
//  not be touched or completed here; the check may have broken conflicting
//  oplocks, so IsFastIoPossible is refreshed before releasing the lock.
//
static NTSTATUS BlorgUserFsCtrl(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PCOMMON_CONTEXT node = IrpSp->FileObject->FsContext;
    BOOLEAN sharedRequest = FsRtlOplockIsSharedRequest(Irp);
    BOOLEAN isFile = FALSE;

    switch (IrpSp->Parameters.FileSystemControl.FsControlCode)
    {
        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:
        case FSCTL_REQUEST_FILTER_OPLOCK:
        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        {
            if (BLORGFS_FCB_SIGNATURE != GET_NODE_TYPE(node))
            {
                return STATUS_INVALID_PARAMETER;
            }

            isFile = TRUE;
            break;
        }

        case FSCTL_REQUEST_OPLOCK:
        {
            USHORT nodeType = GET_NODE_TYPE(node);
            PREQUEST_OPLOCK_INPUT_BUFFER inputBuffer = Irp->AssociatedIrp.SystemBuffer;

            isFile = (BLORGFS_FCB_SIGNATURE == nodeType);
            BOOLEAN isDirectory = (BLORGFS_DCB_SIGNATURE == nodeType) || (BLORGFS_ROOT_DCB_SIGNATURE == nodeType);

            if (!isFile && !isDirectory)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if ((IrpSp->Parameters.FileSystemControl.InputBufferLength < sizeof(REQUEST_OPLOCK_INPUT_BUFFER)) ||
                (IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(REQUEST_OPLOCK_OUTPUT_BUFFER)))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            if (!FlagOn(inputBuffer->Flags, REQUEST_OPLOCK_INPUT_FLAG_REQUEST | REQUEST_OPLOCK_INPUT_FLAG_ACK))
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (isDirectory && !sharedRequest)
            {
                return STATUS_INVALID_PARAMETER;
            }

            break;
        }

        default:
        {
            BLORGFS_LOG("UNHANDLED user FSCTL minor=%u code=0x%08x -> STATUS_INVALID_DEVICE_REQUEST\n",
                IrpSp->MinorFunction, IrpSp->Parameters.FileSystemControl.FsControlCode);
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    ExAcquireResourceExclusiveLite(node->Header.Resource, TRUE);

    ULONG oplockCount = sharedRequest
        ? C_CAST(ULONG, isFile && !FsRtlCheckLockForOplockRequest(&C_CAST(PFCB, node)->FileLock, &node->Header.AllocationSize))
        : C_CAST(ULONG, ReadNoFence64(&node->RefCount));

    (void)FsRtlOplockFsctrl(&node->Header.Oplock, Irp, oplockCount);

    node->Header.IsFastIoPossible =
        FsRtlOplockIsFastIoPossible(&node->Header.Oplock) ? FastIoIsPossible : FastIoIsNotPossible;

    ExReleaseResourceLite(node->Header.Resource);

    return STATUS_PENDING;
}

//
// Handles IRP_MN_MOUNT_VOLUME: validates the target device is our DDO,
// creates the volume device object, wires it into the FSDO extension and
// the mount IRP's VPB (marking it mounted), and releases the DDO
// reference the I/O manager took for the mount attempt.
//
static NTSTATUS BlorgMountVolume(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_OBJECT targetDeviceObject = IrpSp->Parameters.MountVolume.DeviceObject;

    if (!targetDeviceObject || GetDeviceExtensionMagic(targetDeviceObject) != BLORGFS_DDO_MAGIC)
    {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    PDEVICE_OBJECT volumeDeviceObject;
    NTSTATUS status = CreateBlorgVolumeDeviceObject(global.DriverObject, &volumeDeviceObject);

    if (!NT_SUCCESS(status))
    {
        return status;
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

    ObDereferenceObject(targetDeviceObject);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}


//
// IRP_MJ_FILE_SYSTEM_CONTROL dispatch entry point: for the volume/FSDO
// devices, routes IRP_MN_USER_FS_REQUEST to BlorgUserFsCtrl (oplock
// requests) and IRP_MN_MOUNT_VOLUME to BlorgMountVolume; the disk device
// has no handling. Leaves the IRP untouched (does not complete it) if the
// result is STATUS_PENDING, since that means an oplock request now owns it.
//
NTSTATUS BlorgFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        case BLORGFS_FSDO_MAGIC:
        {
            switch (irpSp->MinorFunction)
            {
                case IRP_MN_USER_FS_REQUEST:
                {
                    result = BlorgUserFsCtrl(Irp, irpSp);
                    break;
                }
                case IRP_MN_MOUNT_VOLUME:
                {
                    result = BlorgMountVolume(Irp, irpSp);
                    break;
                }
                default:
                {
                    BLORGFS_LOG("UNHANDLED FSCTL minor=%u code=0x%08x -> STATUS_INVALID_DEVICE_REQUEST\n",
                        irpSp->MinorFunction, irpSp->Parameters.FileSystemControl.FsControlCode);
                    result = STATUS_INVALID_DEVICE_REQUEST;
                    break;
                }
            }
            
            break;
        }        
        case BLORGFS_DDO_MAGIC:
        {
            break;
        }
    }

    FsRtlExitFileSystem();

    if (STATUS_PENDING == result)
    {
        return STATUS_PENDING;
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
