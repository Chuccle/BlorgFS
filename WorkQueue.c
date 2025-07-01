#include "Driver.h"

static_assert(FSP_THREAD_COUNT <= MAXIMUM_WAIT_OBJECTS, "System threads cannot exceed MAXIMUM_WAIT_OBJECTS");

NTSTATUS BlorgVolumeDirectoryControl(PIRP Irp, PIO_STACK_LOCATION IrpSp, PIRP_CONTEXT IrpContext);
NTSTATUS BlorgVolumeRead(PIRP Irp, PIO_STACK_LOCATION IrpSp, PIRP_CONTEXT IrpContext);

volatile BOOLEAN g_FspThreadsActive;
KEVENT g_FspTerminationEvent;
HANDLE g_FspThreadHandle[FSP_THREAD_COUNT];
IO_CSQ g_Csq;
KSEMAPHORE g_FspWorkSemaphore;
KSPIN_LOCK g_OverflowQueueSpinLock;
LIST_ENTRY g_OverflowQueue;

VOID CsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&g_OverflowQueue, &Irp->Tail.Overlay.ListEntry);
}

VOID CsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

PIRP CsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
{
    UNREFERENCED_PARAMETER(Csq);
    UNREFERENCED_PARAMETER(PeekContext);

    PLIST_ENTRY nextEntry;

    // If Irp is NULL, we start from the head. Otherwise, we start from the given IRP.
    if (!Irp)
    {
        nextEntry = g_OverflowQueue.Flink;
    }
    else
    {
        nextEntry = Irp->Tail.Overlay.ListEntry.Flink;
    }

    if (nextEntry != &g_OverflowQueue)
    {
        return CONTAINING_RECORD(nextEntry, IRP, Tail.Overlay.ListEntry);
    }
    else
    {
        return NULL;
    }
}

_IRQL_raises_(DISPATCH_LEVEL)
VOID CsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&g_OverflowQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID CsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&g_OverflowQueueSpinLock, Irql);
}

VOID CsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    // The IRP has been cancelled. We just need to complete it.
    CompleteRequest(Irp->Tail.Overlay.DriverContext[0], Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}

// System threads disable kernel APCs so no need to explicitly disable APCs here.
VOID FspDispatch(_In_ PVOID StartContext)

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
    UNREFERENCED_PARAMETER(StartContext);
    BOOLEAN active = TRUE;

    //
    //  Now case on the function code.  For each major function code,
    //  either call the appropriate FSP routine or case on the minor
    //  function and then call the FSP routine.  The FSP routine that
    //  we call is responsible for completing the IRP, and not us.
    //  That way the routine can complete the IRP and then continue
    //  post processing as required.  For example, a read can be
    //  satisfied right away and then read can be done.
    //

    while (active)
    {

        PVOID waitObjectArray[2] = { &g_FspWorkSemaphore, &g_FspTerminationEvent };

        if (STATUS_WAIT_1 == KeWaitForMultipleObjects(2,
            waitObjectArray,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,
            NULL))
        {
            active = FALSE;
        }

        PIRP irp = IoCsqRemoveNextIrp(&g_Csq, NULL);

        NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;
        
        while (irp)
        {

            //
            //  Extract the IrpContext and IrpSp, and loop.
            //

            PIRP_CONTEXT irpContext = irp->Tail.Overlay.DriverContext[0];

            SetFlag(irpContext->Flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

            PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);

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

            switch (irpContext->MajorFunction)
            {
                case IRP_MJ_READ:
                {
                    result = BlorgVolumeRead(irp, irpSp, irpContext);
                    break;
                }
                case IRP_MJ_DIRECTORY_CONTROL:
                {
                    result = BlorgVolumeDirectoryControl(irp, irpSp, irpContext);
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

            CompleteRequest(irpContext, irp, result, IO_DISK_INCREMENT);

            IoSetTopLevelIrp(NULL);

            irp = IoCsqRemoveNextIrp(&g_Csq, NULL);
        }

    }

    PsTerminateSystemThread(STATUS_SUCCESS);
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
        IrpContext->OriginatingIrp->Tail.Overlay.DriverContext[0] = IrpContext;
        
        //
        // IoCsqInsertIrp marks IRPs as pending
        //
        
        IoCsqInsertIrp(&g_Csq, IrpContext->OriginatingIrp, NULL);
        KeReleaseSemaphore(&g_FspWorkSemaphore, IO_NO_INCREMENT, 1, FALSE);
    }
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
}

NTSTATUS FsdPostRequest(
    IN PIRP_CONTEXT IrpContext,
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
    NT_ASSERT(IrpContext->OriginatingIrp == Irp);

    if (!g_FspThreadsActive)
    {
        //
        //  And return to our caller
        //

        return STATUS_DEVICE_REMOVED;
    }

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
        CompleteRequest((PIRP_CONTEXT)Context, Irp, Irp->IoStatus.Status, IO_DISK_INCREMENT);
    }
}

NTSTATUS InitializeWorkQueue(void)
{
    g_FspThreadsActive = TRUE;

    KeInitializeSpinLock(&g_OverflowQueueSpinLock);
    InitializeListHead(&g_OverflowQueue);

    KeInitializeSemaphore(&g_FspWorkSemaphore, 0, FSP_THREAD_COUNT);
    KeInitializeEvent(&g_FspTerminationEvent, NotificationEvent, FALSE);

    NTSTATUS result = IoCsqInitialize(&g_Csq,
        CsqInsertIrp,
        CsqRemoveIrp,
        CsqPeekNextIrp,
        CsqAcquireLock,
        CsqReleaseLock,
        CsqCompleteCanceledIrp);

    for (int i = 0; i < FSP_THREAD_COUNT; ++i)
    {
        result = PsCreateSystemThread(&g_FspThreadHandle[i], DELETE | SYNCHRONIZE, NULL, NULL, NULL, FspDispatch, NULL);
    }

    return result;
}

void DeinitializeWorkQueue(void)
{

#pragma message("Potential race condition in the window before this flag is set to false but maybe IRP didn't get added to queue in time before the flush")
    g_FspThreadsActive = FALSE;

    PVOID* waitObjectArray = ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(PKTHREAD) * FSP_THREAD_COUNT, 'abw');

    if (!waitObjectArray)
    {
        BLORGFS_PRINT("Failed to allocate the wait object array to allow FSP threads to flush and terminate\n");
        return;
    }

    for (int i = 0; i < FSP_THREAD_COUNT; ++i)
    {
        if (NT_ERROR(ObReferenceObjectByHandle(g_FspThreadHandle[i], DELETE | SYNCHRONIZE, *PsThreadType, KernelMode, &waitObjectArray[i], NULL)))
        {
            BLORGFS_PRINT("Failed to initialise the wait object array to allow FSP threads to flush and terminate\n");
            
            for (int j = 0; j < i; ++j)
            {
                ObDereferenceObject(waitObjectArray[j]);
            }

            ExFreePool(waitObjectArray);
            return;
        }
    }

    PVOID waitBlockArray = ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(KWAIT_BLOCK) * FSP_THREAD_COUNT, 'abw');

    if (!waitBlockArray)
    { 
        BLORGFS_PRINT("Failed to allocate the wait block array to allow FSP threads to flush and terminate\n");

        for (int i = 0; i < FSP_THREAD_COUNT; ++i)
        {
            ObDereferenceObject(waitObjectArray[i]);
        }
        
        ExFreePool(waitObjectArray);
        return;
    }

    KeEnterCriticalRegion();
    KeSetEvent(&g_FspTerminationEvent, IO_NO_INCREMENT, TRUE);
    KeWaitForMultipleObjects(FSP_THREAD_COUNT, waitObjectArray, WaitAll, Executive, KernelMode, FALSE, NULL, waitBlockArray);
    KeLeaveCriticalRegion();

    ExFreePool(waitObjectArray);
    ExFreePool(waitBlockArray);

    for (int i = 0; i < FSP_THREAD_COUNT; ++i)
    {
        ZwClose(g_FspThreadHandle[i]);
    }
}