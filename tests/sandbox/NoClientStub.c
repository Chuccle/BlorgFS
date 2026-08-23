//
// The Client.c entry points needed by targets that do not compile Client.c.
//
// Structs.c calls BlorgFreeHttpDirectoryInfo from node teardown, so it must
// resolve everywhere; but ClientSandbox links the real Client.c and must
// not also link a stub of it. Keeping the stub in its own translation unit
// lets each target pick exactly one, the same split NoStatisticsStub.c
// makes for the counter block.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

void BlorgFreeHttpDirectoryInfo(PDIRECTORY_INFO DirInfo)
{
    if (DirInfo)
    {
        ExFreePool(DirInfo);
    }
}
