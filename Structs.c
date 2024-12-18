#include "Driver.h"

NTSTATUS BlorgCreateFCB(FCB** Fcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Fcb = NULL;

    PNON_PAGED_NODE nonPaged = ExAllocateFromNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList);

    if (!nonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

#pragma warning(suppress: 6386) // Prefast thinks this is an overrun?
    RtlZeroMemory(nonPaged, sizeof(NON_PAGED_NODE));

    PFCB fcb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList);

    if (!fcb)
    {
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(fcb, sizeof(FCB));

    if (Name && (0 < Name->Length))
    {
        PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, Name->Length, 'FCB');

        if (!nameBuffer)
        {
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
            ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

        fcb->FullPath.Buffer = nameBuffer;
        fcb->FullPath.Length = Name->Length;
        fcb->FullPath.MaximumLength = Name->Length;
    }

    NTSTATUS result = ExInitializeResourceLite(&nonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        if (fcb->FullPath.Buffer)
        {
            ExFreePool(fcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&nonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&nonPaged->HdrResource);
        if (fcb->FullPath.Buffer)
        {
            ExFreePool(fcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    ExInitializeFastMutex(&nonPaged->HdrFastMutex);

    FsRtlInitializeFileLock(&fcb->FileLock, NULL, NULL);

    FsRtlInitializeOplock(fcb->Header.Oplock);

    FsRtlSetupAdvancedHeader(&fcb->Header, &nonPaged->HdrFastMutex);

    fcb->Header.NodeTypeCode = NodeType;
    fcb->Header.NodeByteSize = sizeof(FCB);
    fcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    fcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    fcb->Header.Resource = &nonPaged->HdrResource;
    fcb->Header.PagingIoResource = &nonPaged->HdrPagingIoResource;

    fcb->NonPaged = nonPaged;
    fcb->VolumeDeviceObject = VolumeDeviceObject;

    *Fcb = fcb;

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreateDCB(DCB** Dcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Dcb = NULL;

    PNON_PAGED_NODE nonPaged = ExAllocateFromNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList);

    if (!nonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

#pragma warning(suppress: 6386) // Prefast thinks this is an overrun?
    RtlZeroMemory(nonPaged, sizeof(NON_PAGED_NODE));

    PDCB dcb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList);

    if (!dcb)
    {
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(dcb, sizeof(DCB));

    InitializeListHead(&dcb->ChildrenList);

    PWCHAR nameBuffer = ExAllocatePoolUninitialized(PagedPool, Name->Length, 'DCB');

    if (!nameBuffer)
    {
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

    dcb->FullPath.Buffer = nameBuffer;
    dcb->FullPath.Length = Name->Length;
    dcb->FullPath.MaximumLength = Name->Length;
    
    NTSTATUS result = ExInitializeResourceLite(&nonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        if (dcb->FullPath.Buffer)
        {
           ExFreePool(dcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&nonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&nonPaged->HdrResource);
        if (dcb->FullPath.Buffer)
        {
            ExFreePool(dcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    ExInitializeFastMutex(&nonPaged->HdrFastMutex);

    FsRtlSetupAdvancedHeader(&dcb->Header, &nonPaged->HdrFastMutex);

    dcb->Header.NodeTypeCode = NodeType;
    dcb->Header.NodeByteSize = sizeof(DCB);
    dcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    dcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    dcb->Header.Resource = &nonPaged->HdrResource;
    dcb->Header.PagingIoResource = &nonPaged->HdrPagingIoResource;

    dcb->NonPaged = nonPaged;
    dcb->VolumeDeviceObject = VolumeDeviceObject;

    *Dcb = dcb;

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCreateCCB(CCB** Ccb, PDEVICE_OBJECT VolumeDeviceObject)
{
    *Ccb = NULL;

    PCCB ccb = ExAllocateFromPagedLookasideList(&GetVolumeDeviceExtension(VolumeDeviceObject)->CcbLookasideList);

    if (!ccb)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ccb, sizeof(CCB));

    ccb->NodeTypeCode = BLORGFS_CCB_SIGNATURE;
    ccb->NodeByteSize = sizeof(CCB);
    ccb->SearchPattern.Buffer = NULL;

    *Ccb = ccb;

    return STATUS_SUCCESS;
}

#define DEALLOCATE_COMMON(ctx)                                                                                                       \
do                                                                                                                                   \
{                                                                                                                                    \
    PCOMMON_CONTEXT commonContext = ctx;                                                                                             \
    ExDeleteResourceLite(&commonContext->NonPaged->HdrPagingIoResource);                                                             \
    ExDeleteResourceLite(&commonContext->NonPaged->HdrResource);                                                                     \
	PDEVICE_OBJECT volumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;             \
    ExFreeToNPagedLookasideList(&GetVolumeDeviceExtension(volumeDeviceObject)->NonPagedNodeLookasideList, commonContext->NonPaged);  \
}                                                                                                                                    \
while(0)

void BlorgFreeFileContext(PVOID Context)
{
    switch (GET_NODE_TYPE(Context))
    {
        case BLORGFS_FCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PFCB fcb = Context;
            ExFreePool(fcb->FullPath.Buffer);
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            RemoveEntryList(&(fcb->Links));
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->FcbLookasideList, fcb);
            break;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PDCB dcb = Context;
            ExFreePool(dcb->FullPath.Buffer);
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            RemoveEntryList(&(dcb->Links));
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->DcbLookasideList, dcb);
            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PVCB vcb = Context;
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->FcbLookasideList, vcb);
            break;
        }
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            DEALLOCATE_COMMON(Context);
            PDCB dcb = Context;
            ExFreePool(dcb->FullPath.Buffer);
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->DcbLookasideList, dcb);
            break;
        }
        case BLORGFS_CCB_SIGNATURE:
        {
            PCCB ccb = Context;
            PDEVICE_OBJECT pVolumeDeviceObject = GetFileSystemDeviceExtension(global.FileSystemDeviceObject)->VolumeDeviceObject;
            if (ccb->SearchPattern.Buffer)
            {
                RtlFreeUnicodeString(&ccb->SearchPattern);
                RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
            }
            FreeHttpDirectoryInfo(&ccb->SubDirectories, &ccb->Files);
            ExFreeToPagedLookasideList(&GetVolumeDeviceExtension(pVolumeDeviceObject)->CcbLookasideList, ccb);
            break;
        }
    }
}

/**
 * GetLastComponent - Extracts the last component of a given path
 *
 * @param Path - Full path to extract from
 *
 * @return UNICODE_STRING - The last component of the path (filename or last directory)
 */
static UNICODE_STRING GetLastComponent(PCUNICODE_STRING Path)
{
    UNICODE_STRING lastComponent = { 0 };

    if (Path == NULL || Path->Length == 0 || Path->Buffer == NULL)
    {
        return lastComponent;
    }

    PWCHAR pathEnd = Path->Buffer + (Path->Length / sizeof(WCHAR)) - 1;
    PWCHAR current = pathEnd;

    // Skip trailing separator if present
    if (*current == L'\\' && current > Path->Buffer)
    {
        current--;
        pathEnd = current;
    }

    while (current >= Path->Buffer)
    {
        if (*current == L'\\')
        {
            break;
        }
        current--;
    }

    // Calculate the start position (after the separator or beginning of string)
    PWCHAR start = (current < Path->Buffer) ? Path->Buffer : current + 1;

    // Calculate component length
    USHORT componentLength = (USHORT)((pathEnd - start + 1) * sizeof(WCHAR));

    if (componentLength > 0)
    {
        lastComponent.Length = componentLength;
        lastComponent.MaximumLength = componentLength;
        lastComponent.Buffer = start;
    }

    return lastComponent;
}

/**
 * ArePathComponentsEqual - Case-insensitive comparison of path components
 *
 * @param Component1 - First component to compare
 * @param Component2 - Second component to compare
 *
 * @return BOOLEAN - TRUE if equal, FALSE otherwise
 */
static BOOLEAN ArePathComponentsEqual(PCUNICODE_STRING Component1, PCUNICODE_STRING Component2)
{
    // Quick length check
    if (Component1->Length != Component2->Length)
    {
        return FALSE;
    }

    // Use the NT case-insensitive comparison for path components
    return RtlEqualUnicodeString(Component1, Component2, TRUE);
}

PCOMMON_CONTEXT SearchByName(PDCB Dcb, PCUNICODE_STRING Name)
{
    PCOMMON_CONTEXT child = NULL;
    UNICODE_STRING lastComponent;

    for (PLIST_ENTRY entry = Dcb->ChildrenList.Flink;
        entry != &Dcb->ChildrenList;
        entry = entry->Flink)
    {
        child = CONTAINING_RECORD(entry, COMMON_CONTEXT, Links);
        lastComponent = GetLastComponent(&child->FullPath);
        
        if (ArePathComponentsEqual(Name, &lastComponent))
        {
            return child;
        }
    }

     return NULL;
}

PCOMMON_CONTEXT SearchByPath(PDCB RootDcb, PCUNICODE_STRING Path)
{
    PDCB currentDcb = RootDcb;
    UNICODE_STRING remainingPath = *Path;
    UNICODE_STRING component, nextRemainingPart, lastComponent;
    PCOMMON_CONTEXT child = NULL;
    PCOMMON_CONTEXT matchingChild = NULL;
    
    while (0 < remainingPath.Length)
    {
        FsRtlDissectName(remainingPath, &component, &nextRemainingPart);

        matchingChild = NULL;

        for (PLIST_ENTRY entry = currentDcb->ChildrenList.Flink;
            entry != &currentDcb->ChildrenList;
            entry = entry->Flink)
        {
            child = CONTAINING_RECORD(entry, COMMON_CONTEXT, Links);
            lastComponent = GetLastComponent(&child->FullPath);

            if (ArePathComponentsEqual(&component, &lastComponent))
            {
                matchingChild = child;
                break;
            }
        }

        if (!matchingChild)
        {
            return NULL;
        }
        
        // If it's not a directory this is our guy
        if (BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(matchingChild))
        {
           return (nextRemainingPart.Length == 0) ? matchingChild : NULL;
        }

        currentDcb = (PDCB)child;
        remainingPath = nextRemainingPart;
    }

    return (PCOMMON_CONTEXT)currentDcb;
}

NTSTATUS InsertByPath(PDCB RootDcb, PCUNICODE_STRING Path, BOOLEAN Directory, PDEVICE_OBJECT VolumeDeviceObject, PCOMMON_CONTEXT* Out)
{
    *Out = NULL;
    UNICODE_STRING remainingPath = *Path;
    UNICODE_STRING firstPart, remainingPart;
    PDCB currentDcb = RootDcb;
    PCOMMON_CONTEXT lastCreated = NULL;

    // Iterate through each path component
    while (0 < remainingPath.Length)
    {
        // Dissect the path into first component and remaining path
        FsRtlDissectName(remainingPath, &firstPart, &remainingPart);

        // Check if this component already exists
        PDCB childDcb = (PDCB)SearchByName(currentDcb, &firstPart);

        if (!childDcb)
        {
            // Component doesn't exist, create a new DCB
            BOOLEAN isLastComponent = (0 == remainingPart.Length);
            NTSTATUS status;

            if (isLastComponent)
            {
                if (!Directory)
                {
                    // Last component is a file, create FCB
                    PFCB newFcb;

                    status = BlorgCreateFCB(&newFcb, BLORGFS_FCB_SIGNATURE, Path, VolumeDeviceObject);

                    if (!NT_SUCCESS(status))
                    {
                        return status;
                    }

                    newFcb->ParentDcb = currentDcb;
                    InsertTailList(&currentDcb->ChildrenList, &newFcb->Links);

                    lastCreated = (PCOMMON_CONTEXT)newFcb;
                }
                else
                {
                    // Last component is a directory, create DCB
                    PDCB newDcb;

                    status = BlorgCreateDCB(&newDcb, BLORGFS_DCB_SIGNATURE, Path, VolumeDeviceObject);

                    if (!NT_SUCCESS(status))
                    {
                        return status;
                    }

                    newDcb->ParentDcb = currentDcb;
                    InsertTailList(&currentDcb->ChildrenList, &newDcb->Links);

                    lastCreated = (PCOMMON_CONTEXT)newDcb;
                }
            }
            else
            {
                // Create directory control block
                PDCB newDcb;
                UNICODE_STRING partialPath =
                {
                    .Length = Path->Length - (remainingPart.Length + sizeof(WCHAR)),
                    .MaximumLength = partialPath.Length,
                    .Buffer = Path->Buffer
                };
                
                status = BlorgCreateDCB(&newDcb, BLORGFS_DCB_SIGNATURE, &partialPath, VolumeDeviceObject);
                
                if (!NT_SUCCESS(status))
                {
                    return status;
                }

                newDcb->ParentDcb = currentDcb;
                InsertTailList(&currentDcb->ChildrenList, &newDcb->Links);

                lastCreated = (PCOMMON_CONTEXT)newDcb;
                childDcb = newDcb;
            }
        }

        if (childDcb)
        {
            currentDcb = childDcb;
        }

        remainingPath = remainingPart;
    }

    *Out = lastCreated;
    return STATUS_SUCCESS;
}