#include "Driver.h"

NTSTATUS BlorgSetSecurity(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	UNREFERENCED_PARAMETER(pDeviceObject);

	// PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	switch (GetDeviceExtensionMagic(pDeviceObject))
	{
		case BLORGFS_VDO_MAGIC:
		{
			// result = BlorgVolumeSetSecurity(pIrp, pIrpSp);
			break;
		}
		case BLORGFS_DDO_MAGIC:
		{
			// result = BlorgDiskSetSecurity(pIrp);
			break;
		}
	}

	pIrp->IoStatus.Status = result;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}