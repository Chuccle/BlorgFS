#include "Driver.h"

NTSTATUS BlorgCreateFCB(FCB** ppFcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name)
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

    PNON_PAGED_NODE pNonPaged = ExAllocatePoolZero(NonPagedPoolNx, sizeof(NON_PAGED_NODE), 'FCB');
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

    KeInitializeGuardedMutex(&pFcb->Lock);
    ExInitializeResourceLite(&pNonPaged->HdrResource);
    ExInitializeResourceLite(&pNonPaged->HdrPagingIoResource);
    ExInitializeFastMutex(&pNonPaged->HdrFastMutex);

#pragma warning(suppress: 4127)
    FsRtlSetupAdvancedHeaderEx2(&pFcb->Header, &pNonPaged->HdrFastMutex, NULL, pFcb->Header.AePushLock);

    pFcb->Header.NodeTypeCode = nodeType;
    pFcb->Header.NodeByteSize = sizeof(FCB);
    pFcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pFcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    pFcb->Header.Resource = &pNonPaged->HdrResource;
    pFcb->Header.PagingIoResource = &pNonPaged->HdrPagingIoResource;

    pFcb->NonPaged = pNonPaged;
    pFcb->VolumeDeviceObject = global.pVolumeDeviceObject;
    pFcb->ParentDcb = parentDcb;

    *ppFcb = pFcb;

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreateDCB(DCB** ppDcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name)
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

    PNON_PAGED_NODE pNonPaged = ExAllocatePoolZero(NonPagedPoolNx, sizeof(NON_PAGED_NODE), 'DCB');
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

    KeInitializeGuardedMutex(&pDcb->Lock);
    ExInitializeResourceLite(&pNonPaged->HdrResource);
    ExInitializeResourceLite(&pNonPaged->HdrPagingIoResource);
    ExInitializeFastMutex(&pNonPaged->HdrFastMutex);

#pragma warning(suppress: 4127)
    FsRtlSetupAdvancedHeaderEx2(&pDcb->Header, &pNonPaged->HdrFastMutex, NULL, pDcb->Header.AePushLock);

    pDcb->Header.NodeTypeCode = nodeType;
    pDcb->Header.NodeByteSize = sizeof(DCB);
    pDcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    pDcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    pDcb->Header.Resource = &pNonPaged->HdrResource;
    pDcb->Header.PagingIoResource = &pNonPaged->HdrPagingIoResource;

    pDcb->NonPaged = pNonPaged;
    pDcb->VolumeDeviceObject = global.pVolumeDeviceObject;
    pDcb->ParentDcb = parentDcb;

    *ppDcb = pDcb;

    return STATUS_SUCCESS;
}

#define DEALLOCATE_FILENODE_COMMON(node)                                  \
do                                                                        \
{                                                                         \
    PCOMMON_CONTEXT pCommonContext = node;                                \
    ExFreePool(pCommonContext->Name.Buffer);                              \
                                                                          \
    ExDeleteResourceLite(&pCommonContext->NonPaged->HdrPagingIoResource); \
    ExDeleteResourceLite(&pCommonContext->NonPaged->HdrResource);         \
    ExFreePool(pCommonContext->NonPaged);                                 \
                                                                          \
    FsRtlFreeAePushLock(pCommonContext->Header.AePushLock);               \
                                                                          \
    ExFreePool(pCommonContext);                                           \
} while(0)

void BlorgFreeFileContext(PVOID pContext) 
{
    switch (GET_NODE_TYPE(pContext))
    {
        case BLORGFS_FILE_NODE_SIGNATURE:
        {
            DEALLOCATE_FILENODE_COMMON(pContext);
            break;
        }
        case BLORGFS_DIRECTORY_NODE_SIGNATURE:
        case BLORGFS_VOLUME_NODE_SIGNATURE:
        case BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE:
        {
            DEALLOCATE_FILENODE_COMMON(pContext);
            break;
        }
    }
}
