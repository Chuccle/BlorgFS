#include "Driver.h"

NTSTATUS BlorgSetVolumeInformation(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	UNREFERENCED_PARAMETER(pDeviceObject);

	// PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

	switch (GetDeviceExtensionMagic(pDeviceObject))
	{
		case BLORGFS_VDO_MAGIC:
		{
			// result = BlorgVolumeSetVolumeInformation(pIrp, pIrpSp);
			break;
		}
		case BLORGFS_DDO_MAGIC:
		{
			// result = BlorgDiskSetVolumeInformation(pIrp);
			break;
		}
	}

	pIrp->IoStatus.Status = result;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return pIrp->IoStatus.Status;
}
