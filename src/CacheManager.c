#include "Driver.h"

//
//  Cache manager callbacks (acquire/release for lazy write and read-ahead)
//  and the FastIoCheckIfPossible fast-I/O entry point.
//

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN
BlorgAcquireNodeForLazyWrite(
    IN PVOID Context,
    IN BOOLEAN Wait
)

/*++

Routine Description:

    The address of this routine is specified when creating a CacheMap for
    a file.  It is subsequently called by the Lazy Writer prior to its
    performing lazy writes to the file.

Arguments:

    Context - The Fcb which was specified as a context parameter for this
          routine.

    Wait - TRUE if the caller is willing to block.

Return Value:

    FALSE - if Wait was specified as FALSE and blocking would have
            been required.  The Fcb is not acquired.

    TRUE - if the Fcb has been acquired

Notes:

    We do not need to disable APC delivery to guard against a rogue user
    issuing a suspend APC, because the caller is guaranteed to be either in
    the system context (to which a user cannot deliver a suspend APC), or
    to have already disabled kernel APC delivery before calling. This holds
    for all the other pre-acquire routines as well.

    The Lazy Writer is assumed to acquire this Fcb only once, so
    LazyWriteThread must be clear on entry (asserted); it is then set so the
    Lazy Writer never tries to advance Valid Data or deadlocks trying to
    get the Fcb exclusive.

    Cc can run this acquire on several worker threads concurrently (for
    different files), so the first-lazy-writer seed of
    global.LazyWriteThread is claimed with an interlocked
    compare-exchange -- a plain check-then-set lets two racing acquires
    both see NULL and the second silently overwrite the first.

    Setting the top-level IRP to FSRTL_CACHE_TOP_LEVEL_IRP is a kludge
    because Cc is really the top level: when it enters the file system, we
    would otherwise think this is a recursive call and complete the request
    with hard errors or verify.

--*/

{
    if (!ExAcquireResourceSharedLite(C_CAST(PFCB, Context)->Header.PagingIoResource, Wait))
    {
        return FALSE;
    }

    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(NULL == C_CAST(PFCB, Context)->LazyWriteThread);

    (C_CAST(PFCB, Context))->LazyWriteThread = PsGetCurrentThread();

    InterlockedCompareExchangePointer(&global.LazyWriteThread, PsGetCurrentThread(), NULL);

    NT_ASSERT(NULL == IoGetTopLevelIrp());

    IoSetTopLevelIrp(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP));

    return TRUE;
}

_Requires_lock_held_(_Global_critical_region_)
VOID
BlorgReleaseNodeFromLazyWrite(
    IN PVOID Context
)

/*++

Routine Description:

    The address of this routine is specified when creating a CacheMap for
    a file.  It is subsequently called by the Lazy Writer after its
    performing lazy writes to the file.

Arguments:

    Context - The Fcb which was specified as a context parameter for this
          routine.

Return Value:

    None

--*/

{
    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(PsGetCurrentThread() == C_CAST(PFCB, Context)->LazyWriteThread);

    (C_CAST(PFCB, Context))->LazyWriteThread = NULL;

    ExReleaseResourceLite(C_CAST(PFCB, Context)->Header.PagingIoResource);

    NT_ASSERT(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP) == IoGetTopLevelIrp());

    IoSetTopLevelIrp(NULL);
}

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN
BlorgAcquireNodeForReadAhead(
    IN PVOID Context,
    IN BOOLEAN Wait
)

/*++

Routine Description:

    The address of this routine is specified when creating a CacheMap for
    a file.  It is subsequently called by the Lazy Writer prior to its
    performing read ahead to the file.

Arguments:

    Context - The Fcb which was specified as a context parameter for this
          routine.

    Wait - TRUE if the caller is willing to block.

Return Value:

    FALSE - if Wait was specified as FALSE and blocking would have
            been required.  The Fcb is not acquired.

    TRUE - if the Fcb has been acquired

Notes:

    The normal file resource (not the paging I/O resource) is acquired
    shared here so read-ahead synchronises correctly with purges. See
    BlorgAcquireNodeForLazyWrite for the APC-delivery and top-level-IRP
    kludge rationale, both of which apply here too.

--*/

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

_Requires_lock_held_(_Global_critical_region_)
VOID
BlorgReleaseNodeFromReadAhead(
    IN PVOID Context
)

/*++

Routine Description:

    The address of this routine is specified when creating a CacheMap for
    a file.  It is subsequently called by the Lazy Writer after its
    read ahead.

Arguments:

    Context - The Fcb which was specified as a context parameter for this
          routine.

Return Value:

    None

--*/

{
    NT_ASSERT(C_CAST(PIRP, FSRTL_CACHE_TOP_LEVEL_IRP) == IoGetTopLevelIrp());

    IoSetTopLevelIrp(NULL);

    ExReleaseResourceLite(C_CAST(PFCB, Context)->Header.Resource);
}

_Function_class_(FAST_IO_CHECK_IF_POSSIBLE)
BOOLEAN
FastIoCheckIfPossible(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN ULONG LockKey,
    IN BOOLEAN CheckForReadOperation,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
)

/*++

Routine Description:

    This routine checks if fast i/o is possible for a read/write operation

Arguments:

    FileObject - Supplies the file object used in the query

    FileOffset - Supplies the starting byte offset for the read/write operation

    Length - Supplies the length, in bytes, of the read/write operation

    Wait - Indicates if we can wait

    LockKey - Supplies the lock key

    CheckForReadOperation - Indicates if this is a check for a read or write
        operation

    IoStatus - Receives the status of the operation if our return value is
        FastIoReturnError

Return Value:

    BOOLEAN - TRUE if fast I/O is possible and FALSE if the caller needs
        to take the long route.

Notes:

    Writes are blanket-failed for now: the write path is not yet
    implemented, so this always routes writes through the slow path.

--*/

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

    if (CheckForReadOperation)
    {

        if (FsRtlFastCheckLockForRead(&fcb->FileLock,
            FileOffset,
            &largeLength,
            LockKey,
            FileObject,
            PsGetCurrentProcess()))
        {
            return TRUE;
        }

    }

   return FALSE;
}