//
// The Security.c entry points Client.c calls, for targets that decode
// responses without compiling Security.c. A descriptor a response carries
// resolves to BLORGFS_SECURITY_LOCKED, as one the driver could not hold
// would, and inheriting from the root's default resolves to the ids the
// driver builds for it at load.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

ULONG BlorgSecurityIntern(const VOID* Descriptor, SIZE_T Length)
{
    UNREFERENCED_PARAMETER(Descriptor);
    UNREFERENCED_PARAMETER(Length);

    return BLORGFS_SECURITY_LOCKED;
}

ULONG BlorgSecurityInherit(ULONG Source, ULONG Depth, BOOLEAN IsDirectory)
{
    if ((0 == Depth) || (BLORGFS_SECURITY_DEFAULT != Source))
    {
        return Source;
    }

    return IsDirectory ? BLORGFS_SECURITY_DEFAULT_DIRECTORY : BLORGFS_SECURITY_DEFAULT_FILE;
}
