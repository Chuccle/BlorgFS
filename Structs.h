#pragma once

/////////////////////////////////////////////
///////FILE CONTEXT SECTION//////////////////
/////////////////////////////////////////////

// 0x8000 - 0xBFFF  reserved for 3rd party file systems
#define BLORGFS_FILE_NODE_SIGNATURE 0x8008
#define BLORGFS_DIRECTORY_NODE_SIGNATURE 0xB00B
#define BLORGFS_ROOT_DIRECTORY_NODE_SIGNATURE 0xBEEF
#define BLORGFS_VOLUME_NODE_SIGNATURE 0xB055

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

typedef struct _FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    PNON_PAGED_NODE NonPaged;

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

typedef FCB COMMON_CONTEXT;
typedef PFCB PCOMMON_CONTEXT;

NTSTATUS BlorgCreateFCB(FCB** ppFcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name);
NTSTATUS BlorgCreateDCB(DCB** ppDcb, PDCB parentDcb, CSHORT nodeType, PCUNICODE_STRING name);
void BlorgFreeFileContext(PVOID pFileNode);

/////////////////////////////////////////////
///////DEVICE EXTENSION SECTION//////////////
/////////////////////////////////////////////

typedef struct _BLORGFS_DEVICE_EXTENSION_HDR
{
    UINT64 Identifier;
} BLORGFS_DEVICE_EXTENSION_HDR, * PBLORGFS_DEVICE_EXTENSION_HDR;

typedef struct _BLORGFS_VDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    PDCB RootDcb;
    PVPB Vpb;
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

typedef struct _BLORGFS_DDO_DEVICE_EXTENSION
{
    BLORGFS_DEVICE_EXTENSION_HDR Hdr;
    UNICODE_STRING SymLinkName;
} BLORGFS_DDO_DEVICE_EXTENSION, * PBLORGFS_DDO_DEVICE_EXTENSION;

inline PBLORGFS_VDO_DEVICE_EXTENSION GetVolumeDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

inline PBLORGFS_DDO_DEVICE_EXTENSION GetDiskDeviceExtension(PDEVICE_OBJECT pDeviceObject)
{
    return pDeviceObject->DeviceExtension;
}

inline ULONG64 GetDeviceExtensionMagic(PDEVICE_OBJECT pDeviceObject)
{
    return ((PBLORGFS_DEVICE_EXTENSION_HDR)pDeviceObject->DeviceExtension)->Identifier;
}