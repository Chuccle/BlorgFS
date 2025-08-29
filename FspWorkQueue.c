#include "Driver.h"

NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject);
NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp);
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp);

IO_WORKITEM_ROUTINE FspDispatch;

IO_CSQ g_FspCsq;
KSPIN_LOCK g_FspIrpQueueSpinLock;
LIST_ENTRY g_FspIrpQueue;

VOID FspCsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&g_FspIrpQueue, &Irp->Tail.Overlay.ListEntry);
}

VOID FspCsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

PIRP FspCsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
{
    UNREFERENCED_PARAMETER(Csq);
    UNREFERENCED_PARAMETER(PeekContext);

    PLIST_ENTRY nextEntry;

    // If Irp is NULL, we start from the head. Otherwise, we start from the given IRP.
    if (!Irp)
    {
        nextEntry = g_FspIrpQueue.Flink;
    }
    else
    {
        nextEntry = Irp->Tail.Overlay.ListEntry.Flink;
    }

    if (nextEntry != &g_FspIrpQueue)
    {
        return CONTAINING_RECORD(nextEntry, IRP, Tail.Overlay.ListEntry);
    }
    else
    {
        return NULL;
    }
}

_IRQL_raises_(DISPATCH_LEVEL)
VOID FspCsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&g_FspIrpQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID FspCsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&g_FspIrpQueueSpinLock, Irql);
}

VOID FspCsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    // The IRP has been cancelled. We just need to complete it.
    CompleteRequest(Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}


VOID FspDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
)
/*++

Routine Description:

    This is the main FSP thread routine that is executed to receive
    and dispatch IRP requests.

Arguments:

    StartContext - Not currently used, required by the KSTART_ROUTINE signature.

Return Value:

    None - This routine never exits

--*/

{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    //
    //  Now case on the function code.  For each major function code,
    //  either call the appropriate FSP routine or case on the minor
    //  function and then call the FSP routine.  The FSP routine that
    //  we call is responsible for completing the IRP, and not us.
    //  That way the routine can complete the IRP and then continue
    //  post processing as required.  For example, a read can be
    //  satisfied right away and then read can be done.
    //

    PIRP irp = IoCsqRemoveNextIrp(&g_FspCsq, NULL);

    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
        
    while (irp)
    {

        //
        //  Extract the IrpContext and IrpSp, and loop.
        //
            
        ULONG_PTR flags = (ULONG_PTR)irp->Tail.Overlay.DriverContext[0];
            
        SetFlag(flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);
            
        irp->Tail.Overlay.DriverContext[0] = (PVOID)flags;

        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

        BLORGFS_PRINT("FspDispatch: Irp = %p\n", irp);

        FsRtlEnterFileSystem();
        
        //
        //  If this Irp was top level, note it in our thread local storage.
        //

        if (FlagOn((ULONG_PTR)irp->Tail.Overlay.DriverContext[0], IRP_CONTEXT_FLAG_RECURSIVE_CALL))
        {
            IoSetTopLevelIrp((PIRP)FSRTL_FSP_TOP_LEVEL_IRP);
        }
        else
        {
            IoSetTopLevelIrp(irp);
        }

        switch (irpSp->MajorFunction)
        {
            case IRP_MJ_CREATE:
            {
                result = BlorgVolumeCreate(irp, irpSp, irpSp->DeviceObject);
                break;
            }
            case IRP_MJ_READ:
            {
                result = BlorgVolumeRead(irp, irpSp);
                break;
            }
            case IRP_MJ_DIRECTORY_CONTROL:
            {
                result = BlorgVolumeDirectoryControl(irp, irpSp);
                break;
            }

            //
            //  For any other major operations, return an invalid
            //  request.
            //

            default:
            {
                break;
            }
        }

        // We can still get STATUS_PENDING in FSP if FSP needs to use inverted call
        if (STATUS_PENDING != result)
        {
            CompleteRequest(irp, result, IO_DISK_INCREMENT);
        }

        IoSetTopLevelIrp(NULL);

        FsRtlExitFileSystem();

        IoFreeWorkItem(irp->Tail.Overlay.DriverContext[1]);
        irp->Tail.Overlay.DriverContext[1] = NULL;

        irp = IoCsqRemoveNextIrp(&g_FspCsq, NULL);
    }
}

//
//  Local support routine.
//

static NTSTATUS AddToWorkqueue(
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
)
{
    //
    //  Check if this request has an associated file object, and thus volume
    //  device object.
    //

    if (IrpSp->FileObject)
    {        
        //
        // IoCsqInsertIrp marks IRPs as pending
        //
        
        IoCsqInsertIrp(&g_FspCsq, Irp, NULL);
    }

    Irp->Tail.Overlay.DriverContext[1] = IoAllocateWorkItem(IrpSp->DeviceObject);

    if (!Irp->Tail.Overlay.DriverContext[1])
    {
        return STATUS_NO_MEMORY;
    }

    IoQueueWorkItem(Irp->Tail.Overlay.DriverContext[1], FspDispatch, CustomPriorityWorkQueue + 13, NULL);

    return STATUS_PENDING;
}

void PrePostIrp(
    IN PVOID Context,
    IN PIRP Irp
)
{

    UNREFERENCED_PARAMETER(Context);
    //
    //  If there is no Irp, we are done.
    //

    if (!Irp)
    {
        return;
    }

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction)
    {
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
        {
            if (!FlagOn(IrpSp->MinorFunction, IRP_MN_MDL))
            {
                LockUserBuffer(Irp,
                    (IrpSp->MajorFunction == IRP_MJ_READ) ?
                    IoWriteAccess : IoReadAccess,
                    (IrpSp->MajorFunction == IRP_MJ_READ) ?
                    IrpSp->Parameters.Read.Length : IrpSp->Parameters.Write.Length);
            }
            break;
        }
        case IRP_MJ_DIRECTORY_CONTROL:
        {
            if (IRP_MN_QUERY_DIRECTORY == IrpSp->MinorFunction)
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
}

NTSTATUS FsdPostRequest(
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
)

/*++

Routine Description:

    This routine enqueues the request packet specified by IrpContext to the
    FSP threads.  This is a FSD routine.

Arguments:

    IrpContext - Pointer to the IrpContext to be queued to the Fsp

    Irp - I/O Request Packet, or NULL if it has already been completed.

    IrpSp - Pointer to the current I/O stack location for the Irp

Return Value:

    STATUS_PENDING


--*/

{
    NT_ASSERT(ARGUMENT_PRESENT(Irp));

    PrePostIrp(NULL, Irp);

    //
    //  And return to our caller
    //

    return AddToWorkqueue(Irp, IrpSp);
}

void OplockComplete(PVOID Context, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Context);

    if (STATUS_SUCCESS == Irp->IoStatus.Status)
    {
        AddToWorkqueue(Irp, IoGetCurrentIrpStackLocation(Irp));
    }
    else
    {
        CompleteRequest(Irp, Irp->IoStatus.Status, IO_DISK_INCREMENT);
    }
}

NTSTATUS InitialiseWorkQueue(void)
{
    KeInitializeSpinLock(&g_FspIrpQueueSpinLock);
    InitializeListHead(&g_FspIrpQueue);

    return IoCsqInitialize(&g_FspCsq,
        FspCsqInsertIrp,
        FspCsqRemoveIrp,
        FspCsqPeekNextIrp,
        FspCsqAcquireLock,
        FspCsqReleaseLock,
        FspCsqCompleteCanceledIrp);
}