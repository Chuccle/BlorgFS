#pragma once

NTSTATUS MapUserBuffer(PIRP Irp, PVOID* Address);

FORCEINLINE
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

FORCEINLINE
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