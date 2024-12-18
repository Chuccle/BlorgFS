#include "Driver.h"

NTSTATUS BlorgLockControl(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	KdBreakPoint();
	UNREFERENCED_PARAMETER(pDeviceObject);

	// PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	switch (GetDeviceExtensionMagic(pDeviceObject))
	{
		case BLORGFS_VDO_MAGIC:
		{
			// result = BlorgVolumeLockControl(pIrp, pIrpSp);
			break;
		}
		case BLORGFS_DDO_MAGIC:
		{
			// result = BlorgDiskLockControl(pIrp);
			break;
		}
	}

	pIrp->IoStatus.Status = result;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}