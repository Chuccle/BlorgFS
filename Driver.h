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

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgFS"
#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_VDO_MAGIC   0xDEAD5609

#define BLORGFS_DDO_STRING  L"\\Device\\BlorgDrive"
#define BLORGFS_DDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_DDO_MAGIC  0xD4ADBAA5
#define BLORGFS_DOS_DRIVELETTER_FORMAT_STRING L"\\DosDevices\\%c:"



typedef struct _BLORGFS_DEVICE_EXTENSION_HDR
{
	UINT32 Identifier;
} BLORGFS_DEVICE_EXTENSION_HDR, * PBLORGFS_DEVICE_EXTENSION_HDR;


typedef struct _BLORGFS_VDO_DEVICE_EXTENSION
{
	BLORGFS_DEVICE_EXTENSION_HDR hdr;
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

typedef struct _BLORGFS_DDO_DEVICE_EXTENSION
{
	BLORGFS_DEVICE_EXTENSION_HDR hdr;
} BLORGFS_DDO_DEVICE_EXTENSION, * PBLORGFS_DDO_DEVICE_EXTENSION;

static inline PBLORGFS_VDO_DEVICE_EXTENSION GetVolumeDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

static inline PBLORGFS_DDO_DEVICE_EXTENSION GetDiskDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
	return pDeviceObject->DeviceExtension;
}

static inline UINT32 GetDeviceExtensionMagic(PDEVICE_OBJECT pDeviceObject)
{
	return ((PBLORGFS_DEVICE_EXTENSION_HDR)pDeviceObject->DeviceExtension)->Identifier;
}

#define BLORGFS_PRINT(...) do { \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
} while(0)


extern struct GLOBAL 
{
	PDRIVER_OBJECT pDriverObject;
	PDEVICE_OBJECT pVolumeDeviceObject;
	PDEVICE_OBJECT pDiskDeviceObject;
} global;