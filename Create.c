#include "Driver.h"

static NTSTATUS BlorgVolumeOpen(PIRP pIrp, PFILE_OBJECT pFileObject, UCHAR createDisposition, USHORT shareAccess, PACCESS_MASK pDesiredAccess)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    if (*pDesiredAccess == 0)
    {
        result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    if (shareAccess == 0)
    {
        result = STATUS_INVALID_PARAMETER;
        goto exit;
    }
    
    if ((createDisposition != FILE_OPEN) &&
        (createDisposition != FILE_OPEN_IF)) 
    {
        result = STATUS_ACCESS_DENIED;
		goto exit;
    }

    //
    // Only allow the volume to be opened for read access.
    //

    if (FlagOn(*pDesiredAccess, ~FILE_GENERIC_READ))
    {
        result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    PDCB pRootDcb = GetVolumeDeviceExtension(global.pVolumeDeviceObject)->RootDcb;

    if (InterlockedIncrement64(&pRootDcb->RefCount) == 1)
    {
        IoSetShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&pRootDcb->RefCount);
            goto exit;
        }
    }
    
    pFileObject->SectionObjectPointer = &pRootDcb->NonPaged->SectionObjectPointers;
	pFileObject->Vpb = global.pDiskDeviceObject->Vpb;
	pFileObject->FsContext = pRootDcb;
	pFileObject->FsContext2 = NULL;
	
	result = STATUS_SUCCESS;
	pIrp->IoStatus.Information = FILE_OPENED;

exit:

    return result;
}

static NTSTATUS BlorgRootDirectoryOpen(PIRP pIrp, PFILE_OBJECT pFileObject, UCHAR createDisposition, PACCESS_MASK pDesiredAccess, USHORT shareAccess)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    if (*pDesiredAccess == 0)
    {
        result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    if (shareAccess == 0)
    {
        result = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    if ((createDisposition != FILE_OPEN) &&
        (createDisposition != FILE_OPEN_IF))
    {
        result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    //
    // Only allow the root directory to be opened for read access.
    //

    if (FlagOn(*pDesiredAccess, ~FILE_GENERIC_READ))
    {
        result = STATUS_ACCESS_DENIED;
        goto exit;
    }

    PDCB pRootDcb = GetVolumeDeviceExtension(global.pVolumeDeviceObject)->RootDcb;

    if (InterlockedIncrement64(&pRootDcb->RefCount) == 1)
    {
        IoSetShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(result))
        {
            InterlockedDecrement64(&pRootDcb->RefCount);
            goto exit;
        }
    }

    pFileObject->SectionObjectPointer = &pRootDcb->NonPaged->SectionObjectPointers;
    pFileObject->Vpb = global.pDiskDeviceObject->Vpb;
    pFileObject->FsContext = pRootDcb;
    pFileObject->FsContext2 = NULL;

	result = STATUS_SUCCESS;
    pIrp->IoStatus.Information = FILE_OPENED;

exit:

    return result;
}

static NTSTATUS BlorgVolumeCreate(PIRP pIrp, PIO_STACK_LOCATION pIrpSp)
{
    // handle the volume object's create request
    KdBreakPoint();
    PFILE_OBJECT pFileObject = pIrpSp->FileObject;
    PFILE_OBJECT pRelatedFileObject = pFileObject->RelatedFileObject;
    UNICODE_STRING fileName = pFileObject->FileName;


    ULONG options = pIrpSp->Parameters.Create.Options;
    USHORT shareAccess = pIrpSp->Parameters.Create.ShareAccess;
    UCHAR createDisposition = (options >> 24) & 0x000000ff;
    PACCESS_MASK pDesiredAccess = &pIrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    if (!pRelatedFileObject)
    {
        // open the volume object
        if (0 == fileName.Length)
        {
            result = BlorgVolumeOpen(pIrp, pFileObject, createDisposition, shareAccess, pDesiredAccess);
            goto exit;
        }

        // open the root directory
        if (sizeof(WCHAR) == fileName.Length && L'\\' == fileName.Buffer[0])
        {
            if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
            {
                result = STATUS_FILE_IS_A_DIRECTORY;
                goto exit;
            }

            result = BlorgRootDirectoryOpen(pIrp, pFileObject, createDisposition, pDesiredAccess, shareAccess);
            goto exit;
        }
    }

exit:

    return result;
}

static NTSTATUS BlorgDiskCreate(PIRP pIrp)
{
	// handle the disk object's create request
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
