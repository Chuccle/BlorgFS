#include "Driver.h"

//
//  The following constant is the maximum number of ExWorkerThreads that we
//  will allow to be servicing a particular target device at any one time.
//

NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp, PIRP_CONTEXT IrpContext);
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp, PIRP_CONTEXT IrpContext);

#define FSP_PER_DEVICE_THRESHOLD         2

//
//  Internal support routine, spinlock wrapper.
//

static PVOID RemoveOverflowEntry(
    IN PBLORGFS_VDO_DEVICE_EXTENSION DeviceExtension
)
{
    PVOID entry;
    KIRQL savedIrql;

    KeAcquireSpinLock(&DeviceExtension->OverflowQueueSpinLock, &savedIrql);

    if (DeviceExtension->OverflowQueueCount > 0)
    {
        //
        //  There is overflow work to do in this volume so we'll
        //  decrement the Overflow count, dequeue the IRP, and release
        //  the Event
        //

        DeviceExtension->OverflowQueueCount -= 1;

        entry = RemoveHeadList(&DeviceExtension->OverflowQueue);
    }
    else
    {
        DeviceExtension->PostedRequestCount -= 1;

        entry = NULL;
    }

    KeReleaseSpinLock(&DeviceExtension->OverflowQueueSpinLock, savedIrql);

    return entry;
}

static void FspDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
)

/*++

Routine Description:

    This is the main FSP thread routine that is executed to receive
    and dispatch IRP requests.  Each FSP thread begins its execution here.
    There is one thread created at system initialization time and subsequent
    threads created as needed.

Arguments:

    DeviceObject - Supplies the device object for this volume.
    Context - Supplies the thread id.

Return Value:

    None - This routine never exits

--*/

{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIRP_CONTEXT irpContext = Context;
    PIRP irp = irpContext->OriginatingIrp;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

    PBLORGFS_VDO_DEVICE_EXTENSION deviceExtension = NULL;

    //
    //  Now because we are the Fsp we will force the IrpContext to
    //  indicate true on Wait.
    //

    SetFlag(irpContext->Flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

    //
    //  If this request has an associated volume device object, remember it.
    //

    if (irpSp->FileObject)
    {
        deviceExtension = GetVolumeDeviceExtension(irpSp->DeviceObject);
    }

    //
    //  Now case on the function code.  For each major function code,
    //  either call the appropriate FSP routine or case on the minor
    //  function and then call the FSP routine.  The FSP routine that
    //  we call is responsible for completing the IRP, and not us.
    //  That way the routine can complete the IRP and then continue
    //  post processing as required.  For example, a read can be
    //  satisfied right away and then read can be done.
    //

    while (TRUE)
    {

        BLORGFS_PRINT("FspDispatch: Irp = %p\n", irp);

        //
        //  If this Irp was top level, note it in our thread local storage.
        //

        if (FlagOn(irpContext->Flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL))
        {
            IoSetTopLevelIrp((PIRP)FSRTL_FSP_TOP_LEVEL_IRP);
        }
        else
        {
            IoSetTopLevelIrp(irp);
        }

        FsRtlEnterFileSystem();

        switch (irpContext->MajorFunction)
        {

            case IRP_MJ_CREATE:
                break;

            case IRP_MJ_CLOSE:
                break;

            case IRP_MJ_READ:
                BlorgVolumeRead(irp, irpSp, irpContext);
                break;

            case IRP_MJ_WRITE:
                break;

            case IRP_MJ_QUERY_INFORMATION:
                break;

            case IRP_MJ_SET_INFORMATION:
                break;

            case IRP_MJ_QUERY_EA:
                break;

            case IRP_MJ_SET_EA:
                break;

            case IRP_MJ_FLUSH_BUFFERS:
                break;

            case IRP_MJ_QUERY_VOLUME_INFORMATION:
                break;

            case IRP_MJ_SET_VOLUME_INFORMATION:
                break;

            case IRP_MJ_CLEANUP:
                break;

            case IRP_MJ_DIRECTORY_CONTROL:
                BlorgVolumeDirectoryControl(irp, irpSp, irpContext);
                break;

            case IRP_MJ_FILE_SYSTEM_CONTROL:
                break;

            case IRP_MJ_LOCK_CONTROL:
                break;

            case IRP_MJ_DEVICE_CONTROL:
                break;

            case IRP_MJ_SHUTDOWN:
                break;

            case IRP_MJ_PNP:
                break;

            //
            //  For any other major operations, return an invalid
            //  request.
            //

            default:
                CompleteRequest(irpContext, irp, STATUS_INVALID_DEVICE_REQUEST);
                break;

        }

        FsRtlExitFileSystem();

        IoSetTopLevelIrp(NULL);

        //
        //  If there are any entries on this volume's overflow queue, service
        //  them.
        //

        if (deviceExtension)
        {
            //
            //  We have a volume device object so see if there is any work
            //  left to do in its overflow queue.
            //

            PVOID entry = RemoveOverflowEntry(deviceExtension);

            //
            //  There wasn't an entry, break out of the loop and return to
            //  the Ex Worker thread.
            //

            if (!entry)
            {
                break;
            }

            //
            //  Extract the IrpContext, Irp, and IrpSp, and loop.
            //

            irpContext = CONTAINING_RECORD(entry,
                IRP_CONTEXT,
                ListEntry);

            SetFlag(irpContext->Flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

            irp = irpContext->OriginatingIrp;

            irpSp = IoGetCurrentIrpStackLocation(irp);

            continue;
        }
        else
        {
            break;
        }
    }
}

//
//  Local support routine.
//

static void AddToWorkqueue(
    IN PIRP_CONTEXT IrpContext,
    IN PIO_STACK_LOCATION IrpSp
)
{
    //
    //  Check if this request has an associated file object, and thus volume
    //  device object.
    //

    if (IrpSp->FileObject)
    {
        PBLORGFS_VDO_DEVICE_EXTENSION deviceExtension = GetVolumeDeviceExtension(IrpSp->DeviceObject);

        //
        //  Check to see if this request should be sent to the overflow
        //  queue.  If not, then send it off to an exworker thread.
        //

        KIRQL savedIrql;

        KeAcquireSpinLock(&deviceExtension->OverflowQueueSpinLock, &savedIrql);

        if (deviceExtension->PostedRequestCount > FSP_PER_DEVICE_THRESHOLD)
        {
            //
            //  We cannot currently respond to this IRP so we'll just enqueue it
            //  to the overflow queue on the volume.
            //

            InsertTailList(&deviceExtension->OverflowQueue, &IrpContext->ListEntry);

            deviceExtension->OverflowQueueCount += 1;

            KeReleaseSpinLock(&deviceExtension->OverflowQueueSpinLock, savedIrql);

            return;
        }
        else
        {
             //
             //  We are going to send this Irp to a worker thread so up
             //  the count.
             //

            deviceExtension->PostedRequestCount += 1;

            KeReleaseSpinLock(&deviceExtension->OverflowQueueSpinLock, savedIrql);
        }
    }

    //
    //  Send it off.....
    //

    IoInitializeWorkItem(global.FileSystemDeviceObject, ((PIO_WORKITEM)(IrpContext->WorkQueueItem)));

    IoQueueWorkItem(((PIO_WORKITEM)(IrpContext->WorkQueueItem)), FspDispatch, CustomPriorityWorkQueue + 13, IrpContext);
}

void PrePostIrp(
    IN PVOID Context,
    IN PIRP Irp
)
{
    //
    //  If there is no Irp, we are done.
    //

    if (!Irp)
    {
        return;
    }

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PIRP_CONTEXT IrpContext = Context;

    switch (IrpContext->MajorFunction)
    {
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
        {
            if (!FlagOn(IrpContext->MinorFunction, IRP_MN_MDL))
            {
                LockUserBuffer(Irp,
                    (IrpContext->MajorFunction == IRP_MJ_READ) ?
                    IoWriteAccess : IoReadAccess,
                    (IrpContext->MajorFunction == IRP_MJ_READ) ?
                    IrpSp->Parameters.Read.Length : IrpSp->Parameters.Write.Length);
            }
            break;
        }
        case IRP_MJ_DIRECTORY_CONTROL:
        {
            if (IRP_MN_QUERY_DIRECTORY == IrpContext->MinorFunction)
            {
                LockUserBuffer(Irp,
                    IoWriteAccess,
                    IrpSp->Parameters.QueryDirectory.Length);
            }
            break;
        }
        case IRP_MJ_QUERY_EA:
        {
            LockUserBuffer(Irp,
                IoWriteAccess,
                IrpSp->Parameters.QueryEa.Length);
            break;
        }
        case IRP_MJ_SET_EA:
        {
            LockUserBuffer(Irp,
                IoReadAccess,
                IrpSp->Parameters.SetEa.Length);
            break;
        }
    }

    //
    //  Mark that we've already returned pending to the user
    //

    IoMarkIrpPending(Irp);

    return;
}


NTSTATUS FsdPostRequest(
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
)

/*++

Routine Description:

    This routine enqueues the request packet specified by IrpContext to the
    Ex Worker threads.  This is a FSD routine.

Arguments:

    IrpContext - Pointer to the IrpContext to be queued to the Fsp

    Irp - I/O Request Packet, or NULL if it has already been completed.

Return Value:

    STATUS_PENDING


--*/

{
    NT_ASSERT(ARGUMENT_PRESENT(Irp));
    NT_ASSERT(IrpContext->OriginatingIrp == Irp);

    PrePostIrp(IrpContext, Irp);

    AddToWorkqueue(IrpContext, IrpSp);

    //
    //  And return to our caller
    //

    return STATUS_PENDING;
}

void OplockComplete(PVOID Context, PIRP Irp)
{
    if (STATUS_SUCCESS == Irp->IoStatus.Status)
    {
        AddToWorkqueue((PIRP_CONTEXT)Context, IoGetCurrentIrpStackLocation(Irp));
    }
    else
    {
        CompleteRequest((PIRP_CONTEXT)Context, Irp, Irp->IoStatus.Status);
    }

    return;
}