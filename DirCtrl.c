#include "Driver.h"

static BOOLEAN MatchPattern(PUNICODE_STRING EntryName, PUNICODE_STRING SearchPattern, ULONGLONG Flags)
{
    // If we're matching all files, return TRUE immediately
    if (FlagOn(Flags, CCB_FLAG_MATCH_ALL))
    {
        return TRUE;
    }

    // If there's no search pattern, we can't match anything
    if (!SearchPattern || !SearchPattern->Buffer || !SearchPattern->Length)
    {
        return FALSE;
    }

    // Check if the search pattern contains wildcards
    BOOLEAN containsWildCards = FALSE;

    for (ULONG i = 0; i < (SearchPattern->Length / (ULONG)sizeof(WCHAR)); i++)
    {
        WCHAR ch = SearchPattern->Buffer[i];

        if (ch == L'*' || ch == L'?' || ch == DOS_DOT || ch == DOS_QM || ch == DOS_STAR)
        {
            containsWildCards = TRUE;
            break;
        }
    }

    BOOLEAN match = FALSE;

    if (containsWildCards)
    {
        __try
        {
            match = FsRtlIsNameInExpression(
                SearchPattern,
                EntryName,
                TRUE,
                NULL
            );
        } 
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            // In low resource conditions, FsRtlIsNameInExpression can raise a structured exception with a code of STATUS_NO_MEMORY
            return FALSE;
        }

    }
    else
    {
        match = FsRtlAreNamesEqual(
            SearchPattern,
            EntryName,
            TRUE,
            NULL
        );
    }

    return match;
}

NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    switch (IrpSp->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
        {
            BLORGFS_PRINT("BlorgVolumeDirectoryControl...\n");
            BLORGFS_PRINT(" Irp                    = %p\n", Irp);
            BLORGFS_PRINT(" ->Length               = %08lx\n", IrpSp->Parameters.QueryDirectory.Length);
            BLORGFS_PRINT(" ->FileName             = %wZ\n", IrpSp->Parameters.QueryDirectory.FileName);
            BLORGFS_PRINT(" ->FileInformationClass = %08lx\n", IrpSp->Parameters.QueryDirectory.FileInformationClass);
            BLORGFS_PRINT(" ->FileIndex            = %08lx\n", IrpSp->Parameters.QueryDirectory.FileIndex);
            BLORGFS_PRINT(" ->UserBuffer           = %p\n", Irp->AssociatedIrp.SystemBuffer);
            BLORGFS_PRINT(" ->RequestorMode        = %lu\n", Irp->RequestorMode);
            BLORGFS_PRINT(" ->RestartScan          = %08lx\n", FlagOn(IrpSp->Flags, SL_RESTART_SCAN));
            BLORGFS_PRINT(" ->ReturnSingleEntry    = %08lx\n", FlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY));
            BLORGFS_PRINT(" ->IndexSpecified       = %08lx\n", FlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED));

            PDCB dcb = IrpSp->FileObject->FsContext;

            switch GET_NODE_TYPE(dcb)
            {
                case BLORGFS_DCB_SIGNATURE:
                {
                    break;
                }
                case BLORGFS_ROOT_DCB_SIGNATURE:
                {
                    break;
                }
                default:
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Invalid node type\n");
                    return STATUS_INVALID_PARAMETER;
                }
            }

            PCCB ccb = IrpSp->FileObject->FsContext2;

            if (!ccb)
            {
                return STATUS_INVALID_PARAMETER;
            }

            SIZE_T bytesWritten = 0;
            BOOLEAN restartScan = FlagOn(IrpSp->Flags, SL_RESTART_SCAN);
            BOOLEAN returnSingleEntry = FlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);
            BOOLEAN indexSpecified = FlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED);

            BOOLEAN initialQuery = (BOOLEAN)((!ccb->SearchPattern.Buffer) &&
                !FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL));

            if (initialQuery || restartScan)
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn((ULONG_PTR)Irp->Tail.Overlay.DriverContext[0], IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return FsdPostRequest(Irp, IrpSp);
                }

                //
                // Protect against race condition where CCB has been modified in the time window before being locked by another thread
                //

                if (!restartScan && (ccb->SearchPattern.Buffer || FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL)))
                {
                    initialQuery = FALSE;
                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }

                ccb->CurrentIndex = 0;
            }
            else
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn((ULONG_PTR)Irp->Tail.Overlay.DriverContext[0], IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return FsdPostRequest(Irp, IrpSp);
                }
            }

            if ((IrpSp->Parameters.QueryDirectory.FileName) && (IrpSp->Parameters.QueryDirectory.FileName->Buffer) && (0 < IrpSp->Parameters.QueryDirectory.FileName->Length))
            {
                //
                // If we're restarting the scan, clear out the pattern in the Ccb and regenerate it.
                //

                if (initialQuery || restartScan)
                {
                    RtlZeroMemory(&ccb->Flags, sizeof(ULONGLONG));

                    if (ccb->SearchPattern.Buffer)
                    {
                        RtlFreeUnicodeString(&ccb->SearchPattern);
                    }

                    result = RtlUpcaseUnicodeString(&ccb->SearchPattern, IrpSp->Parameters.QueryDirectory.FileName, TRUE);

                    if (!NT_SUCCESS(result))
                    {
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return result;
                    }

                    if ((sizeof(WCHAR) == ccb->SearchPattern.Length) &&  (L'*' == ccb->SearchPattern.Buffer[0]))
                    {
                        SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);
                    }

                    FreeHttpDirectoryInfo(&ccb->SubDirectories, &ccb->Files);

                    result = GetHttpDirectoryInfo(&dcb->FullPath, &ccb->SubDirectories, &ccb->Files);

                    if (!NT_SUCCESS(result))
                    {
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return result;
                    }

                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }
            }
            else
            {
                if (initialQuery || restartScan)
                {
                    RtlZeroMemory(&ccb->Flags, sizeof(ULONGLONG));

                    if (ccb->SearchPattern.Buffer)
                    {
                        RtlFreeUnicodeString(&ccb->SearchPattern);
                        RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
                    }

                    SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);

                    FreeHttpDirectoryInfo(&ccb->SubDirectories, &ccb->Files);

                    result = GetHttpDirectoryInfo(&dcb->FullPath, &ccb->SubDirectories, &ccb->Files);

                    if (!NT_SUCCESS(result))
                    {
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return result;
                    }

                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }
            }

            ULONG remainingLength = IrpSp->Parameters.QueryDirectory.Length;
            ULONG entryOffset = 0;
            BOOLEAN moreEntries = TRUE;
            BOOLEAN foundEntry = FALSE;
            BOOLEAN updateCcb = FALSE;
            ULONG index = (indexSpecified) ? IrpSp->Parameters.QueryDirectory.FileIndex : (ULONG)ccb->CurrentIndex;

            ULONG totalEntries = (ULONG)(ccb->SubDirectories.EntryCount + ccb->Files.EntryCount);

            __try
            {
                switch (IrpSp->Parameters.QueryDirectory.FileInformationClass)
                {
                    case FileIdBothDirectoryInformation:
                    {

#pragma message ("doesn't sit right fastfat doesn't probe here in a synchronous request i wonder if i can create POC attack")
                        PFILE_ID_BOTH_DIR_INFORMATION dirInfo = (!Irp->MdlAddress) ? Irp->UserBuffer : MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                        if (!dirInfo)
                        {
                            result = STATUS_INSUFFICIENT_RESOURCES;
                            break;
                        }

                        if (!Irp->MdlAddress && UserMode == Irp->RequestorMode)
                        {
                            ProbeForRead(Irp->UserBuffer, IrpSp->Parameters.QueryDirectory.Length, sizeof(UCHAR));
                        }

                        PFILE_ID_BOTH_DIR_INFORMATION prevDirInfo = NULL;

                        while (index < totalEntries && moreEntries)
                        {
                            BOOLEAN isDirectory = index < ccb->SubDirectories.EntryCount;
                            ULONG entryIndex = isDirectory ? index : index - (ULONG)ccb->SubDirectories.EntryCount;
                            PUNICODE_STRING entryName = isDirectory ?
                                &ccb->SubDirectories.Entries[entryIndex].Name :
                                &ccb->Files.Entries[entryIndex].Name;

                            if (MatchPattern(entryName, &ccb->SearchPattern, ccb->Flags))
                            {
                                ULONG entrySize = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName[0]) + entryName->Length;

                                if (remainingLength < entrySize)
                                {
                                    moreEntries = FALSE;
                                    break;
                                }

                                // Fill in entry information
                                RtlZeroMemory(dirInfo, entrySize);
                                dirInfo->NextEntryOffset = (returnSingleEntry || (index == totalEntries - 1)) ? 0 : entrySize;
                                dirInfo->FileIndex = index;

                                if (isDirectory)
                                {
                                    dirInfo->CreationTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].CreationTime;
                                    dirInfo->LastAccessTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastAccessedTime;
                                    dirInfo->LastWriteTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->ChangeTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->EndOfFile.QuadPart = ccb->SubDirectories.Entries[entryIndex].Size;
                                    dirInfo->AllocationSize.QuadPart = ccb->SubDirectories.Entries[entryIndex].Size;
                                    dirInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                                    dirInfo->FileId.QuadPart = 0;
                                }
                                else
                                {
                                    dirInfo->CreationTime.QuadPart = ccb->Files.Entries[entryIndex].CreationTime;
                                    dirInfo->LastAccessTime.QuadPart = ccb->Files.Entries[entryIndex].LastAccessedTime;
                                    dirInfo->LastWriteTime.QuadPart = ccb->Files.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->ChangeTime.QuadPart = ccb->Files.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->EndOfFile.QuadPart = ccb->Files.Entries[entryIndex].Size;
                                    dirInfo->AllocationSize.QuadPart = ccb->Files.Entries[entryIndex].Size;
                                    dirInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;
                                    dirInfo->FileId.QuadPart = 0;
                                }

                                dirInfo->FileNameLength = entryName->Length;
                                dirInfo->EaSize = 0;
                                dirInfo->ShortNameLength = 0;

                                RtlCopyMemory(dirInfo->FileName, entryName->Buffer, entryName->Length);

                                prevDirInfo = dirInfo;
                                dirInfo = (PFILE_ID_BOTH_DIR_INFORMATION)((PUCHAR)dirInfo + entrySize);
                                remainingLength -= entrySize;
                                entryOffset += entrySize;
                                bytesWritten += entrySize;
                                foundEntry = TRUE;

                                index++;

                                if (returnSingleEntry)
                                {
                                    break;
                                }
                            }
                            else
                            {
                                index++;
                            }

                            updateCcb = TRUE;
                        }

                        if (prevDirInfo)
                        {
                            prevDirInfo->NextEntryOffset = 0;
                        }

                        result = foundEntry ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
                        break;
                    }
                    case FileDirectoryInformation:
                    {
                        result = STATUS_NOT_IMPLEMENTED;
                        break;
                    }
                    case FileFullDirectoryInformation:
                    {
                        PFILE_FULL_DIR_INFORMATION dirInfo = (!Irp->MdlAddress) ? Irp->UserBuffer : MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                        if (!dirInfo)
                        {
                            result = STATUS_INSUFFICIENT_RESOURCES;
                            break;
                        }

                        if (!Irp->MdlAddress && UserMode == Irp->RequestorMode)
                        {
                            ProbeForRead(Irp->UserBuffer, IrpSp->Parameters.QueryDirectory.Length, sizeof(UCHAR));
                        }

                        PFILE_FULL_DIR_INFORMATION prevDirInfo = NULL;

                        while (index < totalEntries && moreEntries)
                        {
                            BOOLEAN isDirectory = index < ccb->SubDirectories.EntryCount;
                            ULONG entryIndex = isDirectory ? index : index - (ULONG)ccb->SubDirectories.EntryCount;
                            PUNICODE_STRING entryName = isDirectory ?
                                &ccb->SubDirectories.Entries[entryIndex].Name :
                                &ccb->Files.Entries[entryIndex].Name;

                            if (MatchPattern(entryName, &ccb->SearchPattern, ccb->Flags))
                            {
                                ULONG entrySize = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName[0]) + entryName->Length;

                                if (remainingLength < entrySize)
                                {
                                    moreEntries = FALSE;
                                    break;
                                }

                                // Fill in entry information
                                RtlZeroMemory(dirInfo, entrySize);
                                dirInfo->NextEntryOffset = (returnSingleEntry || (index == totalEntries - 1)) ? 0 : entrySize;
                                dirInfo->FileIndex = index;

                                if (isDirectory)
                                {
                                    dirInfo->CreationTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].CreationTime;
                                    dirInfo->LastAccessTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastAccessedTime;
                                    dirInfo->LastWriteTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->ChangeTime.QuadPart = ccb->SubDirectories.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->EndOfFile.QuadPart = ccb->SubDirectories.Entries[entryIndex].Size;
                                    dirInfo->AllocationSize.QuadPart = ccb->SubDirectories.Entries[entryIndex].Size;
                                    dirInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                                }
                                else
                                {
                                    dirInfo->CreationTime.QuadPart = ccb->Files.Entries[entryIndex].CreationTime;
                                    dirInfo->LastAccessTime.QuadPart = ccb->Files.Entries[entryIndex].LastAccessedTime;
                                    dirInfo->LastWriteTime.QuadPart = ccb->Files.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->ChangeTime.QuadPart = ccb->Files.Entries[entryIndex].LastModifiedTime;
                                    dirInfo->EndOfFile.QuadPart = ccb->Files.Entries[entryIndex].Size;
                                    dirInfo->AllocationSize.QuadPart = ccb->Files.Entries[entryIndex].Size;
                                    dirInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;
                                }

                                dirInfo->FileNameLength = entryName->Length;
                                dirInfo->EaSize = 0;

                                RtlCopyMemory(dirInfo->FileName, entryName->Buffer, entryName->Length);

                                prevDirInfo = dirInfo;
                                dirInfo = (PFILE_FULL_DIR_INFORMATION)((PUCHAR)dirInfo + entrySize);
                                remainingLength -= entrySize;
                                entryOffset += entrySize;
                                bytesWritten += entrySize;
                                foundEntry = TRUE;

                                index++;

                                if (returnSingleEntry)
                                {
                                    break;
                                }
                            }
                            else
                            {
                                index++;
                            }

                            updateCcb = TRUE;
                        }

                        if (prevDirInfo)
                        {
                            prevDirInfo->NextEntryOffset = 0;
                        }

                        result = foundEntry ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
                        break;
                    }
                    case FileIdFullDirectoryInformation:
                    {
                        result = STATUS_NOT_IMPLEMENTED;
                        break;
                    }
                    case FileNamesInformation:
                    {
                        result = STATUS_NOT_IMPLEMENTED;
                        break;
                    }
                    case FileBothDirectoryInformation:
                    {
                        result = STATUS_NOT_IMPLEMENTED;
                        break;
                    }
                    default:
                    {
                        result = STATUS_INVALID_INFO_CLASS;
                    }
                }
             }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {

                //
                //  We had a problem filling in the user's buffer, so stop and
                //  fail this request.  This is the only reason any exception
                //  would have occured at this level.
                //

                updateCcb = FALSE;
                result = GetExceptionCode();
            }
            
            if (NT_SUCCESS(result))
            {
                Irp->IoStatus.Information = bytesWritten;
            }

            ExReleaseResourceLite(dcb->Header.Resource);
            
            if (updateCcb)
            {
                ccb->CurrentIndex = index;
            }
            
            break;
        }
        case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
        {
            result = STATUS_NOT_IMPLEMENTED;
            break;
        }
        default:
        {
            result = STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    return result;
}

NTSTATUS BlorgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = IsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (GetDeviceExtensionMagic(DeviceObject))
    {
        case BLORGFS_VDO_MAGIC:
        {
            BlorgSetupIrpContext(Irp, IoIsOperationSynchronous(Irp));

            result = BlorgVolumeDirectoryControl(Irp, irpSp);
            if (STATUS_PENDING != result)
            {
                CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BLORGFS_DDO_MAGIC:
        {
            // result = BlorgDiskDirectoryControl(pIrp);
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
        case BLORGFS_FSDO_MAGIC:
        {
            CompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
    }
    FsRtlExitFileSystem();

    if (topLevel)
    {
        IoSetTopLevelIrp(NULL);
    }

    return result;
}