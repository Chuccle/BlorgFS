#include "Driver.h"


static NTSTATUS BlorgVolumeRead(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
	UNREFERENCED_PARAMETER(pIrp);
	UNREFERENCED_PARAMETER(pIrpSp);
	KdBreakPoint();
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    return result;
}

NTSTATUS BlorgRead(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeRead(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskRead(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}