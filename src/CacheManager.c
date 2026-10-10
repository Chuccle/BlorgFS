#include "Driver.h"

//
//  Cache manager callbacks (acquire/release for lazy write and read-ahead)
//  and the BlorgFastIoCheckIfPossible fast-I/O entry point.
//

//
// Cc's lazy-writer pre-acquire for a file, named when its cache map is
// created. Context is the FCB. Takes the paging I/O resource shared and
// returns FALSE without it only when Wait is FALSE and it would block.
//
// APC delivery needs no disabling here, against a rogue suspend APC or
// otherwise: the caller is either in the system context, which a user
// cannot deliver one to, or has already disabled kernel APCs. That holds
// for every pre-acquire routine in this file.
//
// The lazy writer takes an FCB only once, so LazyWriteThread is clear on
// entry (asserted) and is set here, so it never tries to advance valid
// data or deadlocks trying to take the FCB exclusive.
//
// Cc can run this on several worker threads at once for different files,
// so the first lazy writer's seed of global.LazyWriteThread is claimed
// with an interlocked compare-exchange: a plain check-then-set let two
// racing acquires both see NULL and the second overwrite the first.
//
// The top-level IRP is set to FSRTL_CACHE_TOP_LEVEL_IRP because Cc is
// really the top level: without it, its entry into the file system would
// look like a recursive call and be completed with hard errors or verify.
//
_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForLazyWrite(PVOID Context, BOOLEAN Wait)
{
    if (!ExAcquireResourceSharedLite(C_CAST(PFCB, Context)->Header.PagingIoResource, Wait))
    {
        return FALSE;
    }

    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(NULL == C_CAST(PFCB, Context)->LazyWriteThread);

    C_CAST(PFCB, Context)->LazyWriteThread = PsGetCurrentThread();

    InterlockedCompareExchangePointer(&global.LazyWriteThread, PsGetCurrentThread(), NULL);

    NT_ASSERT(NULL == IoGetTopLevelIrp());

    IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));

    return TRUE;
}

//
// Cc's lazy-writer post-release, the pair of BlorgAcquireNodeForLazyWrite.
// Context is the FCB.
//
_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromLazyWrite(PVOID Context)
{
    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(PsGetCurrentThread() == C_CAST(PFCB, Context)->LazyWriteThread);

    C_CAST(PFCB, Context)->LazyWriteThread = NULL;

    ExReleaseResourceLite(C_CAST(PFCB, Context)->Header.PagingIoResource);

    NT_ASSERT(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP) == IoGetTopLevelIrp());

    IoSetTopLevelIrp(NULL);
}

//
// Cc's read-ahead pre-acquire for a file, named when its cache map is
// created. Context is the FCB. Returns FALSE without the resource only
// when Wait is FALSE and it would block.
//
// The main resource, not the paging I/O resource, is taken shared, so
// read-ahead synchronises with purges. BlorgAcquireNodeForLazyWrite's
// notes on APC delivery and the top-level IRP apply here too.
//
_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForReadAhead(PVOID Context, BOOLEAN Wait)
{
    if (!ExAcquireResourceSharedLite(C_CAST(PFCB, Context)->Header.Resource,
        Wait))
    {
        return FALSE;
    }

    NT_ASSERT(NULL == IoGetTopLevelIrp());

    IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));

    return TRUE;
}

//
// Cc's read-ahead post-release, the pair of BlorgAcquireNodeForReadAhead.
// Context is the FCB.
//
_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromReadAhead(PVOID Context)
{
    NT_ASSERT(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP) == IoGetTopLevelIrp());

    IoSetTopLevelIrp(NULL);

    ExReleaseResourceLite(C_CAST(PFCB, Context)->Header.Resource);
}

//
// Whether fast I/O may serve a read or write of Length bytes at
// FileOffset: TRUE sends it down the fast path, FALSE makes the caller
// take the IRP route. A read is allowed when no byte-range lock bars it.
// Writes are always refused, since the write path is not implemented.
//
_Function_class_(FAST_IO_CHECK_IF_POSSIBLE)
BOOLEAN BlorgFastIoCheckIfPossible(
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(Wait);

    if (BLORGFS_FCB_SIGNATURE != GET_NODE_TYPE(FileObject->FsContext))
    {
        return FALSE;
    }

    PFCB fcb = FileObject->FsContext;

    LARGE_INTEGER largeLength =
    {
        .QuadPart = Length
    };

    if (CheckForReadOperation &&
        FsRtlFastCheckLockForRead(&fcb->FileLock,
            FileOffset,
            &largeLength,
            LockKey,
            FileObject,
            PsGetCurrentProcess()))
    {
        return TRUE;
    }

    return FALSE;
}
