#include "Driver.h"
#include "Socket.h"

#define SOCKET_TAG 'HTTP'

WSK_REGISTRATION WskRegistration;
WSK_PROVIDER_NPI WskProviderNpi;

IO_COMPLETION_ROUTINE SocketContextCompletionRoutine;

const WSK_CLIENT_DISPATCH WskAppDispatch =
{
    MAKE_WSK_VERSION(1,0), // Use WSK version 1.0
    0,    // Reserved
    NULL  // WskClientEvent callback not required for WSK version 1.0
};

NTSTATUS SocketContextCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (!Context)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeSetEvent((PKEVENT)Context, EVENT_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS InitialiseSocketContext(PKSOCKET_CONTEXT SocketContext)
{
    KeInitializeEvent(
        &SocketContext->CompletionEvent,
        NotificationEvent,
        FALSE
    );

    SocketContext->Irp = IoAllocateIrp(1, FALSE);

    if (!SocketContext->Irp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoSetCompletionRoutine(
        SocketContext->Irp,
        &SocketContextCompletionRoutine,
        &SocketContext->CompletionEvent,
        TRUE,
        TRUE,
        TRUE
    );

    return STATUS_SUCCESS;
}

static void FreeSocketContext(PKSOCKET_CONTEXT SocketContext)
{
    IoFreeIrp(SocketContext->Irp);
}

static void ReuseSocketContext(PKSOCKET_CONTEXT SocketContext)
{
    //
    // If the WSK application allocated the IRP, or is reusing an IRP
    // that it previously allocated, then it must set an IoCompletion
    // routine for the IRP before calling a WSK function.  In this
    // situation, the WSK application must specify TRUE for the
    // InvokeOnSuccess, InvokeOnError, and InvokeOnCancel parameters that
    // are passed to the IoSetCompletionRoutine function to ensure that
    // the IoCompletion routine is always called. Furthermore, the IoCompletion
    // routine that is set for the IRP must always return
    // STATUS_MORE_PROCESSING_REQUIRED to terminate the completion processing
    // of the IRP.  If the WSK application is done using the IRP after the
    // IoCompletion routine has been called, then it should call the IoFreeIrp
    // function to free the IRP before returning from the IoCompletion routine.
    // If the WSK application does not free the IRP then it can reuse the IRP
    // for a call to another WSK function.
    //
    // (ref: https://docs.microsoft.com/en-us/windows-hardware/drivers/network/using-irps-with-winsock-kernel-functions)
    //

    KeClearEvent(&SocketContext->CompletionEvent);

    IoReuseIrp(SocketContext->Irp, STATUS_UNSUCCESSFUL);

    IoSetCompletionRoutine(
        SocketContext->Irp,
        &SocketContextCompletionRoutine,
        &SocketContext->CompletionEvent,
        TRUE,
        TRUE,
        TRUE
    );
}

static void WaitForCompletionSocketContext(PKSOCKET_CONTEXT SocketContext, PNTSTATUS Status)
{
    if (*Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(
            &SocketContext->CompletionEvent,
            Executive,
            KernelMode,
            FALSE,
            NULL
        );

        *Status = SocketContext->Irp->IoStatus.Status;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////

NTSTATUS InitialiseWskClient(void)
{
    WSK_CLIENT_NPI wskClientNpi =
    {
        .ClientContext = NULL,
        .Dispatch = &WskAppDispatch
    };

    NTSTATUS result = WskRegister(&wskClientNpi, &WskRegistration);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("WSK Registration Failed: 0x%X\n", result);
        return result;
    }

    result = WskCaptureProviderNPI(&WskRegistration, WSK_INFINITE_WAIT, &WskProviderNpi);

    if (!NT_SUCCESS(result))
    {
        WskDeregister(&WskRegistration);
        BLORGFS_PRINT("WSK Provider Capture Failed: 0x%X\n", result);
        return result;
    }

    return STATUS_SUCCESS;
}

void CleanupWskClient(void)
{
    WskReleaseProviderNPI(&WskRegistration);
    WskDeregister(&WskRegistration);
}

NTSTATUS GetWskAddrInfo(PUNICODE_STRING NodeName, PUNICODE_STRING ServiceName, const PADDRINFOEXW Hints, PADDRINFOEXW* RemoteAddrInfo)
{
    KSOCKET_CONTEXT socketContext;

    NTSTATUS result = InitialiseSocketContext(&socketContext);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_PRINT("Failed InitialiseSocketContext(): 0x%X\n", result);
        return result;
    }

    result = WskProviderNpi.Dispatch->WskGetAddressInfo(
        WskProviderNpi.Client,
        NodeName,
        ServiceName,
        0,
        NULL,
        Hints,
        RemoteAddrInfo,
        NULL,
        NULL,
        socketContext.Irp
    );

    WaitForCompletionSocketContext(&socketContext, &result);

    FreeSocketContext(&socketContext);

    return result;
}

void FreeWskAddrInfo(PADDRINFOEXW AddrInfo)
{
    WskProviderNpi.Dispatch->WskFreeAddressInfo(
        WskProviderNpi.Client,
        AddrInfo
    );
}

NTSTATUS CreateWskSocket(PKSOCKET* Socket, USHORT SocketType, ULONG Protocol, ULONG Flags, PSOCKADDR RemoteAddress)
{
    *Socket = NULL;

    PKSOCKET newSocket = ExAllocatePoolZero(NonPagedPoolNx, sizeof(KSOCKET), SOCKET_TAG);

    if (!newSocket)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NTSTATUS result = InitialiseSocketContext(&newSocket->SocketContext);

    if (!NT_SUCCESS(result))
    {
        return result;
    }

    SOCKADDR_IN localAddress =
    {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = 0
    };

    result = WskProviderNpi.Dispatch->WskSocketConnect(
        WskProviderNpi.Client,
        SocketType,
        Protocol,
        (PSOCKADDR)&localAddress,
        RemoteAddress,
        Flags,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        newSocket->SocketContext.Irp
    );

    WaitForCompletionSocketContext(&newSocket->SocketContext, &result);

    if (NT_SUCCESS(result))
    {
        newSocket->WskSocket = (PWSK_SOCKET)newSocket->SocketContext.Irp->IoStatus.Information;
        newSocket->WskDispatch = (PVOID)newSocket->WskSocket->Dispatch;

        *Socket = newSocket;
    }

    return result;
}

NTSTATUS CloseWskSocket(PKSOCKET Socket)
{
    ReuseSocketContext(&Socket->SocketContext);

    NTSTATUS result = Socket->WskConnectionDispatch->WskCloseSocket(
        Socket->WskSocket,
        Socket->SocketContext.Irp
    );

    WaitForCompletionSocketContext(&Socket->SocketContext, &result);

    FreeSocketContext(&Socket->SocketContext);

    ExFreePool(Socket);

    return result;
}

NTSTATUS SendRecvWsk(PKSOCKET Socket, PVOID Buffer, ULONG Length, PULONG BytesWritten, ULONG Flags, BOOLEAN Send)
{
    NTSTATUS result = STATUS_SUCCESS;

    WSK_BUF wskBuffer =
    {
        .Offset = 0,
        .Length = Length,
        .Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL)
    };

    __try
    {
        MmProbeAndLockPages(wskBuffer.Mdl, KernelMode, IoWriteAccess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        IoFreeMdl(wskBuffer.Mdl);
        return GetExceptionCode();
    }

    ReuseSocketContext(&Socket->SocketContext);

    if (Send)
    {
        result = Socket->WskConnectionDispatch->WskSend(
            Socket->WskSocket,
            &wskBuffer,
            Flags,
            Socket->SocketContext.Irp
        );
    }
    else
    {
        result = Socket->WskConnectionDispatch->WskReceive(
            Socket->WskSocket,
            &wskBuffer,
            Flags,
            Socket->SocketContext.Irp
        );
    }

    WaitForCompletionSocketContext(&Socket->SocketContext, &result);

    if (NT_SUCCESS(result))
    {
        if (BytesWritten)
        {
            *BytesWritten = (ULONG)Socket->SocketContext.Irp->IoStatus.Information;
        }
    }

    MmUnlockPages(wskBuffer.Mdl);
    IoFreeMdl(wskBuffer.Mdl);
    return result;
}