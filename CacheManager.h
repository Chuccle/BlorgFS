#pragma once

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForLazyWrite(
    IN PVOID Context,
    IN BOOLEAN Wait
);

_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromLazyWrite(
    IN PVOID Context
);

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForReadAhead(
    IN PVOID Context,
    IN BOOLEAN Wait
);

_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromReadAhead(
    IN PVOID Context
);

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
);