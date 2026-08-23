#pragma once

//
// Small shared helpers used across dispatch routines: pool
// realloc-with-copy, IRP user-buffer locking, IRP context flag access,
// request completion, and top-level IRP tracking.
//

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

//
// Builds and probes/locks an MDL for the IRP's user buffer if one isn't
// already present, returning the outcome so callers can refuse to post a
// request whose buffer could not be locked (posting it would later have a
// worker thread dereference a raw user VA in the wrong process context).
// A zero-length buffer or an MDL that already exists is success with
// nothing to do; IoAllocateMdl failure is STATUS_INSUFFICIENT_RESOURCES;
// a probe fault propagates its exception code (e.g. STATUS_ACCESS_VIOLATION)
// after tearing the half-built MDL back down.
//
inline NTSTATUS BlorgLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG BufferLength)
{
    if (Irp->MdlAddress || 0 == BufferLength)
    {
        return STATUS_SUCCESS;
    }

    PMDL mdl = IoAllocateMdl(Irp->UserBuffer, BufferLength, FALSE, FALSE, Irp);

    if (!mdl)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

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
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

//
// Irp->Tail.Overlay.DriverContext[0] carries the per-request context
// flags (IRP_CONTEXT_FLAG_*) as a raw PVOID -- setting one requires the
// same read-as-ULONG_PTR / SetFlag / write-back-as-PVOID dance at every
// call site (Create.c, DirCtrl.c), since DriverContext[0] can't be
// SetFlag'd directly (wrong type/width). Small enough to inline, shared
// so the cast dance exists in exactly one place.
//
inline void BlorgSetIrpContextFlag(PIRP Irp, ULONG_PTR Flag)
{
    ULONG_PTR flags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);
    SetFlag(flags, Flag);
    Irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);
}

//
// Counterpart to BlorgSetIrpContextFlag, for flags that must be single-shot
// across FSP re-drives of the same IRP: a flag whose payload is consumed
// on one pass (e.g. IRP_CONTEXT_FLAG_NET_DONE and its DriverContext[1]
// stash, BlorgVolumeCreate) is cleared at consumption so a later re-drive
// of the IRP -- an oplock break re-queues it through BlorgOplockComplete --
// cannot act on the flag with the payload already gone.
//
inline void BlorgClearIrpContextFlag(PIRP Irp, ULONG_PTR Flag)
{
    ULONG_PTR flags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);
    ClearFlag(flags, Flag);
    Irp->Tail.Overlay.DriverContext[0] = C_CAST(PVOID, flags);
}

inline void BlorgCompleteRequest(
    _In_opt_ PIRP Irp,
    NTSTATUS Status,
    CCHAR PriorityBoost
)

/*++

Routine Description:

    This routine completes a Irp. On an error status for an input
    operation, Information is zeroed first, since IopCompleteRequest
    would otherwise try to copy that many bytes to the user's buffer.

Arguments:

    Irp - Supplies the Irp being processed

    Status - Supplies the status to complete the Irp with

Return Value:

    None.

--*/

{
    if (Irp)
    {
        if (NT_ERROR(Status) &&
            FlagOn(Irp->Flags, IRP_INPUT_OPERATION))
        {
            Irp->IoStatus.Information = 0;
        }

        Irp->IoStatus.Status = Status;

        IoCompleteRequest(Irp, PriorityBoost);
    }
}

inline BOOLEAN BlorgIsIrpTopLevel(
    PIRP Irp
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
