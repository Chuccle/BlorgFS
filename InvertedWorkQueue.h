#pragma once

typedef struct _INVERTED_CALL_DATA_REQUEST
{
    ULONG RequestId;
    enum InvertedCallType InvertedCallType;
    struct
    {
        NTSTATUS Status;
        ULONG ResponseBufferLength;
        PVOID ResponseBuffer;
    } Completion;
    KEVENT CompletionEvent;
    LIST_ENTRY ListEntry;
    ULONG PathLength;
    WCHAR Path[];
} INVERTED_CALL_DATA_REQUEST, * PINVERTED_CALL_DATA_REQUEST;


NTSTATUS InitializeInvertedCallHandler(void);

NTSTATUS CreateInvertedCallRequest(
    enum InvertedCallType InvertedCallType,
    PCUNICODE_STRING Path,
    PINVERTED_CALL_DATA_REQUEST* OutRequest
);

void CleanupInvertedCallRequest(PINVERTED_CALL_DATA_REQUEST Request);

void FreeDirectoryInfo(PDIRECTORY_INFO DirInfo);

NTSTATUS ProcessControlRequest(PIRP Irp);

BOOLEAN ProcessControlResponse(const PBLORGFS_TRANSACT SystemBuffer, ULONG BufferLength);