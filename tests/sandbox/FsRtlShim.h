#pragma once

//
// The FsRtl / MM surface Structs.h and the dispatch paths need.
//
// These are the ONLY driver-visible types the sandbox declares for
// itself. Everything else -- FCB, DCB, COMMON_CONTEXT, DIRECTORY_INFO,
// READ_STREAM_TRACKER, the padding assertions, READ_AHEAD_GRANULARITY --
// comes from the driver's own headers, because a second copy of any of
// them is a copy that can drift.
//
// These few cannot come from the driver: they belong to the kernel, and
// substituting them is the whole point. They are opaque by design.
// Nothing in the lifetime or dispatch logic reads inside an OPLOCK or a
// FILE_LOCK, so their contents change nothing a test observes -- but
// their SIZES matter, because Structs.h asserts field adjacency through
// them, and those assertions are compiled here too (PADDING_CHECKS). Each
// therefore matches its kernel counterpart's exact size -- SHARE_ACCESS is
// 28 bytes, not a tidy 32, and rounding it up silently shifts every
// COMMON_CONTEXT field after it until the padding chain fails.
//

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SECTION_OBJECT_POINTERS
{
    PVOID DataSectionObject;
    PVOID SharedCacheMap;
    PVOID ImageSectionObject;
    PVOID Reserved;
} SECTION_OBJECT_POINTERS, * PSECTION_OBJECT_POINTERS;

//
// FSRTL_ADVANCED_FCB_HEADER.IsFastIoPossible takes one of these; the
// driver assigns FastIoIsQuestionable so Cc routes fast I/O through
// FastIoCheckIfPossible rather than short-circuiting it.
//
typedef enum _FAST_IO_POSSIBLE
{
    FastIoIsNotPossible = 0,
    FastIoIsPossible,
    FastIoIsQuestionable
} FAST_IO_POSSIBLE;

typedef struct _OPLOCK { PVOID Opaque[4]; } OPLOCK, * POPLOCK;

typedef struct _FILE_LOCK { PVOID Opaque[8]; } FILE_LOCK, * PFILE_LOCK;

typedef struct _SHARE_ACCESS
{
    ULONG OpenCount;
    ULONG Readers;
    ULONG Writers;
    ULONG Deleters;
    ULONG SharedRead;
    ULONG SharedWrite;
    ULONG SharedDelete;
} SHARE_ACCESS, * PSHARE_ACCESS;

//
// Only the members the driver touches, ordered so Structs.h's
// CHECK_PADDING chain through COMMON_CONTEXT holds. Oplock is embedded
// here because that is where the driver reads it from (Header.Oplock).
//
typedef struct _FSRTL_ADVANCED_FCB_HEADER
{
    CSHORT NodeTypeCode;
    CSHORT NodeByteSize;
    UCHAR Flags;
    UCHAR IsFastIoPossible;
    UCHAR Flags2;
    UCHAR Reserved;
    ULONG Reserved2;

    PERESOURCE Resource;
    PERESOURCE PagingIoResource;

    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER FileSize;
    LARGE_INTEGER ValidDataLength;

    PFAST_MUTEX FastMutex;
    LIST_ENTRY FilterContexts;
    OPLOCK Oplock;
} FSRTL_ADVANCED_FCB_HEADER, * PFSRTL_ADVANCED_FCB_HEADER;

typedef struct _NOTIFY_SYNC NOTIFY_SYNC, * PNOTIFY_SYNC;

VOID FsRtlSetupAdvancedHeader(PVOID Header, PFAST_MUTEX FastMutex);
VOID FsRtlTeardownPerStreamContexts(PFSRTL_ADVANCED_FCB_HEADER Header);
VOID FsRtlInitializeFileLock(PFILE_LOCK FileLock, PVOID CompleteLockRoutine, PVOID UnlockRoutine);
VOID FsRtlUninitializeFileLock(PFILE_LOCK FileLock);
VOID FsRtlInitializeOplock(POPLOCK Oplock);
VOID FsRtlUninitializeOplock(POPLOCK Oplock);

//
// Byte-range lock fast checks. No lock is ever held in the sandbox, so
// both succeed: modelling real ranges would be modelling FsRtl rather
// than BlorgFS.
//
//
// An oplock may only be granted when no byte-range lock conflicts. No lock
// is ever taken in the sandbox, so this always agrees.
//
//
// The oplock package's FSCTL entry point. Inert here: BlorgFS's oplock
// tests live in the kernel, and modelling FsRtl's own state machine would
// be testing FsRtl rather than the driver.
//
NTSTATUS FsRtlOplockFsctrl(POPLOCK Oplock, PIRP Irp, ULONG OpenCount);

BOOLEAN FsRtlCheckLockForOplockRequest(PFILE_LOCK FileLock, PLARGE_INTEGER AllocationSize);

BOOLEAN FsRtlFastCheckLockForRead(
    PFILE_LOCK FileLock, PLARGE_INTEGER FileOffset, PLARGE_INTEGER Length,
    ULONG Key, PFILE_OBJECT FileObject, PVOID ProcessId);

BOOLEAN FsRtlFastCheckLockForWrite(
    PFILE_LOCK FileLock, PLARGE_INTEGER FileOffset, PLARGE_INTEGER Length,
    ULONG Key, PFILE_OBJECT FileObject, PVOID ProcessId);

NTSTATUS FsRtlDissectName(UNICODE_STRING Path, PUNICODE_STRING FirstName, PUNICODE_STRING RemainingName);

//
// A volume device object whose extension carries real lookaside lists, so
// nodes under test come from the same allocator the driver uses and their
// accounting proves a reap actually freed something. Defined in
// StructsModel.c; declared here because it needs no driver types.
//
struct _DEVICE_OBJECT;

struct _DEVICE_OBJECT* StructsModelCreateVolume(VOID);
VOID StructsModelDestroyVolume(struct _DEVICE_OBJECT* VolumeDeviceObject);

#ifdef __cplusplus
}
#endif
