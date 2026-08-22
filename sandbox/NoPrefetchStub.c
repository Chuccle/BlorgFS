//
// BlorgPrefetchDetach for the targets that do not compile Prefetch.c.
//
// Structs.c calls it from node teardown, so it has to resolve everywhere;
// but PrefetchSandbox links the real Prefetch.c and must not also link a
// stub that silently does nothing. Keeping the stub in its own
// translation unit is what lets each target pick exactly one -- which is
// the point: a target testing the ring tests the real detach, and a
// target that is not testing the ring does not quietly pretend to.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\Driver.h"

VOID BlorgPrefetchDetach(struct _FCB* Fcb)
{
    (void)Fcb;
}
