//
// Implementation of the kernel rule model. See KernelModel.h for what it
// enforces and why each rule is worth enforcing.
//

#include "KernelModel.h"
#include "Scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define KM_PASSIVE_LEVEL  0
#define KM_APC_LEVEL      1
#define KM_DISPATCH_LEVEL 2

#define KM_MAX_LOCKS      2048
#define KM_MAX_HELD       16

///////////////////////////////////////////////////////////////////////////
// Per-thread state
///////////////////////////////////////////////////////////////////////////

//
// One of these per modelled thread. IRQL and the held-lock stack are
// per-thread because that is what they are in the kernel: a completion
// running at DISPATCH on another processor must not observe the issuing
// thread's PASSIVE.
//
typedef struct _KM_THREAD_STATE
{
    unsigned char Irql;
    int HeldLocks[KM_MAX_HELD];
    int HeldCount;
} KM_THREAD_STATE;

static DWORD ThreadStateSlot = TLS_OUT_OF_INDEXES;
static long ThreadStateSlotInit = 0;

static KM_THREAD_STATE* KmThreadState(void)
{
    if (0 == InterlockedCompareExchange(&ThreadStateSlotInit, 1, 0))
    {
        ThreadStateSlot = TlsAlloc();
    }

    while (TLS_OUT_OF_INDEXES == ThreadStateSlot)
    {
        Sleep(0);
    }

    KM_THREAD_STATE* state = (KM_THREAD_STATE*)TlsGetValue(ThreadStateSlot);

    if (!state)
    {
        state = (KM_THREAD_STATE*)calloc(1, sizeof(KM_THREAD_STATE));
        state->Irql = KM_PASSIVE_LEVEL;
        TlsSetValue(ThreadStateSlot, state);
    }

    return state;
}

///////////////////////////////////////////////////////////////////////////
// Violations
///////////////////////////////////////////////////////////////////////////

static KM_VIOLATION ExpectedViolation = KmViolationNone;
static KM_VIOLATION RecordedViolation = KmViolationNone;

void KmExpectViolation(KM_VIOLATION Kind)
{
    ExpectedViolation = Kind;
    RecordedViolation = KmViolationNone;
}

KM_VIOLATION KmTakeViolation(void)
{
    KM_VIOLATION taken = RecordedViolation;
    RecordedViolation = KmViolationNone;
    ExpectedViolation = KmViolationNone;
    return taken;
}

void KmReportViolation(KM_VIOLATION Kind, const char* Format, ...)
{
    va_list args;
    va_start(args, Format);

    if (ExpectedViolation == Kind)
    {
        RecordedViolation = Kind;
        va_end(args);
        return;
    }

    fprintf(stderr, "\n[kernel-model] VIOLATION: ");
    vfprintf(stderr, Format, args);
    fprintf(stderr, "\n");

    va_end(args);
    fflush(stderr);

    abort();
}

///////////////////////////////////////////////////////////////////////////
// IRQL
///////////////////////////////////////////////////////////////////////////

unsigned char KmGetIrql(void)
{
    return KmThreadState()->Irql;
}

void KmSetIrql(unsigned char Irql)
{
    KmThreadState()->Irql = Irql;
}

unsigned char KmRaiseIrql(unsigned char NewIrql)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (NewIrql < state->Irql)
    {
        KmReportViolation(KmViolationIrql,
            "KeRaiseIrql to %u below current %u", NewIrql, state->Irql);
    }

    unsigned char old = state->Irql;
    state->Irql = NewIrql;
    return old;
}

void KmLowerIrql(unsigned char OldIrql)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (OldIrql > state->Irql)
    {
        KmReportViolation(KmViolationIrql,
            "KeLowerIrql to %u above current %u", OldIrql, state->Irql);
    }

    state->Irql = OldIrql;
}

void KmRequireIrqlAtMost(unsigned char Max, const char* Api)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (state->Irql > Max)
    {
        KmReportViolation(KmViolationIrql,
            "%s called at IRQL %u, contract is <= %u", Api, state->Irql, Max);
    }
}

///////////////////////////////////////////////////////////////////////////
// Locks and order tracking
///////////////////////////////////////////////////////////////////////////

//
// Adjacency matrix over lock ids: Order[a][b] means some thread has been
// observed acquiring b while holding a. A new edge whose reverse already
// exists is an AB/BA inversion -- a deadlock that has not happened yet
// but can. Reporting it at the moment the inversion is created is the
// whole value: the deadlock itself may need a one-in-a-million
// interleaving to actually occur.
//
static unsigned char LockOrder[KM_MAX_LOCKS][KM_MAX_LOCKS];
static const char* LockNames[KM_MAX_LOCKS];
static long NextLockId = 0;

//
// Ids returned by KmReleaseLockId, reused before any new one is minted.
//
static int FreeLockIds[KM_MAX_LOCKS];
static int FreeLockIdCount = 0;
static int LockIdRecycling = 1;

static CRITICAL_SECTION OrderCs;
static long OrderCsInit = 0;

static void KmEnsureOrderCs(void)
{
    if (0 == InterlockedCompareExchange(&OrderCsInit, 1, 0))
    {
        InitializeCriticalSection(&OrderCs);
        InterlockedExchange(&OrderCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&OrderCsInit, 2, 2))
    {
        Sleep(0);
    }
}

void KmSetLockIdRecycling(int Enabled)
{
    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);
    LockIdRecycling = Enabled;
    LeaveCriticalSection(&OrderCs);
}

//
// Clears the observed-order edges. Deliberately does NOT reset the id
// counter: locks created by an earlier test are still live and still hold
// their ids, so handing id 1 out again would give two live locks one
// identity in the graph. Ids come back through KmReleaseLockId instead,
// when a lock is destroyed or re-initialised.
//
void KmResetLockOrder(void)
{
    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);
    memset(LockOrder, 0, sizeof(LockOrder));
    LeaveCriticalSection(&OrderCs);
}

void KmInitializeLock(KM_LOCK* Lock, const char* Name)
{
    if (Lock->Initialized)
    {
        KmReleaseLockId(Lock->Id);
    }

    memset(Lock, 0, sizeof(*Lock));
    InitializeCriticalSection(&Lock->Cs);
    Lock->SchedState = 0;
    Lock->Initialized = 1;
    Lock->Name = Name ? Name : "unnamed";

    //
    // Through KmAllocateLockId, never straight off the counter: ids are
    // recycled when a lock is destroyed, and a second source would hand
    // the same id to two live locks. They would then share a row in the
    // order graph, and the inversions reported against them would be
    // about a lock pairing that never existed.
    //
    Lock->Id = KmAllocateLockId();

    if (Lock->Id <= 0 || Lock->Id >= KM_MAX_LOCKS)
    {
        return;
    }

    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);
    LockNames[Lock->Id] = Lock->Name;
    LeaveCriticalSection(&OrderCs);
}

//
// Records the edges from every lock this thread already holds to the one
// it is about to take, and reports an inversion if the reverse edge is
// already known.
//
static void KmRecordOrder(int AcquiringId)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (AcquiringId <= 0 || AcquiringId >= KM_MAX_LOCKS)
    {
        return;
    }

    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);

    for (int i = 0; i < state->HeldCount; ++i)
    {
        int heldId = state->HeldLocks[i];

        if (heldId <= 0 || heldId >= KM_MAX_LOCKS || heldId == AcquiringId)
        {
            continue;
        }

        if (LockOrder[AcquiringId][heldId])
        {
            LeaveCriticalSection(&OrderCs);

            KmReportViolation(KmViolationLockOrder,
                "lock order inversion: taking '%s' while holding '%s', but the "
                "reverse order was already observed -- AB/BA deadlock",
                LockNames[AcquiringId] ? LockNames[AcquiringId] : "?",
                LockNames[heldId] ? LockNames[heldId] : "?");

            EnterCriticalSection(&OrderCs);
        }

        LockOrder[heldId][AcquiringId] = 1;
    }

    LeaveCriticalSection(&OrderCs);
}

static void KmPushHeld(int Id)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (state->HeldCount < KM_MAX_HELD)
    {
        state->HeldLocks[state->HeldCount] = Id;
    }

    state->HeldCount++;
}

static void KmPopHeld(int Id)
{
    KM_THREAD_STATE* state = KmThreadState();

    if (state->HeldCount > 0)
    {
        state->HeldCount--;
    }

    (void)Id;
}

int KmLocksHeld(void)
{
    return KmThreadState()->HeldCount;
}

//
// Records an acquisition of a lock the model does not own. The id space
// is shared with KM_LOCK's, so a spin lock and a push lock taken in both
// orders is detected exactly like two spin locks would be.
//
void KmNoteLockAcquire(int Id, const char* Name)
{
    if (Id > 0 && Id < KM_MAX_LOCKS)
    {
        KmEnsureOrderCs();

        if (!LockNames[Id])
        {
            LockNames[Id] = Name;
        }
    }

    KmRecordOrder(Id);
    KmPushHeld(Id);
}

void KmNoteLockRelease(int Id)
{
    KmPopHeld(Id);
}

void KmReleaseLockId(int Id)
{
    if (!LockIdRecycling || Id <= 0 || Id >= KM_MAX_LOCKS)
    {
        return;
    }

    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);

    //
    // The order edges go with the id. A reused id inheriting the previous
    // lock's edges would invent inversions between locks that never
    // coexisted.
    //
    for (int i = 0; i < KM_MAX_LOCKS; ++i)
    {
        LockOrder[Id][i] = 0;
        LockOrder[i][Id] = 0;
    }

    LockNames[Id] = NULL;

    if (FreeLockIdCount < KM_MAX_LOCKS)
    {
        FreeLockIds[FreeLockIdCount++] = Id;
    }

    LeaveCriticalSection(&OrderCs);
}

int KmAllocateLockId(void)
{
    KmEnsureOrderCs();
    EnterCriticalSection(&OrderCs);

    if (FreeLockIdCount > 0)
    {
        const int recycled = FreeLockIds[--FreeLockIdCount];
        LeaveCriticalSection(&OrderCs);
        return recycled;
    }

    LeaveCriticalSection(&OrderCs);

    int id = (int)InterlockedIncrement(&NextLockId);

    if (id >= KM_MAX_LOCKS)
    {
        KmReportViolation(KmViolationLifetime, "more than %d locks modelled", KM_MAX_LOCKS);
        return 0;
    }

    return id;
}

static int KmLockFreePredicate(void* Context)
{
    return ((KM_LOCK*)Context)->SchedState == 0;
}

unsigned char KmAcquireLock(KM_LOCK* Lock)
{
    KmRequireIrqlAtMost(KM_DISPATCH_LEVEL, "KeAcquireSpinLock");

    KM_THREAD_STATE* state = KmThreadState();

    if (Lock->OwnerThread == GetCurrentThreadId())
    {
        KmReportViolation(KmViolationLockRecursion,
            "recursive acquisition of spin lock '%s' -- self-deadlock", Lock->Name);
    }

    KmRecordOrder(Lock->Id);

    //
    // Under exploration the spin lock is a flag and a yield, so a waiter
    // is simply not scheduled until the holder releases -- and, more to
    // the point, every acquire and release becomes a scheduling point, so
    // the explorer can preempt inside a critical section rather than only
    // between whole threads.
    //
    if (KmSchedActive())
    {
        KmSchedWaitUntil(KmLockFreePredicate, Lock, "spin lock");
        Lock->SchedState = 1;
    }
    else
    {
        EnterCriticalSection(&Lock->Cs);
    }

    unsigned char old = state->Irql;

    //
    // A spin lock raises to DISPATCH for as long as it is held. Modelling
    // that is what makes "took a paged resource under a spin lock"
    // detectable rather than invisible.
    //
    if (state->Irql < KM_DISPATCH_LEVEL)
    {
        state->Irql = KM_DISPATCH_LEVEL;
    }

    Lock->OwnerThread = GetCurrentThreadId();
    Lock->OwnerPreviousIrql = old;

    KmPushHeld(Lock->Id);

    return old;
}

void KmReleaseLock(KM_LOCK* Lock, unsigned char OldIrql)
{
    if (Lock->OwnerThread != GetCurrentThreadId())
    {
        KmReportViolation(KmViolationLockOwner,
            "spin lock '%s' released by a thread that does not hold it", Lock->Name);
        return;
    }

    KmPopHeld(Lock->Id);

    Lock->OwnerThread = 0;

    KM_THREAD_STATE* state = KmThreadState();
    state->Irql = OldIrql;

    if (KmSchedActive())
    {
        Lock->SchedState = 0;
        KmSchedYield();
        return;
    }

    LeaveCriticalSection(&Lock->Cs);
}

void KmAcquireLockShared(KM_LOCK* Lock)
{
    //
    // Push locks are APC_LEVEL-or-below and must be held inside a
    // critical region; the IRQL check is what catches the classic
    // "guarded a paged structure from a DISPATCH path" mistake.
    //
    KmRequireIrqlAtMost(KM_APC_LEVEL, "ExAcquirePushLockShared");

    KmRecordOrder(Lock->Id);

    EnterCriticalSection(&Lock->Cs);

    KmPushHeld(Lock->Id);
}

void KmReleaseLockShared(KM_LOCK* Lock)
{
    KmPopHeld(Lock->Id);
    LeaveCriticalSection(&Lock->Cs);
}

///////////////////////////////////////////////////////////////////////////
// DPCs
///////////////////////////////////////////////////////////////////////////

static KM_DPC* DpcQueueHead = NULL;
static KM_DPC* DpcQueueTail = NULL;
static CRITICAL_SECTION DpcCs;
static long DpcCsInit = 0;

static void KmEnsureDpcCs(void)
{
    if (0 == InterlockedCompareExchange(&DpcCsInit, 1, 0))
    {
        InitializeCriticalSection(&DpcCs);
        InterlockedExchange(&DpcCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&DpcCsInit, 2, 2))
    {
        Sleep(0);
    }
}

void KmInitializeDpc(KM_DPC* Dpc, PKM_DPC_ROUTINE Routine, void* Context)
{
    memset(Dpc, 0, sizeof(*Dpc));
    Dpc->Routine = Routine;
    Dpc->Context = Context;
}

//
// The kernel allows only one queued instance of a given DPC object; a
// second insert while the first is pending returns FALSE and does
// nothing. Drivers rely on that for dedup, so the model has to reproduce
// it rather than queueing twice.
//
BOOLEAN KmInsertQueueDpc(KM_DPC* Dpc, void* Arg1, void* Arg2)
{
    (void)Arg1;
    (void)Arg2;

    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);

    if (Dpc->Queued)
    {
        LeaveCriticalSection(&DpcCs);
        return FALSE;
    }

    Dpc->Queued = 1;
    Dpc->Next = NULL;

    if (DpcQueueTail)
    {
        DpcQueueTail->Next = Dpc;
    }
    else
    {
        DpcQueueHead = Dpc;
    }

    DpcQueueTail = Dpc;

    LeaveCriticalSection(&DpcCs);

    return TRUE;
}

BOOLEAN KmRemoveQueueDpc(KM_DPC* Dpc)
{
    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);

    if (!Dpc->Queued)
    {
        LeaveCriticalSection(&DpcCs);
        return FALSE;
    }

    KM_DPC** link = &DpcQueueHead;
    KM_DPC* previous = NULL;

    while (*link && *link != Dpc)
    {
        previous = *link;
        link = &(*link)->Next;
    }

    if (*link == Dpc)
    {
        *link = Dpc->Next;

        if (DpcQueueTail == Dpc)
        {
            DpcQueueTail = previous;
        }
    }

    Dpc->Queued = 0;
    Dpc->Next = NULL;

    LeaveCriticalSection(&DpcCs);

    return TRUE;
}

int KmDrainDpcs(void)
{
    int ran = 0;

    for (;;)
    {
        KmEnsureDpcCs();
        EnterCriticalSection(&DpcCs);

        KM_DPC* dpc = DpcQueueHead;

        if (dpc)
        {
            DpcQueueHead = dpc->Next;

            if (!DpcQueueHead)
            {
                DpcQueueTail = NULL;
            }

            dpc->Queued = 0;
            dpc->Next = NULL;
        }

        LeaveCriticalSection(&DpcCs);

        if (!dpc)
        {
            return ran;
        }

        //
        // DPCs run at DISPATCH_LEVEL by definition. Running them at the
        // caller's level would let a DPC routine take a paged lock and
        // the model would say nothing.
        //
        KM_THREAD_STATE* state = KmThreadState();
        unsigned char saved = state->Irql;
        state->Irql = KM_DISPATCH_LEVEL;

        dpc->Routine(dpc, dpc->Context, NULL, NULL);

        state->Irql = saved;
        ran++;
    }
}

///////////////////////////////////////////////////////////////////////////
// Virtual clock and timers
///////////////////////////////////////////////////////////////////////////

static volatile long long VirtualNow = 0;
static KM_TIMER* TimerListHead = NULL;

void KmResetClock(void)
{
    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);
    VirtualNow = 0;
    TimerListHead = NULL;
    LeaveCriticalSection(&DpcCs);
}

long long KmNow(void)
{
    return VirtualNow;
}

void KmInitializeTimer(KM_TIMER* Timer)
{
    memset(Timer, 0, sizeof(*Timer));
}

BOOLEAN KmSetTimer(KM_TIMER* Timer, long long DueTime, KM_DPC* Dpc)
{
    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);

    BOOLEAN wasArmed = Timer->Armed ? TRUE : FALSE;

    //
    // Negative due times are relative to now, positive absolute -- the
    // kernel's convention, which the driver's ArmSocketTimeout relies on
    // (it passes a negative 100ns count).
    //
    Timer->DueTime = (DueTime < 0) ? (VirtualNow - DueTime) : DueTime;
    Timer->Dpc = Dpc;

    if (!Timer->Armed)
    {
        Timer->Armed = 1;
        Timer->Next = TimerListHead;
        TimerListHead = Timer;
    }

    LeaveCriticalSection(&DpcCs);

    return wasArmed;
}

BOOLEAN KmCancelTimer(KM_TIMER* Timer)
{
    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);

    BOOLEAN wasArmed = Timer->Armed ? TRUE : FALSE;

    if (Timer->Armed)
    {
        KM_TIMER** link = &TimerListHead;

        while (*link && *link != Timer)
        {
            link = &(*link)->Next;
        }

        if (*link == Timer)
        {
            *link = Timer->Next;
        }

        Timer->Armed = 0;
        Timer->Next = NULL;
    }

    LeaveCriticalSection(&DpcCs);

    return wasArmed;
}

int KmAdvanceTime(long long Milliseconds)
{
    KmEnsureDpcCs();
    EnterCriticalSection(&DpcCs);

    VirtualNow += Milliseconds * 10000LL;

    int fired = 0;
    KM_TIMER** link = &TimerListHead;

    while (*link)
    {
        KM_TIMER* timer = *link;

        if (timer->DueTime <= VirtualNow)
        {
            *link = timer->Next;
            timer->Armed = 0;
            timer->Next = NULL;

            if (timer->Dpc)
            {
                LeaveCriticalSection(&DpcCs);
                KmInsertQueueDpc(timer->Dpc, NULL, NULL);
                EnterCriticalSection(&DpcCs);
            }

            fired++;
            link = &TimerListHead;
            continue;
        }

        link = &timer->Next;
    }

    LeaveCriticalSection(&DpcCs);

    KmDrainDpcs();

    return fired;
}

///////////////////////////////////////////////////////////////////////////
// Lifetime accounting
///////////////////////////////////////////////////////////////////////////

static volatile long ObjectCounts[KmObjectMax];

//
// The floor KmResetObjects() and KmAssertQuiescent() compare against
// instead of zero. Exists for objects a ::testing::Environment allocates
// once for the whole binary's lifetime (StatisticsEnvironment's counter
// table, PathCacheEnvironment's cache state) -- those are legitimately
// still live through every individual test's reset and teardown, and
// without a floor a per-test ShimReset() would silently drop them from
// the ledger, then the first later call that frees one trips "destroyed
// more times than created" for an object the model no longer remembers
// creating.
//
static long ObjectFloor[KmObjectMax];

static const char* const ObjectNames[KmObjectMax] =
{
    "pool allocation",
    "IRP",
    "work item",
    "MDL",
    "socket"
};

void KmObjectCreated(KM_OBJECT Kind)
{
    InterlockedIncrement(&ObjectCounts[Kind]);
}

void KmObjectDestroyed(KM_OBJECT Kind)
{
    if (InterlockedDecrement(&ObjectCounts[Kind]) < 0)
    {
        KmReportViolation(KmViolationLifetime,
            "%s destroyed more times than created", ObjectNames[Kind]);
    }
}

long KmObjectsLive(KM_OBJECT Kind)
{
    return ObjectCounts[Kind];
}

void KmAssertQuiescent(const char* Where)
{
    for (int i = 0; i < KmObjectMax; ++i)
    {
        if (ObjectCounts[i] != ObjectFloor[i])
        {
            KmReportViolation(KmViolationLifetime,
                "%s: %ld %s(s) still live -- not quiescent",
                Where, ObjectCounts[i] - ObjectFloor[i], ObjectNames[i]);
        }
    }
}

void KmResetObjects(void)
{
    for (int i = 0; i < KmObjectMax; ++i)
    {
        ObjectCounts[i] = ObjectFloor[i];
    }
}

void KmAbsorbBaseline(void)
{
    for (int i = 0; i < KmObjectMax; ++i)
    {
        ObjectFloor[i] = ObjectCounts[i];
    }
}

///////////////////////////////////////////////////////////////////////////
// Threads
///////////////////////////////////////////////////////////////////////////

struct _KM_THREAD
{
    HANDLE Handle;
    PKM_THREAD_ROUTINE Routine;
    void* Context;
};

static DWORD WINAPI KmThreadTrampoline(LPVOID Parameter)
{
    KM_THREAD* thread = (KM_THREAD*)Parameter;

    //
    // A fresh thread starts at PASSIVE, like any kernel thread. Its own
    // TLS state is created on first use inside the routine.
    //
    KmThreadState()->Irql = KM_PASSIVE_LEVEL;

    thread->Routine(thread->Context);

    return 0;
}

KM_THREAD* KmStartThread(PKM_THREAD_ROUTINE Routine, void* Context)
{
    KM_THREAD* thread = (KM_THREAD*)calloc(1, sizeof(KM_THREAD));

    thread->Routine = Routine;
    thread->Context = Context;
    thread->Handle = CreateThread(NULL, 0, KmThreadTrampoline, thread, 0, NULL);

    return thread;
}

void KmJoinThread(KM_THREAD* Thread)
{
    if (!Thread)
    {
        return;
    }

    WaitForSingleObject(Thread->Handle, INFINITE);
    CloseHandle(Thread->Handle);
    free(Thread);
}

void KmInitializeBarrier(KM_BARRIER* Barrier, long Target)
{
    Barrier->Count = 0;
    Barrier->Target = Target;
}

void KmBarrierWait(KM_BARRIER* Barrier)
{
    InterlockedIncrement(&Barrier->Count);

    while (Barrier->Count < Barrier->Target)
    {
        YieldProcessor();
    }
}

void KmJitter(void)
{
    //
    // SwitchToThread rather than a spin: it actually gives up the
    // remainder of the quantum to another runnable thread on this
    // processor, which is what widens a race window. A spin on a
    // many-core box just burns one core while the other thread runs
    // undisturbed on another.
    //
    SwitchToThread();
}

///////////////////////////////////////////////////////////////////////////
// Lifecycle
///////////////////////////////////////////////////////////////////////////

void KmReset(void)
{
    KmResetClock();
    KmResetObjects();
    KmResetLockOrder();

    ExpectedViolation = KmViolationNone;
    RecordedViolation = KmViolationNone;

    KmThreadState()->Irql = KM_PASSIVE_LEVEL;
    KmThreadState()->HeldCount = 0;
}
