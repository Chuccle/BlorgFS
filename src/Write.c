#include "Driver.h"

//
// IRP_MJ_WRITE dispatch. Not yet implemented for any device type: BlorgFS
// is currently read-only against its HTTP backend.
//
NTSTATUS BlorgWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
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

    Irp->IoStatus.Status = result;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
