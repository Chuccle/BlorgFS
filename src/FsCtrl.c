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
//  FSCTL_REQUEST_OPLOCK), then falls through to the common handoff. Returns
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
//  of fastfat's UncleanCount.
//
//  Every oplock code tests the node for NULL before reading its type, and
//  that check is load-bearing rather than defensive. BlorgFileSystemControl
//  routes the FSDO down this same IRP_MN_USER_FS_REQUEST arm as the volume,
//  and a handle on the control device has no node behind it --
//  BlorgFileSystemCreate reports FILE_OPENED and leaves FsContext NULL. The
//  oplock FSCTLs are all FILE_ANY_ACCESS, so reaching them needs only the
//  GENERIC_READ the FSDO's SDDL grants World (BLORGFS_FSDO_DEVICE_SDDL_STRING,
//  Driver.h); without the check, GET_NODE_TYPE dereferences NULL and any
//  local caller can bugcheck the machine with one FSCTL. They answer
//  STATUS_INVALID_DEVICE_REQUEST rather than STATUS_INVALID_PARAMETER: the
//  request is not malformed, it is aimed at a device that has no such
//  operation.
//
//  FSCTL_FILESYSTEM_GET_STATISTICS(_EX) is handled first and separately:
//  it is a volume-wide query with no node to validate, so it must not
//  fall through the FCB/DCB type checks the oplock codes need. Answering
//  it in the documented FILESYSTEM_STATISTICS shape (Statistics.h) is
//  what makes `fsutil fsinfo statistics B:` work against this volume
//  rather than needing a bespoke tool. STATUS_BUFFER_OVERFLOW from the
//  fill is a success outcome for these FSCTLs, not an error -- callers
//  are expected to probe with a short buffer and resize -- so the status
//  is returned as-is alongside the byte count. After FsRtlOplockFsctrl returns, the IRP
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
        case FSCTL_FILESYSTEM_GET_STATISTICS:
        case FSCTL_FILESYSTEM_GET_STATISTICS_EX:
        {
            ULONG bytesReturned = 0;

            NTSTATUS statisticsStatus = BlorgStatisticsFillFsctlBuffer(
                Irp->AssociatedIrp.SystemBuffer,
                IrpSp->Parameters.FileSystemControl.OutputBufferLength,
                C_CAST(BOOLEAN, FSCTL_FILESYSTEM_GET_STATISTICS_EX == IrpSp->Parameters.FileSystemControl.FsControlCode),
                &bytesReturned);

            Irp->IoStatus.Information = bytesReturned;
            return statisticsStatus;
        }

        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:
        case FSCTL_REQUEST_FILTER_OPLOCK:
        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        {
            if (!node || BLORGFS_FCB_SIGNATURE != GET_NODE_TYPE(node))
            {
                return STATUS_INVALID_DEVICE_REQUEST;
            }

            isFile = TRUE;
            break;
        }

        case FSCTL_REQUEST_OPLOCK:
        {
            if (!node)
            {
                return STATUS_INVALID_DEVICE_REQUEST;
            }

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
// Ownership is settled by pointer identity against global.DiskDeviceObject,
// not by reading a magic out of the target's extension. This is the one
// device object that reaches this driver without having been created by
// it: once IoRegisterFileSystem has run, the I/O manager offers every
// arriving volume to every registered file system until one claims it, so
// a disc or a USB stick going in delivers another driver's storage device
// here. Its extension is not ours to interpret -- it can be NULL outright
// (IoCreateDevice with DeviceExtensionSize 0), or shorter than the
// ULONG64 a magic check reads out of it, and a value that happened to
// match would make this driver claim a volume it does not back. A pointer
// comparison dereferences nothing and cannot be imitated. It also stays
// correct in the window DriverEntry opens between IoRegisterFileSystem
// and the DDO existing: global.DiskDeviceObject is still NULL there, and
// the explicit NULL test below keeps a NULL target from matching it.
//
static NTSTATUS BlorgMountVolume(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_OBJECT targetDeviceObject = IrpSp->Parameters.MountVolume.DeviceObject;

    if (!targetDeviceObject || (targetDeviceObject != global.DiskDeviceObject))
    {
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    PDEVICE_OBJECT volumeDeviceObject;
    NTSTATUS status = BlorgCreateVolumeDeviceObject(global.DriverObject, &volumeDeviceObject);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ObReferenceObject(volumeDeviceObject);
    global.VolumeDeviceObject = volumeDeviceObject;

    PVPB vpb = IrpSp->Parameters.MountVolume.Vpb;
    KIRQL irql;

    IoAcquireVpbSpinLock(&irql);
    vpb->DeviceObject = volumeDeviceObject;
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

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        case BlorgDeviceFileSystem:
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
        case BlorgDeviceDisk:
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
