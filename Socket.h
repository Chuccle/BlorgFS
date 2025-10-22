#pragma once

typedef struct _KSOCKET_CONTEXT
{
    PIRP Irp;
    KEVENT CompletionEvent;
} KSOCKET_CONTEXT, * PKSOCKET_CONTEXT;

typedef struct _KSOCKET
{
    PWSK_SOCKET	WskSocket;

    union
    {
        PVOID WskDispatch;

        PWSK_PROVIDER_CONNECTION_DISPATCH WskConnectionDispatch;
        PWSK_PROVIDER_LISTEN_DISPATCH WskListenDispatch;
        PWSK_PROVIDER_DATAGRAM_DISPATCH WskDatagramDispatch;
#if (NTDDI_VERSION >= NTDDI_WIN10_RS2)
        PWSK_PROVIDER_STREAM_DISPATCH WskStreamDispatch;
#endif
    };

    KSOCKET_CONTEXT SocketContext;
} KSOCKET, * PKSOCKET;

NTSTATUS InitialiseWskClient(void);
void CleanupWskClient(void);

NTSTATUS GetWskAddrInfo(const UNICODE_STRING* NodeName, const UNICODE_STRING* ServiceName, const ADDRINFOEXW* Hints, PADDRINFOEXW* RemoteAddrInfo);
void FreeWskAddrInfo(PADDRINFOEXW AddrInfo);

NTSTATUS CreateWskSocket(PKSOCKET* Socket, USHORT SocketType, ULONG Protocol, ULONG Flags, PSOCKADDR RemoteAddress);
NTSTATUS CloseWskSocket(PKSOCKET Socket);

NTSTATUS SendRecvWsk(PKSOCKET Socket, PVOID Buffer, ULONG Length, PULONG BytesWritten, ULONG Flags, BOOLEAN Send);