#pragma once

typedef struct __declspec(align(64)) _BLORGFS_STATISTICS
{
    FILESYSTEM_STATISTICS Base;
    FAT_STATISTICS Specific;
} BLORGFS_STATISTICS, * PBLORGFS_STATISTICS;

NTSTATUS BlorgCreateStatistics(PBLORGFS_STATISTICS* Statistics);

inline void BlorgFreeStatistics(PBLORGFS_STATISTICS Statistics)
{
    if (Statistics)
    {
        ExFreePool(Statistics);
    }
}

NTSTATUS BlorgCopyStatistics(const BLORGFS_STATISTICS* Statistics, PVOID Buffer, PULONG Length);