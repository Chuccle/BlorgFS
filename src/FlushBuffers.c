#include "Driver.h"

//
//  IRP_MJ_FLUSH_BUFFERS handling.
//
//  Succeeds without doing anything, for every device this driver owns. That
//  is the honest answer rather than a stub: the volume is read-only, so
//  nothing is ever dirty and a flush has nothing to write back. There is no
//  work being skipped here that a later change would have to remember.
//
//  It returned STATUS_INVALID_DEVICE_REQUEST until 2026-08-22, which
//  surfaces as "Incorrect function" -- the same misleading error that hid
//  the statistics IOCTL routing bug, and one that some applications treat
//  as fatal rather than as "this volume does not need flushing". Most
//  filesystems answer a flush on a read-only volume with success for
//  exactly that reason.
//
//  Anything that is not one of this driver's three device objects is still
//  refused; a flush arriving on a device we do not own is a routing error,
//  not a no-op.
//
NTSTATUS BlorgFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    NTSTATUS result =
        (BlorgDeviceUnknown == BlorgDeviceKind(DeviceObject))
            ? STATUS_INVALID_DEVICE_REQUEST
            : STATUS_SUCCESS;

    Irp->IoStatus.Status = result;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return result;
}
