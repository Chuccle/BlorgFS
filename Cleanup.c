#include "Driver.h"

static NTSTATUS BlorgVolumeCleanup(PIO_STACK_LOCATION IrpSp)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    PFILE_OBJECT pFileObject = IrpSp->FileObject;

    switch GET_NODE_TYPE(pFileObject->FsContext)
    {
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            result = STATUS_SUCCESS;
            break;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        case BLORGFS_FCB_SIGNATURE:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
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

NTSTATUS BlorgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
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
        case BLORGFS_FSDO_MAGIC:
        {
            break;
        }
    }

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}