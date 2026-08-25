//
// Push locks, resources, lookaside lists and the Unicode helpers the node
// table needs. Split out of NtShim.c to keep that file about the core
// NT surface and this one about the synchronisation the FCB/DCB lifetime
// design rests on.
//
// The push lock is the interesting one. The node table takes it *shared*
// for a lookup and pins under it, and *exclusive* to publish or retire,
// and the whole no-use-after-free argument is that a retire cannot miss a
// pin because both touch the same lock. Modelling shared and exclusive
// as genuinely different -- rather than making everything exclusive --
// is what lets that argument be tested rather than assumed away.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"
#include "Scheduler.h"

///////////////////////////////////////////////////////////////////////////
// Push locks
///////////////////////////////////////////////////////////////////////////

static void EnsurePushLockInitialized(PEX_PUSH_LOCK Lock)
{
    if (!Lock->Initialized)
    {
        //
        // The driver always calls ExInitializePushLock first; this is
        // only a guard against a zeroed lock reaching an acquire, which
        // would otherwise deadlock confusingly rather than fail loudly.
        //
        KmReportViolation(KmViolationLockOwner, "push lock used before ExInitializePushLock");
    }
}

VOID ExInitializePushLock(PEX_PUSH_LOCK Lock)
{
    //
    // Re-initialising returns the previous identity to the pool. Node
    // table fixtures re-init all 256 buckets per test, and without this
    // each run mints 256 fresh ids until the model's lock table is full.
    //
    if (Lock->Initialized)
    {
        KmReleaseLockId(Lock->Id);
    }

    InitializeSRWLock(&Lock->Lock);
    Lock->Initialized = 1;
    Lock->SchedState = 0;
    Lock->ExclusiveOwner = 0;
    Lock->Id = KmAllocateLockId();
    Lock->Name = "push-lock";
}

//
// Push locks are APC_LEVEL-or-below. The IRQL half of that contract is
// checked on every acquire; the critical-region half is NOT tracked --
// KeEnterCriticalRegion records nothing, so a push lock taken outside
// FsRtlEnterFileSystem is invisible here. That is a stated boundary: the
// dispatch paths all enter the file system first, and tracking the
// region would flag every sandbox body that calls into the table
// directly rather than through a dispatch entry.
//
//
// Under systematic exploration a push lock is a counter rather than an
// SRWLOCK: 0 free, -1 held exclusive, n > 0 for n shared holders. It has
// to be, because only one thread runs at a time -- an OS-blocking acquire
// would park the caller where nothing could ever wake it, since the thread
// holding the lock is suspended and only the scheduler can resume it.
//
// Reader/writer semantics are preserved deliberately. The node table's
// whole design is that lookups take the bucket shared and only retirement
// takes it exclusive; collapsing that to a mutex would explore a smaller,
// better-behaved program than the one that ships.
//
static int PushLockFreePredicate(void* Context)
{
    return ((PEX_PUSH_LOCK)Context)->SchedState == 0;
}

static int PushLockSharablePredicate(void* Context)
{
    return ((PEX_PUSH_LOCK)Context)->SchedState >= 0;
}

//
// Both claims run inside KmSchedWaitUntilClaim while the caller still
// holds the baton, immediately after the predicate that justified them.
// That adjacency is the whole mutual-exclusion argument: no scheduling
// point exists between test and claim, so nothing can invalidate the test
// in between. An earlier repair reached the same guarantee with re-test
// loops at each call site, which was correct but made lock safety a
// per-caller convention -- and one caller (the spin lock) did not follow
// it, and double-granted. Claiming through the scheduler makes the
// guarantee structural instead.
//
static void PushLockExclusiveClaim(void* Context)
{
    PEX_PUSH_LOCK lock = (PEX_PUSH_LOCK)Context;

    KmSchedNoteAcquire(lock);

    lock->SchedState = -1;
    lock->ExclusiveOwner = KmSchedThreadId();
}

static void PushLockSharedClaim(void* Context)
{
    PEX_PUSH_LOCK lock = (PEX_PUSH_LOCK)Context;

    KmSchedNoteAcquire(lock);

    lock->SchedState++;
}

VOID ExAcquirePushLockExclusive(PEX_PUSH_LOCK Lock)
{
    EnsurePushLockInitialized(Lock);
    KmRequireIrqlAtMost(APC_LEVEL, "ExAcquirePushLockExclusive");

    if (Lock->ExclusiveOwner == KmSchedThreadId())
    {
        KmReportViolation(KmViolationLockRecursion,
            "recursive exclusive acquisition of a push lock -- self-deadlock");
        return;
    }

    KmNoteLockAcquire(Lock->Id, "push-lock");

    if (KmSchedActive())
    {
        KmSchedWaitUntilClaim(PushLockFreePredicate, Lock,
            PushLockExclusiveClaim, Lock, "push lock exclusive");
        return;
    }

    AcquireSRWLockExclusive(&Lock->Lock);

    Lock->ExclusiveOwner = KmSchedThreadId();
}

VOID ExReleasePushLockExclusive(PEX_PUSH_LOCK Lock)
{
    if (Lock->ExclusiveOwner != KmSchedThreadId())
    {
        KmReportViolation(KmViolationLockOwner,
            "push lock released exclusive by a thread that does not hold it");
        return;
    }

    Lock->ExclusiveOwner = 0;

    if (KmSchedActive())
    {
        Lock->SchedState = 0;
        KmNoteLockRelease(Lock->Id);
        KmSchedNoteRelease(Lock);
        KmSchedYield();
        return;
    }

    ReleaseSRWLockExclusive(&Lock->Lock);

    KmNoteLockRelease(Lock->Id);
}

VOID ExAcquirePushLockShared(PEX_PUSH_LOCK Lock)
{
    EnsurePushLockInitialized(Lock);
    KmRequireIrqlAtMost(APC_LEVEL, "ExAcquirePushLockShared");

    KmNoteLockAcquire(Lock->Id, "push-lock");

    if (KmSchedActive())
    {
        //
        // Same claim-under-the-baton as the exclusive path. A shared count
        // taken on top of an exclusive hold is the node table's worst
        // case: lookups take the bucket shared and only retirement takes
        // it exclusive, so this is exactly the pairing the table's whole
        // design rests on.
        //
        KmSchedWaitUntilClaim(PushLockSharablePredicate, Lock,
            PushLockSharedClaim, Lock, "push lock shared");
        return;
    }

    AcquireSRWLockShared(&Lock->Lock);
}

VOID ExReleasePushLockShared(PEX_PUSH_LOCK Lock)
{
    if (KmSchedActive())
    {
        //
        // An unmatched shared release drives SchedState negative, which
        // makes the sharable predicate permanently false and surfaces
        // later as a spurious deadlock attributed to the wrong cause.
        // Rejecting it here puts the diagnostic on the offending call.
        //
        if (Lock->SchedState <= 0)
        {
            KmReportViolation(KmViolationLockOwner,
                "push lock released shared without a shared hold");
            return;
        }

        Lock->SchedState--;
        KmNoteLockRelease(Lock->Id);
        KmSchedNoteRelease(Lock);
        KmSchedYield();
        return;
    }

    ReleaseSRWLockShared(&Lock->Lock);

    KmNoteLockRelease(Lock->Id);
}

///////////////////////////////////////////////////////////////////////////
// Resources
///////////////////////////////////////////////////////////////////////////

NTSTATUS ExInitializeResourceLite(PERESOURCE Resource)
{
    if (Resource->Initialized)
    {
        KmReleaseLockId(Resource->Id);
    }

    InitializeSRWLock(&Resource->Lock);
    Resource->Initialized = 1;
    Resource->ExclusiveOwner = 0;
    Resource->SchedState = 0;
    Resource->Id = KmAllocateLockId();

    return STATUS_SUCCESS;
}

static int EresourceFreePredicate(void* Context)
{
    return ((PERESOURCE)Context)->SchedState == 0;
}

static int EresourceSharablePredicate(void* Context)
{
    return ((PERESOURCE)Context)->SchedState >= 0;
}

//
// Both claims run inside KmSchedWaitUntilClaim while the caller still
// holds the baton, immediately after the predicate that justified them.
//
// This is the site of the original double-grant: the acquire used to
// claim on the strength of its wait having returned. KmSchedWaitUntil
// returned once RefreshRunnable had marked the thread runnable -- which
// happens the moment the predicate holds, not when the thread is next
// scheduled -- so anything runnable in between could re-acquire and leave
// two threads believing they held the resource. Releases then disagreed
// with ExclusiveOwner, SchedState settled on a value no release would
// return to zero, and every later exclusive waiter blocked against a
// resource nobody owned: a deadlock in a thread whose partner had already
// finished. A re-test loop at this call site fixed it locally; claiming
// under the baton fixes it for every primitive at once.
//
static void EresourceExclusiveClaim(void* Context)
{
    PERESOURCE resource = (PERESOURCE)Context;

    KmSchedNoteAcquire(resource);

    resource->SchedState = -1;
    resource->ExclusiveOwner = KmSchedThreadId();
}

static void EresourceSharedClaim(void* Context)
{
    PERESOURCE resource = (PERESOURCE)Context;

    KmSchedNoteAcquire(resource);

    resource->SchedState++;
}

NTSTATUS ExDeleteResourceLite(PERESOURCE Resource)
{
    if (Resource->ExclusiveOwner != 0)
    {
        KmReportViolation(KmViolationLifetime, "ExDeleteResourceLite on a held resource");
    }

    Resource->Initialized = 0;

    KmReleaseLockId(Resource->Id);
    Resource->Id = 0;

    return STATUS_SUCCESS;
}

BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait)
{
    (void)Wait;

    KmRequireIrqlAtMost(APC_LEVEL, "ExAcquireResourceExclusiveLite");

    if (Resource->ExclusiveOwner == KmSchedThreadId())
    {
        KmReportViolation(KmViolationLockRecursion,
            "recursive exclusive acquisition of an ERESOURCE in the model "
            "(the kernel allows it; the model does not, because the driver "
            "does not rely on it and a recursive take usually means a "
            "path was entered twice by mistake)");
        return TRUE;
    }

    KmNoteLockAcquire(Resource->Id, "eresource");

    if (KmSchedActive())
    {
        KmSchedWaitUntilClaim(EresourceFreePredicate, Resource,
            EresourceExclusiveClaim, Resource, "eresource exclusive");
    }
    else
    {
        AcquireSRWLockExclusive(&Resource->Lock);
    }

    Resource->ExclusiveOwner = KmSchedThreadId();

    return TRUE;
}

BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait)
{
    (void)Wait;

    KmRequireIrqlAtMost(APC_LEVEL, "ExAcquireResourceSharedLite");

    KmNoteLockAcquire(Resource->Id, "eresource");

    if (KmSchedActive())
    {
        //
        // Same claim-under-the-baton as the exclusive path: an exclusive
        // acquirer running between this wait returning and the count below
        // would corrupt the state both sides release against.
        //
        KmSchedWaitUntilClaim(EresourceSharablePredicate, Resource,
            EresourceSharedClaim, Resource, "eresource shared");
    }
    else
    {
        AcquireSRWLockShared(&Resource->Lock);
    }

    return TRUE;
}

VOID ExReleaseResourceLite(PERESOURCE Resource)
{
    const BOOLEAN wasExclusive = (Resource->ExclusiveOwner == KmSchedThreadId());

    if (wasExclusive)
    {
        Resource->ExclusiveOwner = 0;
    }

    if (KmSchedActive())
    {
        //
        // Same unmatched-release guard as ExReleasePushLockShared: a
        // blind decrement below zero wedges every later sharable waiter,
        // and the failure must be attributed to this call, not to a
        // mystery deadlock several schedules later.
        //
        if (!wasExclusive && Resource->SchedState <= 0)
        {
            KmReportViolation(KmViolationLockOwner,
                "resource released shared without a hold");
            return;
        }

        Resource->SchedState = wasExclusive ? 0 : Resource->SchedState - 1;
        KmNoteLockRelease(Resource->Id);
        KmSchedNoteRelease(Resource);
        KmSchedYield();
        return;
    }

    if (wasExclusive)
    {
        ReleaseSRWLockExclusive(&Resource->Lock);
    }
    else
    {
        ReleaseSRWLockShared(&Resource->Lock);
    }

    KmNoteLockRelease(Resource->Id);
}

VOID ExInitializeFastMutex(PFAST_MUTEX Mutex)
{
    InitializeSRWLock(&Mutex->Lock);
}

//
// FsRtlEnterFileSystem disables normal kernel APCs; the model treats it
// as entering a critical region, which is what makes a push lock taken
// inside one legal.
//
VOID FsRtlEnterFileSystem(VOID)
{
    KeEnterCriticalRegion();
}

VOID FsRtlExitFileSystem(VOID)
{
    KeLeaveCriticalRegion();
}

///////////////////////////////////////////////////////////////////////////
// Paged lookaside lists
///////////////////////////////////////////////////////////////////////////

VOID ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PVOID Allocate, PVOID Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth)
{
    ExInitializeNPagedLookasideList(Lookaside, Allocate, Free, Flags, Size, Tag, Depth);
}

VOID ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside)
{
    ExDeleteNPagedLookasideList(Lookaside);
}

//
// Paged, so allocation and free are APC_LEVEL-or-below. FCB and DCB nodes
// come from these lists, which is precisely why the reap list is guarded
// by a push lock rather than a spin lock -- a spin lock would raise to
// DISPATCH and fault on the paged link fields. Enforcing the IRQL here is
// what would catch that regression.
//
PVOID ExAllocateFromPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside)
{
    KmRequireIrqlAtMost(APC_LEVEL, "ExAllocateFromPagedLookasideList");

    PVOID entry = ExAllocatePoolUninitialized(PagedPool, Lookaside->L.Size, Lookaside->L.Tag);

    if (entry)
    {
        InterlockedIncrement(&Lookaside->Outstanding);
    }

    return entry;
}

VOID ExFreeToPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry)
{
    KmRequireIrqlAtMost(APC_LEVEL, "ExFreeToPagedLookasideList");

    InterlockedDecrement(&Lookaside->Outstanding);

    ExFreePool(Entry);
}

///////////////////////////////////////////////////////////////////////////
// Unicode
///////////////////////////////////////////////////////////////////////////

WCHAR RtlUpcaseUnicodeChar(WCHAR Source)
{
    if (Source >= L'a' && Source <= L'z')
    {
        return (WCHAR)(Source - (L'a' - L'A'));
    }

    return Source;
}

//
// The driver hashes case-insensitively and falls back to its own loop if
// this fails, so the exact algorithm does not matter -- only that equal
// strings hash equally under the same case folding the comparison uses.
// A hash that disagreed with the compare would put a node in a bucket
// where lookup never finds it, so the two are kept deliberately
// consistent.
//
NTSTATUS RtlHashUnicodeString(const UNICODE_STRING* String, BOOLEAN CaseInSensitive, ULONG Algorithm, PULONG Value)
{
    (void)Algorithm;

    if (!String || !Value)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG hash = 0;

    for (USHORT i = 0; i < String->Length / sizeof(WCHAR); ++i)
    {
        WCHAR c = String->Buffer[i];

        if (CaseInSensitive)
        {
            c = RtlUpcaseUnicodeChar(c);
        }

        hash = (hash * 131u) + c;
    }

    *Value = hash;

    return STATUS_SUCCESS;
}

BOOLEAN RtlEqualUnicodeString(const UNICODE_STRING* String1, const UNICODE_STRING* String2, BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
    {
        return FALSE;
    }

    for (USHORT i = 0; i < String1->Length / sizeof(WCHAR); ++i)
    {
        WCHAR a = String1->Buffer[i];
        WCHAR b = String2->Buffer[i];

        if (CaseInSensitive)
        {
            a = RtlUpcaseUnicodeChar(a);
            b = RtlUpcaseUnicodeChar(b);
        }

        if (a != b)
        {
            return FALSE;
        }
    }

    return TRUE;
}

VOID RtlFreeUnicodeString(PUNICODE_STRING String)
{
    if (String->Buffer)
    {
        ExFreePool(String->Buffer);
        String->Buffer = NULL;
    }

    String->Length = 0;
    String->MaximumLength = 0;
}

//
// The reap teardown poll sleeps between checks. In the model the other
// thread is a real OS thread, so yielding is both sufficient and what
// keeps a teardown poll from spinning a core.
//
//
// Sleeping is precisely when the system's worker threads get to run, so
// this drains pending work items before yielding. Without it a driver
// loop that waits for a work item to finish -- BlorgNodeTableTeardown
// polls NodeReap.Queued exactly that way -- spins forever here while the
// work that would clear its condition sits queued behind it.
//
// The item runs on the waiting thread rather than a system worker
// thread. Both are PASSIVE_LEVEL, so IRQL and lock-order accounting are
// unaffected; the one thing it cannot model is a waiter that holds a lock
// the worker needs, which in the kernel would proceed and here would
// self-deadlock. No BlorgFS wait does that -- teardown holds nothing when
// it polls.
//
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
{
    (void)WaitMode;
    (void)Alertable;

    KmRequireIrqlAtMost(PASSIVE_LEVEL, "KeDelayExecutionThread");

    ShimDrainWorkItems();

    LONGLONG hundredNs = Interval ? -Interval->QuadPart : 0;
    DWORD milliseconds = (DWORD)(hundredNs / 10000);

    if (KmSchedActive())
    {
        //
        // A delay is precisely when the kernel may run anyone, so under
        // the executor it is a scheduling point: hand another modelled
        // thread the baton. Virtual time makes the duration irrelevant,
        // and the drain above already ran whatever the waiter needed.
        //
        (void)milliseconds;
        KmSchedYield();
        return STATUS_SUCCESS;
    }

    Sleep(milliseconds ? milliseconds : 1);

    return STATUS_SUCCESS;
}
