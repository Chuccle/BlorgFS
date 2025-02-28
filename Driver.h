#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include <wsk.h>

#include "Client.h"
#include "Structs.h"

#define BLORGFS_FSDO_STRING  L"\\BlorgFS"
#define BLORGFS_FSDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_FSDO_MAGIC   0xDEAD00D

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgVolume"
#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_VDO_MAGIC   0xD3ADBEAF

#define BLORGFS_DDO_STRING  L"\\Device\\BlorgDrive"
#define BLORGFS_DDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_DDO_MAGIC  0xD4ADBAA5
#define BLORGFS_DOS_DRIVELETTER_FORMAT_STRING L"\\DosDevices\\%C:"

#ifdef DBG

#define BLORGFS_PRINT(...)                                                     \
do                                                                             \
{                                                                              \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
} while(0)

#define BLORGFS_VERIFY(expr) \
do                           \
{                            \
    ASSERT(expr);            \
} while (0)

#else

#define BLORGFS_PRINT(...)

#define BLORGFS_VERIFY(expr) \
do                           \
{                            \
    (void) (expr);           \
} while (0)

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

NTSTATUS CreateBlorgVolumeDeviceObject(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT* VolumeDeviceObject);

extern struct GLOBAL
{
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT FileSystemDeviceObject;
    PDEVICE_OBJECT DiskDeviceObject;
    PADDRINFOEXW   RemoteAddressInfo;
} global;