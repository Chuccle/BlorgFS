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
static inline ULONG DirCtrlAlignEntrySize(ULONG Size)
{
    return (Size + 7u) & ~7u;
}

//
// The largest entry any fill routine writes: FILE_ID_BOTH_DIR_INFORMATION
// has the longest fixed part, and a listing admits names of up to
// MAX_NAME_LEN - 1 characters. Entries are built in a scratch block of this
// size before they are copied to the caller's buffer.
//
#define DIRCTRL_ENTRY_MAX_BYTES \
    C_CAST(ULONG, (FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) + ((MAX_NAME_LEN - 1) * sizeof(WCHAR)) + 7u) & ~7u)

C_ASSERT(FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) <= FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName));
C_ASSERT(FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) <= FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName));

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
static inline BOOLEAN DirCtrlMatchPattern(const PUNICODE_STRING EntryName, const PUNICODE_STRING SearchPattern, ULONGLONG Flags)
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
static inline NTSTATUS DirCtrlFillFileIdBothDirInfo(
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
    ULONG alignedSize = DirCtrlAlignEntrySize(rawSize);

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

static inline NTSTATUS DirCtrlFillFileFullDirInfo(
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
    ULONG alignedSize = DirCtrlAlignEntrySize(rawSize);

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

static inline NTSTATUS DirCtrlFillFileBothDirInfo(
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
    ULONG alignedSize = DirCtrlAlignEntrySize(rawSize);

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
// OutBuffer can be the caller's user-mode buffer, so it is only written
// through the user-mode accessors, as OutMode says: each entry is built in
// Scratch (DIRCTRL_ENTRY_MAX_BYTES) and copied out whole. A fault raises
// to the caller's handler. The fill is offered at most the scratch's size,
// so a name longer than a listing admits overflows instead of overrunning
// it.
//
static NTSTATUS DirCtrlEnumerateDirectoryEntries(
    const PCCB Ccb,
    ULONG StartIndex,
    ULONG TotalEntries,
    const PUNICODE_STRING Pattern,
    ULONGLONG Flags,
    BOOLEAN ReturnSingle,
    PVOID OutBuffer,
    ULONG OutLength,
    KPROCESSOR_MODE OutMode,
    PVOID Scratch,
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

        if (DirCtrlMatchPattern(&name, Pattern, Flags))
        {
            SIZE_T written = 0;
            NTSTATUS st = FillFn(
                Scratch,
                (remaining < DIRCTRL_ENTRY_MAX_BYTES) ? remaining : DIRCTRL_ENTRY_MAX_BYTES,
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

            CopyToMode(cursor, Scratch, written, OutMode);
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
        WriteULongToMode(C_CAST(PULONG, lastEntry), 0, OutMode);
    }

    *BytesUsed = totalWritten;
    *FinalIndex = index;
    return found ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
}

#define DIRCTRL_FETCH_TAG 'FDLB'
#define DIRCTRL_ENTRY_TAG 'EDLB'

//
// One directory-listing fetch in flight: the query waiting on it, or none
// for a background refresh, which instead owns a copy of the directory's
// path because no handle keeps the DCB alive for it. The ticket is taken
// at issue, so the result is refused by the listing and path caches if an
// invalidation overtook it on the wire. NonPagedPoolNx throughout: a fetch
// that fails is completed on the failure path, which frees this context.
//
typedef struct _DIRCTRL_FETCH
{
    PIRP              Irp;    // the query, or NULL for a refresh
    UNICODE_STRING    Path;   // owned copy, refresh only
    PATH_CACHE_TICKET Ticket; // taken immediately before the request is issued
} DIRCTRL_FETCH, * PDIRCTRL_FETCH;

CHECK_PADDING_BETWEEN(DIRCTRL_FETCH, Irp, Path);
CHECK_PADDING_BETWEEN(DIRCTRL_FETCH, Path, Ticket);
CHECK_PADDING_END(DIRCTRL_FETCH, Ticket);

//
//  Offers a fetched listing to the listing cache and, if it is the current
//  truth for the directory, seeds the path cache from it: every child
//  becomes a live entry with the listing's metadata, replacing any stale
//  not-found, and other children are dropped, deeper entries too unless the
//  change feed is live (BlorgPathCacheSeedListing).
//  Both refuse it if an invalidation ran after the fetch was issued, and the
//  listing cache also refuses one older than the snapshot it already holds.
//  Runs at PASSIVE_LEVEL from a successful fetch's completion.
//
//  A subtree answer's descendants are taken off the listing first, so the
//  snapshot the cache and the handle share never holds them, and are then
//  published as listings of their own. The seed only touches the path
//  cache, which they do not seed, so the two may run in either order.
//
static VOID DirCtrlPublish(const UNICODE_STRING* Dir, PDIRECTORY_INFO DirInfo, const PATH_CACHE_TICKET* Ticket)
{
    SIZE_T count = 0;
    PDIRECTORY_DESCENDANT descendants = BlorgTakeDescendants(DirInfo, &count);

    if (BlorgPathCachePublishListing(Dir, DirInfo, Ticket))
    {
        BlorgPathCacheSeedListing(Dir, DirInfo, Ticket);
    }

    if (descendants)
    {
        const SIZE_T published = BlorgPathCachePublishDescendants(Dir, DirInfo, descendants, count, Ticket);

        BLORGFS_STAT_ADD(ListingsPrefetched, published);
        BlorgReleaseDescendants(descendants, count);
    }
}

//
//  Completion for the fetch a directory query issued on a listing-cache
//  miss. A success runs at PASSIVE_LEVEL (Client.c bounces deserialization
//  there), so the ERESOURCE is legal; it is wrapped in a critical region
//  since a system worker thread does not disable APCs the way an FSP thread
//  does. A failure only completes the IRP and frees nonpaged memory.
//
//  The listing becomes this handle's snapshot unless the handle already has
//  one: a second query on the same handle can race this fetch with its own
//  (see BlorgVolumeDirectoryControl), and an enumeration that has started on
//  one snapshot must not have it swapped underneath. The loser's reference
//  is dropped. The IRP is then re-queued with NET_DONE set so the PASSIVE
//  enumeration runs on an FSP thread; if BlorgFsdRequeueRequest fails (FSP
//  threads tearing down), the snapshot already belongs to the CCB and is
//  released with it, so the query is simply failed.
//
static VOID DirCtrlComplete(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext)
{
    PDIRCTRL_FETCH fetch = CallerContext;
    PIRP irp = fetch->Irp;

    if (!NT_SUCCESS(Status))
    {
        ExFreePool(fetch);
        BlorgCompleteRequest(irp, Status, IO_DISK_INCREMENT);
        return;
    }

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);
    PDCB dcb = irpSp->FileObject->FsContext;
    PCCB ccb = irpSp->FileObject->FsContext2;

    DirCtrlPublish(&dcb->FullPath, DirInfo, &fetch->Ticket);
    ExFreePool(fetch);

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(dcb->Header.Resource, TRUE);

    if (!ccb->Entries)
    {
        ccb->Entries = DirInfo;
        DirInfo = NULL;
    }

    ExReleaseResourceLite(dcb->Header.Resource);
    KeLeaveCriticalRegion();

    BlorgReleaseDirectoryInfo(DirInfo);

    BlorgSetIrpContextFlag(irp, IRP_CONTEXT_FLAG_NET_DONE);

    NTSTATUS requeue = BlorgFsdRequeueRequest(irp);

    if (STATUS_PENDING != requeue)
    {
        BlorgCompleteRequest(irp, requeue, IO_DISK_INCREMENT);
    }
}

//
//  Issues the listing fetch for a query that missed the listing cache.
//  STATUS_PENDING means DirCtrlComplete owns the IRP and the context; any
//  other result means the completion never ran, so the context is freed
//  here and the caller completes the IRP with the result.
//
static NTSTATUS DirCtrlFetch(PIRP Irp, const DCB* Dcb)
{
    PDIRCTRL_FETCH fetch = ExAllocatePoolZero(NonPagedPoolNx, sizeof(DIRCTRL_FETCH), DIRCTRL_FETCH_TAG);

    if (!fetch)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fetch->Irp = Irp;
    BlorgPathCacheTakeTicket(&fetch->Ticket);

    NTSTATUS result = BlorgHttpGetDirectoryInfo(&Dcb->FullPath, global.SubtreeEntries, DirCtrlComplete, fetch);

    if (STATUS_PENDING != result)
    {
        ExFreePool(fetch);
    }

    return result;
}

//
//  Completion for a background refresh: publishes the new snapshot for the
//  queries that come after, and drops the fetch's own reference. Nothing
//  waits on it. A failure leaves the stale snapshot to age out (see
//  BlorgPathCacheLookupListing).
//
static VOID DirCtrlRefreshComplete(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext)
{
    PDIRCTRL_FETCH fetch = CallerContext;

    if (NT_SUCCESS(Status))
    {
        DirCtrlPublish(&fetch->Path, DirInfo, &fetch->Ticket);
        BlorgReleaseDirectoryInfo(DirInfo);
    }

    ExFreePool(fetch->Path.Buffer);
    ExFreePool(fetch);
}

//
//  Refetches a directory whose cached listing a query was just answered from
//  stale, so the next query gets a fresh one. Called with no resource held;
//  a refresh that cannot be issued is dropped, the same as one that fails.
//
static VOID DirCtrlRefresh(const UNICODE_STRING* Dir)
{
    PDIRCTRL_FETCH fetch = ExAllocatePoolZero(NonPagedPoolNx, sizeof(DIRCTRL_FETCH), DIRCTRL_FETCH_TAG);

    if (!fetch)
    {
        return;
    }

    fetch->Path.Buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, Dir->Length, DIRCTRL_FETCH_TAG);

    if (!fetch->Path.Buffer)
    {
        ExFreePool(fetch);
        return;
    }

    RtlCopyMemory(fetch->Path.Buffer, Dir->Buffer, Dir->Length);
    fetch->Path.Length = Dir->Length;
    fetch->Path.MaximumLength = Dir->Length;

    BLORGFS_STAT_INC(ListingRefreshes);
    BlorgPathCacheTakeTicket(&fetch->Ticket);

    if (STATUS_PENDING != BlorgHttpGetDirectoryInfo(&fetch->Path, 0, DirCtrlRefreshComplete, fetch))
    {
        ExFreePool(fetch->Path.Buffer);
        ExFreePool(fetch);
    }
}

//
//  Gives a handle that has none the snapshot it will enumerate. An initial
//  query or a restart scan drops the one the handle had first, holding the
//  DCB resource exclusive: a restart is a request to read the directory
//  again. Any other query may get here beside another on the same handle
//  under the shared resource, so the install is a compare-exchange from
//  NULL and the loser drops its reference -- the pointer must be published
//  exactly once without a lock that both hold.
//
static VOID DirCtrlInstallSnapshot(PCCB Ccb, PDIRECTORY_INFO Snapshot)
{
    if (NULL != InterlockedCompareExchangePointer(C_CAST(PVOID volatile*, &Ccb->Entries), Snapshot, NULL))
    {
        BlorgReleaseDirectoryInfo(Snapshot);
    }
}

//
// Handles IRP_MJ_DIRECTORY_CONTROL for the volume device: QUERY_DIRECTORY
// (acquire DCB resource per scan-restart/pattern-change rules, take a
// listing snapshot from the listing cache or fetch one over HTTP and
// re-enter via NET_DONE, then enumerate into the caller's
// FILE_*_DIR_INFORMATION buffer) and NOTIFY_CHANGE_DIRECTORY (register with
// the FsRtl notify package; nothing reports changes yet, so the IRP is only
// completed at handle cleanup).
//
// Each handle enumerates its own snapshot (CCB.Entries), taken on its
// initial query and again on every restart scan, and kept unchanged in
// between: an enumeration in progress never sees entries move under its
// index, which is what NTFS and SMB give a caller too. A newer listing of
// the same directory replaces the cached one for the queries that come
// after, never the one a handle is reading. The CCB is re-checked for a
// pattern/MATCH_ALL after acquiring the resource exclusive, since it could
// have been set by another thread in the window before the lock was taken.
// A restart scan that finds it set keeps the resource exclusive: it is
// about to replace the handle's snapshot, which a query holding the
// resource shared may be enumerating.
// On a restart scan or initial query, the CCB's search pattern is cleared
// and regenerated from the query's FileName (or CCB_FLAG_MATCH_ALL if none
// given).
//
// The listing cache answers fresh snapshots outright, and stale ones too
// within its bounded window, in which case this query owes one background
// refresh (DirCtrlRefresh), issued after the resource is released. Only a
// miss fetches in the foreground: the ERESOURCE cannot be held across the
// async completion (it runs on a different thread), so it is released
// before issuing, and a restart scan drops the handle's old snapshot first
// so the completion installs the new one. DirCtrlComplete publishes the
// listing and re-queues this IRP with NET_DONE set.
//
// A snapshot is looked for whenever the handle has none, not only on an
// initial query or a restart: a second QUERY_DIRECTORY on the same handle,
// arriving after the first has set the pattern but before that first
// fetch has completed, is neither, and used to fall through to "no
// listing, therefore no more files", which is wrong; NULL only ever means
// "not fetched yet", never "empty" (an empty directory still produces a
// real zero-count DIRECTORY_INFO). That second query may issue a second
// fetch; whichever completes first becomes the handle's snapshot and the
// other is released (DirCtrlComplete). The NET_DONE pass looks again for
// the same reason if a racing restart on the handle dropped the snapshot
// the completion installed.
//
// A miss is the only query posted to the FSP, and it is posted when not
// already there: the fetch can complete the IRP before it returns, so it
// is only issued once the IRP is pending. An initial query or a restart
// sets the handle's pattern and takes its snapshot in the FSD, where a
// cached listing answers it at once; they used to be posted first, which
// cost a cached tree walk a worker hop, a buffer lock and a system-PTE
// map per directory. The FSP pass of one that missed finds the pattern
// set, so it is no longer an initial query and takes the resource shared,
// which is all a lookup needs.
//
// NOTIFY_CHANGE_DIRECTORY registers the watch with the FsRtl notify
// package, which captures its own copy of the directory name and holds
// the IRP pending until a change is reported under it or the handle is
// cleaned up (FsRtlNotifyCleanup in CleanupVolume). Changes are reported
// only from the server's change feed (ChangeFeed.c), and the package
// matches them against this name, so the name is the DCB's FullPath,
// spelled as the feed's paths are converted (HttpDeserializeChangeBatch).
// The package marks the IRP pending itself, so this function returns
// STATUS_PENDING and must not touch the IRP afterward.
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

            switch (GET_NODE_TYPE(dcb))
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

                    if (restartScan)
                    {
                        ccb->CurrentIndex = 0;
                    }
                    else
                    {
                        ExConvertExclusiveToSharedLite(dcb->Header.Resource);
                    }
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

            PUNICODE_STRING pattern = IrpSp->Parameters.QueryDirectory.FileName;
            BOOLEAN hasPattern = pattern && pattern->Buffer && (0 < pattern->Length);

            if ((initialQuery || restartScan) && !netDone)
            {
                RtlZeroMemory(&ccb->Flags, sizeof(ULONGLONG));

                if (ccb->SearchPattern.Buffer)
                {
                    RtlFreeUnicodeString(&ccb->SearchPattern);
                    RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
                }

                if (!hasPattern)
                {
                    SetFlag(ccb->Flags, CCB_FLAG_MATCH_ALL);
                }
                else
                {
                    result = RtlUpcaseUnicodeString(&ccb->SearchPattern, pattern, TRUE);

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

            BOOLEAN freshSnapshot = !netDone && (initialQuery || restartScan);
            BOOLEAN refreshOwed = FALSE;

            NT_ASSERT(!freshSnapshot || ExIsResourceAcquiredExclusiveLite(dcb->Header.Resource));

            if (freshSnapshot)
            {
                BlorgReleaseDirectoryInfo(ccb->Entries);
                ccb->Entries = NULL;
            }

            if (!ReadPointerAcquire(C_CAST(PVOID volatile*, &ccb->Entries)))
            {
                BOOLEAN stale = FALSE;
                PDIRECTORY_INFO snapshot = BlorgPathCacheLookupListing(&dcb->FullPath, TRUE, &stale, &refreshOwed, NULL);

                if (!snapshot)
                {
                    ExReleaseResourceLite(dcb->Header.Resource);

                    if (!BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_IN_FSP))
                    {
                        BLORGFS_PRINT("BlorgVolumeDirectoryControl: Enqueue to Fsp\n");
                        return BlorgFsdPostRequest(Irp, IrpSp);
                    }

                    BLORGFS_STAT_INC(ListingCacheMisses);
                    return DirCtrlFetch(Irp, dcb);
                }

                if (stale)
                {
                    BLORGFS_STAT_INC(ListingCacheStaleHits);
                }
                else
                {
                    BLORGFS_STAT_INC(ListingCacheHits);
                }

                DirCtrlInstallSnapshot(ccb, snapshot);
            }

            if (!ccb->Entries)
            {
                ExReleaseResourceLite(dcb->Header.Resource);
                return STATUS_NO_MORE_FILES;
            }

            ULONG remainingLength = IrpSp->Parameters.QueryDirectory.Length;
            BOOLEAN updateCcb = FALSE;
            ULONG index = (indexSpecified) ? IrpSp->Parameters.QueryDirectory.FileIndex : C_CAST(ULONG, ccb->CurrentIndex);

            ULONG totalEntries = C_CAST(ULONG, ccb->Entries->FileCount + ccb->Entries->SubDirCount);

            PFILL_ROUTINE fill = NULL;

            switch (IrpSp->Parameters.QueryDirectory.FileInformationClass)
            {
                case FileIdBothDirectoryInformation:
                {
                    fill = DirCtrlFillFileIdBothDirInfo;
                    break;
                }
                case FileFullDirectoryInformation:
                {
                    fill = DirCtrlFillFileFullDirInfo;
                    break;
                }
                case FileBothDirectoryInformation:
                {
                    fill = DirCtrlFillFileBothDirInfo;
                    break;
                }
                case FileDirectoryInformation:
                case FileIdFullDirectoryInformation:
                case FileNamesInformation:
                {
                    result = STATUS_NOT_IMPLEMENTED;
                    break;
                }
                default:
                {
                    result = STATUS_INVALID_INFO_CLASS;
                }
            }

            PVOID scratch = NULL;

            if (fill)
            {
                scratch = ExAllocatePoolUninitialized(PagedPool, DIRCTRL_ENTRY_MAX_BYTES, DIRCTRL_ENTRY_TAG);

                if (!scratch)
                {
                    result = STATUS_INSUFFICIENT_RESOURCES;
                }
            }

            if (scratch)
            {
                __try
                {
                    PVOID buffer = (!Irp->MdlAddress) ?
                        Irp->UserBuffer :
                        MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);

                    if (!buffer)
                    {
                        result = STATUS_INSUFFICIENT_RESOURCES;
                    }
                    else
                    {
                        SIZE_T used = 0;
                        result = DirCtrlEnumerateDirectoryEntries(
                            ccb,
                            index,
                            totalEntries,
                            &ccb->SearchPattern,
                            ccb->Flags,
                            returnSingleEntry,
                            buffer,
                            remainingLength,
                            (!Irp->MdlAddress) ? Irp->RequestorMode : KernelMode,
                            scratch,
                            fill,
                            &used,
                            &index
                        );

                        if (NT_SUCCESS(result))
                        {
                            Irp->IoStatus.Information = used;
                        }

                        updateCcb = !indexSpecified;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    updateCcb = FALSE;
                    result = GetExceptionCode();
                }

                ExFreePool(scratch);
            }

            if (updateCcb)
            {
                ccb->CurrentIndex = index;
            }

            ExReleaseResourceLite(dcb->Header.Resource);

            if (refreshOwed)
            {
                DirCtrlRefresh(&dcb->FullPath);
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
// One switch body covers everything that is not the volume, unknown kinds
// included -- the same unconditional-completion rule as BlorgRead's switch
// and for the same reason: the cases complete inside themselves, so a kind
// matching none of them would strand the IRP.
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
