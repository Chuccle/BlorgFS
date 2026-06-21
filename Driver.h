#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <wsk.h>

#include <limits.h>
#define C_CAST(T, expr) ((T)(expr))

#define CACHE_LINE_SIZE 64

//
// BLORGFS_KERNEL_BUILD (set in BlorgFS.vcxproj) tells Tls.h that
// ntifs.h/wsk.h are already visible, so it skips the <windows.h> it
// needs in the usermode TlsTest/TlsHandshakeTest/TlsFuzzTest harnesses
// instead. Included early, ahead of Socket.h (which embeds
// TLS_CONNECTION_STATE in KSOCKET).
//
#include "Tls.h"

//
// Cc's read-ahead granularity for cached reads (Read.c). Also the basis
// for PREFETCH_CHUNK (Prefetch.h): Cc clusters read-ahead into a paging
// IRP sized at a multiple of this granularity (observed 2x), so the
// prefetcher's slot size is derived from this constant rather than
// duplicated. If PREFETCH_CHUNK drifts out of step with the actual
// paging-read size, every slot's Length is too small, the hit/park
// check's Length > Hot[i].Length always trips, and every read falls
// through to a direct fetch.
//
#define READ_AHEAD_GRANULARITY (PAGE_SIZE * 64)

#include "Structs.h"
#include "Util.h"
#include "Client.h"
#include "Prefetch.h"
#include "CacheManager.h"
#include "FspWorkQueue.h"

#define BLORGFS_FSDO_STRING  L"\\BlorgFS"
#define BLORGFS_FSDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_FSDO_MAGIC   0xDEAD00D

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgVolume"

#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;WD)"
#define BLORGFS_VDO_MAGIC   0xD3ADBEAF

#define BLORGFS_DDO_STRING  L"\\Device\\BlorgDrive"

#define BLORGFS_DDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;WD)"
#define BLORGFS_DDO_MAGIC  0xD4ADBAA5
#define BLORGFS_DOS_DRIVELETTER_FORMAT_STRING L"\\DosDevices\\%C:"

#define BLORGFS_REG_HOST_MAX_CHARS 128 // 127-char hostname + NUL, with headroom
#define BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES (BLORGFS_REG_HOST_MAX_CHARS + 8) // host plus ":65535" + NUL -- bounds global.RemoteHostAnsi (Client.c)

#ifdef DBG

//
// Runtime log level. The per-IRP trace output below is enormous (one block
// per create/query/read pass) and is itself a synchronous DbgPrint cost, so
// it is gated on this level and silent by default. Raise it live from the
// debugger to enable:
//
//     ed blorgfs!global.LogLevel 1
//

#define BLORGFS_PRINT(...)                                                     \
do                                                                             \
{                                                                              \
    if (global.LogLevel >= 1)                                                 \
    {                                                                          \
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
    }                                                                          \
} while(0)

#define BLORGFS_LOG(...)                                                       \
do                                                                             \
{                                                                              \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
} while(0)

#else

#define BLORGFS_PRINT(...)
#define BLORGFS_LOG(...)

#endif

_Dispatch_type_(IRP_MJ_CREATE)                   DRIVER_DISPATCH BlorgCreate;

_Dispatch_type_(IRP_MJ_CLOSE)                    DRIVER_DISPATCH BlorgClose;
_Dispatch_type_(IRP_MJ_READ)                     DRIVER_DISPATCH BlorgRead;
_Dispatch_type_(IRP_MJ_WRITE)                    DRIVER_DISPATCH BlorgWrite;
_Dispatch_type_(IRP_MJ_QUERY_INFORMATION)        DRIVER_DISPATCH BlorgQueryInformation;
_Dispatch_type_(IRP_MJ_SET_INFORMATION)          DRIVER_DISPATCH BlorgSetInformation;
_Dispatch_type_(IRP_MJ_QUERY_EA)                 DRIVER_DISPATCH BlorgQueryEa;
_Dispatch_type_(IRP_MJ_SET_EA)                   DRIVER_DISPATCH BlorgSetEa;
_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)            DRIVER_DISPATCH BlorgFlushBuffers;
_Dispatch_type_(IRP_MJ_QUERY_VOLUME_INFORMATION) DRIVER_DISPATCH BlorgQueryVolumeInformation;
_Dispatch_type_(IRP_MJ_SET_VOLUME_INFORMATION)   DRIVER_DISPATCH BlorgSetVolumeInformation;
_Dispatch_type_(IRP_MJ_DIRECTORY_CONTROL)        DRIVER_DISPATCH BlorgDirectoryControl;
_Dispatch_type_(IRP_MJ_FILE_SYSTEM_CONTROL)      DRIVER_DISPATCH BlorgFileSystemControl;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)           DRIVER_DISPATCH BlorgDeviceControl;

_Dispatch_type_(IRP_MJ_SHUTDOWN)                 DRIVER_DISPATCH BlorgShutdown;
_Dispatch_type_(IRP_MJ_LOCK_CONTROL)             DRIVER_DISPATCH BlorgLockControl;
_Dispatch_type_(IRP_MJ_CLEANUP)                  DRIVER_DISPATCH BlorgCleanup;

_Dispatch_type_(IRP_MJ_QUERY_SECURITY)           DRIVER_DISPATCH BlorgQuerySecurity;
_Dispatch_type_(IRP_MJ_SET_SECURITY)             DRIVER_DISPATCH BlorgSetSecurity;

NTSTATUS BlorgInitializeSecurityDescriptor(void);
VOID BlorgFreeSecurityDescriptor(void);

NTSTATUS CreateBlorgVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject);

// Driver-wide global state, one instance for the whole driver load.
extern struct GLOBAL
{
    PDRIVER_OBJECT DriverObject;               // this driver's DRIVER_OBJECT
    PDEVICE_OBJECT FileSystemDeviceObject;     // the FSD (control) device object
    PDEVICE_OBJECT DiskDeviceObject;           // the disk device object backing the B: symlink
    PADDRINFOEXW   RemoteAddressInfo;          // resolved backend address/port for the HTTP client
    PSTR RemoteHostAnsi; // ANSI "host[:port]" authority for outgoing Host headers

    //
    //  ANSI hostname (no port) for the ClientHello SNI extension, built in
    //  DriverEntry alongside RemoteHostAnsi. NULL when TLS is disabled at
    //  load, when the configured host is an IPv4/IPv6 literal (RFC 6066
    //  forbids literals in SNI -- see HostStringIsIpLiteral, Driver.c), or
    //  on allocation failure; TlsStartHandshakeAsync omits the extension
    //  in all three cases. NUL-terminated (pool-zero allocated), bounded
    //  by BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES like RemoteHostAnsi.
    //
    PSTR RemoteHostSniAnsi;

    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks; // Cc lazy-write/read-ahead callback table
    PVOID LazyWriteThread;                     // thread pointer Cc supplies to lazy-write callbacks

    //
    //  A single self-relative security descriptor handed out (in the
    //  requested portions) for every IRP_MJ_QUERY_SECURITY. BlorgFS does not
    //  store per-file security -- the volume is a read-only public share --
    //  so one permissive descriptor serves all nodes. Built once in
    //  DriverEntry, freed in DriverUnload.
    //
    PSECURITY_DESCRIPTOR FileSecurityDescriptor;

    //
    //  Master switch for the TLS client (Tls.c/TlsHandshake.c). Defaults to
    //  FALSE, so HttpOnSocket only attempts a TLS handshake on a fresh
    //  connection when this is TRUE.
    //
    //  Settable two ways, which interact --
    //
    //    * Registry, read once at DriverEntry (ReadBlorgfsRegistryConfig
    //      in Driver.c): HKLM\<service key>\Parameters\TlsEnabled
    //      (REG_DWORD). This also picks the default remote port (443 if
    //      TRUE, 8080 if FALSE, unless Parameters\RemotePort explicitly
    //      overrides it) -- the port is resolved once, at load time, via
    //      GetHttpAddrInfo.
    //
    //    * The debugger, live, no rebuild or reload needed:
    //
    //          ed blorgfs!global.TlsEnabled 1
    //
    //      but this does NOT re-resolve global.RemoteAddressInfo -- that
    //      already happened at DriverEntry with whichever port the
    //      registry (or its default) picked at the time. A live toggle
    //      only reaches a working target if the registry already pointed
    //      the port at a TLS-speaking listener before this driver
    //      instance loaded. Toggling this against a port still speaking
    //      plaintext fails every connection cleanly (a plaintext server
    //      can't parse a ClientHello), but does nothing useful.
    //
    //  A handshake also needs Parameters\TlsPin (REG_BINARY, 32 bytes --
    //  see TlsSetPin/TlsCheckPin in TlsHandshake.c) or the runtime
    //  IOCTL_BLORGFS_SET_TLS_PIN (DevIoCtrl.c) configured, or every
    //  handshake fails closed at the Certificate message regardless of
    //  this flag.
    //
    //  volatile: read from HttpOnSocket at <= DISPATCH_LEVEL and writable
    //  live from the debugger at any time from any core -- documents the
    //  intentional cross-IRQL, asynchronously-toggled read and stops the
    //  compiler from caching the value across that branch.
    //
    volatile BOOLEAN TlsEnabled;  // TRUE to attempt TLS on new connections

#ifdef DBG
    ULONG LogLevel;  // BLORGFS_PRINT verbosity; see macro above
#endif
} global;