#pragma once

//
// HTTP client interface: address resolution and the directory-info /
// file-info / file-read / change-feed request calls, each with an async
// completion callback. Backs the filesystem's network-facing operations.
//

//
// Blocks until every in-flight HTTP request has finished and refuses any
// new one. PASSIVE_LEVEL only, called once from BlorgDriverUnload before the
// device objects are torn down. See BlorgDrainHttpClient in Client.c.
//
VOID BlorgDrainHttpClient(VOID);

NTSTATUS BlorgInitialiseHttpClient(VOID);
VOID BlorgCleanupHttpClient(VOID);

NTSTATUS BlorgGetHttpAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo);
VOID BlorgFreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

//
// Completion callback signatures per operation. Exactly one of these is
// invoked, exactly once, for a given request. IRQL contract is
// per-operation (enforced by HttpMustBounceToPassive in Client.c):
//
//  - FILEREAD callbacks run at <= DISPATCH_LEVEL directly on the WSK
//    completion chain (the read hot path -- no work-item hop). They must
//    not block, touch paged memory/code, or take push locks /
//    KeEnterCriticalRegion.
//
//  - DIRINFO / FILEINFO / CHANGES callbacks always run at PASSIVE_LEVEL
//    (success and failure alike; the client bounces to a work item
//    first). They may take push locks, enter critical regions, and touch
//    paged data -- CreateComplete/DirCtrlComplete rely on this for the
//    PathCache and DCB listing cache.
//
//  A DIRINFO or FILEINFO result the server marked Cache-Control: no-store
//  arrives with NoStore set, and the path cache refuses it: the server
//  could not vouch that it reflects every change its feed has reported.
//

typedef VOID(*PBLORG_DIRINFO_COMPLETION)(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext);
typedef VOID(*PBLORG_FILEINFO_COMPLETION)(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* FileInfo, PVOID CallerContext);
typedef VOID(*PBLORG_FILEREAD_COMPLETION)(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext);
typedef VOID(*PBLORG_CHANGES_COMPLETION)(NTSTATUS Status, PCHANGE_BATCH Batch, PVOID CallerContext);

//
// Fetches Path's listing. A nonzero SubtreeEntries asks for the listings
// beneath it too, up to that many entries in all, which arrive attached
// to the listing as its Descendants; the caller takes them off with
// BlorgTakeDescendants before sharing the listing, and frees them with
// BlorgReleaseDescendants.
//
NTSTATUS BlorgHttpGetDirectoryInfo(
    const UNICODE_STRING* Path,
    ULONG SubtreeEntries,
    PBLORG_DIRINFO_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

VOID BlorgReferenceDirectoryInfo(PDIRECTORY_INFO DirInfo);
VOID BlorgReleaseDirectoryInfo(PDIRECTORY_INFO DirInfo);

//
// Builds a listing (DIRECTORY_INFO). Allocate sizes one zeroed PagedPool
// block for FileCount files, SubDirCount subdirectories and NameBytes of
// names, terminators included, holding one reference, or returns NULL. The
// caller fills every entry, pointing its Name into the block from
// NamesOffset on, then Index chains each into the name index, through
// which Find looks a name up: the first entry, files before
// subdirectories, whose name equals Name case-insensitively, as *Entry
// counting files first. PASSIVE_LEVEL.
//
PDIRECTORY_INFO BlorgAllocateDirectoryInfo(SIZE_T FileCount, SIZE_T SubDirCount, SIZE_T NameBytes);
VOID BlorgIndexDirectoryInfo(PDIRECTORY_INFO DirInfo);
BOOLEAN BlorgFindDirectoryEntry(PDIRECTORY_INFO DirInfo, const UNICODE_STRING* Name, PSIZE_T Entry);
PDIRECTORY_DESCENDANT BlorgTakeDescendants(PDIRECTORY_INFO DirInfo, PSIZE_T Count);
VOID BlorgReleaseDescendants(PDIRECTORY_DESCENDANT Descendants, SIZE_T Count);

NTSTATUS BlorgHttpGetFileInformation(
    const UNICODE_STRING* Path,
    PBLORG_FILEINFO_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

NTSTATUS BlorgHttpGetFile(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

//
// Zero-copy variant: the response body is received directly into
// TargetMdl (already-locked pages -- a paging-IO MDL, or one locked via
// BlorgLockUserBuffer), which must describe at least Length writable bytes and
// stay locked until CompletionRoutine has run. On success the FILE_BUFFER
// passed to CompletionRoutine carries only the byte count
// (BodyBuffer/BaseAddress are NULL; BlorgFreeHttpFile on it is a no-op) -- the
// data is already in place.
//
NTSTATUS BlorgHttpGetFileMdl(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PMDL TargetMdl,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

VOID BlorgFreeHttpFile(PFILE_BUFFER FileBuffer);

//
// Long-polls the server's change feed for what changed after generation
// Since of server process Epoch (both zero for a client with neither yet,
// which is answered at once with a reset). The server holds the request
// until something changes or its hold time passes, well inside the
// receive timeout. On success the batch belongs to CompletionRoutine,
// which frees it with BlorgFreeChangeBatch.
//
NTSTATUS BlorgHttpGetChanges(
    ULONG64 Epoch,
    ULONG64 Since,
    PBLORG_CHANGES_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

VOID BlorgFreeChangeBatch(PCHANGE_BATCH Batch);