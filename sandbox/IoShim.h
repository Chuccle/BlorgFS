#pragma once

//
// The IRP dispatch surface: IO_STACK_LOCATION, the IRP_MJ/IRP_MN codes,
// and the NTSTATUS values the driver returns.
//
// This is the layer that lets the dispatch translation units -- every
// IRP_MJ handler, the FSP work queue, the cache-manager callbacks --
// compile and run against the kernel model. It is separated from NtShim.h
// because it is a different kind of thing: NtShim models kernel
// *behaviour* (pool that can fail, DPCs that run at DISPATCH, IRPs that
// can only be completed once), whereas almost everything here is a
// *layout* or a constant, and the only requirement on it is that it match
// what ntifs.h says so the driver's own field accesses mean the same
// thing in both builds.
//
// Only the Parameters members the driver actually reads are declared. The
// real union has around thirty arms; carrying the rest would be surface
// with nothing behind it, and each one is a place for this file to drift
// from ntifs.h without anything noticing.
//

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////
// Status codes
///////////////////////////////////////////////////////////////////////////

#ifndef STATUS_WAIT_0
#define STATUS_WAIT_0                    ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_WAIT_1
#define STATUS_WAIT_1                    ((NTSTATUS)0x00000001L)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT                   ((NTSTATUS)0x00000102L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING                   ((NTSTATUS)0x00000103L)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW           ((NTSTATUS)0x80000005L)
#endif
#ifndef STATUS_NO_MORE_FILES
#define STATUS_NO_MORE_FILES             ((NTSTATUS)0x80000006L)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010L)
#endif
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_OBJECT_PATH_NOT_FOUND
#define STATUS_OBJECT_PATH_NOT_FOUND     ((NTSTATUS)0xC000003AL)
#endif
#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION         ((NTSTATUS)0xC0000043L)
#endif
#ifndef STATUS_FILE_LOCK_CONFLICT
#define STATUS_FILE_LOCK_CONFLICT        ((NTSTATUS)0xC0000054L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
#define STATUS_NOT_A_DIRECTORY           ((NTSTATUS)0xC0000103L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY       ((NTSTATUS)0xC00000BAL)
#endif
#ifndef STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_MEDIA_WRITE_PROTECTED     ((NTSTATUS)0xC00000A2L)
#endif
#ifndef STATUS_DEVICE_REMOVED
#define STATUS_DEVICE_REMOVED            ((NTSTATUS)0xC00002B6L)
#endif
#ifndef STATUS_OPLOCK_NOT_GRANTED
#define STATUS_OPLOCK_NOT_GRANTED        ((NTSTATUS)0xC00000E2L)
#endif
#ifndef STATUS_CANNOT_DELETE
#define STATUS_CANNOT_DELETE             ((NTSTATUS)0xC0000121L)
#endif

///////////////////////////////////////////////////////////////////////////
// Major and minor function codes
///////////////////////////////////////////////////////////////////////////

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_QUERY_INFORMATION        0x05
#define IRP_MJ_SET_INFORMATION          0x06
#define IRP_MJ_QUERY_EA                 0x07
#define IRP_MJ_SET_EA                   0x08
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0a
#define IRP_MJ_SET_VOLUME_INFORMATION   0x0b
#define IRP_MJ_DIRECTORY_CONTROL        0x0c
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x0d
#define IRP_MJ_DEVICE_CONTROL           0x0e
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_LOCK_CONTROL             0x11
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_QUERY_SECURITY           0x14
#define IRP_MJ_SET_SECURITY             0x15

#define IRP_MN_QUERY_DIRECTORY          0x01
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#define IRP_MN_USER_FS_REQUEST          0x00
#define IRP_MN_MOUNT_VOLUME             0x01
#define IRP_MN_VERIFY_VOLUME            0x02
#define IRP_MN_MDL                      0x01
#define IRP_MN_COMPLETE                 0x02
#define IRP_MN_COMPRESSED               0x08

#define IO_NO_INCREMENT                 0
#define IO_DISK_INCREMENT               1

#define SL_RESTART_SCAN                 0x01
#define SL_RETURN_SINGLE_ENTRY          0x02
#define SL_INDEX_SPECIFIED              0x04
#define SL_WATCH_TREE                   0x01

#define FILE_SUPERSEDED                 0x00000000
#define FILE_OPENED                     0x00000001
#define FILE_CREATED                    0x00000002

#define FILE_DIRECTORY_FILE             0x00000001
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_DELETE_ON_CLOSE            0x00001000

#define FO_SYNCHRONOUS_IO               0x00000002

///////////////////////////////////////////////////////////////////////////
// I/O stack location
///////////////////////////////////////////////////////////////////////////

typedef struct _IO_SECURITY_CONTEXT
{
    PVOID SecurityQos;
    PVOID AccessState;
    ACCESS_MASK DesiredAccess;
    ULONG FullCreateOptions;
} IO_SECURITY_CONTEXT, * PIO_SECURITY_CONTEXT;

struct _IO_STACK_LOCATION
{
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;

    union
    {
        struct
        {
            PIO_SECURITY_CONTEXT SecurityContext;
            ULONG Options;
            USHORT FileAttributes;
            USHORT ShareAccess;
            ULONG EaLength;
        } Create;

        struct
        {
            ULONG Length;
            ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Read;

        struct
        {
            ULONG Length;
            ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Write;

        struct
        {
            ULONG Length;
            PUNICODE_STRING FileName;
            ULONG FileInformationClass;
            ULONG FileIndex;
        } QueryDirectory;

        struct
        {
            ULONG Length;
            ULONG CompletionFilter;
        } NotifyDirectory;

        struct
        {
            ULONG Length;
            ULONG FileInformationClass;
        } QueryFile;

        struct
        {
            ULONG Length;
            ULONG FileInformationClass;
            PFILE_OBJECT FileObject;
        } SetFile;

        struct
        {
            ULONG Length;
            ULONG FsInformationClass;
        } QueryVolume;

        struct
        {
            ULONG Length;
            ULONG FsInformationClass;
        } SetVolume;

        struct
        {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG FsControlCode;
            PVOID Type3InputBuffer;
        } FileSystemControl;

        struct
        {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;

        struct
        {
            ULONG SecurityInformation;
            ULONG Length;
        } QuerySecurity;

        struct
        {
            ULONG SecurityInformation;
            PVOID SecurityDescriptor;
        } SetSecurity;

        struct
        {
            ULONG Length;
            PVOID EaList;
            ULONG EaListLength;
            ULONG EaIndex;
        } QueryEa;

        struct
        {
            ULONG Length;
        } SetEa;

        struct
        {
            PVOID Length;
            ULONG Key;
            LARGE_INTEGER ByteOffset;
        } LockControl;

        struct
        {
            PVPB Vpb;
            PDEVICE_OBJECT DeviceObject;
        } MountVolume;

        struct
        {
            PVOID Argument1;
            PVOID Argument2;
            PVOID Argument3;
            PVOID Argument4;
        } Others;
    } Parameters;

    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;

    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID Context;
};

//
// The driver's stack location, as the kernel hands it over. The model
// stores one per IRP rather than the kernel's stack of them: nothing in
// BlorgFS calls down to a lower driver, so the next-lower location is
// never used and modelling it would be surface without behaviour.
//
PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp);

typedef enum _WAIT_TYPE { WaitAll, WaitAny } WAIT_TYPE;

LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency);

///////////////////////////////////////////////////////////////////////////
// Filesystem statistics
///////////////////////////////////////////////////////////////////////////

//
// The documented FSCTL_FILESYSTEM_GET_STATISTICS shape. Statistics.c fills
// these directly, so the layouts have to match ntifs.h field for field --
// fsutil parses them by offset.
//
#define FILESYSTEM_STATISTICS_TYPE_NTFS 1
#define FILESYSTEM_STATISTICS_TYPE_FAT  2
#define FILESYSTEM_STATISTICS_TYPE_EXFAT 3

typedef struct _FILESYSTEM_STATISTICS
{
    USHORT FileSystemType;
    USHORT Version;
    ULONG SizeOfCompleteStructure;
    ULONG UserFileReads;
    ULONG UserFileReadBytes;
    ULONG UserDiskReads;
    ULONG UserFileWrites;
    ULONG UserFileWriteBytes;
    ULONG UserDiskWrites;
    ULONG MetaDataReads;
    ULONG MetaDataReadBytes;
    ULONG MetaDataDiskReads;
    ULONG MetaDataWrites;
    ULONG MetaDataWriteBytes;
    ULONG MetaDataDiskWrites;
} FILESYSTEM_STATISTICS, * PFILESYSTEM_STATISTICS;

typedef struct _FILESYSTEM_STATISTICS_EX
{
    USHORT FileSystemType;
    USHORT Version;
    ULONG SizeOfCompleteStructure;
    ULONGLONG UserFileReads;
    ULONGLONG UserFileReadBytes;
    ULONGLONG UserDiskReads;
    ULONGLONG UserFileWrites;
    ULONGLONG UserFileWriteBytes;
    ULONGLONG UserDiskWrites;
    ULONGLONG MetaDataReads;
    ULONGLONG MetaDataReadBytes;
    ULONGLONG MetaDataDiskReads;
    ULONGLONG MetaDataWrites;
    ULONGLONG MetaDataWriteBytes;
    ULONGLONG MetaDataDiskWrites;
} FILESYSTEM_STATISTICS_EX, * PFILESYSTEM_STATISTICS_EX;

typedef struct _FAT_STATISTICS
{
    ULONG CreateHits;
    ULONG SuccessfulCreates;
    ULONG FailedCreates;
    ULONG NonCachedReads;
    ULONG NonCachedReadBytes;
    ULONG NonCachedWrites;
    ULONG NonCachedWriteBytes;
    ULONG NonCachedDiskReads;
    ULONG NonCachedDiskWrites;
} FAT_STATISTICS, * PFAT_STATISTICS;

#ifdef __cplusplus
}
#endif
