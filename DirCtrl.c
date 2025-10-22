#include "Driver.h"

typedef NTSTATUS(*PFILL_ROUTINE)(
    PVOID Out,
    ULONG RemainingLength,
    ULONG Index,
    const PUNICODE_STRING Name,
    LARGE_INTEGER Creation,
    LARGE_INTEGER Access,
    LARGE_INTEGER Write,
    LARGE_INTEGER FileSize,
    ULONG Attributes,
    BOOLEAN ReturnSingle,
    BOOLEAN IsLast,
    SIZE_T* BytesWritten
    );

static inline ULONG AlignEntrySize(ULONG size)
{
    return (size + 7u) & ~7u;
}

static inline BOOLEAN MatchPattern(const PUNICODE_STRING EntryName, const PUNICODE_STRING SearchPattern, ULONGLONG Flags)
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

    for (ULONG i = 0; i < (SearchPattern->Length / C_CAST(ULONG, sizeof(WCHAR))); i++)
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

static inline NTSTATUS FillFileIdBothDirInfo(
    PFILE_ID_BOTH_DIR_INFORMATION Out,
    ULONG RemainingLength,
    ULONG Index,
    const PUNICODE_STRING Name,
    LARGE_INTEGER CreationTime,
    LARGE_INTEGER LastAccessTime,
    LARGE_INTEGER LastWriteTime,
    LARGE_INTEGER FileSize,
    ULONG FileAttributes,
    BOOLEAN ReturnSingle,
    BOOLEAN IsLast,
    SIZE_T* BytesWritten
)
{
    ULONG rawSize = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) + Name->Length;
    ULONG alignedSize = AlignEntrySize(rawSize);

    if (RemainingLength < alignedSize)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    RtlZeroMemory(Out, alignedSize);

    Out->NextEntryOffset = (ReturnSingle || IsLast) ? 0 : alignedSize;
    Out->FileIndex = Index;
    Out->CreationTime = CreationTime;
    Out->LastAccessTime = LastAccessTime;
    Out->LastWriteTime = LastWriteTime;
    Out->ChangeTime = LastWriteTime;
    Out->EndOfFile = FileSize;
    Out->AllocationSize = FileSize;
    Out->FileAttributes = FileAttributes;
    Out->FileId.QuadPart = 0;
    Out->FileNameLength = Name->Length;
    Out->EaSize = 0;
    Out->ShortNameLength = 0;

    RtlCopyMemory(Out->FileName, Name->Buffer, Name->Length);

    *BytesWritten = alignedSize;
    return STATUS_SUCCESS;
}

static inline NTSTATUS FillFileFullDirInfo(
    PFILE_FULL_DIR_INFORMATION Out,
    ULONG RemainingLength,
    ULONG Index,
    const PUNICODE_STRING Name,
    LARGE_INTEGER CreationTime,
    LARGE_INTEGER LastAccessTime,
    LARGE_INTEGER LastWriteTime,
    LARGE_INTEGER FileSize,
    ULONG FileAttributes,
    BOOLEAN ReturnSingle,
    BOOLEAN IsLast,
    SIZE_T* BytesWritten
)
{
    ULONG rawSize = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) + Name->Length;
    ULONG alignedSize = AlignEntrySize(rawSize);

    if (RemainingLength < alignedSize)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    RtlZeroMemory(Out, alignedSize);

    Out->NextEntryOffset = (ReturnSingle || IsLast) ? 0 : alignedSize;
    Out->FileIndex = Index;
    Out->CreationTime = CreationTime;
    Out->LastAccessTime = LastAccessTime;
    Out->LastWriteTime = LastWriteTime;
    Out->ChangeTime = LastWriteTime;
    Out->EndOfFile = FileSize;
    Out->AllocationSize = FileSize;
    Out->FileAttributes = FileAttributes;
    Out->FileNameLength = Name->Length;
    Out->EaSize = 0;

    RtlCopyMemory(Out->FileName, Name->Buffer, Name->Length);

    *BytesWritten = alignedSize;
    return STATUS_SUCCESS;
}

/**
 * @brief Enumerates directory entries and fills the output buffer with matching entries
 *
 * This function iterates through directory entries (both files and subdirectories),
 * applies pattern matching, and fills the output buffer with formatted directory
 * information for entries that match the search criteria.
 *
 * @param Ccb           Pointer to the Context Control Block containing directory metadata
 *                      and search state. Must contain valid Entries with file/subdir data.
 * @param StartIndex    Zero-based index indicating where to begin enumeration.
 *                      Files are indexed 0 to (FileCount-1), subdirectories are
 *                      indexed from FileCount to (FileCount + SubDirCount - 1).
 * @param TotalEntries  Total number of entries in the directory (FileCount + SubDirCount).
 *                      Used as upper bound for iteration.
 * @param Pattern       Optional Unicode string containing the search pattern.
 *                      May contain wildcards (*, ?, DOS_DOT, DOS_QM, DOS_STAR).
 *                      If NULL and CCB_FLAG_MATCH_ALL is not set, no entries match.
 * @param Flags         Control flags, specifically CCB_FLAG_MATCH_ALL to match all entries
 *                      regardless of pattern.
 * @param ReturnSingle  If TRUE, returns only the first matching entry and stops enumeration.
 *                      Affects NextEntryOffset field in output structures.
 * @param OutBuffer     User or system buffer to receive the formatted directory information.
 *                      Must be properly aligned and have sufficient space.
 * @param OutLength     Size in bytes of the output buffer. Function ensures entries
 *                      don't exceed this limit.
 * @param FillFn        Function pointer to the specific fill routine based on the
 *                      FileInformationClass (e.g., FillFileIdBothDirInfo).
 *                      This allows the same enumeration logic for different info classes.
 * @param BytesUsed     [OUT] Returns the total number of bytes written to OutBuffer.
 *                      Set even on partial success or buffer overflow.
 * @param FinalIndex    [OUT] Returns the index where enumeration stopped.
 *                      On success with more entries available, points to next entry.
 *                      On buffer overflow, points to entry that couldn't fit.
 *
 * @return STATUS_SUCCESS          - At least one matching entry was successfully written
 *         STATUS_NO_MORE_FILES    - No matching entries found (end of enumeration)
 *         STATUS_BUFFER_OVERFLOW  - Buffer too small for next entry (partial success possible)
 *         Other NTSTATUS          - Error from FillFn or invalid parameters
 *
 * @note Thread Safety: Caller must hold appropriate locks on the DCB/CCB structures.
 *       Typically requires shared lock for enumeration, exclusive for initialization.
 *
 * @note Buffer Layout: Entries are written contiguously with proper alignment.
 *       Each entry's NextEntryOffset points to the next entry, except the last
 *       entry which has NextEntryOffset = 0.
 *
 * @note Performance: O(n) where n is the number of entries from StartIndex to either
 *       the first match (if ReturnSingle) or TotalEntries. Pattern matching cost
 *       depends on pattern complexity.
 *
 * @example
 *   SIZE_T bytesUsed = 0;
 *   ULONG nextIndex = 0;
 *   NTSTATUS status = EnumerateDirectoryEntries(
 *       ccb,
 *       0,                          // Start from beginning
 *       totalEntries,
 *       &searchPattern,
 *       ccb->Flags,
 *       FALSE,                      // Return all matching entries
 *       outputBuffer,
 *       bufferLength,
 *       FillFileIdBothDirInfo,
 *       &bytesUsed,
 *       &nextIndex
 *   );
 *
 * @see MatchPattern, FillFileIdBothDirInfo, FillFileFullDirInfo
 */

static NTSTATUS EnumerateDirectoryEntries(
    const PCCB Ccb,
    ULONG StartIndex,
    ULONG TotalEntries,
    const PUNICODE_STRING Pattern,
    ULONGLONG Flags,
    BOOLEAN ReturnSingle,
    PVOID OutBuffer,
    ULONG OutLength,
    PFILL_ROUTINE FillFn,
    SIZE_T* BytesUsed,
    ULONG* FinalIndex
)
{
    ULONG index = StartIndex;
    ULONG remaining = OutLength;
    PUCHAR cursor = C_CAST(PUCHAR, OutBuffer);
    SIZE_T totalWritten = 0;
    BOOLEAN found = FALSE;

    while (index < TotalEntries)
    {
        BOOLEAN isDirectory = index >= Ccb->Entries->FileCount;
        UNICODE_STRING name;
        LARGE_INTEGER creation = { 0 }, access = { 0 }, write = { 0 }, size = { 0 };
        ULONG attrs = 0;

        if (isDirectory)
        {
            PDIRECTORY_SUBDIR_METADATA sub = GetSubDirEntry(Ccb->Entries, index - Ccb->Entries->FileCount);

            if (!sub)
            {
                break;
            }

            RtlInitUnicodeString(&name, sub->Name);
            creation.QuadPart = sub->CreationTime;
            access.QuadPart = sub->LastAccessedTime;
            write.QuadPart = sub->LastModifiedTime;
            attrs = FILE_ATTRIBUTE_DIRECTORY;
        }
        else
        {
            PDIRECTORY_FILE_METADATA file = GetFileEntry(Ccb->Entries, index);

            if (!file)
            {
                break;
            }

            RtlInitUnicodeString(&name, file->Name);
            creation.QuadPart = file->CreationTime;
            access.QuadPart = file->LastAccessedTime;
            write.QuadPart = file->LastModifiedTime;
            size.QuadPart = file->Size;
            attrs = FILE_ATTRIBUTE_NORMAL;
        }

        if (MatchPattern(&name, Pattern, Flags))
        {
            SIZE_T written = 0;
            NTSTATUS st = FillFn(
                cursor,
                remaining,
                index,
                &name,
                creation,
                access,
                write,
                size,
                attrs,
                ReturnSingle,
                (index == TotalEntries - 1), // IsLast flag for NextEntryOffset
                &written
            );

            if (!NT_SUCCESS(st))
            {
                //
                // Buffer overflow or other error - return what we've written so far
                //
                *BytesUsed = totalWritten;
                *FinalIndex = index;
                return st;
            }

            cursor += written;
            remaining -= C_CAST(ULONG, written);
            totalWritten += written;
            found = TRUE;

            index++;

            if (ReturnSingle)
            {
                // Only wanted one entry - stop here
                break;
            }
        }
        else
        {
            // Entry doesn't match pattern - skip to next
            index++;
        }
    }

    *BytesUsed = totalWritten;
    *FinalIndex = index;
    return found ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
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

            BOOLEAN restartScan = FlagOn(IrpSp->Flags, SL_RESTART_SCAN);
            BOOLEAN returnSingleEntry = FlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);
            BOOLEAN indexSpecified = FlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED);

            BOOLEAN initialQuery = !ccb->SearchPattern.Buffer &&
                !FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL);

            if (initialQuery)
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return FsdPostRequest(Irp, IrpSp);
                }

                //
                // Protect against race condition where CCB has been modified in the time window before being locked by another thread
                //

                if (ccb->SearchPattern.Buffer || FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL))
                {
                    initialQuery = FALSE;
                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }
            }
            else if (restartScan)
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return FsdPostRequest(Irp, IrpSp);
                }

                ccb->CurrentIndex = 0;
            }
            else
            {
                if (!ExAcquireResourceSharedLite(dcb->Header.Resource, BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_WAIT)))
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
                    if (!BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_IN_FSP))
                    {
                        BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return FsdPostRequest(Irp, IrpSp);
                    }

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

                    if ((sizeof(WCHAR) == ccb->SearchPattern.Length) && (L'*' == ccb->SearchPattern.Buffer[0]))
                    {
                        SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);
                    }

                    FreeHttpDirectoryInfo(ccb->Entries);

                    result = GetHttpDirectoryInfo(&dcb->FullPath, &ccb->Entries);

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
                    if (!BooleanFlagOn(C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]), IRP_CONTEXT_FLAG_IN_FSP))
                    {
                        BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return FsdPostRequest(Irp, IrpSp);
                    }

                    RtlZeroMemory(&ccb->Flags, sizeof(ULONGLONG));

                    if (ccb->SearchPattern.Buffer)
                    {
                        RtlFreeUnicodeString(&ccb->SearchPattern);
                        RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
                    }

                    SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);

                    FreeHttpDirectoryInfo(ccb->Entries);

                    result = GetHttpDirectoryInfo(&dcb->FullPath, &ccb->Entries);

                    if (!NT_SUCCESS(result))
                    {
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return result;
                    }

                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }
            }

            ULONG remainingLength = IrpSp->Parameters.QueryDirectory.Length;
            BOOLEAN updateCcb = FALSE;
            ULONG index = (indexSpecified) ? IrpSp->Parameters.QueryDirectory.FileIndex : C_CAST(ULONG, ccb->CurrentIndex);

            ULONG totalEntries = C_CAST(ULONG, ccb->Entries->FileCount + ccb->Entries->SubDirCount);

            __try
            {
                switch (IrpSp->Parameters.QueryDirectory.FileInformationClass)
                {
                    case FileIdBothDirectoryInformation:
                    {
                        PFILE_ID_BOTH_DIR_INFORMATION dirInfo = (!Irp->MdlAddress) ?
                            Irp->UserBuffer :
                            MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                        if (!dirInfo)
                        {
                            result = STATUS_INSUFFICIENT_RESOURCES;
                            break;
                        }

                        if (!Irp->MdlAddress && UserMode == Irp->RequestorMode)
                        {
                            ProbeForRead(Irp->UserBuffer, IrpSp->Parameters.QueryDirectory.Length, sizeof(UCHAR));
                        }

                        SIZE_T used = 0;
                        result = EnumerateDirectoryEntries(
                            ccb,
                            index,
                            totalEntries,
                            &ccb->SearchPattern,
                            ccb->Flags,
                            returnSingleEntry,
                            dirInfo,
                            remainingLength,
                            FillFileIdBothDirInfo,
                            &used,
                            &index
                        );

                        if (NT_SUCCESS(result))
                        {
                            Irp->IoStatus.Information = used;
                        }

                        updateCcb = !indexSpecified;
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

                        SIZE_T used = 0;
                        result = EnumerateDirectoryEntries(
                            ccb,
                            index,
                            totalEntries,
                            &ccb->SearchPattern,
                            ccb->Flags,
                            returnSingleEntry,
                            dirInfo,
                            remainingLength,
                            FillFileFullDirInfo,
                            &used,
                            &index
                        );

                        if (NT_SUCCESS(result))
                        {
                            Irp->IoStatus.Information = used;
                        }

                        updateCcb = !indexSpecified;
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
            __except (EXCEPTION_EXECUTE_HANDLER)
            {

                //
                //  We had a problem filling in the user's buffer, so stop and
                //  fail this request.  This is the only reason any exception
                //  would have occured at this level.
                //

                updateCcb = FALSE;
                result = GetExceptionCode();
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