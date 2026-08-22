#pragma once

//
// An executable model of the kernel rules BlorgFS depends on.
//
// The first sandbox replaced kernel APIs with stubs so Client.c would
// link. That is enough to test parsing and it is not enough to test
// anything the user of a filesystem driver actually loses sleep over:
// whether a lock is taken in a legal order, whether a completion can
// re-enter a path that is already on the stack, whether a DPC racing a
// completion frees the context twice, whether a drain really drains.
//
// So the primitives here are not stubs. They enforce:
//
//   IRQL        Tracked per thread. Raised by spinlock acquisition,
//               lowered by release, forced to DISPATCH inside DPCs, and
//               checked on entry to every API with an IRQL contract. A
//               paged allocation at DISPATCH aborts here exactly as it
//               bugchecks there.
//
//   Locks       Real mutual exclusion plus rule checking: recursive
//               acquisition (self-deadlock), release at the wrong IRQL,
//               release by the wrong thread, and -- the one that finds
//               real bugs -- lock ORDER. Every acquisition while holding
//               another lock records an edge; an edge that closes a cycle
//               is a latent AB/BA deadlock and fails the test that
//               provoked it, whether or not it deadlocked this time.
//
//   DPCs        A real queue, drained at DISPATCH_LEVEL, with the
//               kernel's one-instance-per-DPC rule enforced.
//
//   Timers      On a virtual clock the test advances. This is what makes
//               timeout handling testable at all: a 30-second watchdog is
//               not something a test can wait for, but advancing a
//               counter past its due time fires the same DPC through the
//               same code.
//
//   Quiescence  Everything with a lifetime -- allocations, work items,
//               DPCs, timers, threads -- is counted, so "did teardown
//               actually drain" is an assertion rather than a hope.
//
// Threads are real OS threads, because reentrancy and lock ordering are
// not observable single-threaded. Determinism comes from the test
// controlling the interleaving points (a barrier, a scripted stall),
// not from pretending concurrency away.
//
// What this still cannot prove, and no usermode model can: that the real
// kernel's IRQL, pool and MDL semantics match this model. It encodes the
// documented rules; Driver Verifier in a VM is what checks the encoding
// against reality. The two are complementary, and this one runs in
// seconds on every change.
//

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////
// IRQL
///////////////////////////////////////////////////////////////////////////

//
// Per-thread, because IRQL is a per-processor concept and each modelled
// thread stands in for a processor. A completion routine running on the
// "WSK thread" at DISPATCH must not see the PASSIVE level of the thread
// that issued the request.
//
unsigned char KmGetIrql(void);
void KmSetIrql(unsigned char Irql);

//
// Raise/lower with the kernel's rules: raising to a lower level is
// illegal, and lowering to a higher one is a bug in the caller's
// save/restore pairing. Both abort.
//
unsigned char KmRaiseIrql(unsigned char NewIrql);
void KmLowerIrql(unsigned char OldIrql);

//
// Aborts with a diagnostic if the current IRQL exceeds Max. Every
// modelled API with an IRQL contract calls this, so a violation is
// reported at the offending call rather than as a mystery later.
//
void KmRequireIrqlAtMost(unsigned char Max, const char* Api);

///////////////////////////////////////////////////////////////////////////
// Failure reporting
///////////////////////////////////////////////////////////////////////////

//
// Model violations are routed through here rather than abort() directly,
// so a test can arm an expectation ("this next operation must be
// rejected") and assert on it instead of dying. Unexpected violations
// still terminate the process with a diagnostic, which is the right
// outcome for a rule nobody claimed to be testing.
//
typedef enum _KM_VIOLATION
{
    KmViolationNone = 0,
    KmViolationIrql,
    KmViolationLockOrder,
    KmViolationLockRecursion,
    KmViolationLockOwner,
    KmViolationPool,
    KmViolationDpc,
    KmViolationLifetime
} KM_VIOLATION;

void KmReportViolation(KM_VIOLATION Kind, const char* Format, ...);

//
// Arms expectation of a specific violation. The next matching violation
// is recorded instead of aborting, which is how a test proves the model
// itself detects what it claims to.
//
void KmExpectViolation(KM_VIOLATION Kind);
KM_VIOLATION KmTakeViolation(void);

///////////////////////////////////////////////////////////////////////////
// Spin locks
///////////////////////////////////////////////////////////////////////////

typedef struct _KM_LOCK
{
    //
    // Identity for order tracking. Assigned on first acquisition, so a
    // lock needs no registration step and every KSPIN_LOCK in the driver
    // participates automatically.
    //
    int Id;

    const char* Name;

    CRITICAL_SECTION Cs;
    long Initialized;

    unsigned long OwnerThread;
    unsigned char OwnerPreviousIrql;

    //
    // Held-state under systematic exploration only. A spin lock cannot use
    // the CRITICAL_SECTION there: exactly one thread runs at a time, so a
    // blocking acquire parks the caller where nothing can wake it.
    //
    int SchedState;
} KM_LOCK;

void KmInitializeLock(KM_LOCK* Lock, const char* Name);
unsigned char KmAcquireLock(KM_LOCK* Lock);
void KmReleaseLock(KM_LOCK* Lock, unsigned char OldIrql);

//
// Shared/exclusive, for the push locks the node table and path cache
// use. Shared acquisition participates in order tracking the same way;
// two readers do not conflict but a reader taking a second lock still
// records the edge.
//
void KmAcquireLockShared(KM_LOCK* Lock);
void KmReleaseLockShared(KM_LOCK* Lock);

// Number of locks held by the calling thread, for invariant assertions.
int KmLocksHeld(void);

//
// Order tracking for locks the model does not own -- push locks and
// ERESOURCEs, whose mutual exclusion comes from an SRWLOCK rather than a
// KM_LOCK. The caller notes the acquisition before blocking and the
// release after, so the same edge graph that catches an AB/BA inversion
// between two spin locks also catches one between a bucket push lock and
// the VCB resource, which is the inversion that would actually deadlock
// this driver.
//
void KmNoteLockAcquire(int Id, const char* Name);
void KmNoteLockRelease(int Id);

// Allocates an id in the same space KM_LOCK uses, so ids never collide.
int KmAllocateLockId(void);

//
// Returns an id to the pool when its lock is destroyed. Without this a
// workload that creates and destroys locks in a loop -- systematic
// exploration replaying a body that builds a node each time -- exhausts
// KM_MAX_LOCKS and the exhaustion is reported as if the driver were at
// fault.
//
void KmReleaseLockId(int Id);

//
// Recycling is off by default and on only during systematic exploration.
//
// It exists for the replay workload, which builds and destroys a node per
// interleaving and would otherwise exhaust KM_MAX_LOCKS in a few thousand
// runs. Under exploration exactly one thread runs at a time, so reusing an
// identity is safe. Under real threads it is not: a lock can be destroyed
// while another thread is still between KmNoteLockAcquire and
// KmNoteLockRelease on it, and handing that identity to a new lock in the
// window corrupts the held-lock stack.
//
void KmSetLockIdRecycling(int Enabled);

// Clears the global lock-order graph between test cases.
void KmResetLockOrder(void);

///////////////////////////////////////////////////////////////////////////
// DPCs and the virtual clock
///////////////////////////////////////////////////////////////////////////

typedef struct _KM_DPC KM_DPC;

typedef void (*PKM_DPC_ROUTINE)(KM_DPC* Dpc, void* Context, void* Arg1, void* Arg2);

struct _KM_DPC
{
    PKM_DPC_ROUTINE Routine;
    void* Context;
    long Queued;
    struct _KM_DPC* Next;
};

void KmInitializeDpc(KM_DPC* Dpc, PKM_DPC_ROUTINE Routine, void* Context);
BOOLEAN KmInsertQueueDpc(KM_DPC* Dpc, void* Arg1, void* Arg2);
BOOLEAN KmRemoveQueueDpc(KM_DPC* Dpc);

//
// Runs every queued DPC at DISPATCH_LEVEL, including ones queued by the
// DPCs themselves. Returns how many ran, so a test can assert that a
// timeout actually fired rather than inferring it.
//
int KmDrainDpcs(void);

typedef struct _KM_TIMER
{
    KM_DPC* Dpc;
    long long DueTime;      // virtual clock units (100ns), absolute
    long Armed;
    struct _KM_TIMER* Next;
} KM_TIMER;

void KmInitializeTimer(KM_TIMER* Timer);

//
// DueTime follows the kernel convention: negative is relative to now.
// Returns TRUE if the timer was already armed, matching KeSetTimer.
//
BOOLEAN KmSetTimer(KM_TIMER* Timer, long long DueTime, KM_DPC* Dpc);

// Returns TRUE if the timer was still armed (so its DPC will not run).
BOOLEAN KmCancelTimer(KM_TIMER* Timer);

long long KmNow(void);

//
// Advances the virtual clock and fires every timer that came due, then
// drains the resulting DPCs. Returns the number of timers that fired.
// This is the whole reason a 30-second watchdog is testable.
//
int KmAdvanceTime(long long Milliseconds);

void KmResetClock(void);

///////////////////////////////////////////////////////////////////////////
// Lifetime accounting
///////////////////////////////////////////////////////////////////////////

typedef enum _KM_OBJECT
{
    KmObjectPool = 0,
    KmObjectIrp,
    KmObjectWorkItem,
    KmObjectMdl,
    KmObjectSocket,
    KmObjectMax
} KM_OBJECT;

void KmObjectCreated(KM_OBJECT Kind);
void KmObjectDestroyed(KM_OBJECT Kind);
long KmObjectsLive(KM_OBJECT Kind);

// Aborts naming the offender if anything is still live. Used at teardown.
void KmAssertQuiescent(const char* Where);

void KmResetObjects(void);

// Snapshots current object counts as the floor that KmResetObjects() and
// KmAssertQuiescent() compare against instead of zero. Call once, after
// allocating anything meant to outlive every individual test -- a
// ::testing::Environment that sets up a process-lifetime table is the
// only intended caller. Cumulative and order-independent across multiple
// environments: each call re-snapshots the current total, so whichever
// environment runs last still captures everything allocated so far.
void KmAbsorbBaseline(void);

///////////////////////////////////////////////////////////////////////////
// Threads
///////////////////////////////////////////////////////////////////////////

typedef void (*PKM_THREAD_ROUTINE)(void* Context);

typedef struct _KM_THREAD KM_THREAD;

KM_THREAD* KmStartThread(PKM_THREAD_ROUTINE Routine, void* Context);
void KmJoinThread(KM_THREAD* Thread);

//
// A rendezvous for N threads, so a test can line several threads up at
// the exact instant it wants them to collide. Without this, a
// "concurrent" test mostly measures thread-start latency.
//
typedef struct _KM_BARRIER
{
    volatile long Count;
    long Target;
} KM_BARRIER;

void KmInitializeBarrier(KM_BARRIER* Barrier, long Target);
void KmBarrierWait(KM_BARRIER* Barrier);

//
// Yields in a way that widens race windows: on a machine with more cores
// than modelled threads, a plain spin can keep two threads from ever
// interleaving inside a short critical section.
//
void KmJitter(void);

///////////////////////////////////////////////////////////////////////////
// Whole-model lifecycle
///////////////////////////////////////////////////////////////////////////

void KmReset(void);

#ifdef __cplusplus
}
#endif
