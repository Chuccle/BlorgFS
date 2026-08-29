#pragma once

//
// The one place the kernel is substituted.
//
// The usermode kernel sandbox (tests\sandbox\SandboxPrelude.h) supplies an
// executable model of these four headers -- NT types, IRQL, pool,
// locks, DPCs, timers, MDLs, IRPs, FsRtl and WSK. Everything below
// this point is the driver's own code and is compiled identically in
// both builds, which is the point: there is exactly one definition of
// FCB, of DIRECTORY_INFO, of READ_AHEAD_GRANULARITY, of the padding
// assertions. A sandbox that re-declared any of them would drift from
// the driver silently, and a test that passes against a drifted copy
// is worse than no test.
//
#ifdef BLORGFS_SANDBOX_BUILD
//
// Unqualified: this header lives in src/ and the prelude in tests/sandbox/,
// so a path relative to this file would have to reach across the tree. The
// sandbox projects put their own directory on the include path instead,
// which keeps the driver from naming a test directory.
//
#include "SandboxPrelude.h"
#else
#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <wsk.h>
#endif

#include <limits.h>
#define C_CAST(T, expr) ((T)(expr))

//
// An unnamed member of named struct type is an MSVC extension to C that
// C++ does not have, so nodes embed COMMON_CONTEXT one way when this
// header is read as C and derive from it when read as C++. Both are the
// same layout: single non-virtual base, standard layout.
//
#ifdef __cplusplus
#define BLORGFS_COMMON_CONTEXT_BASE : COMMON_CONTEXT
#define BLORGFS_COMMON_CONTEXT_MEMBER
#else
#define BLORGFS_COMMON_CONTEXT_BASE
#define BLORGFS_COMMON_CONTEXT_MEMBER COMMON_CONTEXT DUMMYSTRUCTNAME;
#endif

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
// Cc's read-ahead granularity for cached reads (Read.c), and now the only
// lookahead this driver has -- the prefetch ring that used to derive its
// slot size from this constant is gone.
//
// 128 pages, i.e. 512 KB. Re-measured on a Release driver with Driver
// Verifier off, 8 concurrent streams, each run bracketed by a usermode HTTP
// client immediately before and after so network drift cannot be mistaken
// for a result:
//
//   granularity   ratio to usermode   avg paging read   p99
//     256 KB           0.96               254 KB        165 ms
//     512 KB           1.06               390 KB        208 ms
//       1 MB           1.07               546 KB        365 ms
//
// 256 KB is plainly worse: the reads stay small and the driver pays the
// per-request HTTP overhead more often for the same bytes. 1 MB matches
// 512 KB on throughput and costs 76% on the latency tail, which for media
// playback is the wrong trade -- a stall is what a viewer notices, and the
// aggregate they never see.
//
// The earlier version of this comment reached the same conclusion from a
// measurement taken under Debug + full Driver Verifier, where Cc clustered
// very differently (204-281 KB where this environment gives 390-546 KB).
// The instrumentation was not a constant tax; it changed the I/O shape. The
// answer survived re-measurement, the reasoning behind it did not.
//
#define READ_AHEAD_GRANULARITY (PAGE_SIZE * 128)

#include "Structs.h"
#include "Util.h"
#include "Client.h"
#include "Statistics.h"
#include "CacheManager.h"
#include "FspWorkQueue.h"

#define BLORGFS_FSDO_STRING  L"\\BlorgFS"
#define BLORGFS_FSDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgVolume"

#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;WD)"

#define BLORGFS_DDO_STRING  L"\\Device\\BlorgDrive"

#define BLORGFS_DDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;WD)"
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

//
// FastIoRead, wrapping FsRtlCopyRead so the read a caller actually waited
// on can be timed.
//
// Nearly every application read of a cached file arrives here and never
// becomes an IRP at all -- measured, a 780-read playback produced one
// IRP_MJ_READ, the first, taken before the cache map existed. Timing only
// the IRP path therefore measures almost nothing an application does.
//
// Spelled out rather than declared as FAST_IO_READ: the usermode sandbox
// compiles this header against NtShim.h, which has no such typedef.
BOOLEAN BlorgFastIoRead(
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject);
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

NTSTATUS BlorgInitializeSecurityDescriptor(VOID);
VOID BlorgFreeSecurityDescriptor(VOID);

NTSTATUS BlorgCreateVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject);

// Driver-wide global state, one instance for the whole driver load.
extern struct GLOBAL
{
    PDRIVER_OBJECT DriverObject;               // this driver's DRIVER_OBJECT
    PDEVICE_OBJECT FileSystemDeviceObject;     // the FSD (control) device object
    PDEVICE_OBJECT DiskDeviceObject;           // the disk device object backing the B: symlink

    //
    //  The mounted volume device object, or NULL before BlorgMountVolume
    //  has run. Held here rather than in an FSDO device extension because
    //  these three pointers are jointly what identifies an incoming
    //  DEVICE_OBJECT (BlorgDeviceKind, Structs.h) -- keeping one of them
    //  behind a dereference of the very thing being identified would defeat
    //  that. BlorgFS mounts exactly one volume, which is what makes a
    //  single pointer sufficient.
    //
    PDEVICE_OBJECT VolumeDeviceObject;
    PADDRINFOEXW   RemoteAddressInfo;          // resolved backend address/port for the HTTP client
    PSTR RemoteHostAnsi; // ANSI "host[:port]" authority for outgoing Host headers

    //
    //  ANSI hostname (no port) for the ClientHello SNI extension, built in
    //  DriverEntry alongside RemoteHostAnsi. NULL when TLS is disabled at
    //  load, when the configured host is an IPv4/IPv6 literal (RFC 6066
    //  forbids literals in SNI -- see HostStringIsIpLiteral, Driver.c), or
    //  on allocation failure; BlorgTlsStartHandshakeAsync omits the extension
    //  in all three cases. NUL-terminated (pool-zero allocated), bounded
    //  by BLORGFS_REMOTE_HOST_ANSI_MAX_BYTES like RemoteHostAnsi.
    //
    PSTR RemoteHostSniAnsi;

    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks; // Cc lazy-write/read-ahead callback table
    PVOID LazyWriteThread;                     // thread pointer Cc supplies to lazy-write callbacks

    //
    // Cc read-ahead granularity actually in force, in bytes. Defaults to
    // READ_AHEAD_GRANULARITY and is overridden by the
    // ReadAheadGranularityKb registry value; zero means leave Cc's own
    // default alone and never call CcSetReadAheadGranularity.
    //
    ULONG ReadAheadGranularity;

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
    //      BlorgGetHttpAddrInfo.
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
    //  see BlorgTlsSetPin/BlorgTlsCheckPin in TlsHandshake.c) or the runtime
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

//
// Which of this driver's three device objects a DEVICE_OBJECT is, decided
// by pointer identity against the three global pointers (Driver.h).
//
// There is no type tag in the extension to read, and deliberately so. The
// driver creates every one of these devices and already holds their
// addresses, so a tag stored in their own extensions only ever restated
// what the pointer already said -- while requiring a dereference of the
// object being identified. That is unsafe for the one DEVICE_OBJECT that
// arrives from outside: IRP_MN_MOUNT_VOLUME's
// Parameters.MountVolume.DeviceObject is another driver's storage device,
// offered to every registered file system as volumes arrive, and its
// extension may be absent entirely or shorter than the tag. Comparing
// pointers dereferences nothing, cannot be imitated by a value in foreign
// memory, and answers BlorgDeviceUnknown for anything not ours -- which
// every dispatch switch already handles by declining the request.
//
// Ordered volume-first: it takes every create, read, and directory query,
// while the other two see only mount and control traffic.
//
// This works because there is exactly one device object of each kind. The
// single volume is already assumed by BlorgMountVolume, which stores one
// pointer; a driver mounting several would need the tag back.
//
typedef enum _BLORGFS_DEVICE_KIND
{
    BlorgDeviceUnknown = 0,
    BlorgDeviceVolume,
    BlorgDeviceDisk,
    BlorgDeviceFileSystem
} BLORGFS_DEVICE_KIND;

inline BLORGFS_DEVICE_KIND BlorgDeviceKind(const DEVICE_OBJECT* DeviceObject)
{
    if (!DeviceObject)
    {
        return BlorgDeviceUnknown;
    }

    if (DeviceObject == global.VolumeDeviceObject)
    {
        return BlorgDeviceVolume;
    }

    if (DeviceObject == global.DiskDeviceObject)
    {
        return BlorgDeviceDisk;
    }

    if (DeviceObject == global.FileSystemDeviceObject)
    {
        return BlorgDeviceFileSystem;
    }

    return BlorgDeviceUnknown;
}
