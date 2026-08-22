#pragma once

//
// HTTP client interface: address resolution and the directory-info /
// file-info / file-read request calls, each with an async completion
// callback. Backs the filesystem's network-facing operations.
//

//
// Blocks until every in-flight HTTP request has finished and refuses any
// new one. PASSIVE_LEVEL only, called once from DriverUnload before the
// device objects are torn down. See DrainHttpClient in Client.c.
//
VOID DrainHttpClient(void);

NTSTATUS InitialiseHttpClient(void);
void CleanupHttpClient(void);

NTSTATUS GetHttpAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeHttpAddrInfo(PADDRINFOEXW AddrInfo);

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
//  - DIRINFO / FILEINFO callbacks always run at PASSIVE_LEVEL (success
//    and failure alike; the client bounces to a work item first). They
//    may take push locks, enter critical regions, and touch paged data
//    -- BlorgCreateComplete/BlorgDirComplete rely on this for the
//    PathCache and DCB listing cache.
//

typedef VOID(*PBLORG_DIRINFO_COMPLETION)(NTSTATUS Status, PDIRECTORY_INFO DirInfo, PVOID CallerContext);
typedef VOID(*PBLORG_FILEINFO_COMPLETION)(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* FileInfo, PVOID CallerContext);
typedef VOID(*PBLORG_FILEREAD_COMPLETION)(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext);

NTSTATUS BlorgHttpGetDirectoryInfo(
    const UNICODE_STRING* Path,
    PBLORG_DIRINFO_COMPLETION CompletionRoutine,
    PVOID CallerContext
);

void FreeHttpDirectoryInfo(PDIRECTORY_INFO DirInfo);

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
// LockUserBuffer), which must describe at least Length writable bytes and
// stay locked until CompletionRoutine has run. On success the FILE_BUFFER
// passed to CompletionRoutine carries only the byte count
// (BodyBuffer/BaseAddress are NULL; FreeHttpFile on it is a no-op) -- the
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

void FreeHttpFile(PFILE_BUFFER FileBuffer);