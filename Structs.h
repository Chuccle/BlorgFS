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

typedef struct _DIRECTORY_ENTRY
{
    UNICODE_STRING Name;
    ULONG64 Size;
    ULONG64 CreationTime;
    ULONG64 LastAccessedTime;
    ULONG64 LastModifiedTime;
} DIRECTORY_ENTRY, * PDIRECTORY_ENTRY;

CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY, Name, Size);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY, Size, CreationTime);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_END(DIRECTORY_ENTRY, LastModifiedTime);

typedef struct _DIRECTORY_INFO
{
    DIRECTORY_ENTRY* Entries;
    SIZE_T EntryCount;
} DIRECTORY_INFO, * PDIRECTORY_INFO;

CHECK_PADDING_BETWEEN(DIRECTORY_INFO, Entries, EntryCount);
CHECK_PADDING_END(DIRECTORY_INFO, EntryCount);

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
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, CacheAcquired, RefCount);
CHECK_PADDING_END(COMMON_CONTEXT, RefCount);

typedef struct _FCB
{
#pragma warning(suppress: 4201)
    COMMON_CONTEXT  DUMMYSTRUCTNAME;
    FILE_LOCK       FileLock;
} FCB, * PFCB;

CHECK_PADDING_BETWEEN(FCB, Header, NonPaged);
CHECK_PADDING_BETWEEN(FCB, NonPaged, Links);
CHECK_PADDING_BETWEEN(FCB, Links, FullPath);
CHECK_PADDING_BETWEEN(FCB, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(FCB, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(FCB, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(FCB, ShareAccess, CacheAcquired);
CHECK_PADDING_BETWEEN(FCB, CacheAcquired, RefCount);
CHECK_PADDING_BETWEEN(FCB, RefCount, FileLock);
CHECK_PADDING_END(FCB, FileLock);

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
CHECK_PADDING_BETWEEN(DCB, CacheAcquired, RefCount);
CHECK_PADDING_BETWEEN(DCB, RefCount, ChildrenList);
CHECK_PADDING_END(DCB, ChildrenList);

typedef struct _CCB
{
    ULONG NodeTypeCode: 16;
    ULONG NodeByteSize: 16;
    ULONGLONG Flags;
    UINT64 CurrentIndex;
    UNICODE_STRING SearchPattern;
    DIRECTORY_INFO SubDirectories;
    DIRECTORY_INFO Files;
} CCB, * PCCB;

#define CCB_FLAG_MATCH_ALL 0x0001

CHECK_PADDING_BETWEEN(CCB, NodeTypeCode, NodeByteSize);
CHECK_PADDING_BETWEEN(CCB, NodeByteSize, Flags);
CHECK_PADDING_BETWEEN(CCB, Flags, CurrentIndex);
CHECK_PADDING_BETWEEN(CCB, CurrentIndex, SearchPattern);
CHECK_PADDING_BETWEEN(CCB, SearchPattern, SubDirectories);
CHECK_PADDING_BETWEEN(CCB, SubDirectories, Files);
CHECK_PADDING_END(CCB, Files);

typedef FCB VCB;
typedef PFCB PVCB;

NTSTATUS BlorgCreateFCB(FCB** Fcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject);
NTSTATUS BlorgCreateDCB(DCB** Dcb, CSHORT NodeType, PCUNICODE_STRING Name, PDEVICE_OBJECT VolumeDeviceObject);
NTSTATUS BlorgCreateCCB(CCB** Ccb, PDEVICE_OBJECT VolumeDeviceObject);
void BlorgFreeFileContext(PVOID FileNode);

PCOMMON_CONTEXT SearchByPath(PDCB RootDcb, PCUNICODE_STRING Path);
PCOMMON_CONTEXT SearchByName(PDCB ParentDcb, PCUNICODE_STRING Name);
NTSTATUS InsertByPath(PDCB RootDcb, PCUNICODE_STRING Path, BOOLEAN Directory, PDEVICE_OBJECT VolumeDeviceObject, PCOMMON_CONTEXT* Out);

/////////////////////////////////////////////
///////DEVICE EXTENSION SECTION//////////////
/////////////////////////////////////////////

typedef struct _BLORGFS_DEVICE_EXTENSION_HDR
{
    UINT64 Identifier;
} BLORGFS_DEVICE_EXTENSION_HDR, * PBLORGFS_DEVICE_EXTENSION_HDR;

CHECK_PADDING_END(BLORGFS_DEVICE_EXTENSION_HDR, Identifier);

typedef struct _BLORGFS_VDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    PVCB Vcb;
    PDCB RootDcb;
    NPAGED_LOOKASIDE_LIST NonPagedNodeLookasideList;
    PAGED_LOOKASIDE_LIST FcbLookasideList;
    PAGED_LOOKASIDE_LIST DcbLookasideList;
    PAGED_LOOKASIDE_LIST CcbLookasideList;
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, Hdr, Vcb);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, Vcb, RootDcb);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, RootDcb, NonPagedNodeLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, NonPagedNodeLookasideList, FcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, FcbLookasideList, DcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, DcbLookasideList, CcbLookasideList);
CHECK_PADDING_END(BLORGFS_VDO_DEVICE_EXTENSION, CcbLookasideList);

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