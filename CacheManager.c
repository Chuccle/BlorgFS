#include "Driver.h"

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

--*/

{
    //
    //  Check here for the EA File.  It turns out we need the normal
    //  resource shared in this case.  Otherwise we take the paging
    //  I/O resource shared.
    //

    //
    //  Note that we do not need to disable APC delivery to guard 
    //  against a rogue user issuing a suspend APC. That is because 
    //  it is guaranteed that the caller is either in the system context,
    //  to which a user cannot deliver a suspend APC, or the caller has
    //  already disabled kernel APC delivery before calling. This is true
    //  for all the other pre-acquire routines as well.
    //

    if (!ExAcquireResourceSharedLite(C_CAST(PFCB, Context)->Header.PagingIoResource, Wait))
    {
        return FALSE;
    }

    //
    // We assume the Lazy Writer only acquires this Fcb once.
    // Therefore, it should be guaranteed that this flag is currently
    // clear (the ASSERT), and then we will set this flag, to ensure
    // that the Lazy Writer will never try to advance Valid Data, and
    // also not deadlock by trying to get the Fcb exclusive.
    //


    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(NULL == C_CAST(PFCB, Context)->LazyWriteThread);

    (C_CAST(PFCB, Context))->LazyWriteThread = PsGetCurrentThread();

    if (!global.LazyWriteThread)
    {
        global.LazyWriteThread = PsGetCurrentThread();
    }

    //
    //  This is a kludge because Cc is really the top level.  When it
    //  enters the file system, we will think it is a resursive call
    //  and complete the request with hard errors or verify.  It will
    //  then have to deal with them, somehow....
    //

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
    //
    //  Assert that this really is an fcb and that this thread really owns
    //  the lazy writer mark in the fcb.
    //

    NT_ASSERT(BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(Context));
    NT_ASSERT(NULL != PsGetCurrentThread());
    NT_ASSERT(PsGetCurrentThread() == C_CAST(PFCB, Context)->LazyWriteThread);

    //
    //  Release the lazy writer mark.
    //

    (C_CAST(PFCB, Context))->LazyWriteThread = NULL;

    //
    //  Check here for the EA File.  It turns out we needed the normal
    //  resource shared in this case.  Otherwise it was the PagingIoResource.
    //

    ExReleaseResourceLite(C_CAST(PFCB, Context)->Header.PagingIoResource);

    //
    //  Clear the kludge at this point.
    //

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

--*/

{
    //
    //  We acquire the normal file resource shared here to synchronize
    //  correctly with purges.
    //

    //
    //  Note that we do not need to disable APC delivery to guard 
    //  against a rogue user issuing a suspend APC. That is because 
    //  it is guaranteed that the caller is either in the system context,
    //  to which a user cannot deliver a suspend APC, or the caller has
    //  already disabled kernel APC delivery before calling. This is true
    //  for all the other pre-acquire routines as well.
    //

    if (!ExAcquireResourceSharedLite(C_CAST(PFCB, Context)->Header.Resource,
        Wait))
    {

        return FALSE;
    }

    //
    //  This is a kludge because Cc is really the top level.  We it
    //  enters the file system, we will think it is a resursive call
    //  and complete the request with hard errors or verify.  It will
    //  have to deal with them, somehow....
    //

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
    //
    //  Clear the kludge at this point.
    //

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

--*/

{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(Wait);
    //
    //  Decode the file object to get our fcb, the only one we want
    //  to deal with is a UserFileOpen
    //

    if (BLORGFS_FCB_SIGNATURE != GET_NODE_TYPE(FileObject->FsContext))
    {
        return FALSE;
    }

    PFCB fcb = FileObject->FsContext;

    LARGE_INTEGER largeLength = 
    {
        .QuadPart = Length
    };

    //
    //  Based on whether this is a read or write operation we call
    //  fsrtl check for read/write
    //

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
   
    //
    // Blanket fail all writes for now
    //
       
   return FALSE;
}