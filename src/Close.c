#include "Driver.h"

//
//  IRP_MJ_CLOSE handling: drops the final reference on a node (FCB/DCB/VCB).
//  Never takes the VCB resource: FCB/DCB closes go through
//  BlorgNodeDereference (one interlocked decrement under the node's table
//  bucket lock shared), and a node whose last handle leaves is deferred to
//  the reap worker, which retires idle nodes and their newly empty
//  ancestors in batches under a single VCB-exclusive acquire. The root DCB
//  and VCB are never table-resident or reaped, so their counts are plain
//  interlocked drops.
//

static NTSTATUS BlorgVolumeClose(PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PVCB vcb = GetVolumeDeviceExtension(VolumeDeviceObject)->Vcb;

    switch GET_NODE_TYPE(fileObject->FsContext)
    {
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            BlorgFreeFileContext(fileObject->FsContext2, VolumeDeviceObject);
            PDCB dcb = fileObject->FsContext;
            InterlockedDecrement64(&dcb->RefCount);

            return STATUS_SUCCESS;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            BlorgFreeFileContext(fileObject->FsContext2, VolumeDeviceObject);
            BlorgNodeDereference(fileObject->FsContext);

            return STATUS_SUCCESS;
        }
        case BLORGFS_FCB_SIGNATURE:
        {
            BlorgNodeDereference(fileObject->FsContext);

            return STATUS_SUCCESS;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            InterlockedDecrement64(&vcb->RefCount);

            return STATUS_SUCCESS;
        }
        default:
        {
            BLORGFS_PRINT("BlorgVolumeClose: Unknown Node type\n");
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    }
}

//
//  IRP_MJ_CLOSE dispatch entry: dispatches by device type to the
//  per-node close handler, then completes the IRP.
//
NTSTATUS BlorgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    FsRtlEnterFileSystem();
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            result = BlorgVolumeClose(irpSp, DeviceObject);
            break;
        }
        case BlorgDeviceDisk:
        {
            break;
        }
        case BlorgDeviceFileSystem:
        {
            break;
        }
    }
    FsRtlExitFileSystem();
    
    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}