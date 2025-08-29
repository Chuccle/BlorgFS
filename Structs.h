#pragma once

#ifdef PADDING_CHECKS

#include <assert.h>  // For static_assert
#include <stddef.h>  // For offsetof

#define CHECK_PADDING_BETWEEN(STRUCT_NAME, FIELD1, FIELD2)                                                        \
static_assert(offsetof(STRUCT_NAME, FIELD2) == offsetof(STRUCT_NAME, FIELD1) + sizeof(((STRUCT_NAME*)0)->FIELD1), \
    "Padding detected between " #FIELD1 " and " #FIELD2 " members")

#define CHECK_PADDING_END(STRUCT_NAME, LAST_FIELD)                                                              \
static_assert(sizeof(STRUCT_NAME) == offsetof(STRUCT_NAME, LAST_FIELD) + sizeof(((STRUCT_NAME*)0)->LAST_FIELD), \
    "Padding detected between " #LAST_FIELD " and end of struct")

#else

#define CHECK_PADDING_BETWEEN(STRUCT_NAME, FIELD1, FIELD2)

#define CHECK_PADDING_END(STRUCT_NAME, LAST_FIELD)

#endif

/////////////////////////////////////////////
///////FILE CONTEXT SECTION//////////////////
/////////////////////////////////////////////

// 0x8000 - 0xBFFF  reserved for 3rd party file systems
#define BLORGFS_FCB_SIGNATURE 0x8008
#define BLORGFS_DCB_SIGNATURE 0xB00B
#define BLORGFS_ROOT_DCB_SIGNATURE 0xBEEF
#define BLORGFS_VCB_SIGNATURE 0xB055
#define BLORGFS_CCB_SIGNATURE 0xBE55

#define GET_NODE_TYPE(Nodeptr) (*((USHORT*)(Nodeptr)))

typedef struct _NON_PAGED_NODE
{
    //
    //  The following field contains a record of special pointers used by
    //  MM and Cache to manipulate section objects.  Note that the values
    //  are set outside of the file system.  However the file system on an
    //  open/create will set the file object's SectionObject field to point
    //  to this field
    //
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HdrFastMutex;
    ERESOURCE  HdrResource;
    ERESOURCE  HdrPagingIoResource;
} NON_PAGED_NODE, * PNON_PAGED_NODE;

CHECK_PADDING_BETWEEN(NON_PAGED_NODE, SectionObjectPointers, HdrFastMutex);
CHECK_PADDING_BETWEEN(NON_PAGED_NODE, HdrFastMutex, HdrResource);
CHECK_PADDING_BETWEEN(NON_PAGED_NODE, HdrResource, HdrPagingIoResource);
CHECK_PADDING_END(NON_PAGED_NODE, HdrPagingIoResource);

typedef struct _COMMON_CONTEXT
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    PNON_PAGED_NODE NonPaged;

    LIST_ENTRY Links;
    // Path
    UNICODE_STRING  FullPath;

    PDEVICE_OBJECT VolumeDeviceObject;

    struct _DCB* ParentDcb;

    SHARE_ACCESS ShareAccess;

    LONG CacheAcquired;

    ULONG64 CreationTime;
    
    ULONG64 LastAccessedTime;
    
    ULONG64 LastModifiedTime;

    // Interlocked
    LONG64 RefCount;
} COMMON_CONTEXT, * PCOMMON_CONTEXT;

CHECK_PADDING_BETWEEN(COMMON_CONTEXT, Header, NonPaged);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, NonPaged, Links);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, Links, FullPath);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, ShareAccess, CacheAcquired);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, CacheAcquired, CreationTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, LastModifiedTime, RefCount);
CHECK_PADDING_END(COMMON_CONTEXT, RefCount);

typedef struct _FCB
{
#pragma warning(suppress: 4201)
    COMMON_CONTEXT  DUMMYSTRUCTNAME;
    FILE_LOCK       FileLock;
    PVOID           LazyWriteThread;
} FCB, * PFCB;

CHECK_PADDING_BETWEEN(FCB, Header, NonPaged);
CHECK_PADDING_BETWEEN(FCB, NonPaged, Links);
CHECK_PADDING_BETWEEN(FCB, Links, FullPath);
CHECK_PADDING_BETWEEN(FCB, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(FCB, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(FCB, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(FCB, ShareAccess, CacheAcquired);
CHECK_PADDING_BETWEEN(FCB, CacheAcquired, CreationTime);
CHECK_PADDING_BETWEEN(FCB, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(FCB, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(FCB, LastModifiedTime, RefCount);
CHECK_PADDING_BETWEEN(FCB, RefCount, FileLock);
CHECK_PADDING_BETWEEN(FCB, FileLock, LazyWriteThread);
CHECK_PADDING_END(FCB, LazyWriteThread);

typedef struct _DCB
{
#pragma warning(suppress: 4201)
    COMMON_CONTEXT  DUMMYSTRUCTNAME;
    LIST_ENTRY ChildrenList;
} DCB, * PDCB;

CHECK_PADDING_BETWEEN(DCB, Header, NonPaged);
CHECK_PADDING_BETWEEN(DCB, NonPaged, Links);
CHECK_PADDING_BETWEEN(DCB, Links, FullPath);
CHECK_PADDING_BETWEEN(DCB, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(DCB, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(DCB, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(DCB, ShareAccess, CacheAcquired);
CHECK_PADDING_BETWEEN(DCB, CacheAcquired, CreationTime);
CHECK_PADDING_BETWEEN(DCB, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DCB, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(DCB, LastModifiedTime, RefCount);
CHECK_PADDING_BETWEEN(DCB, RefCount, ChildrenList);
CHECK_PADDING_END(DCB, ChildrenList);

typedef struct _CCB
{
    ULONG NodeTypeCode;
    ULONG NodeByteSize;
    ULONGLONG Flags;
    UINT64 CurrentIndex;
    UNICODE_STRING SearchPattern;
    PDIRECTORY_INFO Entries;
} CCB, * PCCB;

#define CCB_FLAG_MATCH_ALL 0x0001

CHECK_PADDING_BETWEEN(CCB, NodeTypeCode, NodeByteSize);
CHECK_PADDING_BETWEEN(CCB, NodeByteSize, Flags);
CHECK_PADDING_BETWEEN(CCB, Flags, CurrentIndex);
CHECK_PADDING_BETWEEN(CCB, CurrentIndex, SearchPattern);
CHECK_PADDING_BETWEEN(CCB, SearchPattern, Entries);
CHECK_PADDING_END(CCB, Entries);

typedef FCB VCB;
typedef PFCB PVCB;

NTSTATUS BlorgCreateFCB(FCB** Fcb, CSHORT NodeType, PCUNICODE_STRING Name, const PDEVICE_OBJECT VolumeDeviceObject, ULONGLONG Size);
NTSTATUS BlorgCreateDCB(DCB** Dcb, CSHORT NodeType, PCUNICODE_STRING Name, const PDEVICE_OBJECT VolumeDeviceObject);
extern inline NTSTATUS BlorgCreateCCB(CCB** Ccb, const PDEVICE_OBJECT VolumeDeviceObject);
void BlorgFreeFileContext(PVOID Context, const PDEVICE_OBJECT VolumeDeviceObject);

PCOMMON_CONTEXT SearchByPath(const PDCB RootDcb, PCUNICODE_STRING Path);
NTSTATUS InsertByPath(const PDCB RootDcb, PCUNICODE_STRING Path, const PDIRECTORY_ENTRY_METADATA DirEntryInfo, const PDEVICE_OBJECT VolumeDeviceObject, PCOMMON_CONTEXT* Out);

#define IRP_CONTEXT_FLAG_DISABLE_DIRTY              0x00000001
#define IRP_CONTEXT_FLAG_WAIT                       0x00000002
#define IRP_CONTEXT_FLAG_WRITE_THROUGH              0x00000004
#define IRP_CONTEXT_FLAG_DISABLE_WRITE_THROUGH      0x00000008
#define IRP_CONTEXT_FLAG_RECURSIVE_CALL             0x00000010
#define IRP_CONTEXT_FLAG_DISABLE_POPUPS             0x00000020
#define IRP_CONTEXT_FLAG_DEFERRED_WRITE             0x00000040
#define IRP_CONTEXT_FLAG_VERIFY_READ                0x00000080
#define IRP_CONTEXT_STACK_IO_CONTEXT                0x00000100
#define IRP_CONTEXT_FLAG_IN_FSP                     0x00000200
#define IRP_CONTEXT_FLAG_USER_IO                    0x00000400       // for performance counters
#define IRP_CONTEXT_FLAG_DISABLE_RAISE              0x00000800
#define IRP_CONTEXT_FLAG_OVERRIDE_VERIFY            0x00001000
#define IRP_CONTEXT_FLAG_CLEANUP_BREAKING_OPLOCK    0x00002000

#if (NTDDI_VERSION >= NTDDI_WINTHRESHOLD)
#define IRP_CONTEXT_FLAG_SWAPPED_STACK              0x00100000
#endif

#define IRP_CONTEXT_FLAG_PARENT_BY_CHILD            0x80000000

inline void BlorgSetupIrpContext(PIRP Irp, BOOLEAN Wait)
{
    ULONG_PTR flags = (ULONG_PTR)Irp->Tail.Overlay.DriverContext[0];

    NT_ASSERT(0 == flags);

    if (Wait)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_WAIT);
    }

    //
    //  Set the recursive file system call parameter.  We set it true if
    //  the TopLevelIrp field in the thread local storage is not the current
    //  irp, otherwise we leave it as FALSE.
    //

    if (IoGetTopLevelIrp() != Irp)
    {
        SetFlag(flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL);
    }

    Irp->Tail.Overlay.DriverContext[0] = (PVOID)flags;
}

/////////////////////////////////////////////
///////DEVICE EXTENSION SECTION//////////////
/////////////////////////////////////////////

typedef struct _BLORGFS_DEVICE_EXTENSION_HDR
{
    UINT64 Identifier;
} BLORGFS_DEVICE_EXTENSION_HDR, * PBLORGFS_DEVICE_EXTENSION_HDR;

CHECK_PADDING_END(BLORGFS_DEVICE_EXTENSION_HDR, Identifier);

typedef struct BLORGFS_VDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    NPAGED_LOOKASIDE_LIST NonPagedNodeLookasideList;
    PAGED_LOOKASIDE_LIST FcbLookasideList;
    PAGED_LOOKASIDE_LIST DcbLookasideList;
    PAGED_LOOKASIDE_LIST CcbLookasideList;
    PDCB RootDcb;
    PVCB Vcb;
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, Hdr, NonPagedNodeLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, NonPagedNodeLookasideList, FcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, FcbLookasideList, DcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, DcbLookasideList, CcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, CcbLookasideList, RootDcb);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, RootDcb, Vcb);
CHECK_PADDING_END(BLORGFS_VDO_DEVICE_EXTENSION, Vcb);

typedef struct _BLORGFS_DDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    UNICODE_STRING SymLinkName;
} BLORGFS_DDO_DEVICE_EXTENSION, * PBLORGFS_DDO_DEVICE_EXTENSION;

CHECK_PADDING_BETWEEN(BLORGFS_DDO_DEVICE_EXTENSION, Hdr, SymLinkName);
CHECK_PADDING_END(BLORGFS_DDO_DEVICE_EXTENSION, SymLinkName);

typedef struct _BLORGFS_FSDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    PDEVICE_OBJECT VolumeDeviceObject;
} BLORGFS_FSDO_DEVICE_EXTENSION, * PBLORGFS_FSDO_DEVICE_EXTENSION;

CHECK_PADDING_BETWEEN(BLORGFS_FSDO_DEVICE_EXTENSION, Hdr, VolumeDeviceObject);
CHECK_PADDING_END(BLORGFS_FSDO_DEVICE_EXTENSION, VolumeDeviceObject);

inline PBLORGFS_VDO_DEVICE_EXTENSION GetVolumeDeviceExtension(PDEVICE_OBJECT VolumeDeviceObject)
{
    return VolumeDeviceObject->DeviceExtension;
}

inline PBLORGFS_DDO_DEVICE_EXTENSION GetDiskDeviceExtension(PDEVICE_OBJECT DiskDeviceObject)
{
    return DiskDeviceObject->DeviceExtension;
}

inline PBLORGFS_FSDO_DEVICE_EXTENSION GetFileSystemDeviceExtension(PDEVICE_OBJECT FileSystemDeviceObject)
{
    return FileSystemDeviceObject->DeviceExtension;
}

inline ULONG64 GetDeviceExtensionMagic(PDEVICE_OBJECT DeviceObject)
{
    return ((PBLORGFS_DEVICE_EXTENSION_HDR)DeviceObject->DeviceExtension)->Identifier;
}