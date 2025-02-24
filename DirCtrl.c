#include "Driver.h"

static NTSTATUS BlorgVolumeDirectoryControl(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (pIrpSp->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
        {
            result = STATUS_NOT_IMPLEMENTED;
            pIrp->IoStatus.Information = 0;
            break;
        }
        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        {
            result = STATUS_NOT_IMPLEMENTED;
            pIrp->IoStatus.Information = 0;
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
            pIrp->IoStatus.Information = 0;
        }
    }

    return result;
}


NTSTATUS BlorgDirectoryControl(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeDirectoryControl(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskDirectoryControl(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}