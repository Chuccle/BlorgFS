//
// The DiskCache.c entry points Read.c calls, for targets that do not
// compile DiskCache.c, which is all of them: it opens a file and sends
// IRPs to another file system, neither of which the sandbox models. The
// cache is off here, as it is in a driver with no DiskCacheMb, so every
// read is fetched. DiskCacheIndex.c, which decides what the cache holds,
// has no I/O and is compiled and tested for real.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

VOID BlorgDiskCacheNoteFile(PNON_PAGED_NODE Node, const UNICODE_STRING* Path, ULONG64 Size, ULONG64 ModifiedTime)
{
    UNREFERENCED_PARAMETER(Node);
    UNREFERENCED_PARAMETER(Path);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(ModifiedTime);
}

BOOLEAN BlorgDiskCacheRead(PIRP Irp, PNON_PAGED_NODE Node, ULONG64 Offset, ULONG Length, ULONG Valid, PDISK_CACHE_READ_COMPLETION Completion)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Node);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Valid);
    UNREFERENCED_PARAMETER(Completion);

    return FALSE;
}

BOOLEAN BlorgDiskCacheLive(VOID)
{
    return FALSE;
}

VOID BlorgDiskCacheAdmit(PNON_PAGED_NODE Node, const FILE_BUFFER* FileBuffer, ULONG64 Offset, ULONG Length)
{
    UNREFERENCED_PARAMETER(Node);
    UNREFERENCED_PARAMETER(FileBuffer);
    UNREFERENCED_PARAMETER(Offset);
    UNREFERENCED_PARAMETER(Length);
}
