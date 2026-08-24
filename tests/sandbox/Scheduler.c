//
// Implementation of systematic interleaving exploration. See Scheduler.h
// for what it is for and what it does not cover.
//
// The mechanism is a single baton. Every participating thread blocks on a
// condition variable until it is the one nominated to run, so exactly one
// runs at a time and the OS scheduler never gets a say. At each scheduling
// point the running thread hands the baton on, and which thread it hands
// it to is read from a schedule vector the explorer is enumerating.
//
// Depth-first enumeration works by replay: run the body to completion
// recording, at each scheduling point, how many threads were runnable.
// Then find the last point that still has an unexplored alternative,
// increment it, discard everything after, and run the whole body again.
// The body must therefore be deterministic apart from the scheduling --
// which is why it sets up and tears down its own state each time.
//

#include "Scheduler.h"
#include "KernelModel.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#define KM_SCHED_MAX_THREADS 8
//
// Generous: atomic-granularity exploration reaches a few hundred
// scheduling points on a two-thread body, and truncation now costs
// coverage rather than correctness, so the cap is a safety net rather
// than a tuning knob.
//
#define KM_SCHED_MAX_DEPTH   4096

//
// Distinct from -1 ("nothing runnable, unwind") so a thread spawned before
// the baton is first handed out keeps waiting instead of exiting.
//
#define KM_SCHED_NOT_STARTED (-2)

typedef enum _KM_SCHED_STATE
{
    KmSchedRunnable = 0,
    KmSchedBlocked,
    KmSchedDone
} KM_SCHED_STATE;

typedef struct _KM_SCHED_THREAD
{
    HANDLE Handle;
    KM_SCHED_BODY Routine;
    void* Context;
    KM_SCHED_STATE State;

    //
    // A blocked thread names the condition it is waiting for rather than
    // just "blocked", so the scheduler can re-test it: a lock waiter
    // becomes runnable the moment the holder releases, without anyone
    // having to signal it.
    //
    KM_SCHED_PREDICATE Predicate;
    void* PredicateContext;
    const char* Waiting;
} KM_SCHED_THREAD;

static CRITICAL_SECTION SchedCs;
static CONDITION_VARIABLE SchedCv;
static long SchedCsInit = 0;

static KM_SCHED_THREAD Threads[KM_SCHED_MAX_THREADS];
static int ThreadCount = 0;
static int Current = -1;
static int Active = 0;
static int AtomicYields = 0;

//
// The schedule under test. Choice[d] is which of the runnable threads was
// picked at depth d; Options[d] is how many there were, which is what
// bounds the search.
//
static int Choice[KM_SCHED_MAX_DEPTH];
static int Options[KM_SCHED_MAX_DEPTH];
static int Depth = 0;
static int RecordedDepth = 0;
static int Truncated = 0;
static int Deadlocked = 0;
static int DeadlockReported = 0;

static DWORD SelfSlot = TLS_OUT_OF_INDEXES;

static void EnsureSched(void)
{
    if (0 == InterlockedCompareExchange(&SchedCsInit, 1, 0))
    {
        InitializeCriticalSection(&SchedCs);
        InitializeConditionVariable(&SchedCv);
        SelfSlot = TlsAlloc();
        InterlockedExchange(&SchedCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&SchedCsInit, 2, 2))
    {
        Sleep(0);
    }
}

int KmSchedActive(void)
{
    return Active;
}

static int SelfIndex(void)
{
    void* value = TlsGetValue(SelfSlot);
    return value ? ((int)(INT_PTR)value - 1) : -1;
}

static void SetSelfIndex(int Index)
{
    TlsSetValue(SelfSlot, (void*)(INT_PTR)(Index + 1));
}

//
// A blocked thread becomes runnable again as soon as its predicate holds.
// Re-testing here rather than on release is what keeps lock handoff out
// of the lock implementations.
//
static void RefreshRunnable(void)
{
    for (int i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].State == KmSchedBlocked && Threads[i].Predicate &&
            Threads[i].Predicate(Threads[i].PredicateContext))
        {
            Threads[i].State = KmSchedRunnable;
            Threads[i].Predicate = NULL;
            Threads[i].Waiting = NULL;
        }
    }
}

//
// Picks the next thread to run from the schedule, extending the schedule
// with choice 0 when this run has gone deeper than any before it.
// Returns -1 when nothing can run.
//
static int ChooseNext(void)
{
    RefreshRunnable();

    int runnable[KM_SCHED_MAX_THREADS];
    int count = 0;

    for (int i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].State == KmSchedRunnable)
        {
            runnable[count++] = i;
        }
    }

    if (0 == count)
    {
        for (int i = 0; i < ThreadCount; ++i)
        {
            if (Threads[i].State == KmSchedBlocked)
            {
                //
                // A deadlock count on its own says a schedule ended with
                // every thread blocked, which is not enough to tell a real
                // lock cycle from a modelling artifact. Report what each
                // thread was waiting on the first time it happens; the
                // count still carries the frequency.
                //
                if (!Deadlocked && !DeadlockReported)
                {
                    DeadlockReported = 1;

                    fprintf(stderr, "\n[sched] DEADLOCK at depth %d\n", Depth);

                    for (int t = 0; t < ThreadCount; ++t)
                    {
                        fprintf(stderr, "[sched]   thread %d state=%d waiting=%s\n",
                            t, (int)Threads[t].State,
                            Threads[t].Waiting ? Threads[t].Waiting : "-");
                    }

                    fprintf(stderr, "[sched]   prefix:");

                    for (int d = 0; d <= Depth && d < 40; ++d)
                    {
                        fprintf(stderr, " %d/%d", Choice[d], Options[d]);
                    }

                    fprintf(stderr, "\n\n");
                }

                Deadlocked = 1;
                break;
            }
        }

        return -1;
    }

    if (Depth >= KM_SCHED_MAX_DEPTH)
    {
        //
        // Past the cap, stop EXPLORING but keep RUNNING: take the first
        // runnable thread and record no choice.
        //
        // Returning -1 here is what caused "replay diverged". -1 means
        // "nothing can run, unwind", and the wait loop in HandOff exits on
        // it -- for every parked thread at once. They then resumed in real
        // parallelism, mid-way through driver code, and scribbled over the
        // state the next replay was supposed to start from. The divergence
        // was reported several depths later, nowhere near the cause.
        //
        Truncated = 1;
        Depth++;

        return runnable[0];
    }

    if (Depth >= RecordedDepth)
    {
        Choice[Depth] = 0;
        Options[Depth] = count;
        RecordedDepth = Depth + 1;
    }
    else if (Options[Depth] != count)
    {
        //
        // The set of runnable threads must be a function of the schedule
        // prefix, or replay is not replay. If this fires the body is doing
        // something nondeterministic outside the scheduler's control.
        //
        fprintf(stderr, "\n[sched] DIVERGENCE at depth %d: %d runnable, expected %d\n",
            Depth, count, Options[Depth]);

        for (int i = 0; i < ThreadCount; ++i)
        {
            fprintf(stderr, "[sched]   thread %d state=%d waiting=%s\n",
                i, (int)Threads[i].State, Threads[i].Waiting ? Threads[i].Waiting : "-");
        }

        fprintf(stderr, "[sched]   prefix:");

        for (int d = 0; d <= Depth && d < 40; ++d)
        {
            fprintf(stderr, " %d/%d", Choice[d], Options[d]);
        }

        fprintf(stderr, "\n");
        fflush(stderr);

        KmReportViolation(KmViolationLifetime,
            "scheduler replay diverged: %d runnable threads at depth %d, expected %d",
            count, Depth, Options[Depth]);
    }

    const int picked = runnable[Choice[Depth] % count];
    Depth++;

    return picked;
}

//
// Hands the baton on. The caller blocks until it is nominated again.
//
static void HandOff(int Blocking, KM_SCHED_PREDICATE Predicate, void* PredicateContext, const char* What)
{
    const int me = SelfIndex();

    if (me < 0 || !Active)
    {
        return;
    }

    EnterCriticalSection(&SchedCs);

    if (Blocking)
    {
        Threads[me].State = KmSchedBlocked;
        Threads[me].Predicate = Predicate;
        Threads[me].PredicateContext = PredicateContext;
        Threads[me].Waiting = What;
    }

    Current = ChooseNext();
    WakeAllConditionVariable(&SchedCv);

    while (Current != me && Current != -1)
    {
        SleepConditionVariableCS(&SchedCv, &SchedCs, INFINITE);
    }

    LeaveCriticalSection(&SchedCs);
}

void KmSchedYield(void)
{
    if (!Active)
    {
        return;
    }

    HandOff(0, NULL, NULL, NULL);
}

void KmSchedWaitUntil(KM_SCHED_PREDICATE Predicate, void* Context, const char* What)
{
    if (!Active)
    {
        return;
    }

    while (!Predicate(Context))
    {
        HandOff(1, Predicate, Context, What);

        if (Current == -1)
        {
            return;
        }
    }

    HandOff(0, NULL, NULL, NULL);
}

static DWORD WINAPI SchedTrampoline(LPVOID Parameter)
{
    const int index = (int)(INT_PTR)Parameter;

    SetSelfIndex(index);

    EnterCriticalSection(&SchedCs);

    while (Current != index && Current != -1)
    {
        SleepConditionVariableCS(&SchedCv, &SchedCs, INFINITE);
    }

    LeaveCriticalSection(&SchedCs);

    if (Current == index)
    {
        Threads[index].Routine(Threads[index].Context);
    }

    EnterCriticalSection(&SchedCs);
    Threads[index].State = KmSchedDone;
    Current = ChooseNext();
    WakeAllConditionVariable(&SchedCv);
    LeaveCriticalSection(&SchedCs);

    return 0;
}

void KmSchedSpawn(KM_SCHED_BODY Routine, void* Context)
{
    EnsureSched();

    EnterCriticalSection(&SchedCs);

    if (ThreadCount >= KM_SCHED_MAX_THREADS)
    {
        LeaveCriticalSection(&SchedCs);
        KmReportViolation(KmViolationLifetime, "more than %d scheduled threads", KM_SCHED_MAX_THREADS);
        return;
    }

    const int index = ThreadCount++;

    Threads[index].Routine = Routine;
    Threads[index].Context = Context;
    Threads[index].State = KmSchedRunnable;
    Threads[index].Predicate = NULL;
    Threads[index].Waiting = NULL;
    Threads[index].Handle = CreateThread(NULL, 0, SchedTrampoline, (LPVOID)(INT_PTR)index, 0, NULL);

    LeaveCriticalSection(&SchedCs);
}

//
// One run of the body under the schedule currently in Choice[]. The body
// spawns its threads, then this hands the baton over and waits for every
// thread to finish.
//
static void RunOnce(KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context)
{
    EnterCriticalSection(&SchedCs);
    ThreadCount = 0;

    //
    // Not -1. That value means "nothing can run, unwind", and a thread
    // spawned while it was set would fall straight out of its wait loop
    // and finish without ever running its routine.
    //
    Current = KM_SCHED_NOT_STARTED;

    Depth = 0;
    Truncated = 0;
    Deadlocked = 0;
    LeaveCriticalSection(&SchedCs);

    SetSelfIndex(-1);

    Setup(Context);

    EnterCriticalSection(&SchedCs);
    Current = ChooseNext();
    WakeAllConditionVariable(&SchedCv);
    LeaveCriticalSection(&SchedCs);

    for (int i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].Handle)
        {
            //
            // A deadlocked or truncated run leaves threads parked on the
            // condition variable. Waking them with Current == -1 is what
            // lets them fall out of their wait loops and exit.
            //
            WaitForSingleObject(Threads[i].Handle, 5000);
            CloseHandle(Threads[i].Handle);
            Threads[i].Handle = NULL;
        }
    }

    if (Teardown)
    {
        Teardown(Context);
    }
}

//
// Advances Choice[] to the next unexplored schedule, depth-first. Returns
// 0 when the space is exhausted.
//
static int NextSchedule(void)
{
    for (int d = RecordedDepth - 1; d >= 0; --d)
    {
        if (Choice[d] + 1 < Options[d])
        {
            Choice[d]++;
            RecordedDepth = d + 1;
            return 1;
        }
    }

    return 0;
}

KM_SCHED_RESULT KmExploreInterleavings(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context, int MaxSchedules)
{
    KM_SCHED_RESULT result = { 0, 0, 0, 0 };

    EnsureSched();

    RecordedDepth = 0;

    for (int d = 0; d < KM_SCHED_MAX_DEPTH; ++d)
    {
        Choice[d] = 0;
        Options[d] = 0;
    }

    //
    // Lock identities are recycled only for the duration of the
    // exploration: replaying a body that builds a node each time would
    // otherwise exhaust the model's lock table.
    //
    KmSetLockIdRecycling(1);

    Active = 1;

    do
    {
        RunOnce(Setup, Teardown, Context);

        result.Schedules++;

        if (Depth > result.MaxDepth)
        {
            result.MaxDepth = Depth;
        }

        result.Deadlocks += Deadlocked;
        result.Truncated += Truncated;

        if (result.Schedules >= MaxSchedules)
        {
            break;
        }
    } while (NextSchedule());

    Active = 0;

    KmSetLockIdRecycling(0);

    return result;
}

///////////////////////////////////////////////////////////////////////////
// Interlocked operations as scheduling points
///////////////////////////////////////////////////////////////////////////

//
// These call the compiler intrinsics directly rather than the Win32
// macros, because NtShim.h redirects those here -- going through them
// again would recurse.
//
void KmSchedSetAtomicYields(int Enabled)
{
    AtomicYields = Enabled;
}

static void AtomicYield(void)
{
    if (AtomicYields)
    {
        KmSchedYield();
    }
}

long KmSchedInterlockedIncrement(long volatile* Target)
{
    AtomicYield();
    return _InterlockedIncrement(Target);
}

long KmSchedInterlockedDecrement(long volatile* Target)
{
    AtomicYield();
    return _InterlockedDecrement(Target);
}

long KmSchedInterlockedExchange(long volatile* Target, long Value)
{
    AtomicYield();
    return _InterlockedExchange(Target, Value);
}

long KmSchedInterlockedCompareExchange(long volatile* Target, long Exchange, long Comparand)
{
    AtomicYield();
    return _InterlockedCompareExchange(Target, Exchange, Comparand);
}

__int64 KmSchedInterlockedIncrement64(__int64 volatile* Target)
{
    AtomicYield();
    return _InterlockedIncrement64(Target);
}

__int64 KmSchedInterlockedDecrement64(__int64 volatile* Target)
{
    AtomicYield();
    return _InterlockedDecrement64(Target);
}
