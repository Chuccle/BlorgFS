//
// InvertedTest.c
//
// Win32 console mode program to fire up requests to the
// Inverted Example driver using Thread Pool API.
//
#define _CRT_SECURE_NO_WARNINGS  

#include <cstdio>
#include <cstdlib>
#include <Windows.h>
#include <memory>
#include "..\BlorgFSCTL.h"

constexpr DWORD MAX_REQUESTS = 2;

struct OVL_WRAPPER
{
    OVERLAPPED  Overlapped;
    BLORGFS_INVERTED_CALL foo;
};

HANDLE driverHandle;
PTP_IO pTpIo;

inline static bool CreateInvertedCallRequest(void)
{
    StartThreadpoolIo(pTpIo);

    auto wrap = std::make_unique<OVL_WRAPPER>();

    std::memset(wrap.get(), 0, sizeof(OVL_WRAPPER));

    BOOL result = DeviceIoControl(driverHandle,
        static_cast<DWORD>(FSCTL_BLORGFS_INVERTED_CALL),
        nullptr,
        0,
        &wrap->foo,
        sizeof(BLORGFS_INVERTED_CALL),
        nullptr,
        &wrap->Overlapped);

    DWORD code = GetLastError();

    if (result || code == ERROR_IO_PENDING)
    {
        wrap.release(); // wrap ownership to the thread pool I/O
        return true;
    }
    else
    {
        // Operation failed immediately
        printf("DeviceIoControl failed with error 0x%lx\n", code);
        // Cancel the thread pool I/O since the operation failed
        CancelThreadpoolIo(pTpIo);
        return false;
    }
}

VOID CALLBACK IoCompletionCallback(
    PTP_CALLBACK_INSTANCE Instance,
    PVOID Context,
    PVOID Overlapped,
    ULONG IoResult,
    ULONG_PTR NumberOfBytesTransferred,
    PTP_IO Io
)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Io);

    if (Overlapped == nullptr)
    {
        return;
    }

    auto wrap = static_cast<OVL_WRAPPER*>(Overlapped);

    if (IoResult == NO_ERROR)
    {
        if (NumberOfBytesTransferred == 0)
        {
            // Handle case where no bytes were transferred
            printf("I/O completed with no data transferred\n");


        }
        else
        {
            // Process the completed I/O operation
            switch (wrap->foo.InvertedCallType)
            {
                case InvertedCallTypeListDirectory:
                {

                    break;
                }
                case InvertedCallTypeQueryFile:
                {

                    break;
                }
                case InvertedCallTypeReadFile:
                {

                    break;
                }
                default:
                {

                    break;
                }
            }

            printf("I/O completed successfully, %llu bytes transferred\n",
                NumberOfBytesTransferred);

        }
    }
    else
    {
        printf("I/O completed with error 0x%lx\n", IoResult);
    }

    // always send another I/O request
    if (!CreateInvertedCallRequest())
    {
        printf("Could not create an inverted call request\n");
    }

    // Clean up the wrapper structure
    delete wrap;
}

int main()
{
    DWORD code;

    //
    // Open the Inverted device by name
    //
    driverHandle = CreateFile(LR"(\\.\BlorgFS)", // Name of the device to open
        GENERIC_READ | GENERIC_WRITE,  // Access rights requested
        0,                           // Share access - NONE
        nullptr,                     // Security attributes - not used!
        OPEN_EXISTING,               // Device must exist to open it.
        FILE_FLAG_OVERLAPPED,        // Open for overlapped I/O
        nullptr);                    // extended attributes - not used!

    if (driverHandle == INVALID_HANDLE_VALUE)
    {
        code = GetLastError();
        printf("CreateFile failed with error 0x%lx\n", code);
        return(code);
    }

    //
    // Create a thread pool I/O object
    //
    pTpIo = CreateThreadpoolIo(driverHandle,
        IoCompletionCallback,
        nullptr,  // Context
        nullptr); // Environment

    if (pTpIo == nullptr)
    {
        code = GetLastError();
        printf("CreateThreadpoolIo failed with error 0x%lx\n", code);
        CloseHandle(driverHandle);
        return(code);
    }

    printf("Thread pool I/O initialized. Starting operations...\n");

    for  (int i = 0; i < MAX_REQUESTS; ++i)
    {
        if (!CreateInvertedCallRequest())
        {
            printf("Could not create an inverted call request\n");
            break;
        }
    }

    while (1)
    {
        Sleep(5000); // Keep main thread alive to allow I/O callbacks to process
    }

    // Cleanup
    if (pTpIo)
    {
        WaitForThreadpoolIoCallbacks(pTpIo, FALSE);
        CloseThreadpoolIo(pTpIo);
    }

    CloseHandle(driverHandle);
    return 0;
}