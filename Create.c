#include "Driver.h"

NTSTATUS BlorgFCBCreate(FCB** ppFcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name)
{
    *ppFcb = NULL;
    
    switch (nodeType)
    {
        case BLORGFS_DIRECTORY_NODE_SIGNATURE:
        {
            return STATUS_INVALID_PARAMETER;
        }
        case BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE:
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    PNON_PAGED_FCB pNonPaged = ExAllocatePoolZero(NonPagedPoolNx, sizeof(NON_PAGED_FCB), 'FCB');
    if (NULL == pNonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    PFCB pFcb = ExAllocatePoolZero(PagedPool, sizeof(FCB), 'FCB');
    if (NULL == pFcb)
    {
        ExFreePool(pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pFcb->Header.NodeTypeCode = nodeType;
    pFcb->Header.NodeByteSize = sizeof(FCB);
    pFcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pFcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;

    KeInitializeGuardedMutex(&pFcb->Lock);

    PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, name->Length, 'FCB');
    if (NULL == nameBuffer)
    {
        ExFreePool(pFcb);
        ExFreePool(pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(nameBuffer, name->Buffer, name->Length);

    pFcb->Name.Buffer = nameBuffer;
    pFcb->Name.Length = name->Length;
    pFcb->Name.MaximumLength = name->Length;

    pFcb->Header.AePushLock = FsRtlAllocateAePushLock(PagedPool, 'FCB');
	if (NULL == pFcb->Header.AePushLock)
	{
        ExFreePool(pFcb->Name.Buffer);
		ExFreePool(pFcb);
		ExFreePool(pNonPaged);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

    ExInitializeFastMutex(&pNonPaged->HeaderFastMutex);

#pragma warning(suppress: 4127)
    FsRtlSetupAdvancedHeaderEx2(&pFcb->Header, &pNonPaged->HeaderFastMutex, NULL, pFcb->Header.AePushLock);
    pFcb->NonPaged = pNonPaged;
    pFcb->RefCount = 1;
    pFcb->VolumeDeviceObject = global.pVolumeDeviceObject;
	pFcb->ParentDcb = parentDcb;

    *ppFcb = pFcb;

    return STATUS_SUCCESS;
}


NTSTATUS BlorgDCBCreate(DCB** ppDcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name)
{
    *ppDcb = NULL;

    switch (nodeType)
    {
        case BLORGFS_FILE_NODE_SIGNATURE:
        {
            return STATUS_INVALID_PARAMETER;
        }
        case BLORGFS_VOLUME_NODE_SIGNATURE:
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    PNON_PAGED_FCB pNonPaged = ExAllocatePoolZero(NonPagedPoolNx, sizeof(NON_PAGED_FCB), 'DCB');
    if (NULL == pNonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PDCB pDcb = ExAllocatePoolZero(PagedPool, sizeof(DCB), 'DCB');
    if (NULL == pDcb)
    {
        ExFreePool(pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pDcb->Header.NodeTypeCode = nodeType;
    pDcb->Header.NodeByteSize = sizeof(DCB);
    pDcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pDcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;

    KeInitializeGuardedMutex(&pDcb->Lock);
    KeInitializeGuardedMutex(&pDcb->ListLock);
    InitializeListHead(&pDcb->ListHead);

    PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, name->Length, 'DCB');
    if (NULL == nameBuffer)
    {
        ExFreePool(pDcb);
        ExFreePool(pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(nameBuffer, name->Buffer, name->Length);

    pDcb->Name.Buffer = nameBuffer;
    pDcb->Name.Length = name->Length;
    pDcb->Name.MaximumLength = name->Length;

    pDcb->Header.AePushLock = FsRtlAllocateAePushLock(PagedPool, 'DCB');
    if (NULL == pDcb->Header.AePushLock)
    {
        ExFreePool(pDcb->Name.Buffer);
        ExFreePool(pDcb);
        ExFreePool(pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExInitializeFastMutex(&pNonPaged->HeaderFastMutex);

#pragma warning(suppress: 4127)
    FsRtlSetupAdvancedHeaderEx2(&pDcb->Header, &pNonPaged->HeaderFastMutex, NULL, pDcb->Header.AePushLock);
    pDcb->NonPaged = pNonPaged;
    pDcb->RefCount = 1;
    pDcb->VolumeDeviceObject = global.pVolumeDeviceObject;
    pDcb->ParentDcb = parentDcb;

    *ppDcb = pDcb;

    return STATUS_SUCCESS;
}

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

    KeAcquireGuardedMutex(&pRootDcb->Lock);

    if (pRootDcb->RefCount == 1)
    {
        IoSetShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(result))
        {
            KeReleaseGuardedMutex(&pRootDcb->Lock);
        }
    }

    pRootDcb->RefCount += 1;
    KeReleaseGuardedMutex(&pRootDcb->Lock);

    pFileObject->Vpb = global.pDiskDeviceObject->Vpb;
	pFileObject->FsContext = pRootDcb;
	pFileObject->FsContext2 = NULL;
	pIrp->IoStatus.Information = FILE_OPENED;
	result = STATUS_SUCCESS;

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

    KeAcquireGuardedMutex(&pRootDcb->Lock);

    if (pRootDcb->RefCount == 1) 
    {
        IoSetShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess);
    }
    else
    {
        result = IoCheckShareAccess(*pDesiredAccess, shareAccess, pFileObject, &pRootDcb->ShareAccess, TRUE);
        if (!NT_SUCCESS(result))
        {
            KeReleaseGuardedMutex(&pRootDcb->Lock);
        }
    }

    pRootDcb->RefCount += 1;
    KeReleaseGuardedMutex(&pRootDcb->Lock);


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
