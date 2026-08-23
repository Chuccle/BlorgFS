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
// The approach here is the one CHESS used: run the code concretely, but
// take the scheduler away from the OS. Exactly one thread runs at a time,
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
// Cooperative lock support. Acquire spins over yields until Predicate
// holds, so a thread waiting on a held lock is simply not scheduled until
// the holder releases it.
//
typedef int (*KM_SCHED_PREDICATE)(void* Context);

void KmSchedWaitUntil(KM_SCHED_PREDICATE Predicate, void* Context, const char* What);

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
// KNOWN LIMITATION: turning it on for the node-table proof makes the
// explorer report replay divergence at depth 17. That is a defect in this
// scheduler, not in the driver -- the divergence check is telling the
// truth about something in the deeper exploration not being reproducible
// from the schedule prefix, and it has not been tracked down. Until it is,
// that proof runs at lock granularity.
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
