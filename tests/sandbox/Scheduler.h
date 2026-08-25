#pragma once

//
// Systematic interleaving exploration: run a concurrent body under EVERY
// thread schedule rather than under whichever one the OS happens to pick.
//
// This exists because the two tools either side of it cannot do this job.
// A stress test with real threads samples interleavings -- it passed a
// node table whose reap worker did not revalidate PinCount at all. CBMC
// encodes interleavings symbolically, but refuses any program with a
// shared variable of pointer type, and LIST_ENTRY.Flink is exactly that,
// so it checks nothing at all on this code.
//
// The approach here is the one CHESS used, taken to its conclusion: run
// the code concretely, but take the scheduler -- and the OS threads --
// away. Every modelled thread is a fiber on one host thread, so a
// scheduling point costs a stack-pointer swap rather than two kernel
// transitions and a context switch, and exactly one thread runs at a time,
// every lock operation and every KmJitter is a scheduling point, and the
// explorer replays the whole body once per distinct sequence of choices,
// depth-first, until the space is exhausted. Pointers cost nothing because
// they are real pointers.
//
// What it proves: for a body with a bounded number of scheduling points,
// no interleaving exists in which the assertions fail. That is a proof
// over schedules, not over input data -- the complement of what CBMC
// gives, which is why both are worth having.
//
// What it does not model: weak memory. Every thread sees every write
// immediately, so this finds ordering bugs and missing mutual exclusion,
// not missing barriers. BlorgFS uses interlocked operations and push locks
// (both full fences) for everything cross-thread, so that gap is narrow --
// but it is a gap, and ReadNoFence is where it would hide.
//

#ifdef __cplusplus
extern "C" {
#endif

//
// Most modelled threads a single exploration may spawn. The executor
// keeps one fiber per slot for the life of the process and re-runs them
// each replay, so this bounds both the schedule vector's fan-out and the
// fiber pool.
//
#define KM_SCHED_MAX_THREADS 8

//
// A run of the body under one schedule. Called once per interleaving, and
// must set up and tear down all of its own state: the explorer replays it
// from scratch every time.
//
typedef void (*KM_SCHED_BODY)(void* Context);

typedef struct _KM_SCHED_RESULT
{
    int Schedules;       // distinct interleavings actually executed
    int MaxDepth;        // most scheduling points seen in any one run
    int Deadlocks;       // runs where every thread was blocked
    int Truncated;       // runs cut off at KM_SCHED_MAX_DEPTH
} KM_SCHED_RESULT;

//
// Explores every interleaving of Body up to MaxSchedules. Returns when the
// space is exhausted or the cap is hit; Result->Schedules against the cap
// is how a caller tells "proved" from "sampled a lot".
//
// Setup runs first and spawns the threads; Teardown runs once every
// thread has finished. Both run on the exploring thread, outside the
// schedule, because a replay must start from the same state every time --
// state left behind by one interleaving would make the next one a
// different program.
KM_SCHED_RESULT KmExploreInterleavings(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context, int MaxSchedules);

//
// Samples MaxSchedules schedules uniformly at random instead of
// enumerating depth-first. No coverage claim -- breadth, not proof -- but
// it reaches spaces far too large to enumerate, at full scheduling
// granularity. Seeded, so a failure reproduces exactly. Deadlocks still
// unwind through the serial drain, and everything the enumeration path
// checks (violations, IRQL contracts, quiescence) is checked here too.
//
KM_SCHED_RESULT KmExploreInterleavingsSeeded(
    KM_SCHED_BODY Setup, KM_SCHED_BODY Teardown, void* Context,
    int MaxSchedules, unsigned int Seed);

//
// Happens-before race detection over the explored schedules, FastTrack
// style: program order advances each modelled thread's vector clock, a
// lock release publishes it under the lock's identity, an acquire joins
// it, and two accesses to the same registered address race when neither
// covers the other. Opt-in per exploration; unchecked memory is whatever
// nobody registered.
//
void KmSchedSetRaceDetection(int Enabled);
long KmSchedRaceCount(void);

//
// Register one memory access by the calling modelled thread. IsWrite
// distinguishes the FastTrack epochs. Call sites today: bodies that want
// their own shared state checked. Lock words are deliberately NOT
// registered -- the model mutates SchedState non-atomically under the
// baton, so registering it would report modelling artifacts rather than
// driver bugs; the publish/join hooks still give protected accesses
// exact ordering.
//
void KmSchedNoteAccess(const void* Address, int IsWrite);

//
// Publish/join synchronization order for a lock. Acquire must be
// registered BEFORE the claiming write, release AFTER the releasing
// write -- both while the caller holds the baton, which is what makes
// the ordering exact.
//
void KmSchedNoteAcquire(const void* LockAddress);
void KmSchedNoteRelease(const void* LockAddress);

//
// The next lever, deliberately not pulled yet: partial-order reduction.
// Depth-first enumeration replays ever-longer shared prefixes and
// explores interleavings that differ only in the order of independent
// operations. Sleep sets or happens-before pruning would cut these
// spaces by orders of magnitude -- but SOUND reduction needs to know
// which memory each scheduling point's step actually touched, and
// nothing records that today. Guessing conflicts would silently shrink
// coverage, which is the one failure mode worse than slowness. If this
// is ever built: instrument the primitives' reads and writes first, and
// let the reduction derive from what was really accessed.

//
// Starts a thread that participates in the exploration. Only valid inside
// a body; the thread does not run until the scheduler picks it.
//
void KmSchedSpawn(KM_SCHED_BODY Routine, void* Context);

//
// A scheduling point. Every lock operation reaches one, and a test can
// place extra ones anywhere a preemption would be interesting.
//
void KmSchedYield(void);

//
// True while an exploration is running. The lock primitives consult this
// to decide between OS blocking and cooperative blocking -- an OS-blocking
// acquire would deadlock instantly here, because the thread holding the
// lock is suspended and only the scheduler can wake it.
//
int KmSchedActive(void);

//
// The calling modelled thread's index (0..KM_SCHED_MAX_THREADS-1), or -1
// when the caller is the exploring thread itself or no exploration is
// running. This is the identity primitive for everything per-thread in
// the model: the executor runs every modelled thread as a fiber on ONE
// OS thread, so OS-thread identity (TLS, GetCurrentThreadId) is shared
// by all of them and would alias their state together.
//
int KmSchedSelfIndex(void);

//
// A stable per-modelled-thread identifier for ownership checks (lock
// owner fields and the like). Under an exploration each modelled thread
// gets a distinct synthetic id in a range no real thread id occupies;
// outside one this is just GetCurrentThreadId(). Ownership comparisons
// must go through this rather than mixing the two.
//
unsigned long KmSchedThreadId(void);

//
// Cooperative lock support. Acquire spins over yields until Predicate
// holds, so a thread waiting on a held lock is simply not scheduled until
// the holder releases it.
//
typedef int (*KM_SCHED_PREDICATE)(void* Context);

//
// The claim is invoked by the scheduler, while the caller still holds the
// baton, immediately after the last successful predicate test. That
// adjacency IS the mutual-exclusion argument: no scheduling point exists
// between test and claim, so under the one-thread-at-a-time model nothing
// can invalidate the test in between. Callers must therefore claim through
// this callback rather than after return -- any yield between the two is
// a TOCTOU window, and one such window (in the spin lock) let two threads
// hold the same lock.
//
// On an abandoned schedule (see the deadlock note on
// KmExploreInterleavings) the predicate may never hold; the claim then
// runs without it once the drainer reaches this thread. The drain is
// serial, so the unearned grant still cannot race anything, and the
// caller's acquire/release bookkeeping stays consistent enough to exit.
//
// Called from Setup or Teardown -- on the exploring fiber itself -- a
// failed predicate can never be satisfied by waiting, because HandOff
// cannot park the explorer and no worker runs between two tests. That is
// a replay that ended holding state: it is reported as a violation and
// the claim runs anyway, so teardown bookkeeping stays consistent and
// the run fails loudly instead of spinning forever.
//
typedef void (*KM_SCHED_CLAIM)(void* Context);

void KmSchedWaitUntilClaim(KM_SCHED_PREDICATE Predicate, void* PredicateContext,
    KM_SCHED_CLAIM Claim, void* ClaimContext, const char* What);

//
// Interlocked operations as scheduling points.
//
// Lock-based code interleaves at its lock operations, but a refcount
// protocol has no locks -- the socket watchdog's whole arbitration is two
// threads racing an InterlockedDecrement, and the node table's pin count
// is the same shape. Without a scheduling point at the
// atomic itself the explorer can only run one thread's entire body then
// the other's, which is the two orderings a hand-written test already
// covers.
//
// The yield goes BEFORE the operation: that is the point at which the
// other thread's view of the counter can still change the outcome.
//
//
// Atomic-granularity preemption is opt-in per exploration.
//
// With it off, threads interleave at lock operations and explicit yields.
// That is enough for lock-based protocols like the node table's pin/retire
// arbitration, and it is the granularity that proof was mutation-verified
// at.
//
// With it on, every interlocked operation is also a scheduling point,
// which is required for a protocol that has no locks at all -- the socket
// watchdog's whole arbitration is two threads racing one
// InterlockedDecrement, and at lock granularity the explorer finds only
// the two orderings a hand-written test already covers.
//
// RESOLVED 2026-08-24. This used to record a known limitation: turning
// atomic yields on for the node-table proof reported replay divergence at
// depth 17, blamed on this scheduler and never tracked down.
//
// It was the lock models. Both ERESOURCE and push locks claimed the lock
// on the strength of their wait having returned, without re-testing
// the predicate. RefreshRunnable promotes a waiter the moment its
// predicate holds -- not when it is next scheduled -- and discards the
// predicate as it does, so anything runnable in between could re-acquire
// and leave two threads believing they held the same lock. That makes the
// runnable set stop being a function of the schedule prefix, which is
// precisely what the divergence check reports.
//
// The first repair put re-test loops in the shim acquirers. The audit that
// followed found the class was not closed, twice over, and both are now
// fixed structurally rather than by convention:
//
// 1. KmSchedWaitUntil yielded the baton AFTER its final predicate test and
//    BEFORE returning, so a caller that claimed on return -- the spin lock
//    had no re-test loop -- still had a scheduling point between check and
//    claim. Two threads could both pass the check while the lock was free
//    and both claim afterwards; ten such interleavings exist in a trivial
//    two-thread body (SchedulerAudit.NoInterleavingDoubleGrantsTheSpinLock).
//    Claims now run inside KmSchedWaitUntilClaim, under the baton.
//
// 2. A deadlocked schedule woke every parked thread at once with "nothing
//    runnable". A shim whose acquire looped on the predicate re-tested,
//    found it false, and waited again -- forever. The join timed out, the
//    handle was closed on a live thread, and the zombie kept driving
//    scheduler state into the next replay (~7.5 s and one corrupted run
//    per deadlocked schedule:
//    SchedulerAudit.DeadlockedScheduleUnwindsPromptly). Deadlock now
//    drains the parked threads SERIALLY, in index order, recording no
//    choices: the abandoned run's bookkeeping stays consistent, its
//    threads exit promptly, and the next replay starts clean.
//
// The node-table pin proof has since run 2,000,000 atomic-granularity
// schedules to depth 42 with no divergence, no deadlock and no violation.
// That is a sample rather than a proof -- it hit the cap -- but it is
// three orders of magnitude past where the divergence used to appear.
//
// Cost is no longer the reason the gated proofs run at lock granularity:
// under the fiber executor the exhaustive lock-granularity proofs take
// seconds and the two-million-schedule soak about two minutes. They stay
// DISABLED_ tests in NodeTableSchedTest.cpp purely to keep the default
// gate fast; run them on demand.
//
void KmSchedSetAtomicYields(int Enabled);

long KmSchedInterlockedIncrement(long volatile* Target);
long KmSchedInterlockedDecrement(long volatile* Target);
long KmSchedInterlockedExchange(long volatile* Target, long Value);
long KmSchedInterlockedCompareExchange(long volatile* Target, long Exchange, long Comparand);
__int64 KmSchedInterlockedIncrement64(__int64 volatile* Target);
__int64 KmSchedInterlockedDecrement64(__int64 volatile* Target);

#ifdef __cplusplus
}
#endif
