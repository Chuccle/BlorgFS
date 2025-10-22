#pragma once

struct _INVERTED_CALL_DATA_REQUEST;

typedef void BLORG_TRANSACTION_COMPLETION_ROUTINE(
    struct _INVERTED_CALL_DATA_REQUEST* Request,
    struct _BLORGFS_TRANSACT* TransactionData,
    ULONG ResponseBufferLength
    );

typedef BLORG_TRANSACTION_COMPLETION_ROUTINE* PBLORG_TRANSACTION_COMPLETION_ROUTINE;

typedef struct _INVERTED_CALL_DATA_REQUEST
{
    LIST_ENTRY ListEntry;
    LARGE_INTEGER ExpiryTime;
    PIRP OriginalIrp;
    PBLORG_TRANSACTION_COMPLETION_ROUTINE CompletionRoutine;
    PVOID ResponseBuffer;
    ULONG ResponseBufferLength;
    ULONG RequestId;
    enum InvertedCallType InvertedCallType;
    NTSTATUS Status;
    ULONG PathLength;
    WCHAR PathBuffer[];
} INVERTED_CALL_DATA_REQUEST, * PINVERTED_CALL_DATA_REQUEST;

NTSTATUS InitializeInvertedCallHandler(void);

NTSTATUS CreateInvertedCallRequest(
    enum InvertedCallType InvertedCallType,
    PCUNICODE_STRING Path,
    PIRP OriginalIrp,
    PBLORG_TRANSACTION_COMPLETION_ROUTINE CompletionRoutine,
    const union TRANSACTION_CONTEXT* TransactionContext
);

void CleanupInvertedCallRequest(PINVERTED_CALL_DATA_REQUEST Request);

void FreeDirectoryInfo(PDIRECTORY_INFO DirInfo);

NTSTATUS ProcessControlRequest(PIRP Irp);

BOOLEAN ProcessControlResponse(const PBLORGFS_TRANSACT SystemBuffer, ULONG BufferLength);