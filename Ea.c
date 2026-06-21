#include "Driver.h"

//
// IRP_MJ_QUERY_EA / IRP_MJ_SET_EA dispatch. BlorgFS has no extended
// attribute store, so both handlers just report/reject accordingly per
// device type.
//

//
// Reports "no EAs" for a volume file rather than a hard error: some
// callers (e.g. the image loader activating an executable) query EAs as
// part of open, and a hard failure there aborts the open entirely.
//
NTSTATUS BlorgQueryEa(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            Irp->IoStatus.Information = 0;
            result = STATUS_NO_EAS_ON_FILE;
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

// No device type supports setting EAs; always rejected.
NTSTATUS BlorgSetEa(PDEVICE_OBJECT DeviceObject, PIRP Irp)
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
