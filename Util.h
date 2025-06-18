#pragma once

inline
__drv_allocatesMem(Mem)
_When_((PoolType& PagedPool) != 0, _IRQL_requires_max_(APC_LEVEL))
_When_((PoolType& PagedPool) == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
_When_((PoolType& NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    _When_((PoolType& (NonPagedPoolMustSucceed |
        POOL_RAISE_IF_ALLOCATION_FAILURE)) == 0,
        _Post_maybenull_ _Must_inspect_result_)
    _When_((PoolType& (NonPagedPoolMustSucceed |
        POOL_RAISE_IF_ALLOCATION_FAILURE)) != 0,
        _Post_notnull_)
    _Post_writable_byte_size_(NumberOfBytes)
    PVOID
    NTAPI
    ReallocateBufferUninitialized(
        _In_ PVOID OldBuffer,
        _In_ SIZE_T OldNumberOfBytes,
        _In_ __drv_strictTypeMatch(__drv_typeExpr) POOL_TYPE PoolType,
        _In_ SIZE_T NumberOfBytes,
        _In_ ULONG Tag
    )
{
    PVOID newBuffer = ExAllocatePoolUninitialized(PoolType, NumberOfBytes, Tag);

    if (!newBuffer)
    {
        return OldBuffer;
    }

    RtlCopyMemory(newBuffer, OldBuffer, OldNumberOfBytes);

    ExFreePool(OldBuffer);

    return newBuffer;
}

inline
__drv_allocatesMem(Mem)
_When_((PoolType& PagedPool) != 0, _IRQL_requires_max_(APC_LEVEL))
_When_((PoolType& PagedPool) == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
_When_((PoolType& NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    _When_((PoolType& (NonPagedPoolMustSucceed |
        POOL_RAISE_IF_ALLOCATION_FAILURE)) == 0,
        _Post_maybenull_ _Must_inspect_result_)
    _When_((PoolType& (NonPagedPoolMustSucceed |
        POOL_RAISE_IF_ALLOCATION_FAILURE)) != 0,
        _Post_notnull_)
    _Post_writable_byte_size_(NumberOfBytes)
    PVOID
    NTAPI
    ReallocateBufferZero(
        _In_ PVOID OldBuffer,
        _In_ SIZE_T OldNumberOfBytes,
        _In_ __drv_strictTypeMatch(__drv_typeExpr) POOL_TYPE PoolType,
        _In_ SIZE_T NumberOfBytes,
        _In_ ULONG Tag
    )
{
    PVOID newBuffer = ExAllocatePoolZero(PoolType, NumberOfBytes, Tag);

    if (!newBuffer)
    {
        return OldBuffer;
    }

    RtlCopyMemory(newBuffer, OldBuffer, OldNumberOfBytes);

    ExFreePool(OldBuffer);

    return newBuffer;
}

inline void LockUserBuffer(IN OUT PIRP Irp, IN LOCK_OPERATION Operation, IN ULONG BufferLength)
{
    if (!Irp->MdlAddress)
    {
        PMDL mdl = IoAllocateMdl(Irp->UserBuffer, BufferLength, FALSE, FALSE, Irp);

        if (mdl)
        {
           //
           // Now probe the buffer described by the Irp.  If we get an exception,
           // deallocate the Mdl and return the appropriate "expected" status.
           //

            __try
            {
                MmProbeAndLockPages(mdl,
                    Irp->RequestorMode,
                    Operation);

            } 
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                IoFreeMdl(mdl);
                Irp->MdlAddress = NULL;
            }
        }
    }
}

inline void CompleteRequest(
    IN PIRP Irp OPTIONAL,
    IN NTSTATUS Status,
    IN CCHAR PriorityBoost
)

/*++

Routine Description:

    This routine completes a Irp

Arguments:

    Irp - Supplies the Irp being processed

    Status - Supplies the status to complete the Irp with

Return Value:

    None.

--*/

{
    //
    //  If we have an Irp then complete the irp.
    //

    if (Irp)
    {
        //
        //  We got an error, so zero out the information field before
        //  completing the request if this was an input operation.
        //  Otherwise IopCompleteRequest will try to copy to the user's buffer.
        //

        if (NT_ERROR(Status) &&
            FlagOn(Irp->Flags, IRP_INPUT_OPERATION))
        {
            Irp->IoStatus.Information = 0;
        }

        Irp->IoStatus.Status = Status;

        IoCompleteRequest(Irp, PriorityBoost);
    }
}

inline BOOLEAN IsIrpTopLevel(
    IN PIRP Irp
)

/*++

Routine Description:

    This routine detects if an Irp is the Top level requestor, ie. if it os OK
    to do a verify or pop-up now.  If TRUE is returned, then no file system
    resources are held above us.

Arguments:

    Irp - Supplies the Irp being processed

    Status - Supplies the status to complete the Irp with

Return Value:

    None.

--*/

{
    if (!IoGetTopLevelIrp())
    {
        IoSetTopLevelIrp(Irp);

        return TRUE;
    }
    else
    {
        return FALSE;
    }
}