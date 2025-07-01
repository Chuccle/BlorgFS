#include "Driver.h"

#define INVERTED_CALL_TIMEOUT_SECONDS 10
#define INVERTED_CALL_TIMEOUT (INVERTED_CALL_TIMEOUT_SECONDS * -10000000LL)

KDEFERRED_ROUTINE InvertedCallTimeoutDpcRoutine;

static LONG g_RequestId = 0xFFFFFFFE;

IO_CSQ g_InvertedCallHandlerCsq;

KSPIN_LOCK g_InvertedCallHandlerIrpQueueSpinLock;
LIST_ENTRY g_InvertedCallHandlerIrpQueue;

// this may need to be changed to a spinlock because it is used in DPC context
KSPIN_LOCK g_InvertedCallHandlerRequestSpinLock;
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

_IRQL_raises_(DISPATCH_LEVEL)
VOID InvertedCallHandlerCsqAcquireLock(IO_CSQ* Csq, _At_(*Irql, _IRQL_saves_) PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&g_InvertedCallHandlerIrpQueueSpinLock, Irql);
}

_IRQL_requires_(DISPATCH_LEVEL)
VOID InvertedCallHandlerCsqReleaseLock(IO_CSQ* Csq, _IRQL_restores_ KIRQL Irql)
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
   KIRQL oldIrql;
   KeAcquireSpinLock(&g_InvertedCallHandlerRequestSpinLock, &oldIrql);
   if (!IsListEmpty(&g_InvertedCallHandlerRequestQueue))
   {
       entry = RemoveHeadList(&g_InvertedCallHandlerRequestQueue);
   }
   KeReleaseSpinLock(&g_InvertedCallHandlerRequestSpinLock, oldIrql);
   return entry;
}

static inline void InsertRequestIntoQueue(PLIST_ENTRY ListEntry)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_InvertedCallHandlerRequestSpinLock, &oldIrql);
    InsertTailList(&g_InvertedCallHandlerRequestQueue, ListEntry);
    KeReleaseSpinLock(&g_InvertedCallHandlerRequestSpinLock, oldIrql);
}

static inline PINVERTED_CALL_DATA_REQUEST RemoveRequestFromQueueById(
    ULONG RequestId
)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_InvertedCallHandlerRequestSpinLock, &oldIrql);
    for (PLIST_ENTRY listEntry = g_InvertedCallHandlerRequestQueue.Flink;
        listEntry != &g_InvertedCallHandlerRequestQueue;
        listEntry = listEntry->Flink)
    {
        PINVERTED_CALL_DATA_REQUEST request = CONTAINING_RECORD(listEntry, INVERTED_CALL_DATA_REQUEST, ListEntry);

        if (RequestId == request->RequestId)
        {
            RemoveEntryList(listEntry);
            KeReleaseSpinLock(&g_InvertedCallHandlerRequestSpinLock, oldIrql);
            return request;

        }
    }
    KeReleaseSpinLock(&g_InvertedCallHandlerRequestSpinLock, oldIrql);
    return NULL;
}

VOID InvertedCallTimeoutDpcRoutine(
    _In_ struct _KDPC* Dpc,
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
        request->Completion.Status = STATUS_UNSUCCESSFUL;
        KeSetEvent(&request->CompletionEvent, EVENT_INCREMENT, FALSE);
    }
}

// Requires IRP to be in FSP context and pending
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

    PINVERTED_CALL_DATA_REQUEST request = ExAllocatePoolZero(NonPagedPoolNx, sizeof(INVERTED_CALL_DATA_REQUEST) + Path->Length, 'ICRS');

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
        PBLORGFS_TRANSACT systemBuffer = (irp->MdlAddress) ? MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) : NULL;

        if (!systemBuffer)
        {
            IoCsqInsertIrp(&g_InvertedCallHandlerCsq, irp, NULL);
            ExFreePool(request);
            return STATUS_INVALID_PARAMETER;
        }

        systemBuffer->Header.RequestId = request->RequestId;
        systemBuffer->InvertedCallType = InvertedCallType;
        systemBuffer->PathLength = request->PathLength;

        RtlCopyMemory(systemBuffer->Path, request->Path, request->PathLength);

        switch (InvertedCallType)
        {
            case InvertedCallTypeReadFile:
            {
               // systemBuffer->Context.ReadFile.StartOffset = ...
                // systemBuffer->Context.ReadFile.Length = ...
                break;
            }

            default:
            { 
                break;
            }
        }

        irp->IoStatus.Information = UFIELD_OFFSET(BLORGFS_TRANSACT, PathLength) + request->PathLength;

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

BOOLEAN ProcessControlResponse(const PBLORGFS_TRANSACT SystemBuffer, ULONG ResponseBufferLength)
{
    PINVERTED_CALL_DATA_REQUEST request = RemoveRequestFromQueueById(SystemBuffer->Header.RequestId);

    if (request)
    {
        // Process the response and copy data over to request response fields
        request->Completion.Status = SystemBuffer->Payload.Status;

        if (!NT_SUCCESS(request->Completion.Status))
        {
            BLORGFS_PRINT("ResponseBuffer has an empty buffer\n");
            KeSetEvent(&request->CompletionEvent, 0, FALSE);
            return FALSE;
        }
        
        // validate the response buffer length with the expected length based on the request type
        switch (request->InvertedCallType)
        {
            case InvertedCallTypeListDirectory:
            {
                PDIRECTORY_INFO dirInfo = (PDIRECTORY_INFO)SystemBuffer->Payload.ResponseBuffer;

                ULONG dirInfoHeader = sizeof(DIRECTORY_INFO);

                if (ResponseBufferLength < dirInfoHeader)
                {
                    BLORGFS_PRINT("ResponseBuffer is smalle\n");
                    KeSetEvent(&request->CompletionEvent, EVENT_INCREMENT, FALSE);
                    return FALSE;
                }
                
                ULONG filesEntryArraySize = (ULONG)dirInfo->FileCount * sizeof(DIRECTORY_FILE_METADATA);
                ULONG subDirArraySize = (ULONG)dirInfo->SubDirCount * sizeof(DIRECTORY_SUBDIR_METADATA);

                ULONG expectedLength = dirInfoHeader + filesEntryArraySize + subDirArraySize;

                if (ResponseBufferLength < expectedLength)
                {
                    BLORGFS_PRINT("ResponseBuffer has an empty buffer\n");
                    KeSetEvent(&request->CompletionEvent, EVENT_INCREMENT, FALSE);
                    return FALSE;
                }

                break;
            }
        }

        request->Completion.ResponseBufferLength = ResponseBufferLength;

        request->Completion.ResponseBuffer = ExAllocatePoolUninitialized(PagedPool, ResponseBufferLength, 'ipcr');

        if (!request->Completion.ResponseBuffer)
        {
            BLORGFS_PRINT("ResponseBuffer local buffer could not be allocated\n");
            KeSetEvent(&request->CompletionEvent, 0, FALSE);
            return FALSE;
        }

        RtlCopyMemory(request->Completion.ResponseBuffer, SystemBuffer->Payload.ResponseBuffer, ResponseBufferLength);

        KeSetEvent(&request->CompletionEvent, EVENT_INCREMENT, FALSE);
        return TRUE;
    }
    else
    {
        BLORGFS_PRINT("Response request ID likely timed out %lu\n", SystemBuffer->Header.RequestId);
        return FALSE;
    }
}

NTSTATUS ProcessControlRequest(PIRP Irp)
{
    PLIST_ENTRY entry = RemoveRequestFromQueue();

    if (entry)
    {
        PINVERTED_CALL_DATA_REQUEST request = CONTAINING_RECORD(entry, INVERTED_CALL_DATA_REQUEST, ListEntry);

        PBLORGFS_TRANSACT systemBuffer = (Irp->MdlAddress) ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute) : NULL;

        if (!systemBuffer)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        systemBuffer->Header.RequestId = request->RequestId;
        systemBuffer->InvertedCallType = request->InvertedCallType;
        systemBuffer->PathLength = request->PathLength;

        RtlCopyMemory(systemBuffer->Path, request->Path, request->PathLength);

        Irp->IoStatus.Information = UFIELD_OFFSET(BLORGFS_TRANSACT, PathLength) + request->PathLength;

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

    KeInitializeSpinLock(&g_InvertedCallHandlerRequestSpinLock);
    InitializeListHead(&g_InvertedCallHandlerRequestQueue);

    return IoCsqInitialize(&g_InvertedCallHandlerCsq,
        InvertedCallHandlerCsqInsertIrp,
        InvertedCallHandlerCsqRemoveIrp,
        InvertedCallHandlerCsqPeekNextIrp,
        InvertedCallHandlerCsqAcquireLock,
        InvertedCallHandlerCsqReleaseLock,
        InvertedCallHandlerCsqCompleteCanceledIrp);
}

void FreeDirectoryInfo(PDIRECTORY_INFO DirInfo)
{
    if (DirInfo)
    {
        ExFreePool(DirInfo);
    }
}