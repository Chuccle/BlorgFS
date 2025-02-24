#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdmsec.h>

#define BLORGFS_VDO_STRING  L"\\Device\\BlorgFS"
#define BLORGFS_VDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_VDO_MAGIC   0xDEAD5609

#define BLORGFS_DDO_STRING  L"\\Device\\BlorgDrive"
#define BLORGFS_DDO_DEVICE_SDDL_STRING L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)"
#define BLORGFS_DDO_MAGIC  0xD4ADBAA5
#define BLORGFS_DOS_DRIVELETTER_FORMAT_STRING L"\\DosDevices\\%C:"

// 0x8000 - 0xBFFF  reserved for 3rd party file systems
#define BLORGFS_FILE_NODE_SIGNATURE 0x8008
#define BLORGFS_DIRECTORY_NODE_SIGNATURE 0xB00B
#define BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE 0xBEEF
#define BLORGFS_VOLUME_NODE_SIGNATURE 0xB055

#define GetNodeType(Nodeptr) (*((USHORT*)(Nodeptr)))

typedef struct _NON_PAGED_FCB 
{
    //
    //  The following field contains a record of special pointers used by
    //  MM and Cache to manipluate section objects.  Note that the values
    //  are set outside of the file system.  However the file system on an
    //  open/create will set the file object's SectionObject field to point
    //  to this field
    //

    SECTION_OBJECT_POINTERS SectionObjectPointers;

    FAST_MUTEX HeaderFastMutex;

} NON_PAGED_FCB, * PNON_PAGED_FCB;

typedef struct _FCB 
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    PNON_PAGED_FCB NonPaged;

    PFILE_OBJECT FileObject;

    LIST_ENTRY FcbLinks;
    KGUARDED_MUTEX  Lock;

    UNICODE_STRING  Name;

    PDEVICE_OBJECT VolumeDeviceObject;

    struct _DCB* ParentDcb;

    LONG64 RefCount;

    SHARE_ACCESS ShareAccess;

} FCB, * PFCB;

typedef struct _DCB
{
#pragma warning(suppress: 4201)
    FCB  DUMMYSTRUCTNAME;
    
    LIST_ENTRY ListHead;
    KGUARDED_MUTEX  ListLock;


} DCB, * PDCB;

typedef struct _BLORGFS_DEVICE_EXTENSION_HDR
{
    UINT64 Identifier;
} BLORGFS_DEVICE_EXTENSION_HDR, * PBLORGFS_DEVICE_EXTENSION_HDR;

typedef struct _BLORGFS_VDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    PDCB RootDcb;
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

typedef struct _BLORGFS_DDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    UNICODE_STRING SymLinkName;
} BLORGFS_DDO_DEVICE_EXTENSION, * PBLORGFS_DDO_DEVICE_EXTENSION;

static inline PBLORGFS_VDO_DEVICE_EXTENSION GetVolumeDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

static inline PBLORGFS_DDO_DEVICE_EXTENSION GetDiskDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

static inline ULONG64 GetDeviceExtensionMagic(PDEVICE_OBJECT pDeviceObject)
{
    return ((PBLORGFS_DEVICE_EXTENSION_HDR)pDeviceObject->DeviceExtension)->Identifier;
}

#define BLORGFS_PRINT(...)                                                     \
do                                                                             \
{                                                                              \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "BLORGFS: " __VA_ARGS__); \
} while(0)

#define BLORGFS_VERIFY(expr) \
do                           \
{                            \
    (void) (expr);           \
} while (0)

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

extern struct GLOBAL 
{
	PDRIVER_OBJECT pDriverObject;
	PDEVICE_OBJECT pVolumeDeviceObject;
	PDEVICE_OBJECT pDiskDeviceObject;
} global;