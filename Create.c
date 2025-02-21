#include "Driver.h"

static NTSTATUS BlorgVolumeCreate(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
    PFILE_OBJECT FileObject = pIrpSp->FileObject;
    PFILE_OBJECT RelatedFileObject = FileObject->RelatedFileObject;
    UNICODE_STRING FileName = FileObject->FileName;
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    /* open the volume object? */
    if (!RelatedFileObject && 0 == FileName.Length)
    {
        if (global.pDiskDeviceObject)
        {
            FileObject->Vpb = global.pDiskDeviceObject->Vpb;
        }

        FileObject->FsContext2 = global.pVolumeDeviceObject;

        pIrp->IoStatus.Information = FILE_OPENED;
        result = STATUS_SUCCESS;
    }

    return result;
}

static NTSTATUS BlorgDiskCreate(PIRP pIrp)
{
    pIrp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}


NTSTATUS BlorgCreate(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (GetDeviceExtensionMagic(pDeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            result = BlorgVolumeCreate(pIrp, pIrpSp);
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
			result = BlorgDiskCreate(pIrp);
            break;
        }
    }

    pIrp->IoStatus.Status = result;

    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return pIrp->IoStatus.Status;
}