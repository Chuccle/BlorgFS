#include "Driver.h"

//
// IRP_MJ_WRITE dispatch. Not yet implemented for any device type: BlorgFS
// is currently read-only against its HTTP backend.
//
NTSTATUS BlorgWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
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

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
