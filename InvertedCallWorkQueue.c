#include "Driver.h"

#define INVERTED_CALL_TIMEOUT_SECONDS 10
#define INVERTED_CALL_TIMEOUT (INVERTED_CALL_TIMEOUT_SECONDS * -10000000LL)

static LONG g_RequestId = 0xFFFFFFFE;

IO_CSQ g_InvertedCallHandlerCsq;

KSPIN_LOCK g_InvertedCallHandlerIrpQueueSpinLock;
LIST_ENTRY g_InvertedCallHandlerIrpQueue;

KGUARDED_MUTEX g_InvertedCallHandlerRequestMutex;
LIST_ENTRY g_InvertedCallHandlerRequestQueue;

VOID InvertedCallHandlerCsqInsertIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&g_InvertedCallHandlerIrpQueue, &Irp->Tail.Overlay.ListEntry);
}

VOID InvertedCallHandlerCsqRemoveIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

PIRP InvertedCallHandlerCsqPeekNextIrp(IO_CSQ* Csq, PIRP Irp, PVOID PeekContext)
{
    UNREFERENCED_PARAMETER(Csq);
    UNREFERENCED_PARAMETER(PeekContext);

    PLIST_ENTRY nextEntry;

    // If Irp is NULL, we start from the head. Otherwise, we start from the given IRP.
    if (!Irp)
    {
        nextEntry = g_InvertedCallHandlerIrpQueue.Flink;
    }
    else
    {
        nextEntry = Irp->Tail.Overlay.ListEntry.Flink;
    }

    if (nextEntry != &g_InvertedCallHandlerIrpQueue)
    {
        return CONTAINING_RECORD(nextEntry, IRP, Tail.Overlay.ListEntry);
    }
    else
    {
        return NULL;
    }
}

VOID InvertedCallHandlerCsqAcquireLock(IO_CSQ* Csq, PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&g_InvertedCallHandlerIrpQueueSpinLock, Irql);
}

VOID InvertedCallHandlerCsqReleaseLock(IO_CSQ* Csq, KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&g_InvertedCallHandlerIrpQueueSpinLock, Irql);
}

VOID InvertedCallHandlerCsqCompleteCanceledIrp(IO_CSQ* Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    // The IRP has been cancelled. We just need to complete it.
    CompleteRequest(Irp, STATUS_CANCELLED, IO_NO_INCREMENT);
}

static inline PLIST_ENTRY RemoveRequestFromQueue(void)
{
   PLIST_ENTRY entry = NULL;
   KeAcquireGuardedMutex(&g_InvertedCallHandlerRequestMutex);
   if (!IsListEmpty(&g_InvertedCallHandlerRequestQueue))
   {
       entry = RemoveHeadList(&g_InvertedCallHandlerRequestQueue);
   }
   KeReleaseGuardedMutex(&g_InvertedCallHandlerRequestMutex);
   return entry;
}

static inline void InsertRequestIntoQueue(PLIST_ENTRY ListEntry)
{
    KeAcquireGuardedMutex(&g_InvertedCallHandlerRequestMutex);
    InsertTailList(&g_InvertedCallHandlerRequestQueue, ListEntry);
    KeReleaseGuardedMutex(&g_InvertedCallHandlerRequestMutex);
}

static inline PINVERTED_CALL_DATA_REQUEST RemoveRequestFromQueueById(
    ULONG RequestId
)
{
    KeAcquireGuardedMutex(&g_InvertedCallHandlerRequestMutex);
    for (PLIST_ENTRY listEntry = g_InvertedCallHandlerRequestQueue.Flink;
        listEntry != &g_InvertedCallHandlerRequestQueue;
        listEntry = listEntry->Flink)
    {
        PINVERTED_CALL_DATA_REQUEST request = CONTAINING_RECORD(listEntry, INVERTED_CALL_DATA_REQUEST, ListEntry);

        if (RequestId == request->RequestId)
        {
            RemoveEntryList(listEntry);
            KeReleaseGuardedMutex(&g_InvertedCallHandlerRequestMutex);
            return request;

        }
    }
    KeReleaseGuardedMutex(&g_InvertedCallHandlerRequestMutex);
    return NULL;
}

VOID InvertedCallTimeoutDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ULONG requestId = (ULONG)(ULONG_PTR)DeferredContext;

    PINVERTED_CALL_DATA_REQUEST request = RemoveRequestFromQueueById(requestId);

    if (request)
    {
        // We won the race - this request timed out
        BLORGFS_PRINT("InvertedCall: Request %lu timed out after %d seconds\n",
            request->RequestId, INVERTED_CALL_TIMEOUT_SECONDS);

        // Set timeout status and signal waiting thread
        request->Completion.Status = STATUS_TIMEOUT;
        KeSetEvent(&request->CompletionEvent, EVENT_INCREMENT, FALSE);
    }
}

NTSTATUS CreateInvertedCallRequest(
    enum InvertedCallType InvertedCallType,
    PCUNICODE_STRING Path,
    PINVERTED_CALL_DATA_REQUEST* OutRequest
)
{
    if (PATH_MAX * sizeof(WCHAR) < Path->Length)
    {
        return STATUS_INVALID_PARAMETER_3;
    }

    PINVERTED_CALL_DATA_REQUEST request = ExAllocatePoolZero(PagedPool, sizeof(INVERTED_CALL_DATA_REQUEST) + Path->Length, 'ICRS');

    if (!request)
    {
        return STATUS_NO_MEMORY;
    }

    request->InvertedCallType = InvertedCallType;

    RtlCopyMemory(request->Path, Path->Buffer, Path->Length);

    request->PathLength = Path->Length;

    request->RequestId = (ULONG)InterlockedIncrement(&g_RequestId);

    KeInitializeEvent(&request->CompletionEvent, NotificationEvent, FALSE);

    KDPC dpc;
    KTIMER timer;

    KeInitializeDpc(&dpc, InvertedCallTimeoutDpcRoutine, (PVOID)request->RequestId);
    KeInitializeTimer(&timer);

    PIRP irp = IoCsqRemoveNextIrp(&g_InvertedCallHandlerCsq, NULL);

    if (irp)
    {
        PBLORGFS_INVERTED_CALL systemBuffer = (irp->MdlAddress) ? MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) : NULL;

        if (!systemBuffer)
        {
            IoCsqInsertIrp(&g_InvertedCallHandlerCsq, irp, NULL);
            ExFreePool(request);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        systemBuffer->Header.RequestId = request->RequestId;
        systemBuffer->InvertedCallType = InvertedCallType;
        systemBuffer->PathLength = request->PathLength;

        RtlCopyMemory(systemBuffer->Path, request->Path, request->PathLength);

        irp->IoStatus.Information = UFIELD_OFFSET(BLORGFS_INVERTED_CALL, PathLength) + request->PathLength;

        CompleteRequest(irp, STATUS_SUCCESS, IO_DISK_INCREMENT);
    }
    else
    {
        InsertRequestIntoQueue(&request->ListEntry);
    }

    LARGE_INTEGER timeout = { .QuadPart = INVERTED_CALL_TIMEOUT };
    KeSetTimer(&timer, timeout, &dpc);

    KeWaitForSingleObject(
        &request->CompletionEvent,
        Executive,
        KernelMode,
        FALSE,
        NULL
    );

    // Cancel the timer in case completion happened before timeout
    KeCancelTimer(&timer);

    *OutRequest = request;

    return request->Completion.Status;
}

void CleanupInvertedCallRequest(PINVERTED_CALL_DATA_REQUEST Request)
{
    if (!Request)
    {
        return;
    }

    if (Request->Completion.ResponseBuffer)
    { 
        ExFreePool(Request->Completion.ResponseBuffer);
    }

    ExFreePool(Request);
}

BOOLEAN ProcessControlResponse(PIRP Irp, ULONG ResponseBufferLength)
{
    // Get response from IRP
    PBLORGFS_INVERTED_CALL systemBuffer = (Irp->MdlAddress) ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) : NULL;

    if (!systemBuffer)
    {
        return FALSE;
    }

    PINVERTED_CALL_DATA_REQUEST request = RemoveRequestFromQueueById(systemBuffer->Header.RequestId);

    if (request)
    {
        // Process the response and copy data over to request response fields
        request->Completion.ResponseBufferLength = ResponseBufferLength;

        if (0 == request->Completion.ResponseBufferLength)
        {
            BLORGFS_PRINT("ResponseBuffer has an empty buffer\n");
            return FALSE;
        }

        request->Completion.ResponseBuffer = ExAllocatePoolUninitialized(PagedPool, request->Completion.ResponseBufferLength, 'ipcr');

        if (!request->Completion.ResponseBuffer)
        {
            BLORGFS_PRINT("ResponseBuffer local buffer could not be allocated\n");
            return FALSE;
        }

        RtlCopyMemory(request->Completion.ResponseBuffer, systemBuffer->Payload.ResponseBuffer, ResponseBufferLength);

        request->Completion.Status = STATUS_SUCCESS;
        KeSetEvent(&request->CompletionEvent, 0, FALSE);
        return TRUE;
    }
    else
    {
        BLORGFS_PRINT("Response request ID likely timed out %lu\n", systemBuffer->Header.RequestId);
        return FALSE;
    }
}

NTSTATUS ProcessControlRequest(PIRP Irp)
{
    PLIST_ENTRY entry = RemoveRequestFromQueue();

    if (entry)
    {
        PINVERTED_CALL_DATA_REQUEST request = CONTAINING_RECORD(entry, INVERTED_CALL_DATA_REQUEST, ListEntry);

        PBLORGFS_INVERTED_CALL systemBuffer = (Irp->MdlAddress) ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) : NULL;

        if (!systemBuffer)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        systemBuffer->Header.RequestId = request->RequestId;
        systemBuffer->InvertedCallType = request->InvertedCallType;
        systemBuffer->PathLength = request->PathLength;

        RtlCopyMemory(systemBuffer->Path, request->Path, request->PathLength);

        Irp->IoStatus.Information = UFIELD_OFFSET(BLORGFS_INVERTED_CALL, PathLength) + request->PathLength;

        return STATUS_SUCCESS;
    }
    else
    {
        IoCsqInsertIrp(&g_InvertedCallHandlerCsq, Irp, NULL);
        return STATUS_PENDING;
    }
}

NTSTATUS InitializeInvertedCallHandler(void)
{
    KeInitializeSpinLock(&g_InvertedCallHandlerIrpQueueSpinLock);
    InitializeListHead(&g_InvertedCallHandlerIrpQueue);

    KeInitializeGuardedMutex(&g_InvertedCallHandlerRequestMutex);
    InitializeListHead(&g_InvertedCallHandlerRequestQueue);

    return IoCsqInitialize(&g_InvertedCallHandlerCsq,
        InvertedCallHandlerCsqInsertIrp,
        InvertedCallHandlerCsqRemoveIrp,
        InvertedCallHandlerCsqPeekNextIrp,
        InvertedCallHandlerCsqAcquireLock,
        InvertedCallHandlerCsqReleaseLock,
        InvertedCallHandlerCsqCompleteCanceledIrp);
}
