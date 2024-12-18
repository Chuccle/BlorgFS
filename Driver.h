#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

#include "Cleanup.h"
#include "Close.h"
#include "Create.h"
#include "DirCtrl.h"
#include "FlushBuffers.h"
#include "FsCtrl.h"
#include "LockCtrl.h"
#include "QueryEa.h"
#include "QueryInfo.h"
#include "QuerySecurity.h"
#include "QueryVolumeInfo.h"
#include "Read.h"
#include "SetEa.h"
#include "SetInfo.h"
#include "SetSecurity.h"
#include "SetVolumeInfo.h"
#include "Write.h"

#define BLORGFS_DO_STRING  L"\\Device\\BlorgFS"
#define BLORGFS_DO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_DO_MAGIC   0xDEAD5609

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgDrive"
#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_VDO_MAGIC  0xD4ADBAA5
#define BLORGFS_DOS_DRIVELETTER_FORMAT_STRING L"\\DosDevices\\%c:"

typedef struct _BLORGFS_DO_DEVICE_EXTENSION
{
	PDEVICE_OBJECT pDeviceObject;
	UINT32         Identifier;

} BLORGFS_DO_DEVICE_EXTENSION, * PBLORGFS_DO_DEVICE_EXTENSION;

static inline PBLORGFS_DO_DEVICE_EXTENSION GetDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

static inline UINT32 GetDeviceExtensionMagic(PDEVICE_OBJECT pDeviceObject)
{
	return GetDeviceExtension(pDeviceObject)->Identifier;
}

#define BLORGFS_PRINT(...) do { \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
} while(0)