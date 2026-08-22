//
// Kernel primitives modelled for CBMC, replacing the gtest sandbox's shim.
//
// The gtest shim builds its push locks on SRWLOCK, which is exactly right
// when real threads run but useless to a model checker: CBMC cannot see
// inside AcquireSRWLockShared, so it would explore interleavings the lock
// actually forbids and report races that cannot happen. A concurrency
// proof is only meaningful if the synchronisation is expressed in terms
// the checker understands.
//
// So each lock here is a small integer manipulated inside
// __CPROVER_atomic_begin/end, with __CPROVER_assume standing in for
// "wait". The assume prunes the interleaving where the lock is held --
// which is precisely what a blocking acquire does to the set of reachable
// states, and is the standard way to model a lock for a bounded model
// checker.
//
// Everything else is modelled only as far as the invariant under proof
// can observe it. The pool is the exception: the whole claim is about a
// free happening at the wrong moment, so frees are recorded rather than
// performed.
//

#include "..\..\Driver.h"

///////////////////////////////////////////////////////////////////////////
// Locks
///////////////////////////////////////////////////////////////////////////

//
// One word per lock: 0 free, -1 held exclusive, n > 0 for n shared
// holders. Reader/writer semantics matter here -- the node table's whole
// design is that lookups take the bucket shared and only retirement takes
// it exclusive, so collapsing this to a mutex would prove a different,
// stronger program than the one that ships.
//
static int PushLockState[64];

static int PushLockSlot(PEX_PUSH_LOCK Lock)
{
    return (int)(((ULONG_PTR)Lock >> 3) % 64);
}

VOID ExInitializePushLock(PEX_PUSH_LOCK Lock)
{
    PushLockState[PushLockSlot(Lock)] = 0;
}

VOID ExAcquirePushLockExclusive(PEX_PUSH_LOCK Lock)
{
    int slot = PushLockSlot(Lock);

    __CPROVER_atomic_begin();
    __CPROVER_assume(PushLockState[slot] == 0);
    PushLockState[slot] = -1;
    __CPROVER_atomic_end();
}

VOID ExReleasePushLockExclusive(PEX_PUSH_LOCK Lock)
{
    int slot = PushLockSlot(Lock);

    __CPROVER_atomic_begin();
    PushLockState[slot] = 0;
    __CPROVER_atomic_end();
}

VOID ExAcquirePushLockShared(PEX_PUSH_LOCK Lock)
{
    int slot = PushLockSlot(Lock);

    __CPROVER_atomic_begin();
    __CPROVER_assume(PushLockState[slot] >= 0);
    PushLockState[slot] = PushLockState[slot] + 1;
    __CPROVER_atomic_end();
}

VOID ExReleasePushLockShared(PEX_PUSH_LOCK Lock)
{
    int slot = PushLockSlot(Lock);

    __CPROVER_atomic_begin();
    PushLockState[slot] = PushLockState[slot] - 1;
    __CPROVER_atomic_end();
}

//
// The reap worker takes the VCB resource exclusive for its whole pass.
// Modelled as a plain mutex: nothing in this proof acquires it shared.
//
static int ResourceState[8];

static int ResourceSlot(PERESOURCE Resource)
{
    return (int)(((ULONG_PTR)Resource >> 3) % 8);
}

NTSTATUS ExInitializeResourceLite(PERESOURCE Resource)
{
    ResourceState[ResourceSlot(Resource)] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS ExDeleteResourceLite(PERESOURCE Resource)
{
    (void)Resource;
    return STATUS_SUCCESS;
}

BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait)
{
    int slot = ResourceSlot(Resource);

    (void)Wait;

    __CPROVER_atomic_begin();
    __CPROVER_assume(ResourceState[slot] == 0);
    ResourceState[slot] = -1;
    __CPROVER_atomic_end();

    return TRUE;
}

BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait)
{
    int slot = ResourceSlot(Resource);

    (void)Wait;

    __CPROVER_atomic_begin();
    __CPROVER_assume(ResourceState[slot] >= 0);
    ResourceState[slot] = ResourceState[slot] + 1;
    __CPROVER_atomic_end();

    return TRUE;
}

VOID ExReleaseResourceLite(PERESOURCE Resource)
{
    int slot = ResourceSlot(Resource);

    __CPROVER_atomic_begin();
    ResourceState[slot] = (ResourceState[slot] < 0) ? 0 : ResourceState[slot] - 1;
    __CPROVER_atomic_end();
}

//
// Critical-region entry only defers APCs; it is not mutual exclusion and
// modelling it as such would forbid the very concurrency under test.
//
VOID KeEnterCriticalRegion(VOID) { }
VOID KeLeaveCriticalRegion(VOID) { }
VOID FsRtlEnterFileSystem(VOID) { }
VOID FsRtlExitFileSystem(VOID) { }

VOID ExInitializeFastMutex(PFAST_MUTEX FastMutex) { (void)FastMutex; }

///////////////////////////////////////////////////////////////////////////
// Pool
///////////////////////////////////////////////////////////////////////////

//
// The claim under proof is about a node being freed while someone still
// holds a pin on it, so a free is recorded rather than performed: freeing
// for real would hand CBMC a dangling pointer and turn a precise
// invariant violation into a generic dereference failure that says much
// less about why.
//
PVOID CbmcWatchedNode = NULL;
volatile int CbmcNodeFreed = 0;

static unsigned char CbmcNodeStorage[4096];
static unsigned char CbmcNonPagedStorage[1024];
static unsigned char CbmcNameStorage[256];
static int CbmcNodeTaken = 0;
static int CbmcNonPagedTaken = 0;

PVOID ExAllocateFromPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside)
{
    (void)Lookaside;

    if (CbmcNodeTaken)
    {
        return NULL;
    }

    CbmcNodeTaken = 1;
    return CbmcNodeStorage;
}

VOID ExFreeToPagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry)
{
    (void)Lookaside;

    if (Entry == CbmcWatchedNode)
    {
        CbmcNodeFreed = 1;
    }
}

PVOID ExAllocateFromNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside)
{
    (void)Lookaside;

    if (CbmcNonPagedTaken)
    {
        return NULL;
    }

    CbmcNonPagedTaken = 1;
    return CbmcNonPagedStorage;
}

VOID ExFreeToNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry)
{
    (void)Lookaside;
    (void)Entry;
}

PVOID ExAllocatePoolZero(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag)
{
    (void)PoolType;
    (void)Tag;

    if (NumberOfBytes > sizeof(CbmcNameStorage))
    {
        return NULL;
    }

    for (SIZE_T i = 0; i < sizeof(CbmcNameStorage); i++)
    {
        CbmcNameStorage[i] = 0;
    }

    return CbmcNameStorage;
}

PVOID ExAllocatePoolUninitialized(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag)
{
    return ExAllocatePoolZero(PoolType, NumberOfBytes, Tag);
}

VOID ExFreePool(PVOID P) { (void)P; }

///////////////////////////////////////////////////////////////////////////
// The rest of the surface Structs.c touches
///////////////////////////////////////////////////////////////////////////

VOID ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST L, PVOID A, PVOID F, ULONG Fl, SIZE_T S, ULONG T, USHORT D)
{
    (void)A; (void)F; (void)Fl; (void)D;
    L->L.Size = (ULONG)S;
    L->L.Tag = T;
}

VOID ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST L, PVOID A, PVOID F, ULONG Fl, SIZE_T S, ULONG T, USHORT D)
{
    ExInitializeNPagedLookasideList(L, A, F, Fl, S, T, D);
}

VOID ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST L) { (void)L; }
VOID ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST L) { (void)L; }

VOID FsRtlSetupAdvancedHeader(PVOID Header, PFAST_MUTEX FastMutex)
{
    PFSRTL_ADVANCED_FCB_HEADER header = (PFSRTL_ADVANCED_FCB_HEADER)Header;

    header->FastMutex = FastMutex;
    InitializeListHead(&header->FilterContexts);
}

VOID FsRtlTeardownPerStreamContexts(PFSRTL_ADVANCED_FCB_HEADER H) { (void)H; }
VOID FsRtlInitializeFileLock(PFILE_LOCK F, PVOID C, PVOID U) { (void)F; (void)C; (void)U; }
VOID FsRtlUninitializeFileLock(PFILE_LOCK F) { (void)F; }
VOID FsRtlInitializeOplock(POPLOCK O) { (void)O; }
VOID FsRtlUninitializeOplock(POPLOCK O) { (void)O; }

VOID BlorgPrefetchDetach(struct _FCB* Fcb) { (void)Fcb; }
void FreeHttpDirectoryInfo(PDIRECTORY_INFO D) { (void)D; }

//
// One node lives in this proof, so every path hashes to the same bucket
// and compares equal. That is not a shortcut around the hash -- the
// property under proof is about the pin protocol, and a second bucket
// would only add states in which the two threads never touch the same
// lock at all.
//
NTSTATUS RtlHashUnicodeString(const UNICODE_STRING* S, BOOLEAN C, ULONG A, PULONG Out)
{
    (void)S; (void)C; (void)A;
    *Out = 0;
    return STATUS_SUCCESS;
}

BOOLEAN RtlEqualUnicodeString(const UNICODE_STRING* A, const UNICODE_STRING* B, BOOLEAN CaseInsensitive)
{
    (void)CaseInsensitive;
    return (BOOLEAN)(A->Length == B->Length);
}

WCHAR RtlUpcaseUnicodeChar(WCHAR C) { return C; }

PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT D) { (void)D; return (PIO_WORKITEM)CbmcNameStorage; }
VOID IoFreeWorkItem(PIO_WORKITEM W) { (void)W; }

//
// The proof drives the reap path directly rather than through the work
// queue, so a queue request is recorded and otherwise ignored. Modelling
// the worker as a third thread would prove the same invariant with a
// larger state space and no extra coverage: the worker's body is
// NodeTableTryRetire, which thread B calls.
//
volatile int CbmcWorkItemQueued = 0;

VOID IoQueueWorkItem(PIO_WORKITEM W, PVOID R, WORK_QUEUE_TYPE Q, PVOID C)
{
    (void)W; (void)R; (void)Q; (void)C;
    CbmcWorkItemQueued = 1;
}

NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE W, BOOLEAN A, PLARGE_INTEGER I)
{
    (void)W; (void)A; (void)I;
    return STATUS_SUCCESS;
}

NTSTATUS FsRtlDissectName(UNICODE_STRING Path, PUNICODE_STRING First, PUNICODE_STRING Rest)
{
    *First = Path;
    Rest->Length = 0;
    Rest->MaximumLength = 0;
    Rest->Buffer = NULL;
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// List unlink
///////////////////////////////////////////////////////////////////////////

//
// The one construct CBMC's concurrency encoding cannot take: writing
// through a pointer loaded from shared memory. The shipping version does
// blink->Flink = flink, and CBMC responds by refusing to check the whole
// program rather than by being imprecise about that one statement.
//
// The proof's bucket holds a single node, so unlinking it is the same
// thing as emptying the bucket -- expressible by writing to objects CBMC
// can name. The entry's own links are cleared exactly as the real one
// does, because BlorgNodeTablePublish and NodeTableTryRetire both read
// Flink to decide whether a node is currently published.
//
extern PLIST_ENTRY CbmcProofBucketList;

void RemoveEntryList(PLIST_ENTRY Entry)
{
    CbmcProofBucketList->Flink = CbmcProofBucketList;
    CbmcProofBucketList->Blink = CbmcProofBucketList;

    Entry->Flink = NULL;
    Entry->Blink = NULL;
}

//
// The reap queue's linkage, recorded rather than built. The proof's
// retiring thread goes through NodeTableTryRetire, not the queue, so the
// only thing any assertion observes about a deferral is that it happened.
//
volatile int CbmcReapDeferred = 0;

void PushEntryList(PSINGLE_LIST_ENTRY Head, PSINGLE_LIST_ENTRY Entry)
{
    (void)Head;
    (void)Entry;

    CbmcReapDeferred = 1;
}
