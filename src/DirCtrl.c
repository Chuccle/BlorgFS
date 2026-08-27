#include "Driver.h"

//
// Directory control (query directory / notify) IRP handling: pattern
// matching, filling FILE_*_DIR_INFORMATION output buffers, the async
// directory-listing fetch and its completion, and directory-change
// notification registration.
//

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

//
// Rounds a directory-entry size up to the next 8-byte boundary, as
// required for NextEntryOffset alignment in FILE_*_DIR_INFORMATION buffers.
//
static inline ULONG AlignEntrySize(ULONG size)
{
    return (size + 7u) & ~7u;
}

//
// Shared by all three Fill*DirInfo routines below -- they fill three
// distinct Windows FILE_*_DIR_INFORMATION struct types (no common base
// type to write generic code against in C), but all three lay out the
// same eleven common fields identically, differing only in 1-2 extra
// fields (FileId, ShortNameLength) and which struct type Out points to.
// A macro keeps the duplication out of the source without losing each
// function's concrete pointer type (a shared PVOID-taking function would
// give up that type safety). Field write order doesn't matter -- none
// of these depend on another already being set -- so this is free to
// group them together regardless of each struct's own field order.
//
#define FILL_DIR_INFO_COMMON_FIELDS(Out, AlignedSize, Index, CreationTime, LastAccessTime, LastWriteTime, FileSize, FileAttributes, ReturnSingle, IsLast) \
    do { \
        RtlZeroMemory((Out), (AlignedSize)); \
        (Out)->NextEntryOffset = ((ReturnSingle) || (IsLast)) ? 0 : (AlignedSize); \
        (Out)->FileIndex = (Index); \
        (Out)->CreationTime = (CreationTime); \
        (Out)->LastAccessTime = (LastAccessTime); \
        (Out)->LastWriteTime = (LastWriteTime); \
        (Out)->ChangeTime = (LastWriteTime); \
        (Out)->EndOfFile = (FileSize); \
        (Out)->AllocationSize = (FileSize); \
        (Out)->FileAttributes = (FileAttributes); \
        (Out)->EaSize = 0; \
    } while (0)

//
// Tests whether EntryName satisfies the query's search criteria: always
// true under CCB_FLAG_MATCH_ALL, false with no pattern, else wildcard
// matching (FsRtlIsNameInExpression) or exact comparison depending on
// whether SearchPattern contains wildcard/DOS characters. Wrapped in a
// __try since FsRtlIsNameInExpression can raise STATUS_NO_MEMORY under
// low resources.
//
static inline BOOLEAN MatchPattern(const PUNICODE_STRING EntryName, const PUNICODE_STRING SearchPattern, ULONGLONG Flags)
{
    if (FlagOn(Flags, CCB_FLAG_MATCH_ALL))
    {
        return TRUE;
    }

    if (!SearchPattern || !SearchPattern->Buffer || !SearchPattern->Length)
    {
        return FALSE;
    }

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

//
// Out/Name are not restrict-qualified even though every call site passes
// disjoint objects: these functions are called indirectly through the
// PFILL_ROUTINE function pointer above, and MSVC's function-pointer-type
// compatibility check (C4113, /WX-fatal in this project) treats a
// restrict-qualified parameter as incompatible with PFILL_ROUTINE's
// unqualified one.
//
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

    FILL_DIR_INFO_COMMON_FIELDS(Out, alignedSize, Index, CreationTime, LastAccessTime, LastWriteTime, FileSize, FileAttributes, ReturnSingle, IsLast);

    Out->FileId.QuadPart = 0;
    Out->FileNameLength = Name->Length;
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

    FILL_DIR_INFO_COMMON_FIELDS(Out, alignedSize, Index, CreationTime, LastAccessTime, LastWriteTime, FileSize, FileAttributes, ReturnSingle, IsLast);

    Out->FileNameLength = Name->Length;

    RtlCopyMemory(Out->FileName, Name->Buffer, Name->Length);

    *BytesWritten = alignedSize;
    return STATUS_SUCCESS;
}

static inline NTSTATUS FillFileBothDirInfo(
    PFILE_BOTH_DIR_INFORMATION Out,
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
    ULONG rawSize = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + Name->Length;
    ULONG alignedSize = AlignEntrySize(rawSize);

    if (RemainingLength < alignedSize)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    FILL_DIR_INFO_COMMON_FIELDS(Out, alignedSize, Index, CreationTime, LastAccessTime, LastWriteTime, FileSize, FileAttributes, ReturnSingle, IsLast);

    Out->FileNameLength = Name->Length;
    Out->ShortNameLength = 0;

    RtlCopyMemory(Out->FileName, Name->Buffer, Name->Length);

    *BytesWritten = alignedSize;
    return STATUS_SUCCESS;
}

//
// Iterates directory entries from StartIndex to TotalEntries (files
// indexed 0..FileCount-1, subdirs FileCount..FileCount+SubDirCount-1),
// applies pattern matching, and fills OutBuffer via FillFn with entries
// that match. Stops on ReturnSingle, on running out of entries, or when
// an entry doesn't fit OutLength. BytesUsed and FinalIndex are set on
// every return; FinalIndex points to where the next query should resume.
// Caller must hold the DCB/CCB lock appropriate for the access (shared
// for enumeration, exclusive for initialization).
//
// Returns STATUS_SUCCESS if at least one entry was written,
// STATUS_NO_MORE_FILES if none matched, STATUS_BUFFER_OVERFLOW if the
// first candidate entry didn't fit, or an error from FillFn. On a fill
// error after at least one entry already fit, that is a normal partial
// result (STATUS_SUCCESS, not the error), since returning
// STATUS_BUFFER_OVERFLOW after a successful partial fill is what
// surfaces as the ERROR_MORE_DATA popup in Explorer; only an error on
// the very first candidate is propagated. NextEntryOffset (the first
// ULONG of every FILE_*_DIR_INFORMATION) is zeroed on the entry actually
// written last regardless of why enumeration stopped, since a caller
// walking that chain would otherwise read past the last written entry
// into an unwritten slot.
//
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
    PUCHAR lastEntry = NULL;
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
            PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(Ccb->Entries, index - Ccb->Entries->FileCount);

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
            PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(Ccb->Entries, index);

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
                (index == TotalEntries - 1),
                &written
            );

            if (!NT_SUCCESS(st))
            {
                if (!found)
                {
                    *BytesUsed = 0;
                    *FinalIndex = index;
                    return st;
                }

                break;
            }

            lastEntry = cursor;

            cursor += written;
            remaining -= C_CAST(ULONG, written);
            totalWritten += written;
            found = TRUE;

            index++;

            if (ReturnSingle)
            {
                break;
            }
        }
        else
        {
            index++;
        }
    }

    if (lastEntry)
    {
        *C_CAST(PULONG, lastEntry) = 0;
    }

    *BytesUsed = totalWritten;
    *FinalIndex = index;
    return found ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
}

//
//  Completion for the async directory-listing fetch issued by
//  BlorgVolumeDirectoryControl. Runs on the WSK completion path at
//  <= DISPATCH_LEVEL: it takes ownership of the deserialized DIRECTORY_INFO
//  (NonPagedPoolNx, so reachable here), stores it on the CCB, and re-queues
//  the IRP with NET_DONE set so the PASSIVE_LEVEL enumeration runs on an FSP
//  thread. CallerContext is the PIRP.
//
//  DirInfo is published as the DCB's cached listing, shared by every
//  handle to this directory and freed only when the last handle closes.
//  The publish is a release write (WritePointerRelease): BlorgVolumeCreate
//  reads the pointer holding only the VCB resource, not this DCB's, so
//  the release/acquire pair -- not a common lock -- is what makes the
//  listing's contents visible before the pointer on weakly-ordered
//  architectures (ARM64). The pointer is write-once, NULL -> non-NULL,
//  never replaced until the DCB itself is freed (see DCB.CachedListing).
//  If a concurrent query on another handle already cached a listing
//  while the resource was released across this async fetch, the
//  existing one is kept and this duplicate freed. This runs at
//  PASSIVE_LEVEL, so the ERESOURCE is legal; it is wrapped in a critical
//  region since a system worker thread does not disable APCs the way an
//  FSP thread does.
//
//  A freshly fetched listing is authoritative for its subtree, so on an
//  actual publish (not a duplicate) the path cache beneath dcb->FullPath
//  is invalidated: a stale not-found memoized before the file appeared
//  on the backend would otherwise shadow the new listing until its TTL
//  lapses, since Create consults the path cache before the listing.
//  Re-resolution re-seeds it locally with no network I/O.
//
//  If BlorgFsdRequeueRequest fails (FSP threads tearing down), the listing
//  already belongs to the DCB cache (freed at DCB teardown), so the
//  query is simply failed.
//
static VOID BlorgDirComplete(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext)
{
    PIRP irp = CallerContext;

    if (!NT_SUCCESS(Status))
    {
        BlorgCompleteRequest(irp, Status, IO_DISK_INCREMENT);
        return;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);
    PDCB dcb = irpSp->FileObject->FsContext;
    PCCB ccb = irpSp->FileObject->FsContext2;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(dcb->Header.Resource, TRUE);

    BOOLEAN published = FALSE;

    if (!dcb->CachedListing)
    {
        WritePointerRelease(C_CAST(PVOID volatile*, &dcb->CachedListing), DirInfo);
        published = TRUE;
    }
    else
    {
        BlorgFreeHttpDirectoryInfo(DirInfo);
    }

    ccb->Entries = dcb->CachedListing;

    ExReleaseResourceLite(dcb->Header.Resource);
    KeLeaveCriticalRegion();

    if (published)
    {
        BlorgPathCacheInvalidatePrefix(&dcb->FullPath);
    }

    BlorgSetIrpContextFlag(irp, IRP_CONTEXT_FLAG_NET_DONE);

    NTSTATUS requeue = BlorgFsdRequeueRequest(irp);

    if (STATUS_PENDING != requeue)
    {
        BlorgCompleteRequest(irp, requeue, IO_DISK_INCREMENT);
    }
}

//
// Handles IRP_MJ_DIRECTORY_CONTROL for the volume device: QUERY_DIRECTORY
// (acquire DCB resource per scan-restart/pattern-change rules, fetch the
// directory listing over HTTP on a cache miss and re-enter via NET_DONE
// once cached, then enumerate into the caller's FILE_*_DIR_INFORMATION
// buffer) and NOTIFY_CHANGE_DIRECTORY (register with the FsRtl notify
// package; this volume is read-only so notifications are never fired,
// only completed at handle cleanup).
//
// On the NET_DONE second pass, BlorgDirComplete has already cached the
// listing on the DCB, so the fetch is skipped and enumeration runs
// directly; queries on other handles to the same directory reuse that
// cache too. The CCB is re-checked for a pattern/MATCH_ALL after
// acquiring the resource exclusive, since it could have been set by
// another thread in the window before the lock was taken. On a restart
// scan or initial query, the CCB's search pattern is cleared and
// regenerated from the query's FileName (or CCB_FLAG_MATCH_ALL if none
// given).
//
// The HTTP fetch is only issued on a cache miss (no dcb->CachedListing
// yet); a listing already cached (by a prior query on any handle to
// this directory) is reused directly with no round trip and no NET_DONE
// second pass. The ERESOURCE cannot be held across the async completion
// (it runs on a different thread), so it is released before issuing;
// BlorgDirComplete caches the listing on the DCB and re-queues this IRP
// with NET_DONE set. The fetch depends only on dcb->FullPath, so it is
// hoisted out of both pattern branches above. ccb->Entries then points
// at the DCB's shared cached listing (populated by an earlier query on
// a hit, or by BlorgDirComplete on the NET_DONE pass); if still NULL,
// there is nothing to enumerate and it is never dereferenced.
//
// The fetch-issuing check below is gated on !dcb->CachedListing alone,
// not also on (initialQuery || restartScan): a second QUERY_DIRECTORY on
// the same handle, arriving after the first has set the pattern but
// before that first fetch has completed, is neither an initial query nor
// a restart -- ccb->SearchPattern is already set -- so gating on those
// used to fall through to "no listing, therefore no more files", which
// is wrong; NULL only ever means "not fetched yet", never "empty" (an
// empty directory still publishes a real zero-count DIRECTORY_INFO).
// Issuing a second fetch here in that race is redundant but not unsafe:
// BlorgDirComplete already discards whichever of two racing fetches
// loses the publish (see its own comment), the same protection this
// leans on for two different handles racing the same DCB.
//
// NOTIFY_CHANGE_DIRECTORY registers the watch with the FsRtl notify
// package, which captures its own copy of the directory name and holds
// the IRP pending. This volume is read-only and never changes, so
// FsRtlNotifyFullReportChange is never called -- the IRP simply waits
// until the handle is cleaned up (FsRtlNotifyCleanup in
// BlorgVolumeCleanup completes it). The name is only used by the
// package to match reported changes; since none are ever reported, its
// exact encoding is immaterial. The package marks the IRP pending
// itself, so this function returns STATUS_PENDING and must not touch
// the IRP afterward.
//
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

            ULONG_PTR irpFlags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);
            BOOLEAN netDone = BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_NET_DONE);

            BOOLEAN initialQuery = !ccb->SearchPattern.Buffer &&
                !FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL);

            if (initialQuery)
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return BlorgFsdPostRequest(Irp, IrpSp);
                }

                if (ccb->SearchPattern.Buffer || FlagOn(ccb->Flags, CCB_FLAG_MATCH_ALL))
                {
                    initialQuery = FALSE;
                    ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                }
            }
            else if (restartScan)
            {
                if (!ExAcquireResourceExclusiveLite(dcb->Header.Resource, BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return BlorgFsdPostRequest(Irp, IrpSp);
                }

                ccb->CurrentIndex = 0;
            }
            else
            {
                if (!ExAcquireResourceSharedLite(dcb->Header.Resource, BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_WAIT)))
                {
                    BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                    return BlorgFsdPostRequest(Irp, IrpSp);
                }
            }

            if ((IrpSp->Parameters.QueryDirectory.FileName) && (IrpSp->Parameters.QueryDirectory.FileName->Buffer) && (0 < IrpSp->Parameters.QueryDirectory.FileName->Length))
            {
                if ((initialQuery || restartScan) && !netDone)
                {
                    if (!BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_IN_FSP))
                    {
                        BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return BlorgFsdPostRequest(Irp, IrpSp);
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
                }
            }
            else
            {
                if ((initialQuery || restartScan) && !netDone)
                {
                    if (!BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_IN_FSP))
                    {
                        BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                        ExReleaseResourceLite(dcb->Header.Resource);
                        return BlorgFsdPostRequest(Irp, IrpSp);
                    }

                    RtlZeroMemory(&ccb->Flags, sizeof(ULONGLONG));

                    if (ccb->SearchPattern.Buffer)
                    {
                        RtlFreeUnicodeString(&ccb->SearchPattern);
                        RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
                    }

                    SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);
                }
            }

            if (!netDone && !dcb->CachedListing)
            {
                ExReleaseResourceLite(dcb->Header.Resource);
                return BlorgHttpGetDirectoryInfo(&dcb->FullPath, BlorgDirComplete, Irp);
            }

            ccb->Entries = dcb->CachedListing;

            if (!ccb->Entries)
            {
                ExReleaseResourceLite(dcb->Header.Resource);
                return STATUS_NO_MORE_FILES;
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
                        PFILE_BOTH_DIR_INFORMATION dirInfo = (!Irp->MdlAddress) ?
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
                            FillFileBothDirInfo,
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
                    default:
                    {
                        result = STATUS_INVALID_INFO_CLASS;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
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
            PDCB dcb = IrpSp->FileObject->FsContext;

            if (BLORGFS_DCB_SIGNATURE != GET_NODE_TYPE(dcb) &&
                BLORGFS_ROOT_DCB_SIGNATURE != GET_NODE_TYPE(dcb))
            {
                result = STATUS_INVALID_PARAMETER;
                break;
            }

            PCCB ccb = IrpSp->FileObject->FsContext2;

            if (!ccb)
            {
                result = STATUS_INVALID_PARAMETER;
                break;
            }

            PBLORGFS_VDO_DEVICE_EXTENSION devExt = BlorgGetVolumeDeviceExtension(dcb->VolumeDeviceObject);

            FsRtlNotifyFullChangeDirectory(
                devExt->NotifySync,
                &devExt->NotifyList,
                ccb,
                C_CAST(PSTRING, &dcb->FullPath),
                BooleanFlagOn(IrpSp->Flags, SL_WATCH_TREE),
                FALSE,
                IrpSp->Parameters.NotifyDirectory.CompletionFilter,
                Irp,
                NULL,
                NULL);

            result = STATUS_PENDING;
            break;
        }
        default:
        {
            BLORGFS_LOG("UNHANDLED DirectoryControl minor=%u -> STATUS_INVALID_DEVICE_REQUEST\n", IrpSp->MinorFunction);
            result = STATUS_INVALID_DEVICE_REQUEST;
        }
    }

    return result;
}

//
// IRP_MJ_DIRECTORY_CONTROL dispatch entry point: routes to
// BlorgVolumeDirectoryControl for the volume device, completes as
// unsupported for the disk/FSDO devices, and completes synchronously
// unless the volume handler returns STATUS_PENDING (async HTTP fetch or
// a pending notify registration).
//
NTSTATUS BlorgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = BlorgIsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            BlorgSetupIrpContext(Irp, IoIsOperationSynchronous(Irp));

            result = BlorgVolumeDirectoryControl(Irp, irpSp);
            if (STATUS_PENDING != result)
            {
                BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BlorgDeviceDisk:
        case BlorgDeviceFileSystem:
        default:
        {
            //
            // One body for everything that is not the volume, unknown
            // included -- same unconditional-completion rule as BlorgRead's
            // switch, for the same reason: the cases complete inside
            // themselves, so a kind that matched none of them would strand
            // the IRP.
            //
            BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
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
