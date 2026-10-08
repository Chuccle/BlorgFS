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
// 2. Execution side. All fibers execute serially on one OS thread. These
//    accesses therefore belong to one host thread, regardless of which
//    modeled thread the current fiber represents. SwitchToFiber is a
//    usermode switch; no kernel transition or hardware fence is assumed.
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

//
// This translation unit compiles under /volatile:iso and ISO conformance
// mode (see the Scheduler.c items in the sandbox .vcxproj files), where
// volatile carries NO synchronization meaning -- deliberately. Anything
// whose visibility crosses contexts without a fiber switch between reader
// and writer goes through KmAtomicGet and KmAtomicExchange below, thin
// wrappers over the documented _Interlocked* intrinsics -- the same
// primitives the driver itself uses. Their architecture story lives at
// those wrappers, not repeated at each use. Every other static in this
// file is reached only across SwitchToFiber boundaries, and that
// executor argument is architecture-neutral too: the opaque external
// switch preserves the escaped state on this single host thread. Weak
// visibility between modeled threads is represented by WmLocations.
//

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
// Read by AtomicYield at every interlocked scheduling point; written by
// KmSchedSetAtomicYields from whatever configures the exploration. All
// access goes through KmAtomicGet/KmAtomicExchange. In practice writer
// and readers share the exploring thread and program order would
// suffice -- naming the orderings keeps the flag correct even if that
// execution shape ever changes.
//
static long AtomicYields = 0;

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
// Addresses are REGISTERED, not inferred: bodies register their own via
// KmSchedNoteAccess, and nothing else does. Lock words stay out for the
// reason given at KmSchedNoteAccess, and interlocked targets and the
// ReadNoFence family are atomic accesses, which cannot race. Driver memory
// is therefore not checked -- the plain accesses that could race are not
// visible without compiler instrumentation. A stated boundary, not a
// hidden one. The detector is opt-in per exploration
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
static int WeakMemory = 0;
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

    KmSchedNoteFootprint(Address, 1, IsWrite);

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

    KmSchedNoteFootprint(LockAddress, 1, 1);

    if ((!RaceDetection && !WeakMemory) || !LockAddress)
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
    // Join: everything the releasers of THIS lock had seen is now
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

    KmSchedNoteFootprint(LockAddress, 1, 1);

    if ((!RaceDetection && !WeakMemory) || !LockAddress)
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
    // Joined rather than overwritten: a lock's releasers have each seen
    // the last one, so for a lock the two agree, and a queue's pushers
    // (the shim's work queue) have not.
    //
    for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
    {
        if (myVc[t] > clock->Clock[t])
        {
            clock->Clock[t] = myVc[t];
        }
    }

    //
    // What this thread does next is not part of what it released.
    //
    myVc[slot]++;
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
// ---- Partial-order reduction ------------------------------------------
//
// Sleep sets (Godefroid), opt-in per exploration. A step is everything one
// thread does between two scheduling points, and its footprint is the
// memory it was seen to touch: lock words and predicate contexts, the
// targets of the interlocked and ReadNoFence-family shims, pool blocks,
// the shim's own shared queues, and whatever a body registers. Two steps
// of different threads whose footprints do not overlap on a write commute:
// running them in either order reaches the same state.
//
// At each node the explorer keeps the threads whose next step is already
// covered -- asleep. Picking thread t at a node puts every earlier sibling
// to sleep for the subtrees that follow, and a sleeping thread stays
// asleep down a branch for as long as the steps taken there commute with
// its own. A node at which every runnable thread is asleep starts only
// orders that were explored elsewhere; the run is finished, still checked,
// and not branched from (KM_SCHED_RESULT.Pruned).
//
// The footprint a sleeping thread carries is the one it recorded when it
// ran from that node in an earlier sibling. That is its next step exactly,
// because nothing that ran since touched what it touches. What makes this
// sound is that every way one thread affects another passes through
// something recorded: lock-protected data is covered by the lock, and the
// shared-without-a-lock accesses go through the shims. A plain racy
// access the driver makes outside both is not seen, and is the boundary
// of the reduction; the full search still explores every order of it.
//
// One symptom of an unrecorded access is checked on every pruned run. The
// run goes on with a sleeping thread, whose step was recorded at the node
// it was put to sleep at and should be repeated exactly: nothing since has
// written what it touches. A step that touches something else instead
// means one of the steps in between changed what it does without
// recording the write, and is reported as a violation.
//
#define KM_FOOTPRINT_SLOTS 16

typedef struct _KM_FOOTPRINT
{
    ULONG_PTR Start[KM_FOOTPRINT_SLOTS];
    ULONG_PTR End[KM_FOOTPRINT_SLOTS];
    unsigned char Write[KM_FOOTPRINT_SLOTS];
    unsigned char Count;

    //
    // More distinct ranges than slots: treated as touching everything,
    // which costs reduction and never coverage.
    //
    unsigned char Overflow;

    //
    // The union of the paths a step took for different values of its
    // weak-memory reads; a step taken again must fall within it rather
    // than repeat it.
    //
    unsigned char Merged;
} KM_FOOTPRINT;

static int Reduction = 0;

//
// Footprint of the step now running, and of each thread's step from each
// recorded node: the one it took there, or carried down asleep.
//
static KM_FOOTPRINT Step;
static KM_FOOTPRINT StepFootprint[KM_SCHED_MAX_DEPTH][KM_SCHED_MAX_THREADS];

//
// Per recorded node: the thread picked, the runnable set, the threads
// asleep on arrival, and the threads already explored from it. Bit i is
// slot i, which KM_SCHED_MAX_THREADS <= 8 keeps in a byte.
//
static unsigned char Picked[KM_SCHED_MAX_DEPTH];
static unsigned char RunnableSet[KM_SCHED_MAX_DEPTH];
static unsigned char Asleep[KM_SCHED_MAX_DEPTH];
static unsigned char Explored[KM_SCHED_MAX_DEPTH];

//
// Set when a node's thread is picked, so the first step taken from it
// replaces whatever an earlier subtree recorded there, and cleared once
// it has: later runs of the same step, down other values of its reads,
// add to its footprint instead.
//
static unsigned char StepFresh[KM_SCHED_MAX_DEPTH];

//
// Nodes that choose a value for a weak-memory read rather than a thread,
// and the thread node whose step made the read.
//
static unsigned char ValueNode[KM_SCHED_MAX_DEPTH];
static int ValueOwner[KM_SCHED_MAX_DEPTH];

//
// Whether the running step has read with more than one value to choose
// from.
//
static int StepValues = 0;

//
// Depth of the recorded node whose step is running, or -1 when the step
// belongs to no recorded node (truncated, abandoned or pruned).
//
static int StepNode = -1;
static int Pruned = 0;

//
// The step a pruned run continues with, and the footprint it was recorded
// with when it was put to sleep.
//
static int SleeperCheck = 0;
static KM_FOOTPRINT SleeperFootprint;

//
// One footprint report per exploration: once one is known to be wrong,
// every later pruning decision is suspect and the first report says all
// there is to say.
//
static int FootprintReported = 0;

//
// ---- Weak memory -------------------------------------------------------
//
// Opt-in per exploration (KmSchedSetWeakMemory). Without it every read of
// a shared location returns the last value written, which is sequential
// consistency, and neither ARM64 nor the compiler promise that: a read
// may return an older value than the last one written, as long as the
// orderings in the program allow it.
//
// The model tracks instrumented accesses over the happens-before
// clocks the race detector keeps. Every location the shims touch keeps its
// writes in the order they happened. A read may return the newest write,
// or any older one down to the newest write that happens-before the
// reader, and never older than one the same thread already read or wrote
// there. It never invents a write not yet made. An explicitly declared
// independent relaxed load/store pair may instead execute its store first,
// reaching load buffering with real writes and no speculative values.
// Each permitted order/value is a branch recorded in the schedule.
//
// The orderings (KM_ORDER_*):
//
// - Relaxed (NoFence): orders nothing. A relaxed write publishes only
//   what the writer's last full fence covered, and what a relaxed read
//   reads is joined at the reader's next full fence.
// - Acquire: the read joins the clock released with the write it reads.
// - Release: the write publishes the writer's clock.
// - Acquire-release: both, for a read-modify-write.
// - Sequentially consistent: the unsuffixed Interlocked operations, which
//   Windows makes full barriers, as are KeMemoryBarrier and MemoryBarrier.
//   A full fence acquires everything earlier fences published and
//   publishes everything its thread has seen, so the fences fall in one
//   order every thread agrees on.
// - Consume is not modelled apart from acquire, which is how compilers
//   implement it and what ReadPointerAcquire asks for. ReadPointerNoFence
//   is relaxed, though ARM64 orders a dereference after the read it
//   depends on: the model is weaker there than the hardware, never
//   stronger.
//
// A read-modify-write reads the newest write and carries forward what
// that write released, so an acquire that reads it synchronizes with the
// release it continues. Locks acquire and release their own clocks and
// are not fences. A write made without a shim, which the model cannot see
// happen, is taken to be visible to every thread as soon as a shim next
// touches the location.
//
#define KM_WM_LOCATIONS 256
#define KM_WM_HISTORY 128

typedef struct _KM_WM_WRITE
{
    __int64 Value;

    //
    // What an acquire that reads this write joins.
    //
    unsigned long Released[KM_SCHED_MAX_THREADS + 1];
    unsigned long Clock;

    //
    // Slot of the writer, or -1 for a write every thread already sees,
    // and the recorded node whose step made it, or -1.
    //
    int Thread;
    int Node;
} KM_WM_WRITE;

typedef struct _KM_WM_LOCATION
{
    const volatile void* Address;
    int Size;
    int Count;

    //
    // Per thread, the oldest write it may still read here.
    //
    int Seen[KM_SCHED_MAX_THREADS + 1];
    KM_WM_WRITE Writes[KM_WM_HISTORY];
} KM_WM_LOCATION;

static KM_WM_LOCATION WmLocations[KM_WM_LOCATIONS];
static int WmLocationCount = 0;

//
// The clock full fences publish to each other; per thread, its clock at
// its last full fence, which its relaxed writes publish; and the clocks
// of the writes its relaxed reads read since then, which its next full
// fence acquires.
//
static unsigned long WmFenceClock[KM_SCHED_MAX_THREADS + 1];
static unsigned long WmFenced[KM_SCHED_MAX_THREADS + 1][KM_SCHED_MAX_THREADS + 1];
static unsigned long WmPending[KM_SCHED_MAX_THREADS + 1][KM_SCHED_MAX_THREADS + 1];

//
// A read that began its step and returned an older write than the newest
// one: the footprint entry of the read, and the first recorded node since
// the reader's previous step that wrote the location again, or -1.
//
static int StaleNode = -1;
static ULONG_PTR StaleStart;
static ULONG_PTR StaleEnd;

static int ChooseValue(int Count);

void KmSchedSetWeakMemory(int Enabled)
{
    WeakMemory = Enabled;
}

static void ClockJoin(unsigned long* Into, const unsigned long* From)
{
    for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
    {
        if (From[t] > Into[t])
        {
            Into[t] = From[t];
        }
    }
}

static void WmFullFence(int Slot)
{
    //
    // The fence order decides what later reads may return, so for the
    // reduction every full fence writes it, and no two commute.
    //
    KmSchedNoteFootprint((const void*)WmFenceClock, sizeof(WmFenceClock), 1);

    ClockJoin(Vc[Slot], WmFenceClock);
    ClockJoin(Vc[Slot], WmPending[Slot]);
    ZeroMemory(WmPending[Slot], sizeof(WmPending[Slot]));
    ClockJoin(WmFenceClock, Vc[Slot]);
    CopyMemory(WmFenced[Slot], Vc[Slot], sizeof(WmFenced[Slot]));
}

static __int64 WmLoad(const volatile void* Address, int Size)
{
    return (8 == Size) ? *(const volatile __int64*)Address : *(const volatile long*)Address;
}

static KM_WM_WRITE* WmAppend(KM_WM_LOCATION* Location, __int64 Value, int Slot)
{
    if (KM_WM_HISTORY == Location->Count)
    {
        KmReportViolation(KmViolationLifetime,
            "weak memory: more than %d writes to one location in a run", KM_WM_HISTORY);
        return NULL;
    }

    KM_WM_WRITE* write = &Location->Writes[Location->Count];

    write->Value = Value;
    write->Thread = Slot;
    write->Node = StepNode;
    ZeroMemory(write->Released, sizeof(write->Released));

    if (Slot >= 0)
    {
        write->Clock = Vc[Slot][Slot];
        Location->Seen[Slot] = Location->Count;
    }
    else
    {
        //
        // Seen by everyone from here on.
        //
        write->Clock = 0;

        for (int t = 0; t <= KM_SCHED_MAX_THREADS; ++t)
        {
            Location->Seen[t] = Location->Count;
        }
    }

    Location->Count++;

    return write;
}

//
// The location a shim is about to touch, or NULL when the access is not
// modelled: weak memory off, or no modelled thread running. A value that
// differs from the last write recorded was written without a shim.
//
static KM_WM_LOCATION* WmBegin(const volatile void* Address, int Size, int* Slot)
{
    if (!WeakMemory || Current < 0)
    {
        return NULL;
    }

    RaceStateDirty = 1;
    RaceMyClock(Slot);

    for (int i = 0; i < WmLocationCount; ++i)
    {
        KM_WM_LOCATION* location = &WmLocations[i];

        if (location->Address == Address)
        {
            const __int64 now = WmLoad(Address, Size);

            if (now != location->Writes[location->Count - 1].Value)
            {
                WmAppend(location, now, -1);
            }

            return location;
        }
    }

    if (KM_WM_LOCATIONS == WmLocationCount)
    {
        KmReportViolation(KmViolationLifetime,
            "weak memory: more than %d locations in a run", KM_WM_LOCATIONS);
        return NULL;
    }

    KM_WM_LOCATION* location = &WmLocations[WmLocationCount++];

    location->Address = Address;
    location->Size = Size;
    location->Count = 0;
    WmAppend(location, WmLoad(Address, Size), -1);

    return location;
}

//
// A read that is not part of a read-modify-write: picks the write it
// returns.
//
static __int64 WmRead(KM_WM_LOCATION* Location, int Slot, int Order)
{
    int oldest = Location->Seen[Slot];

    for (int i = Location->Count - 1; i > oldest; --i)
    {
        const KM_WM_WRITE* write = &Location->Writes[i];

        if (write->Thread < 0 || write->Clock <= Vc[Slot][write->Thread])
        {
            oldest = i;
            break;
        }
    }

    const int read = Location->Count - 1 - ChooseValue(Location->Count - oldest);
    const KM_WM_WRITE* write = &Location->Writes[read];

    Location->Seen[Slot] = read;

    if (Reduction && StepNode >= 0 && 1 == Step.Count && !Step.Overflow)
    {
        int previous = StepNode - 1;

        while (previous >= 0 && (ValueNode[previous] || Picked[previous] != Picked[StepNode]))
        {
            previous--;
        }

        for (int i = read + 1; i < Location->Count; ++i)
        {
            if (Location->Writes[i].Node > previous)
            {
                StaleNode = Location->Writes[i].Node;
                StaleStart = Step.Start[0];
                StaleEnd = Step.End[0];
                break;
            }
        }
    }

    ClockJoin((Order & KM_ORDER_ACQUIRE) ? Vc[Slot] : WmPending[Slot], write->Released);

    return write->Value;
}

//
// After the shim's write, or its read-modify-write, which reads the
// newest write: records the value now in memory.
//
static void WmWrite(KM_WM_LOCATION* Location, int Slot, int ReadModifyWrite, int Order)
{
    unsigned long continued[KM_SCHED_MAX_THREADS + 1] = { 0 };

    if (ReadModifyWrite)
    {
        const KM_WM_WRITE* read = &Location->Writes[Location->Count - 1];

        CopyMemory(continued, read->Released, sizeof(continued));
        ClockJoin((Order & KM_ORDER_ACQUIRE) ? Vc[Slot] : WmPending[Slot], read->Released);
    }

    KM_WM_WRITE* write = WmAppend(Location, WmLoad(Location->Address, Location->Size), Slot);

    if (write)
    {
        ClockJoin(write->Released, (Order & KM_ORDER_RELEASE) ? Vc[Slot] : WmFenced[Slot]);
        ClockJoin(write->Released, continued);
    }

    Vc[Slot][Slot]++;
}

//
// A read-modify-write, with a full fence on either side when it is
// sequentially consistent.
//
static KM_WM_LOCATION* WmBeginReadModifyWrite(const volatile void* Address, int Size, int Order, int* Slot)
{
    KM_WM_LOCATION* location = WmBegin(Address, Size, Slot);

    if (location && KM_ORDER_SEQ_CST == Order)
    {
        WmFullFence(*Slot);
    }

    return location;
}

static void WmEndReadModifyWrite(KM_WM_LOCATION* Location, int Slot, int Order)
{
    if (Location)
    {
        WmWrite(Location, Slot, 1, Order);

        if (KM_ORDER_SEQ_CST == Order)
        {
            WmFullFence(Slot);
        }
    }
}

//
// Memory reused by an allocation is a new location.
//
static void WmForget(ULONG_PTR Start, ULONG_PTR End)
{
    for (int i = 0; i < WmLocationCount; )
    {
        const ULONG_PTR address = (ULONG_PTR)WmLocations[i].Address;

        if (address >= Start && address < End)
        {
            WmLocations[i] = WmLocations[--WmLocationCount];
        }
        else
        {
            ++i;
        }
    }
}

//
// Heap blocks allocated during the current replay, sorted by address.
// A freed block stays, marked, until an allocation overlaps it: a step
// that touches it is a use after free.
// A footprint names memory inside one of them by the block's allocation
// serial and an offset, not by its address: the allocator hands the same
// replay prefix different addresses from one replay to the next, and a
// footprint recorded in one replay is compared against steps taken in
// later ones. The serial is stable, because the heap token puts every
// allocating step in a fixed order.
//
#define KM_TRACKED_BLOCKS 4096

typedef struct _KM_BLOCK
{
    ULONG_PTR Start;
    ULONG_PTR End;
    unsigned Serial;
    int Freed;
} KM_BLOCK;

static KM_BLOCK Blocks[KM_TRACKED_BLOCKS];
static int BlockCount = 0;
static unsigned BlockSerial = 0;

//
// Set when a replay allocates more blocks than are tracked; memory in the
// untracked ones cannot be named stably, so every footprint is treated as
// touching everything until the next replay.
//
static int BlocksOverflowed = 0;

//
// An encoded range has the top bit set, which no user-mode address has,
// then the serial and the offset into the block.
//
#define KM_BLOCK_TAG ((ULONG_PTR)1 << 63)

C_ASSERT(sizeof(ULONG_PTR) == 8);

//
// Where a replay's footprints stop repeating the previous replay's: nodes
// above this depth are replayed steps, whose footprints must come out
// exactly as recorded.
//
static int ReplayedDepth = 0;

//
// Splitting the full search between processes. A run's group is its first
// KM_SHARD_DEPTH choices; groups are numbered in the order the search
// reaches them and dealt out round-robin. A run whose group belongs to
// another shard is finished unrecorded, like a pruned one, and the search
// backtracks straight past it.
//
#define KM_SHARD_DEPTH 12

static int ShardIndex = 0;
static int ShardCount = 1;
static int ShardGroups = 0;
static int ShardNewGroup = 0;
static int ShardOwned = 1;
static int Unowned = 0;

//
// Distinct outcomes the bodies reported this exploration, as an open-
// addressed set of mixed signatures (zero marks an empty slot).
//
#define KM_OUTCOME_SLOTS 65536

static unsigned __int64 OutcomeSet[KM_OUTCOME_SLOTS];
static int OutcomeCount = 0;
static unsigned __int64 OutcomeDigest = 0;

void KmSchedNoteFootprint(const void* Address, size_t Length, int IsWrite)
{
    //
    // Current is the running modelled thread only while one is running:
    // Setup and Teardown run with it negative, and their accesses belong
    // to no step.
    //
    if (Current < 0 || !Address)
    {
        return;
    }

    ULONG_PTR start = (ULONG_PTR)Address;
    ULONG_PTR end = start + (Length ? Length : 1);
    const KM_BLOCK* block = NULL;

    int low = 0;
    int high = BlockCount - 1;

    while (low <= high)
    {
        const int middle = (low + high) / 2;

        if (start < Blocks[middle].Start)
        {
            high = middle - 1;
        }
        else if (start >= Blocks[middle].End)
        {
            low = middle + 1;
        }
        else
        {
            block = &Blocks[middle];
            break;
        }
    }

    if (block && block->Freed)
    {
        KmReportViolation(KmViolationPool,
            "thread %d touched a block freed earlier in this run, %zu bytes in (depth %d)",
            Current, (size_t)(start - block->Start), Depth);
    }

    if (!Reduction || Step.Overflow)
    {
        return;
    }

    if (BlocksOverflowed)
    {
        Step.Overflow = 1;
        return;
    }

    if (block)
    {
        const ULONG_PTR base = KM_BLOCK_TAG | ((ULONG_PTR)block->Serial << 32);

        end = base + (end - block->Start);
        start = base + (start - block->Start);
    }

    for (int i = 0; i < Step.Count; ++i)
    {
        if (Step.Start[i] == start && Step.End[i] == end)
        {
            Step.Write[i] |= (unsigned char)(IsWrite != 0);
            return;
        }
    }

    if (Step.Count == KM_FOOTPRINT_SLOTS)
    {
        Step.Overflow = 1;
        return;
    }

    Step.Start[Step.Count] = start;
    Step.End[Step.Count] = end;
    Step.Write[Step.Count] = (unsigned char)(IsWrite != 0);
    Step.Count++;
}

void KmSchedNoteAllocation(const void* Block, size_t Size)
{
    if (!Active || !Block)
    {
        return;
    }

    WmForget((ULONG_PTR)Block, (ULONG_PTR)Block + (Size ? Size : 1));

    if (BlockCount == KM_TRACKED_BLOCKS)
    {
        BlocksOverflowed = 1;
        return;
    }

    const ULONG_PTR start = (ULONG_PTR)Block;
    const ULONG_PTR end = start + (Size ? Size : 1);
    int at = 0;

    while (at < BlockCount && Blocks[at].End <= start)
    {
        at++;
    }

    int past = at;

    while (past < BlockCount && Blocks[past].Start < end)
    {
        past++;
    }

    memmove(&Blocks[at + 1], &Blocks[past], (BlockCount - past) * sizeof(Blocks[0]));
    BlockCount += 1 - (past - at);

    Blocks[at].Start = start;
    Blocks[at].End = end;
    Blocks[at].Serial = ++BlockSerial;
    Blocks[at].Freed = 0;
}

void KmSchedNoteFree(const void* Block)
{
    if (!Active || !Block)
    {
        return;
    }

    for (int i = 0; i < BlockCount; ++i)
    {
        if (Blocks[i].Start == (ULONG_PTR)Block)
        {
            Blocks[i].Freed = 1;
            return;
        }
    }
}

static int FootprintsConflict(const KM_FOOTPRINT* A, const KM_FOOTPRINT* B)
{
    if (A->Overflow || B->Overflow)
    {
        return 1;
    }

    for (int i = 0; i < A->Count; ++i)
    {
        for (int j = 0; j < B->Count; ++j)
        {
            if ((A->Write[i] | B->Write[j]) &&
                A->Start[i] < B->End[j] && B->Start[j] < A->End[i])
            {
                return 1;
            }
        }
    }

    return 0;
}

static int FootprintsEqual(const KM_FOOTPRINT* A, const KM_FOOTPRINT* B)
{
    if (A->Count != B->Count || A->Overflow != B->Overflow)
    {
        return 0;
    }

    for (int i = 0; i < A->Count; ++i)
    {
        if (A->Start[i] != B->Start[i] || A->End[i] != B->End[i] || A->Write[i] != B->Write[i])
        {
            return 0;
        }
    }

    return 1;
}

static void FootprintMerge(KM_FOOTPRINT* Into, const KM_FOOTPRINT* From)
{
    Into->Merged = 1;
    Into->Overflow |= From->Overflow;

    for (int j = 0; j < From->Count && !Into->Overflow; ++j)
    {
        int i = 0;

        while (i < Into->Count && (Into->Start[i] != From->Start[j] || Into->End[i] != From->End[j]))
        {
            i++;
        }

        if (i == Into->Count)
        {
            if (KM_FOOTPRINT_SLOTS == Into->Count)
            {
                Into->Overflow = 1;
                break;
            }

            Into->Start[i] = From->Start[j];
            Into->End[i] = From->End[j];
            Into->Write[i] = 0;
            Into->Count++;
        }

        Into->Write[i] |= From->Write[j];
    }
}

//
// Whether a step taken again touched what it was recorded touching: the
// same, or for a merged footprint, nothing outside it.
//
static int FootprintMatches(const KM_FOOTPRINT* Step, const KM_FOOTPRINT* Recorded)
{
    if (!Recorded->Merged)
    {
        return FootprintsEqual(Step, Recorded);
    }

    if (Recorded->Overflow)
    {
        return 1;
    }

    if (Step->Overflow)
    {
        return 0;
    }

    for (int j = 0; j < Step->Count; ++j)
    {
        int i = 0;

        while (i < Recorded->Count &&
               (Recorded->Start[i] != Step->Start[j] || Recorded->End[i] != Step->End[j]))
        {
            i++;
        }

        if (i == Recorded->Count || (Step->Write[j] && !Recorded->Write[i]))
        {
            return 0;
        }
    }

    return 1;
}

void KmSchedSetReduction(int Enabled)
{
    Reduction = Enabled;
}

void KmSchedSetShard(int Index, int Count)
{
    ShardIndex = Index;
    ShardCount = Count;
}

//
// Called once per run, at the shard depth or at the run's end if it never
// gets there: a run that starts a new group decides whether this shard
// owns it, and every other run inherits the decision of its group.
//
static void ShardDecide(void)
{
    if (ShardNewGroup)
    {
        ShardOwned = (ShardGroups++ % ShardCount) == ShardIndex;
        ShardNewGroup = 0;
    }

    Unowned = !ShardOwned;

    if (Unowned && RecordedDepth > Depth)
    {
        RecordedDepth = Depth;
    }
}

int KmSchedCopyOutcomes(unsigned __int64* Outcomes, int Capacity)
{
    int count = 0;

    for (int slot = 0; slot < KM_OUTCOME_SLOTS && count < Capacity; ++slot)
    {
        if (OutcomeSet[slot])
        {
            Outcomes[count++] = OutcomeSet[slot];
        }
    }

    return count;
}

void KmSchedNoteOutcome(unsigned __int64 Outcome)
{
    if (Unowned)
    {
        return;
    }

    //
    // splitmix64 finaliser: spreads structured signatures over the table
    // and makes the digest below a fair summary of the set.
    //
    unsigned __int64 mixed = Outcome + 0x9E3779B97F4A7C15ull;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    mixed ^= mixed >> 31;

    if (0 == mixed)
    {
        mixed = 1;
    }

    unsigned slot = (unsigned)mixed & (KM_OUTCOME_SLOTS - 1);

    while (OutcomeSet[slot])
    {
        if (OutcomeSet[slot] == mixed)
        {
            return;
        }

        slot = (slot + 1) & (KM_OUTCOME_SLOTS - 1);
    }

    if (OutcomeCount == KM_OUTCOME_SLOTS / 2)
    {
        KmReportViolation(KmViolationLifetime,
            "more than %d distinct outcomes in one exploration", KM_OUTCOME_SLOTS / 2);
        return;
    }

    OutcomeSet[slot] = mixed;
    OutcomeCount++;

    //
    // A sum is independent of the order outcomes were first seen in, so
    // two explorations reaching the same set report the same digest.
    //
    OutcomeDigest += mixed;
}

static void ResetOutcomes(void)
{
    if (OutcomeCount)
    {
        ZeroMemory(OutcomeSet, sizeof(OutcomeSet));
        OutcomeCount = 0;
    }

    OutcomeDigest = 0;
}

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

//
// Whether SelfFls holds a real allocation. Readiness must NOT be inferred
// by inspecting SelfFls itself: FlsAlloc can legitimately return index 0,
// and treating that as "not allocated" would make KmSchedSelfIndex report
// -1 for every fiber -- HandOff would then no-op, every modelled thread
// would run start-to-finish serially, and the exploration would silently
// lose ALL interleaving coverage while still passing its assertions.
//
// Release-published by the initializer after SelfFls and MainFiber are
// fully set, acquire-read by KmSchedSelfIndex, so a reader that sees 1
// also sees the initialized SelfFls no matter which OS thread it is on.
//
static long SelfFlsReady = 0;

#define KM_SCHED_ID_BASE 0xE0000000ul

//
// Named atomic primitives for the cross-context flags. Kept as wrappers
// rather than spread through the code so the architecture story lives in
// exactly one place:
//
// - Every _Interlocked* intrinsic is a full-fence RMW on every target
//   MSVC ships -- x86, x64, ARM32, ARM64: x86/x64 locked instructions are
//   always full fences, and the ARM implementations map to interlocked
//   operations at least as strong. Acquire-and-release-or-stronger at
//   every site, so no caller depends on anything weaker than it looks.
// - The _acq/_rel-suffixed spellings would let an ARM build drop to
//   ldar/stlr strength. They are deliberately NOT used: on this toolset
//   they declare but do not link on x64, and these flags' access rates
//   make full-fence cost unmeasurable next to the fiber switches around
//   them.
// - If this file ever compiles under a non-MSVC compiler, these two
//   wrappers are the only things to rewrite, around <stdatomic.h> or the
//   platform equivalent.
//
static long KmAtomicGet(long* Target)
{
    return _InterlockedOr(Target, 0);
}

static long KmAtomicExchange(long* Target, long Value)
{
    return _InterlockedExchange(Target, Value);
}

int KmSchedActive(void)
{
    return Active;
}

int KmSchedSelfIndex(void)
{
    if (0 == KmAtomicGet(&SelfFlsReady))
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
    //
    // Init-once. The exchange is a full-barrier RMW: the winner's stores
    // of SelfFls, MainFiber and the fiber pool are published before
    // SelfFlsReady's release store, and every later caller's acquire side
    // orders its check against that publication. Only ever 0 -> 1, so an
    // unconditional exchange is equivalent to the compare-exchange it
    // replaced.
    //
    if (0 == KmAtomicExchange(&FiberMode, 1))
    {
        SelfFls = FlsAlloc(NULL);

        MainFiber = ConvertThreadToFiber(NULL);

        if (!MainFiber)
        {
            fprintf(stderr, "[sched] ConvertThreadToFiber failed\n");
            exit(2);
        }

        //
        // Set only after the allocation actually succeeded, and never
        // cleared: fibers exist for the life of the process. The fence on
        // the exchange publishes SelfFls and MainFiber with it.
        //
        KmAtomicExchange(&SelfFlsReady, 1);
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
// Sets up the sleep set of a node reached for the first time and picks
// the first thread to explore from it. Returns 0 when every runnable
// thread is asleep, which means the node is not worth recording.
//
static int ReduceFreshNode(const int* Runnable, int Count)
{
    unsigned char runnableSet = 0;
    unsigned char asleep = 0;

    for (int i = 0; i < Count; ++i)
    {
        runnableSet |= (unsigned char)(1u << Runnable[i]);
    }

    if (Depth > 0)
    {
        //
        // Inherited from the parent: what was asleep there, and the
        // siblings explored before the step just taken, stay asleep here
        // when their next step commutes with it.
        //
        int parent = Depth - 1;

        while (parent > 0 && ValueNode[parent])
        {
            parent--;
        }

        const int ran = Picked[parent];
        const KM_FOOTPRINT* taken = &StepFootprint[parent][ran];
        const unsigned candidates = (Asleep[parent] | Explored[parent]) & ~(1u << ran);

        for (int t = 0; t < ThreadCount; ++t)
        {
            if ((candidates & (1u << t)) &&
                !FootprintsConflict(&StepFootprint[parent][t], taken))
            {
                asleep |= (unsigned char)(1u << t);
                StepFootprint[Depth][t] = StepFootprint[parent][t];
            }
        }
    }

    const unsigned awake = runnableSet & ~asleep;

    if (0 == awake)
    {
        return 0;
    }

    unsigned long first;
    _BitScanForward(&first, awake);

    RunnableSet[Depth] = runnableSet;
    Asleep[Depth] = asleep;
    Explored[Depth] = 0;
    Picked[Depth] = (unsigned char)first;
    StepFresh[Depth] = 1;

    return 1;
}

//
// Which of Count values a weak-memory read returns: a node of the
// schedule like a choice of thread, so replay and backtracking treat both
// alike. 0, the newest write, is the first explored.
//
static int ChooseValue(int Count)
{
    if (Count <= 1)
    {
        return 0;
    }

    StepValues = 1;

    if (RandomMode && !Abandoned)
    {
        return RngNext(Count);
    }

    if (Abandoned || Pruned || Unowned || RandomMode)
    {
        return 0;
    }

    if (Depth >= KM_SCHED_MAX_DEPTH)
    {
        Truncated = 1;
        return 0;
    }

    if (ShardCount > 1 && !Reduction && Depth == KM_SHARD_DEPTH)
    {
        ShardDecide();

        if (Unowned)
        {
            return 0;
        }
    }

    if (Depth >= RecordedDepth)
    {
        Choice[Depth] = 0;
        Options[Depth] = Count;
        StateSnapshot[Depth] = 0;
        ValueNode[Depth] = 1;
        ValueOwner[Depth] = StepNode;
        RecordedDepth = Depth + 1;
    }
    else if (!ValueNode[Depth] || Options[Depth] != Count)
    {
        KmReportViolation(KmViolationLifetime,
            "scheduler replay diverged at depth %d: a read had %d values to choose from, "
            "expected %d", Depth, Count, ValueNode[Depth] ? Options[Depth] : 0);
    }

    return Choice[Depth++];
}

//
// Whether the stale read the step just ended began with is one the
// explorer reaches anyway. Newer writes to the location were made since
// the reader's previous step. If the reader's step commutes with every
// step from the first of those writes on, apart from the writes to the
// location it read, the same execution with the reader's step moved
// before them is an order of its own, explored separately: there the
// value read is the newest, or stale with nothing written since the
// reader's previous step, and the steps it moved past see none of it.
// This run reaches nothing that one does not.
//
static int StaleReadRepeats(void)
{
    KM_FOOTPRINT own = Step;

    for (int i = 0; i < own.Count; ++i)
    {
        if (own.Start[i] == StaleStart && own.End[i] == StaleEnd && !own.Write[i])
        {
            own.Count--;
            own.Start[i] = own.Start[own.Count];
            own.End[i] = own.End[own.Count];
            own.Write[i] = own.Write[own.Count];
            break;
        }
    }

    for (int d = StaleNode; d < StepNode; ++d)
    {
        if (!ValueNode[d] && FootprintsConflict(&own, &StepFootprint[d][Picked[d]]))
        {
            return 0;
        }
    }

    return 1;
}

//
// Picks the next thread to run from the schedule, extending the schedule
// with choice 0 when this run has gone deeper than any before it.
// Returns -1 when nothing can run.
//
static int ChooseNext(void)
{
    if (Reduction)
    {
        //
        // The step that just ended belongs to the node it was picked at.
        //
        if (StepNode >= 0)
        {
            KM_FOOTPRINT* recorded = &StepFootprint[StepNode][Picked[StepNode]];

            //
            // A replayed step must touch what it touched last time. A
            // footprint that names memory by something that changes from
            // replay to replay cannot be compared with anything.
            //
            if (StepNode < ReplayedDepth && !FootprintReported &&
                !FootprintMatches(&Step, recorded))
            {
                FootprintReported = 1;

                KmReportViolation(KmViolationLifetime,
                    "partial-order reduction: a replayed step touched different memory than "
                    "when it was recorded (depth %d)", StepNode);
            }

            if (StepFresh[StepNode])
            {
                *recorded = Step;
                recorded->Merged = (unsigned char)StepValues;
                StepFresh[StepNode] = 0;
            }
            else if (recorded->Merged || StepValues)
            {
                FootprintMerge(recorded, &Step);
            }
            else
            {
                *recorded = Step;
            }

            if (StaleNode >= 0 && StaleReadRepeats())
            {
                Pruned = 1;
            }

            StepNode = -1;
        }
        else if (SleeperCheck)
        {
            SleeperCheck = 0;

            if (!FootprintReported && !FootprintMatches(&Step, &SleeperFootprint))
            {
                FootprintReported = 1;

                KmReportViolation(KmViolationLifetime,
                    "partial-order reduction: a sleeping thread's step changed under steps "
                    "recorded as independent of it -- a shared access is missing from a "
                    "footprint (depth %d)", Depth);
            }
        }

        Step.Count = 0;
        Step.Overflow = 0;
        StepValues = 0;
        StaleNode = -1;
    }

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

    //
    // A pruned run is finished the same way: it is a real execution and
    // its checks still run, but every order it could branch into from
    // here was explored elsewhere.
    //
    if (ShardCount > 1 && !Reduction && Depth == KM_SHARD_DEPTH)
    {
        ShardDecide();
    }

    if (Pruned || Unowned)
    {
        Depth++;
        return runnable[0];
    }

    if (Depth >= RecordedDepth)
    {
        if (Reduction && !ReduceFreshNode(runnable, count))
        {
            Pruned = 1;
            SleeperCheck = 1;
            SleeperFootprint = StepFootprint[Depth][runnable[0]];
            Depth++;

            return runnable[0];
        }

        Choice[Depth] = 0;
        Options[Depth] = count;
        StateSnapshot[Depth] = snapshot;
        ValueNode[Depth] = 0;
        RecordedDepth = Depth + 1;
    }
    else if (ValueNode[Depth] || Options[Depth] != count || StateSnapshot[Depth] != snapshot)
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

    const int picked = Reduction ? Picked[Depth] : runnable[Choice[Depth] % count];

    if (Reduction)
    {
        Explored[Depth] |= (unsigned char)(1u << picked);
        StepNode = Depth;
    }

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

    //
    // The predicate reads the lock and the claim writes it. Each test
    // starts a step once the thread has been parked, so the lock goes into
    // the footprint again after every resume.
    //
    KmSchedNoteFootprint(PredicateContext, 1, 1);

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

        KmSchedNoteFootprint(PredicateContext, 1, 1);
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
        ZeroMemory(WmFenceClock, sizeof(WmFenceClock));
        ZeroMemory(WmFenced, sizeof(WmFenced));
        ZeroMemory(WmPending, sizeof(WmPending));
        LastClockSlot = -1;
        RaceStateDirty = 0;
    }

    WmLocationCount = 0;
    StepValues = 0;
    StaleNode = -1;

    Depth = 0;
    Truncated = 0;
    Deadlocked = 0;
    Abandoned = 0;
    Pruned = 0;
    Unowned = 0;
    StepNode = -1;
    SleeperCheck = 0;
    BlockCount = 0;
    BlockSerial = 0;
    BlocksOverflowed = 0;
    Step.Count = 0;
    Step.Overflow = 0;

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

    if (ShardCount > 1 && !Reduction && ShardNewGroup)
    {
        ShardDecide();
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
        if (ValueNode[d])
        {
            if (Choice[d] + 1 < Options[d])
            {
                Choice[d]++;
                RecordedDepth = d + 1;
                ReplayedDepth = (ValueOwner[d] >= 0) ? ValueOwner[d] : d;
                ShardNewGroup |= d < KM_SHARD_DEPTH;
                return 1;
            }

            continue;
        }

        if (Reduction)
        {
            const unsigned untried = RunnableSet[d] & ~Asleep[d] & ~Explored[d];

            if (untried)
            {
                unsigned long next;
                _BitScanForward(&next, untried);

                Picked[d] = (unsigned char)next;
                StepFresh[d] = 1;
                RecordedDepth = d + 1;
                ReplayedDepth = d;
                return 1;
            }

            continue;
        }

        if (Choice[d] + 1 < Options[d])
        {
            Choice[d]++;
            RecordedDepth = d + 1;
            ShardNewGroup |= d < KM_SHARD_DEPTH;
            return 1;
        }
    }

    return 0;
}

KM_SCHED_RESULT KmExploreInterleavings(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context, int MaxSchedules)
{
    KM_SCHED_RESULT result = { 0 };

    EnsureFibers();

    RecordedDepth = 0;
    ReplayedDepth = 0;
    RacesReported = 0;
    FootprintReported = 0;
    ShardGroups = 0;
    ShardNewGroup = 1;
    ShardOwned = 1;
    ResetOutcomes();

    for (int d = 0; d < KM_SCHED_MAX_DEPTH; ++d)
    {
        Choice[d] = 0;
        Options[d] = 0;
        StateSnapshot[d] = 0;
        ValueNode[d] = 0;
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

        if (Unowned)
        {
            continue;
        }

        result.Schedules++;

        if (Depth > result.MaxDepth)
        {
            result.MaxDepth = Depth;
        }

        result.Deadlocks += Deadlocked;
        result.Truncated += Truncated;
        result.Pruned += Pruned;

        if (result.Schedules >= MaxSchedules)
        {
            break;
        }
    } while (NextSchedule());

    Active = 0;

    KmSetLockIdRecycling(0);

    result.Outcomes = OutcomeCount;
    result.OutcomeDigest = OutcomeDigest;

    return result;
}

KM_SCHED_RESULT KmExploreInterleavingsSeeded(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context,
    int MaxSchedules, unsigned int Seed)
{
    KM_SCHED_RESULT result = { 0 };

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
    ReplayedDepth = 0;
    RacesReported = 0;
    FootprintReported = 0;
    ResetOutcomes();

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
        result.Pruned += Pruned;
    }

    Active = 0;
    RandomMode = 0;

    KmSetLockIdRecycling(0);

    result.Outcomes = OutcomeCount;
    result.OutcomeDigest = OutcomeDigest;

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
    KmAtomicExchange(&AtomicYields, Enabled);
}

static void AtomicYield(void)
{
    if (0 != KmAtomicGet(&AtomicYields))
    {
        KmSchedYield();
    }
}

long KmSchedInterlockedIncrement(long volatile* Target, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    long result = _InterlockedIncrement(Target);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

long KmSchedInterlockedDecrement(long volatile* Target, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    long result = _InterlockedDecrement(Target);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

long KmSchedInterlockedExchange(long volatile* Target, long Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    long result = _InterlockedExchange(Target, Value);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

long KmSchedInterlockedCompareExchange(long volatile* Target, long Exchange, long Comparand, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    long result = _InterlockedCompareExchange(Target, Exchange, Comparand);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

__int64 KmSchedInterlockedIncrement64(__int64 volatile* Target, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    __int64 result = _InterlockedIncrement64(Target);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

__int64 KmSchedInterlockedDecrement64(__int64 volatile* Target, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    __int64 result = _InterlockedDecrement64(Target);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

long KmSchedInterlockedOr(long volatile* Target, long Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    long result = _InterlockedOr(Target, Value);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

__int64 KmSchedInterlockedAdd64(__int64 volatile* Target, __int64 Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    __int64 result = _InterlockedExchangeAdd64(Target, Value) + Value;
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

__int64 KmSchedInterlockedExchangeAdd64(__int64 volatile* Target, __int64 Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    __int64 result = _InterlockedExchangeAdd64(Target, Value);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

void* KmSchedInterlockedCompareExchangePointer(void* volatile* Target, void* Exchange, void* Comparand, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBeginReadModifyWrite(Target, sizeof(*Target), Order, &slot);
    void* result = _InterlockedCompareExchangePointer(Target, Exchange, Comparand);
    WmEndReadModifyWrite(location, slot, Order);

    return result;
}

long KmSchedReadLong(long volatile* Source, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Source, sizeof(*Source), 0);

    int slot = 0;
    KM_WM_LOCATION* location = WmBegin(Source, sizeof(*Source), &slot);

    return location ? (long)WmRead(location, slot, Order) : *Source;
}

__int64 KmSchedReadLong64(__int64 volatile* Source, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Source, sizeof(*Source), 0);

    int slot = 0;
    KM_WM_LOCATION* location = WmBegin(Source, sizeof(*Source), &slot);

    return location ? (__int64)WmRead(location, slot, Order) : *Source;
}

void* KmSchedReadPointer(void* volatile* Source, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Source, sizeof(*Source), 0);

    int slot = 0;
    KM_WM_LOCATION* location = WmBegin(Source, sizeof(*Source), &slot);

    return location ? (void*)WmRead(location, slot, Order) : *Source;
}

void KmSchedWriteLong(long volatile* Target, long Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBegin(Target, sizeof(*Target), &slot);
    *Target = Value;

    if (location)
    {
        WmWrite(location, slot, 0, Order);
    }
}

void KmSchedWriteLong64(__int64 volatile* Target, __int64 Value, int Order)
{
    AtomicYield();
    KmSchedNoteFootprint((const void*)Target, sizeof(*Target), 1);

    int slot = 0;
    KM_WM_LOCATION* location = WmBegin(Target, sizeof(*Target), &slot);
    *Target = Value;

    if (location)
    {
        WmWrite(location, slot, 0, Order);
    }
}

//
// The caller declares independence: no value/address/control dependency
// may connect this load to the store. Track both locations before choosing
// an order, so the reduction cannot commute the choice past a conflicting
// access. Both component accesses retain their usual scheduling points.
//
long KmSchedIndependentRelaxedLoadStore(long volatile* Source, long volatile* Target, long Value)
{
    const ULONG_PTR source = C_CAST(ULONG_PTR, Source);
    const ULONG_PTR target = C_CAST(ULONG_PTR, Target);

    if (!Source || !Target ||
        (source <= target ? target - source : source - target) < sizeof(*Source))
    {
        KmReportViolation(KmViolationLifetime, "independent relaxed pair has overlapping locations");
        return 0;
    }

    AtomicYield();
    KmSchedNoteFootprint(C_CAST(const void*, Source), sizeof(*Source), 0);
    KmSchedNoteFootprint(C_CAST(const void*, Target), sizeof(*Target), 1);

    if (WeakMemory && Current >= 0 && ChooseValue(2))
    {
        KmSchedWriteLong(Target, Value, KM_ORDER_RELAXED);
        return KmSchedReadLong(Source, KM_ORDER_RELAXED);
    }

    const long value = KmSchedReadLong(Source, KM_ORDER_RELAXED);
    KmSchedWriteLong(Target, Value, KM_ORDER_RELAXED);
    return value;
}

//
// A full fence with no access of its own: KeMemoryBarrier and
// MemoryBarrier.
//
void KmSchedMemoryBarrier(void)
{
    AtomicYield();

    if (WeakMemory && Current >= 0)
    {
        int slot = 0;

        RaceStateDirty = 1;
        RaceMyClock(&slot);
        WmFullFence(slot);
    }
}
