#include "Driver.h"

NTSTATUS BlorgCreateStatistics(PBLORGFS_STATISTICS* Statistics)
{
    *Statistics = ExAllocatePoolZero(NonPagedPool, sizeof(BLORGFS_STATISTICS) * global.ProcessorCount, 'TATS');

    if (!*Statistics)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG index = 0; index < global.ProcessorCount; index++)
    {
        PBLORGFS_STATISTICS stats = *Statistics + index;

        stats->Base.FileSystemType = FILESYSTEM_STATISTICS_TYPE_FAT;
        stats->Base.Version = 1;
        stats->Base.SizeOfCompleteStructure = sizeof(BLORGFS_STATISTICS);
    }

    return STATUS_SUCCESS;
}

NTSTATUS BlorgCopyStatistics(const BLORGFS_STATISTICS* Statistics, PVOID Buffer, PULONG Length)
{
    NTSTATUS result;

    if (NULL == Buffer)
    {
        *Length = 0;
        return STATUS_INVALID_PARAMETER;
    }

    if (sizeof(FILESYSTEM_STATISTICS) > *Length)
    {
        *Length = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    ULONG statLength = sizeof(BLORGFS_STATISTICS) * global.ProcessorCount;

    if (*Length >= statLength)
    {
        *Length = statLength;
        result = STATUS_SUCCESS;
    }
    else
    {
        result = STATUS_BUFFER_OVERFLOW;
    }

    RtlCopyMemory(Buffer, Statistics, *Length);

    return result;
}