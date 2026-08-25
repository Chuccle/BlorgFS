//
// Implementation of systematic interleaving exploration. See Scheduler.h
// for what it is for and what it does not cover.
//
// THE EXECUTOR
//
// MEMORY MODEL. Every field of scheduler state (Threads[], Current,
// Depth, Choice[], BlockedCount, ...) is a plain, non-atomic,
// non-volatile object written by one fiber and later read by another.
// That is sound, and it is worth stating precisely why rather than
// leaving it to folklore:
//
// 1. Compiler side. A thread's writes become visible to its successor
//    only across SwitchToFiber, an opaque externally-linked call. MSVC
//    must assume it can touch any object whose address has escaped --
//    and every one of these objects' addresses escapes through the shim
//    and predicate layers -- so it can neither cache them in registers
//    across the call nor reorder their accesses past it. There is no
//    relaxed read anywhere in this file that is not separated from the
//    corresponding write by such a call in both directions.
//
// 2. Hardware/OS side. SwitchToFiber enters the kernel's context-switch
//    path, which issues full barriers on every architecture Windows
//    ships on (x86/x64 get TSO from the hardware itself; ARM64 gets
//    explicit barriers in KiSwapContext). Successor sees predecessor's
//    stores; nothing weaker is relied on.
//
// What this forbids, and what the primitives therefore never do: reaching
// for OS identity (TLS, GetCurrentThreadId -- shared by all fibers of the
// host thread), blocking the host thread, or spawning a real thread mid-
// exploration. Those break the single-runner serialization the argument
// above rests on, and the primitives guard against all three.
//
// The approach is the one CHESS used, taken to its conclusion: not only is
// the OS scheduler taken away, the OS THREADS are too. Every modelled
// thread is a fiber on the exploring thread's single OS thread, and a
// scheduling point is a SwitchToFiber -- a stack-pointer swap worth tens
// of nanoseconds, where the previous design paid two kernel transitions
// and a context switch per handoff. There is no lock anywhere in this
// file, because there is nothing to lock: exactly one fiber runs at any
// instant, so every write to scheduler state has a single writer by
// construction.
//
// Identity is the one thing that cannot come from the OS. All fibers
// share the host thread's TLS and thread id, so per-modelled-thread state
// (IRQL, held locks, top-level IRP) is keyed by KmSchedSelfIndex -- fiber-
// local storage -- and ownership checks go through KmSchedThreadId, which
// hands out synthetic ids under an exploration. Anything that reaches for
// real OS identity or blocks the host thread breaks the model, and the
// primitives guard against both.
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

// Generous: atomic-granularity exploration reaches a few hundred scheduling
// points on a two-thread body, and truncation costs coverage rather than
// correctness, so the cap is a safety net rather than a tuning knob.
#define KM_SCHED_MAX_DEPTH   4096

//
// Distinct from -1 ("nothing can run") so a spawned-but-not-yet-run
// thread keeps waiting instead of exiting.
//
#define KM_SCHED_NOT_STARTED (-2)

typedef enum _KM_SCHED_STATE
{
    KmSchedRunnable = 0,
    KmSchedBlocked,
    KmSchedDone
} KM_SCHED_STATE;

typedef struct __declspec(align(64)) _KM_SCHED_THREAD
{
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

static KM_SCHED_THREAD Threads[KM_SCHED_MAX_THREADS];
static int ThreadCount = 0;
static int Current = -1;
static int Active = 0;

//
// Read by worker fibers inside AtomicYield while only ever written on the
// exploring thread between replays, when no worker is running. volatile
// documents that contract; the happens-before comes from the switch back
// to the explorer between replays.
//
static volatile int AtomicYields = 0;

//
// Random-sample mode: instead of enumerating the space depth-first, run
// the body MaxSchedules times, each choosing uniformly among the runnable
// threads at every scheduling point. Weaker per-run guarantees than
// enumeration -- no coverage claim, just breadth -- but it reaches spaces
// far too large to enumerate. Seeded with xorshift64*, so a failure
// reproduces exactly.
//
static int RandomMode = 0;
static unsigned __int64 RngState = 0;

//
// How many threads are currently KmSchedBlocked. RefreshRunnable runs at
// every scheduling point; skipping it entirely when nothing is blocked
// keeps the common uncontended case to a single compare.
//
static int BlockedCount = 0;

//
// ---- Happens-before race detection ------------------------------------
//
// FastTrack over the serialized execution. The explorer runs one thread
// at a time, so concurrency is reconstructed rather than observed: each
// thread's vector clock advances in program order, a lock release
// publishes the releaser's clock under the lock's identity, and an
// acquire joins it. Two accesses to the same address race when neither
// is covered by the other's clock.
//
// Addresses are REGISTERED, not inferred: interlocked targets and lock
// words are hooked in the shims, and bodies may register their own via
// KmSchedNoteAccess. Unregistered memory is not checked -- a stated
// boundary, not a hidden one. The detector is opt-in per exploration
// (KmSchedSetRaceDetection) so existing proofs pay nothing for it.
//
#define KM_RACE_SLOTS 512

typedef struct _KM_RACE_ENTRY
{
    const void* Address;
    int WriteThread;
    unsigned long WriteClock;
    int ReadThread[KM_SCHED_MAX_THREADS + 1];
    unsigned long ReadClock[KM_SCHED_MAX_THREADS + 1];
} KM_RACE_ENTRY;

typedef struct _KM_LOCK_CLOCK
{
    const void* Address;
    unsigned long Clock[KM_SCHED_MAX_THREADS + 1];
} KM_LOCK_CLOCK;

static KM_RACE_ENTRY RaceTable[KM_RACE_SLOTS];
static KM_LOCK_CLOCK LockClocks[KM_RACE_SLOTS];
static unsigned long Vc[KM_SCHED_MAX_THREADS + 1][KM_SCHED_MAX_THREADS + 1];
static int RaceDetection = 0;
//
// Set the moment any registration happens, so the per-replay reset of
// the race tables -- tens of kilobytes -- is paid only by explorations
// that actually use them.
//
static int RaceStateDirty = 0;
static long RacesReported = 0;
static int LastClockSlot = -1;

void KmSchedSetRaceDetection(int Enabled)
{
    RaceDetection = Enabled;
}

long KmSchedRaceCount(void)
{
    return RacesReported;
}

static KM_RACE_ENTRY* RaceFind(const void* Address)
{
    const unsigned int hash =
        ((unsigned int)(UINT_PTR)Address >> 4) * 2654435761u;
    const int start = (int)(hash % KM_RACE_SLOTS);

    for (int i = 0; i < KM_RACE_SLOTS; ++i)
    {
        KM_RACE_ENTRY* entry = &RaceTable[(start + i) % KM_RACE_SLOTS];

        if (entry->Address == Address)
        {
            return entry;
        }

        if (!entry->Address)
        {
            entry->Address = Address;
            return entry;
        }
    }

    return NULL;
}

static KM_LOCK_CLOCK* LockClockFind(const void* Address)
{
    const unsigned int hash =
        ((unsigned int)(UINT_PTR)Address >> 4) * 2654435761u;
    const int start = (int)(hash % KM_RACE_SLOTS);

    for (int i = 0; i < KM_RACE_SLOTS; ++i)
    {
        KM_LOCK_CLOCK* entry = &LockClocks[(start + i) % KM_RACE_SLOTS];

        if (entry->Address == Address)
        {
            return entry;
        }

        if (!entry->Address)
        {
            entry->Address = Address;
            return entry;
        }
    }

    return NULL;
}

//
// One clock tick per scheduled segment: the first registration a thread
// makes after regaining the baton advances its program-order clock, so
// every access within a segment shares one epoch.
//
static unsigned long* RaceMyClock(int* SlotOut)
{
    const int me = KmSchedSelfIndex();
    const int slot = (me >= 0) ? me : KM_SCHED_MAX_THREADS;

    *SlotOut = slot;

    if (LastClockSlot != slot)
    {
        Vc[slot][slot]++;
        LastClockSlot = slot;
    }

    return Vc[slot];
}

static void RaceReport(
    const char* Kind, const void* Address, int OtherSlot, int MySlot)
{
    if (RacesReported < 8)
    {
        fprintf(stderr,
            "[sched] RACE: unsynchronized %s on %p between modelled threads %d and %d\n",
            Kind, Address, OtherSlot == KM_SCHED_MAX_THREADS ? -1 : OtherSlot,
            MySlot == KM_SCHED_MAX_THREADS ? -1 : MySlot);
        fflush(stderr);
    }

    RacesReported++;

    KmReportViolation(KmViolationLifetime,
        "unsynchronized concurrent %s on a tracked address", Kind);
}

void KmSchedNoteAccess(const void* Address, int IsWrite)
{
    KM_RACE_ENTRY* entry;
    unsigned long* myVc;
    int slot;

    if (!RaceDetection || !Address)
    {
        return;
    }

    RaceStateDirty = 1;

    myVc = RaceMyClock(&slot);
    entry = RaceFind(Address);

    if (!entry)
    {
        return;
    }

    if (IsWrite)
    {
        if (entry->WriteClock > myVc[entry->WriteThread])
        {
            RaceReport("write-write", Address, entry->WriteThread, slot);
        }

        for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
        {
            if (entry->ReadClock[t] > myVc[t])
            {
                RaceReport("write-read", Address, t, slot);
            }
        }

        entry->WriteThread = slot;
        entry->WriteClock = myVc[slot];

        for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
        {
            entry->ReadClock[t] = 0;
        }
    }
    else
    {
        if (entry->WriteClock > myVc[entry->WriteThread])
        {
            RaceReport("read-write", Address, entry->WriteThread, slot);
        }

        entry->ReadThread[slot] = slot;
        entry->ReadClock[slot] = myVc[slot];
    }
}

void KmSchedNoteAcquire(const void* LockAddress)
{
    KM_LOCK_CLOCK* clock;
    unsigned long* myVc;
    int slot;

    if (!RaceDetection || !LockAddress)
    {
        return;
    }

    RaceStateDirty = 1;

    myVc = RaceMyClock(&slot);
    clock = LockClockFind(LockAddress);

    if (!clock)
    {
        return;
    }

    //
    // Join: everything the last releaser of THIS lock had seen is now
    // happens-before us.
    //
    for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
    {
        if (clock->Clock[t] > myVc[t])
        {
            myVc[t] = clock->Clock[t];
        }
    }
}

void KmSchedNoteRelease(const void* LockAddress)
{
    KM_LOCK_CLOCK* clock;
    unsigned long* myVc;
    int slot;

    if (!RaceDetection || !LockAddress)
    {
        return;
    }

    RaceStateDirty = 1;

    myVc = RaceMyClock(&slot);
    clock = LockClockFind(LockAddress);

    if (!clock)
    {
        return;
    }

    for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
    {
        clock->Clock[t] = myVc[t];
    }
}

//
// The schedule under test. Choice[d] is which of the runnable threads was
// picked at depth d; Options[d] is how many there were, which is what
// bounds the search.
//
static int Choice[KM_SCHED_MAX_DEPTH];
static int Options[KM_SCHED_MAX_DEPTH];

//
// Every thread's scheduler state (Runnable/Blocked/Done, two bits per
// slot) at each recorded depth. Neither the runnable COUNT nor the
// runnable SET alone pins the program: equal-sized runnable sets replay
// differently while passing a count check, and a thread that is Done in
// one replay but Blocked in another is invisible to any runnable-set
// comparison because it appears in neither set. The full state vector
// closes both: at a given schedule prefix it is a function of the body
// alone, so any replay-to-replay difference is a state leak.
//
// Two bits per slot in an unsigned short caps this at eight threads.
// Raising KM_SCHED_MAX_THREADS past that would SILENTLY TRUNCATE the
// snapshots and weaken the divergence check back toward count-only;
// the assert turns that into a build error instead.
//
C_ASSERT(KM_SCHED_MAX_THREADS <= 8);

static unsigned short StateSnapshot[KM_SCHED_MAX_DEPTH];
static int Depth = 0;
static int RecordedDepth = 0;
static int Truncated = 0;
static int Deadlocked = 0;
static int DeadlockReported = 0;

//
// Set when a schedule deadlocks and never cleared until the next replay.
// Everything a thread does after this is cleanup of an abandoned run: the
// lock primitives stop waiting on predicates that can no longer be
// satisfied, and the drainer in ChooseNext runs what is left one thread at
// a time.
//
static int Abandoned = 0;

//
// Fiber plumbing. One fiber per slot, created once and reused across
// replays: CreateFiber reserves a stack, and doing that 41k times would
// give back most of what the executor saves. A fiber's C locals persist
// across activations -- the stack is not torn down between switches --
// which is what makes the reuse invisible to FiberMain's loop.
//
static void* MainFiber = NULL;
static void* Fibers[KM_SCHED_MAX_THREADS];
static long FiberMode = 0;

//
// Fiber-local storage holding index+1 for worker fibers. This is the
// whole identity story: TLS cannot be used because every fiber of a host
// thread shares that thread's TLS.
//
static DWORD SelfFls = FLS_OUT_OF_INDEXES;

#define KM_SCHED_ID_BASE 0xE0000000ul

int KmSchedActive(void)
{
    return Active;
}

int KmSchedSelfIndex(void)
{
    if (!SelfFls || SelfFls == FLS_OUT_OF_INDEXES)
    {
        return -1;
    }

    void* value = FlsGetValue(SelfFls);
    return value ? ((int)(INT_PTR)value - 1) : -1;
}

unsigned long KmSchedThreadId(void)
{
    const int me = KmSchedSelfIndex();

    if (me >= 0)
    {
        return KM_SCHED_ID_BASE + (unsigned long)me;
    }

    return GetCurrentThreadId();
}

void CALLBACK FiberTrampoline(PVOID Parameter);

static void EnsureFibers(void)
{
    if (0 == InterlockedCompareExchange(&FiberMode, 1, 0))
    {
        SelfFls = FlsAlloc(NULL);

        MainFiber = ConvertThreadToFiber(NULL);

        if (!MainFiber)
        {
            fprintf(stderr, "[sched] ConvertThreadToFiber failed\n");
            exit(2);
        }
    }

    for (int i = 0; i < KM_SCHED_MAX_THREADS; ++i)
    {
        if (!Fibers[i])
        {
            Fibers[i] = CreateFiber(0, FiberTrampoline, (LPVOID)(INT_PTR)i);
        }
    }
}

//
// A blocked thread becomes runnable again as soon as its predicate holds.
// Re-testing here rather than on release is what keeps lock handoff out
// of the lock implementations.
//
static void RefreshRunnable(void)
{
    if (0 == BlockedCount)
    {
        return;
    }

    for (int i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].State == KmSchedBlocked && Threads[i].Predicate &&
            Threads[i].Predicate(Threads[i].PredicateContext))
        {
            Threads[i].State = KmSchedRunnable;
            Threads[i].Predicate = NULL;
            Threads[i].Waiting = NULL;
            BlockedCount--;
        }
    }
}

//
// xorshift64*: tiny, deterministic, adequate for spreading samples across
// a space. Used for nothing but choosing among runnable threads.
//
static int RngNext(int Bound)
{
    RngState ^= RngState >> 12;
    RngState ^= RngState << 25;
    RngState ^= RngState >> 27;
    const unsigned __int64 value = RngState * 2685821657736338717ull;

    return (int)(value % (unsigned __int64)Bound);
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
    unsigned short snapshot = 0;

    for (int i = 0; i < ThreadCount; ++i)
    {
        if (Threads[i].State == KmSchedRunnable)
        {
            runnable[count++] = i;
        }

        snapshot |= (unsigned short)((unsigned)Threads[i].State << (2 * i));
    }

    if (RandomMode && !Abandoned && count > 0)
    {
        //
        // No schedule vector, no replay: each run chooses uniformly among
        // the runnable threads and nothing is recorded. A deadlocked
        // random run still unwinds through the serial drain below, so the
        // process stays consistent either way.
        //
        Depth++;
        return runnable[RngNext(count)];
    }

    //
    // Past this point the run is abandoned: everything left is cleanup,
    // not exploration. The remaining threads are drained SERIALLY, lowest
    // index first, recording no choices -- their order is forced, so there
    // is nothing to enumerate and no reason to spend schedule vector on
    // it. Serial is the point, not an optimisation: an earlier design woke
    // every parked thread at once here, which resumed them in real
    // parallelism mid-driver-code; whatever they did to each other there
    // corrupted the next replay's starting state, and a waiter whose
    // acquire looped on its predicate spun forever instead of exiting,
    // costing a five-second join timeout and one zombie thread per
    // deadlocked schedule. One at a time keeps even the abandoned run's
    // bookkeeping consistent, so its threads exit promptly and cleanly.
    //
    if (Abandoned)
    {
        if (count > 0)
        {
            Depth++;
            return runnable[0];
        }

        for (int i = 0; i < ThreadCount; ++i)
        {
            if (Threads[i].State == KmSchedBlocked)
            {
                Threads[i].State = KmSchedRunnable;
                Threads[i].Predicate = NULL;
                Threads[i].Waiting = NULL;
                BlockedCount--;
                Depth++;
                return i;
            }
        }

        return -1;
    }

    if (0 == count)
    {
        int blocked = -1;

        for (int i = 0; i < ThreadCount; ++i)
        {
            if (Threads[i].State == KmSchedBlocked)
            {
                blocked = i;
                break;
            }
        }

        if (blocked < 0)
        {
            return -1;
        }

        //
        // A deadlock count on its own says a schedule ended with every
        // thread blocked, which is not enough to tell a real lock cycle
        // from a modelling artifact. Report what each thread was waiting
        // on the first time it happens; the count still carries the
        // frequency.
        //
        if (!DeadlockReported)
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
        Abandoned = 1;

        Threads[blocked].State = KmSchedRunnable;
        Threads[blocked].Predicate = NULL;
        Threads[blocked].Waiting = NULL;
        BlockedCount--;
        Depth++;

        return blocked;
    }

    if (Depth >= KM_SCHED_MAX_DEPTH)
    {
        //
        // Past the cap, stop EXPLORING but keep RUNNING: take the first
        // runnable thread and record no choice.
        //
        Truncated = 1;
        Depth++;

        return runnable[0];
    }

    if (Depth >= RecordedDepth)
    {
        Choice[Depth] = 0;
        Options[Depth] = count;
        StateSnapshot[Depth] = snapshot;
        RecordedDepth = Depth + 1;
    }
    else if (Options[Depth] != count || StateSnapshot[Depth] != snapshot)
    {
        //
        // The per-thread STATE VECTOR must be a function of the schedule
        // prefix, or replay is not replay. Count alone is not enough --
        // equal-sized but different runnable sets pass a count check --
        // and even the runnable set is not enough, because a thread that
        // finished in one replay but blocked in another shows up in
        // neither set. Any difference here means the body is doing
        // something nondeterministic outside the scheduler's control, and
        // every schedule explored after this point belongs to a different
        // program than the one recorded.
        //
        fprintf(stderr, "\n[sched] DIVERGENCE at depth %d: %d runnable, state 0x%04x, expected 0x%04x (RD=%d)\n",
            Depth, count, (unsigned)snapshot, (unsigned)StateSnapshot[Depth], RecordedDepth);

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
            "scheduler replay diverged at depth %d: state 0x%04x, expected 0x%04x",
            Depth, (unsigned)snapshot, (unsigned)StateSnapshot[Depth]);
    }

    const int picked = runnable[Choice[Depth] % count];
    Depth++;

    return picked;
}

//
// Hands the baton on. The caller is the current runner; on return either
// the baton is back (Current == Me, possibly immediately) or nothing can
// run (Current == -1). One SwitchToFiber per actual handoff -- tens of
// nanoseconds, no kernel involvement, no locks: the runner is the sole
// writer of everything it touches, and the switch orders memory for the
// successor (see MEMORY MODEL at the top of this file for the precise
// compiler and hardware arguments).
//
static void HandOff(int Blocking, KM_SCHED_PREDICATE Predicate, void* PredicateContext, const char* What)
{
    const int me = KmSchedSelfIndex();

    if (me < 0 || !Active)
    {
        return;
    }

    if (Blocking)
    {
        Threads[me].State = KmSchedBlocked;
        Threads[me].Predicate = Predicate;
        Threads[me].PredicateContext = PredicateContext;
        Threads[me].Waiting = What;
        BlockedCount++;
    }

    Current = ChooseNext();

    if (Current != me && Current >= 0)
    {
        SwitchToFiber(Fibers[Current]);
    }
}

void KmSchedYield(void)
{
    if (!Active)
    {
        return;
    }

    HandOff(0, NULL, NULL, NULL);
}

void KmSchedWaitUntilClaim(KM_SCHED_PREDICATE Predicate, void* PredicateContext,
    KM_SCHED_CLAIM Claim, void* ClaimContext, const char* What)
{
    if (!Active)
    {
        return;
    }

    //
    // Explorer identity (Setup and Teardown run on the exploring fiber).
    // HandOff cannot park this caller -- me is -1 -- and no worker can run
    // between two predicate tests here, because control never leaves this
    // fiber. A predicate that fails can therefore NEVER be satisfied by
    // waiting: it means a replay ended holding state. Report it and grant
    // under the baton, exactly as the abandoned drain does, so the
    // caller's bookkeeping stays consistent and the run fails loudly
    // instead of spinning a core forever. Before this guard such a wait
    // hung the exploration silently; the repro is
    // SchedulerAudit.TeardownWaitOnAReplayLeftHoldFailsLoudly.
    //
    if (KmSchedSelfIndex() < 0)
    {
        if (!Predicate(PredicateContext))
        {
            KmReportViolation(KmViolationLifetime,
                "%s waited on from the exploring thread with nothing left "
                "that could satisfy it -- a replay ended holding state", What);
        }

        Claim(ClaimContext);
        return;
    }

    while (!Predicate(PredicateContext))
    {
        //
        // A drained thread reaches this only by waiting again inside its
        // own body: the schedule is over, nothing will ever satisfy the
        // predicate, and blocking would ping-pong against the drainer
        // forever. Proceeding without the grant cannot race anything --
        // the drain runs one thread at a time, so the claim below is as
        // exclusive as it would have been with the predicate held.
        //
        if (Abandoned)
        {
            break;
        }

        HandOff(1, Predicate, PredicateContext, What);
    }

    //
    // The claim runs while this thread still holds the baton: no other
    // modelled thread can observe or mutate anything between the predicate
    // test above and the claim below. That adjacency IS the mutual-
    // exclusion argument, and it is why callers must claim through this
    // callback rather than after return -- any yield between the two is a
    // TOCTOU window, and one such window (in the spin lock) let two
    // threads hold the same lock.
    //
    Claim(ClaimContext);

    HandOff(0, NULL, NULL, NULL);
}

//
// Body of one pooled fiber, for the life of the process. The loop is what
// makes reuse work: after a replay ends, the fiber sits just past its
// last switch, falls through to the top, and picks up whatever routine
// the NEXT replay's Setup stored in its slot. Every activation of a given
// fiber shares these locals, so `me` is computed once and stays valid.
//
// Invariant worth stating: at a replay boundary every participating fiber
// is parked here, never inside driver code. A replay only ends when
// ChooseNext returns -1, and that requires every thread Done -- the drain
// runs blocked bodies to completion first. So switching into a fiber at
// the start of a replay always lands in this loop, never mid-routine.
//
void CALLBACK FiberTrampoline(PVOID Parameter)
{
    const int me = (int)(INT_PTR)Parameter;

    FlsSetValue(SelfFls, (PVOID)(INT_PTR)(me + 1));

    for (;;)
    {
        Threads[me].Routine(Threads[me].Context);

        Threads[me].State = KmSchedDone;
        Current = ChooseNext();

        if (Current >= 0)
        {
            SwitchToFiber(Fibers[Current]);
        }
        else
        {
            //
            // Nothing left to run: hand control back to the explorer,
            // which resumes inside RunOnce.
            //
            SwitchToFiber(MainFiber);
        }
    }
}

void KmSchedSpawn(KM_SCHED_BODY Routine, void* Context)
{
    EnsureFibers();

    //
    // The explorer holds the baton throughout Setup, so this is
    // single-writer even though no switch has happened yet.
    //
    if (ThreadCount >= KM_SCHED_MAX_THREADS)
    {
        KmReportViolation(KmViolationLifetime, "more than %d scheduled threads", KM_SCHED_MAX_THREADS);
        return;
    }

    const int index = ThreadCount++;

    Threads[index].Routine = Routine;
    Threads[index].Context = Context;
    Threads[index].State = KmSchedRunnable;
    Threads[index].Predicate = NULL;
    Threads[index].Waiting = NULL;
}

//
// One run of the body under the schedule currently in Choice[]. The body
// spawns its threads, then the explorer switches into the first chosen
// fiber and control returns when some fiber finds nothing left to run.
//
static void RunOnce(KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context)
{
    //
    // Fresh per-modelled-thread state (IRQL, held locks, shim scratch):
    // fibers are POOLED, so unlike the old spawn-a-real-thread-per-replay
    // design their state would otherwise leak across replays.
    //
    KmResetPerThreadModelState();

    ThreadCount = 0;
    BlockedCount = 0;
    Current = KM_SCHED_NOT_STARTED;

    //
    // Fresh happens-before state: clocks, publications and accesses
    // describe THIS replay only. Reset only when the previous replay
    // actually touched the tables -- the common proof pays nothing here.
    //
    if (RaceStateDirty)
    {
        ZeroMemory(Vc, sizeof(Vc));
        ZeroMemory(RaceTable, sizeof(RaceTable));
        ZeroMemory(LockClocks, sizeof(LockClocks));
        LastClockSlot = -1;
        RaceStateDirty = 0;
    }

    Depth = 0;
    Truncated = 0;
    Deadlocked = 0;
    Abandoned = 0;

    Setup(Context);

    Current = ChooseNext();

    if (Current >= 0)
    {
        SwitchToFiber(Fibers[Current]);

        //
        // Back here means Current went to -1: every thread is Done. That
        // is an invariant of the drain, but it is cheap to check and a
        // broken invariant here would silently corrupt every following
        // replay, so it fails loudly instead.
        //
        for (int i = 0; i < ThreadCount; ++i)
        {
            if (Threads[i].State != KmSchedDone)
            {
                fprintf(stderr, "[sched] INVARIANT: thread %d not Done at replay end (state=%d)\n",
                    i, (int)Threads[i].State);
                fflush(stderr);
                exit(2);
            }
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

    EnsureFibers();

    RecordedDepth = 0;
    RacesReported = 0;

    for (int d = 0; d < KM_SCHED_MAX_DEPTH; ++d)
    {
        Choice[d] = 0;
        Options[d] = 0;
        StateSnapshot[d] = 0;
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

KM_SCHED_RESULT KmExploreInterleavingsSeeded(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context,
    int MaxSchedules, unsigned int Seed)
{
    KM_SCHED_RESULT result = { 0, 0, 0, 0 };

    EnsureFibers();

    //
    // Non-zero even for Seed == 0: xorshift64* degenerates on an all-zero
    // state.
    //
    RngState = Seed
        ? (((unsigned __int64)Seed << 32) | 0x2545F4914F6CDD1Dull)
        : 0x9E3779B97F4A7C15ull;

    RandomMode = 1;
    RecordedDepth = 0;
    RacesReported = 0;

    KmSetLockIdRecycling(1);

    Active = 1;

    for (int i = 0; i < MaxSchedules; ++i)
    {
        RunOnce(Setup, Teardown, Context);

        result.Schedules++;

        if (Depth > result.MaxDepth)
        {
            result.MaxDepth = Depth;
        }

        result.Deadlocks += Deadlocked;
        result.Truncated += Truncated;
    }

    Active = 0;
    RandomMode = 0;

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