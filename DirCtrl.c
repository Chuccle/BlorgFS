#include "Driver.h"

NTSTATUS BlorgDirectoryControl(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION IoStackLocation = IoGetCurrentIrpStackLocation(pIrp);

    switch (IoStackLocation->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            pIrp->IoStatus.Information = 0;
            break;
        }
        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        {
            pIrp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
            pIrp->IoStatus.Information = 0;
            break;
        }
        default:
        {
            pIrp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
            pIrp->IoStatus.Information = 0;
            break;
        }
    }

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}
