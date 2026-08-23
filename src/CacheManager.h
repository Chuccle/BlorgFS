#pragma once

//
// Cache manager callback declarations: lazy-write and read-ahead
// acquire/release pairs, plus the fast I/O possibility check.
//

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForLazyWrite(
    PVOID Context,
    BOOLEAN Wait
);

_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromLazyWrite(
    PVOID Context
);

_Requires_lock_held_(_Global_critical_region_)
BOOLEAN BlorgAcquireNodeForReadAhead(
    PVOID Context,
    BOOLEAN Wait
);

_Requires_lock_held_(_Global_critical_region_)
VOID BlorgReleaseNodeFromReadAhead(
    PVOID Context
);

_Function_class_(FAST_IO_CHECK_IF_POSSIBLE)
BOOLEAN
FastIoCheckIfPossible(
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
);