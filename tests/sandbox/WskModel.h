#pragma once

//
// A WSK provider the test drives, so the real Socket.c can be compiled
// and run.
//
// Socket.c is where the per-operation watchdog lives: arm a timer, issue
// the operation, and let a refcount arbitrate between the completion
// routine and the timeout DPC, either of which may free the shared
// context. That protocol is exactly the kind of code review can only
// argue about -- it needs both orderings executed.
//
// The provider therefore lets a test say precisely how each operation
// completes:
//
//   Inline      the completion runs before WskSend/WskReceive returns,
//               which is what builds the deep synchronous chains the
//               client's stack-expansion logic exists for
//   Deferred    completes when the test releases it, so the test can
//               interleave something else first
//   Never       never completes, so the watchdog timer is the only thing
//               that can end the operation -- the case that proves
//               timeout handling works
//
// and, for the race that matters, to release a deferred completion at the
// same instant the clock is advanced past the timer's due time.
//

#include "NtShim.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////
// WSK types Socket.c uses
///////////////////////////////////////////////////////////////////////////

typedef struct _WSK_CLIENT WSK_CLIENT, * PWSK_CLIENT;

typedef struct _WSK_BUF
{
    PMDL Mdl;
    ULONG Offset;
    SIZE_T Length;
} WSK_BUF, * PWSK_BUF;

typedef struct _WSK_SOCKET WSK_SOCKET, * PWSK_SOCKET;

typedef NTSTATUS (*PFN_WSK_SEND)(PWSK_SOCKET Socket, PWSK_BUF Buffer, ULONG Flags, PIRP Irp);
typedef NTSTATUS (*PFN_WSK_RECEIVE)(PWSK_SOCKET Socket, PWSK_BUF Buffer, ULONG Flags, PIRP Irp);
typedef NTSTATUS (*PFN_WSK_CLOSE_SOCKET)(PWSK_SOCKET Socket, PIRP Irp);

typedef struct _WSK_PROVIDER_CONNECTION_DISPATCH
{
    PFN_WSK_CLOSE_SOCKET WskCloseSocket;
    PFN_WSK_SEND WskSend;
    PFN_WSK_RECEIVE WskReceive;
} WSK_PROVIDER_CONNECTION_DISPATCH, * PWSK_PROVIDER_CONNECTION_DISPATCH;

typedef WSK_PROVIDER_CONNECTION_DISPATCH WSK_PROVIDER_LISTEN_DISPATCH, * PWSK_PROVIDER_LISTEN_DISPATCH;
typedef WSK_PROVIDER_CONNECTION_DISPATCH WSK_PROVIDER_DATAGRAM_DISPATCH, * PWSK_PROVIDER_DATAGRAM_DISPATCH;
typedef WSK_PROVIDER_CONNECTION_DISPATCH WSK_PROVIDER_STREAM_DISPATCH, * PWSK_PROVIDER_STREAM_DISPATCH;

struct _WSK_SOCKET
{
    const WSK_PROVIDER_CONNECTION_DISPATCH* Dispatch;
    struct _WSK_MODEL_CONNECTION* Connection;
};

typedef NTSTATUS (*PFN_WSK_SOCKET_CONNECT)(
    PWSK_CLIENT Client, USHORT SocketType, ULONG Protocol,
    PSOCKADDR LocalAddress, PSOCKADDR RemoteAddress,
    ULONG Flags, PVOID SocketContext, const VOID* Dispatch,
    PVOID OwningProcess, PVOID OwningThread, PVOID SecurityDescriptor, PIRP Irp);

typedef NTSTATUS (*PFN_WSK_GET_ADDRESS_INFO)(
    PWSK_CLIENT Client, PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName,
    ULONG NameSpace, void* Provider, PADDRINFOEXW Hints, PADDRINFOEXW* Result,
    PVOID OwningProcess, PVOID OwningThread, PIRP Irp);

typedef VOID (*PFN_WSK_FREE_ADDRESS_INFO)(PWSK_CLIENT Client, PADDRINFOEXW AddrInfo);

typedef struct _WSK_PROVIDER_DISPATCH
{
    PFN_WSK_SOCKET_CONNECT WskSocketConnect;
    PFN_WSK_GET_ADDRESS_INFO WskGetAddressInfo;
    PFN_WSK_FREE_ADDRESS_INFO WskFreeAddressInfo;
} WSK_PROVIDER_DISPATCH, * PWSK_PROVIDER_DISPATCH;

typedef struct _WSK_PROVIDER_NPI
{
    PWSK_CLIENT Client;
    const WSK_PROVIDER_DISPATCH* Dispatch;
} WSK_PROVIDER_NPI, * PWSK_PROVIDER_NPI;

typedef struct _WSK_REGISTRATION { PVOID Reserved; } WSK_REGISTRATION, * PWSK_REGISTRATION;

typedef struct _WSK_CLIENT_DISPATCH
{
    USHORT Version;
    USHORT Reserved;
    PVOID WskClientEvent;
} WSK_CLIENT_DISPATCH, * PWSK_CLIENT_DISPATCH;

typedef struct _WSK_CLIENT_NPI
{
    PVOID ClientContext;
    const WSK_CLIENT_DISPATCH* Dispatch;
} WSK_CLIENT_NPI, * PWSK_CLIENT_NPI;

#define MAKE_WSK_VERSION(major, minor) ((USHORT)(((major) << 8) | (minor)))
#define WSK_INFINITE_WAIT 0xFFFFFFFF

#define WSK_FLAG_NODELAY 0x00000002
#define WSK_FLAG_WAITALL 0x00000010

NTSTATUS WskRegister(PWSK_CLIENT_NPI ClientNpi, PWSK_REGISTRATION Registration);
NTSTATUS WskCaptureProviderNPI(PWSK_REGISTRATION Registration, ULONG WaitTimeout, PWSK_PROVIDER_NPI ProviderNpi);
VOID WskReleaseProviderNPI(PWSK_REGISTRATION Registration);
VOID WskDeregister(PWSK_REGISTRATION Registration);

///////////////////////////////////////////////////////////////////////////
// Test control
///////////////////////////////////////////////////////////////////////////

typedef enum _WSK_MODEL_COMPLETION
{
    // Complete before the issuing call returns.
    WskModelInline = 0,

    // Complete when WskModelReleaseDeferred runs.
    WskModelDeferred,

    //
    // Never complete on its own. The only way the operation ends is the
    // driver's own watchdog cancelling it -- which is the point.
    //
    WskModelNever
} WSK_MODEL_COMPLETION;

typedef struct _WSK_MODEL_BEHAVIOUR
{
    WSK_MODEL_COMPLETION Completion;

    // Status the operation completes with.
    NTSTATUS Status;

    //
    // Bytes transferred. For a receive, this many bytes of Payload are
    // copied into the caller's buffer; capped at what was asked for.
    //
    SIZE_T Bytes;

    const unsigned char* Payload;
    SIZE_T PayloadLength;
} WSK_MODEL_BEHAVIOUR;

// Behaviour for the next send / receive / connect / close respectively.
VOID WskModelSetSendBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour);
VOID WskModelSetReceiveBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour);
VOID WskModelSetConnectBehaviour(const WSK_MODEL_BEHAVIOUR* Behaviour);

//
// Snapshot of the bytes most recently handed to WskSend, captured at issue
// time (not completion time) -- so it is available even while that send is
// still WskModelDeferred, before the test has decided how it completes.
// This is what lets a test that is scripting the *other side* of a
// protocol (e.g. TlsHandshakeKernelTest.cpp acting as the TLS server) read
// what the driver actually put on the wire -- a ClientHello's random and
// key share are generated inside the driver, not known to the test in
// advance. Truncated, not failed, past the capture buffer's capacity;
// callers that need more than that should be suspicious of the message
// size, not the capture.
//
const unsigned char* WskModelLastSendBytes(SIZE_T* LengthOut);

//
// Completes everything currently deferred, at DISPATCH_LEVEL, the way a
// real transport's DPC would.
//
int WskModelReleaseDeferred(VOID);

int WskModelDeferredCount(VOID);

//
// Counters a test asserts on.
//
ULONG WskModelConnects(VOID);
ULONG WskModelSends(VOID);
ULONG WskModelReceives(VOID);
ULONG WskModelCloses(VOID);

//
// Operations the driver cancelled out from under the transport. A
// watchdog firing shows up here, and the model completes the IRP
// STATUS_CANCELLED on the next pump exactly as WSK does.
//
ULONG WskModelCancelled(VOID);

//
// Completes any outstanding operation whose IRP has been cancelled. This
// is the transport noticing the cancel -- in the kernel it is
// asynchronous, so the test drives it explicitly and can choose to do it
// before or after other events.
//
int WskModelPumpCancellations(VOID);

VOID WskModelReset(VOID);

#ifdef __cplusplus
}
#endif
