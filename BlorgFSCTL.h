#pragma once

#define PATH_MAX 4096

enum InvertedCallType
{
    InvertedCallTypeNone = 0,
    InvertedCallTypeListDirectory,
    InvertedCallTypeReadFile,
    InvertedCallTypeQueryFile
};

typedef struct _BLORGFS_INVERTED_CALL
{
    struct
    {
        ULONG RequestId;
    } Header;

    enum InvertedCallType InvertedCallType;

    ULONG PathLength;

    WCHAR Path[PATH_MAX];

    struct
    {
        UCHAR ResponseBuffer[1]; // ridiculous that c++ does not allow flexible array members in structs
    } Payload;
} BLORGFS_INVERTED_CALL, * PBLORGFS_INVERTED_CALL;

#define FSCTL_BLORGFS_INVERTED_CALL CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x800, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)