#pragma once

#define PATH_MAX 4096 // review - i'm tempted to make this MAX_PATH

#define MAX_NAME_LEN 260

enum InvertedCallType
{
    InvertedCallTypeNone = 0,
    InvertedCallTypeListDirectory,
    InvertedCallTypeReadFile,
    InvertedCallTypeQueryFile
};

///////////////////////////////////////////////////////////
/////////// Structures for QueryFile operation ////////////
///////////////////////////////////////////////////////////

typedef struct _DIRECTORY_ENTRY_METADATA
{
    ULONG64 Size;
    ULONG64 CreationTime;
    ULONG64 LastAccessedTime;
    ULONG64 LastModifiedTime;
    BOOLEAN IsDirectory;
} DIRECTORY_ENTRY_METADATA, * PDIRECTORY_ENTRY_METADATA;

///////////////////////////////////////////////////////////
//////// Structures for ListDirectory operation ///////////
///////////////////////////////////////////////////////////

typedef struct _DIRECTORY_FILE_METADATA
{
    ULONG64 Size;
    ULONG64 CreationTime;
    ULONG64 LastAccessedTime;
    ULONG64 LastModifiedTime;
    SIZE_T  NameLength; // Length of name in characters
    WCHAR   Name[MAX_NAME_LEN];
} DIRECTORY_FILE_METADATA, * PDIRECTORY_FILE_METADATA;

typedef struct _DIRECTORY_SUBDIR_METADATA
{
    ULONG64 CreationTime;
    ULONG64 LastAccessedTime;
    ULONG64 LastModifiedTime;
    SIZE_T  NameLength; // Length of name in characters
    WCHAR   Name[MAX_NAME_LEN];
} DIRECTORY_SUBDIR_METADATA, * PDIRECTORY_SUBDIR_METADATA;

typedef struct _DIRECTORY_INFO
{
    SIZE_T FilesOffset;        // Offset from start of this struct to first file entry
    SIZE_T SubDirsOffset;      // Offset from start of this struct to first subdir entry
    SIZE_T FileCount;
    SIZE_T SubDirCount;
} DIRECTORY_INFO, *PDIRECTORY_INFO;

inline PDIRECTORY_SUBDIR_METADATA GetSubDirEntry(PDIRECTORY_INFO DirInfo, SIZE_T Index)
{
    if (Index >= DirInfo->SubDirCount)
    {
        return NULL;
    }

    return (PDIRECTORY_SUBDIR_METADATA)(
        (PUCHAR)DirInfo +
        DirInfo->SubDirsOffset +
        (Index * sizeof(DIRECTORY_SUBDIR_METADATA))
        );
}

inline PDIRECTORY_FILE_METADATA GetFileEntry(PDIRECTORY_INFO DirInfo, SIZE_T Index)
{
    if (Index >= DirInfo->FileCount)
    {
        return NULL;
    }

    return (PDIRECTORY_FILE_METADATA)(
        (PUCHAR)DirInfo +
        DirInfo->FilesOffset +
        (Index * sizeof(DIRECTORY_FILE_METADATA))
        );
}

///////////////////////////////////////////////////////////

typedef struct _BLORGFS_TRANSACT
{
    struct
    {
        ULONG RequestId;
    } Header;

    enum InvertedCallType InvertedCallType;

    union
    {
        struct
        {
            SIZE_T StartOffset;
            SIZE_T Length;
        } ReadFile;
    } Context;

    WCHAR Path[PATH_MAX];
    
    ULONG PathLength;
    
    struct
    {
        NTSTATUS Status;
        UCHAR ResponseBuffer[1]; // ridiculous that c++ does not allow flexible array members in structs
    } Payload;
} BLORGFS_TRANSACT, * PBLORGFS_TRANSACT;

#define FSCTL_BLORGFS_TRANSACT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)