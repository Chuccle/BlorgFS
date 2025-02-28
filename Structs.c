#include "Driver.h"

NTSTATUS BlorgCreateFCB(FCB** Fcb, PDCB ParentDcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Fcb = NULL;

    PNON_PAGED_NODE pNonPaged = ExAllocateFromNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList);

    if (!pNonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

#pragma warning(suppress: 6386) // Prefast thinks this is an overrun?
    RtlZeroMemory(pNonPaged, sizeof(NON_PAGED_NODE));

    PFCB pFcb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList);

    if (!pFcb)
    {
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pFcb, sizeof(FCB));

    if (Name)
    {
        PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, Name->Length, 'FCB');

        if (!nameBuffer)
        {
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, pFcb);
            ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

        pFcb->Name.Buffer = nameBuffer;
        pFcb->Name.Length = Name->Length;
        pFcb->Name.MaximumLength = Name->Length;
    }

    KeInitializeGuardedMutex(&pFcb->Lock);

    NTSTATUS result = ExInitializeResourceLite(&pNonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        if (Name)
        {
            ExFreePool(pFcb->Name.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, pFcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&pNonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&pNonPaged->HdrResource);
        if (Name)
        {
            ExFreePool(pFcb->Name.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, pFcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return result;
    }

    ExInitializeFastMutex(&pNonPaged->HdrFastMutex);

    FsRtlSetupAdvancedHeader(&pFcb->Header, &pNonPaged->HdrFastMutex);

    pFcb->Header.NodeTypeCode = NodeType;
    pFcb->Header.NodeByteSize = sizeof(FCB);
    pFcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pFcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    pFcb->Header.Resource = &pNonPaged->HdrResource;
    pFcb->Header.PagingIoResource = &pNonPaged->HdrPagingIoResource;

    pFcb->NonPaged = pNonPaged;
    pFcb->VolumeDeviceObject = VolumeDeviceObject;
    pFcb->ParentDcb = ParentDcb;

    *Fcb = pFcb;

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreateDCB(DCB** Dcb, PDCB ParentDcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Dcb = NULL;

    PNON_PAGED_NODE pNonPaged = ExAllocateFromNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList);

    if (!pNonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

#pragma warning(suppress: 6386) // Prefast thinks this is an overrun?
    RtlZeroMemory(pNonPaged, sizeof(NON_PAGED_NODE));

    PDCB pDcb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList);

    if (!pDcb)
    {
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pDcb, sizeof(DCB));

    PWCHAR nameBuffer = ExAllocatePoolUninitialized(PagedPool, Name->Length, 'DCB');

    if (!nameBuffer)
    {
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, pDcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

    pDcb->Name.Buffer = nameBuffer;
    pDcb->Name.Length = Name->Length;
    pDcb->Name.MaximumLength = Name->Length;

    KeInitializeGuardedMutex(&pDcb->Lock);

    NTSTATUS result = ExInitializeResourceLite(&pNonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        ExFreePool(pDcb->Name.Buffer);
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, pDcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&pNonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&pNonPaged->HdrResource);
        ExFreePool(pDcb->Name.Buffer);
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, pDcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, pNonPaged);
        return result;
    }

    ExInitializeFastMutex(&pNonPaged->HdrFastMutex);

    FsRtlSetupAdvancedHeader(&pDcb->Header, &pNonPaged->HdrFastMutex);

    pDcb->Header.NodeTypeCode = NodeType;
    pDcb->Header.NodeByteSize = sizeof(DCB);
    pDcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pDcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    pDcb->Header.Resource = &pNonPaged->HdrResource;
    pDcb->Header.PagingIoResource = &pNonPaged->HdrPagingIoResource;

    pDcb->NonPaged = pNonPaged;
    pDcb->VolumeDeviceObject = VolumeDeviceObject;
    pDcb->ParentDcb = ParentDcb;

    *Dcb = pDcb;

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreateCCB(CCB** Ccb, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Ccb = NULL;

    PCCB pCcb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->CcbLookasideList);

    if (!pCcb)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pCcb->NodeTypeCode = BLORGFS_CCB_SIGNATURE;
    pCcb->NodeByteSize = sizeof(CCB);
    pCcb->SearchPattern.Buffer = NULL;

    *Ccb = pCcb;

    return STATUS_SUCCESS;
}

#define DEALLOCATE_COMMON(ctx)                                                                                                        \
do                                                                                                                                    \
{                                                                                                                                     \
    PCOMMON_CONTEXT pCommonContext = ctx;                                                                                             \
    ExFreePool(pCommonContext->Name.Buffer);                                                                                          \
    ExDeleteResourceLite(&pCommonContext->NonPaged->HdrPagingIoResource);                                                             \
    ExDeleteResourceLite(&pCommonContext->NonPaged->HdrResource);                                                                     \
	PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;             \
    ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->NonPagedNodeLookasideList, pCommonContext->NonPaged); \
} while(0)

void BlorgFreeFileContext(PVOID Context)
{
    switch (GET_NODE_TYPE(Context))
    {
        case BLORGFS_VCB_SIGNATURE:
        case BLORGFS_FCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PFCB pFcb = Context;
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->FcbLookasideList, pFcb);
            break;
        }
        case BLORGFS_DCB_SIGNATURE:
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PDCB pDcb = Context;
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->DcbLookasideList, pDcb);
            break;
        }
        case BLORGFS_CCB_SIGNATURE:
        {
            PCCB pCcb = Context;
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            if (pCcb->SearchPattern.Buffer)
            {
                ExFreePool(pCcb->SearchPattern.Buffer);
                pCcb->SearchPattern.Buffer = NULL;
            }
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->CcbLookasideList, pCcb);
            break;
        }
    }
}