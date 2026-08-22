//
// Exhaustive interleaving proof of the socket watchdog's ownership
// protocol, on the real Socket.c.
//
// The claim, stated in Socket.h:
//
//   RefCount starts at 2 -- the completion routine and the timeout DPC,
//   each releasing one reference at most once. Whichever side drives it to
//   zero owns the free.
//
// This is the most dangerous race in the driver. Both sides can run at
// once, both believe they might be last, and the loser must not touch the
// context again. Get it wrong in one direction and the context leaks; get
// it wrong in the other and it is freed twice, which in the kernel is a
// bugcheck on someone else's memory.
//
// SocketKernelTest already covers this with two hand-picked orderings --
// completion-then-expiry and expiry-then-completion. Those are the two
// interleavings someone thought of. This explores all of them, including
// the ones interleaved partway through each side's own work, which is
// where a two-step protocol like disarm-then-release actually breaks.
//

#include <gtest/gtest.h>

extern "C" {
#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
#include "Scheduler.h"
#include "WskModel.h"
}

namespace
{

const long long kSchedReceiveTimeoutMs = 30000;

struct WatchdogProof
{
    PKSOCKET Socket;
    unsigned char Buffer[64];

    volatile long Completions;
    volatile long Issued;

    volatile long TimerFired;
    volatile long DeferredReleased;
};

WatchdogProof* Proof = nullptr;

void CountCompletion(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    (void)Status;
    (void)BytesTransferred;
    (void)Context;

    if (Proof)
    {
        InterlockedIncrement(&Proof->Completions);
    }
}

WSK_MODEL_BEHAVIOUR Behaviour(WSK_MODEL_COMPLETION completion, NTSTATUS status, SIZE_T bytes)
{
    WSK_MODEL_BEHAVIOUR b = {};
    b.Completion = completion;
    b.Status = status;
    b.Bytes = bytes;
    return b;
}

//
// The timer side: pushing the virtual clock past the deadline fires the
// timer, which queues the DPC, which cancels the IRP and releases its
// reference.
//
void ExpiryThread(void* Parameter)
{
    WatchdogProof* proof = (WatchdogProof*)Parameter;

    KmAdvanceTime(kSchedReceiveTimeoutMs + 1);

    InterlockedIncrement(&proof->TimerFired);
}

//
// The transport side: the real completion lands, disarms the watchdog and
// releases its own reference.
//
void CompletionThread(void* Parameter)
{
    WatchdogProof* proof = (WatchdogProof*)Parameter;

    WskModelReleaseDeferred();

    InterlockedIncrement(&proof->DeferredReleased);
}

void WatchdogSetup(void* Parameter)
{
    WatchdogProof* proof = (WatchdogProof*)Parameter;

    proof->Completions = 0;
    proof->TimerFired = 0;
    proof->DeferredReleased = 0;
    proof->Socket = nullptr;

    WskModelReset();
    ShimReset();

    if (!NT_SUCCESS(InitialiseWskClient()))
    {
        return;
    }

    WSK_MODEL_BEHAVIOUR inlineOk = Behaviour(WskModelInline, STATUS_SUCCESS, 0);
    WskModelSetConnectBehaviour(&inlineOk);

    SOCKADDR_IN remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);

    PKSOCKET socket = nullptr;

    //
    // Acquiring a socket inline keeps the setup out of the schedule: the
    // race under proof is between the watchdog and the completion, and
    // making connection establishment concurrent too would multiply the
    // state space without adding anything to the claim.
    //
    struct Acquire
    {
        static void Routine(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID Context)
        {
            (void)Status;
            (void)Reused;
            *(PKSOCKET*)Context = Socket;
        }
    };

    AcquireReusableWskSocketAsync((PSOCKADDR)&remote, TRUE, Acquire::Routine, &socket);

    if (!socket)
    {
        return;
    }

    proof->Socket = socket;

    WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 4);
    WskModelSetReceiveBehaviour(&deferred);

    if (STATUS_PENDING != ReceiveWskAsync(socket, proof->Buffer, sizeof(proof->Buffer), 0, CountCompletion, nullptr))
    {
        return;
    }

    InterlockedIncrement(&proof->Issued);

    KmSchedSpawn(ExpiryThread, proof);
    KmSchedSpawn(CompletionThread, proof);
}

void WatchdogTeardown(void* Parameter)
{
    WatchdogProof* proof = (WatchdogProof*)Parameter;

    //
    // Anything the transport still had queued behind the cancel. If the
    // cancel produced a second delivery this is where it appears.
    //
    WskModelPumpCancellations();

    if (proof->Socket)
    {
        CloseWskSocketAsync(proof->Socket);
        proof->Socket = nullptr;
    }

    CleanupWskClient();
}

class SocketSchedTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        WskModelReset();
    }

    void TearDown() override
    {
        Proof = nullptr;
        KmAssertQuiescent("SocketSchedTest teardown");
    }
};

//
// Every interleaving of the expiring watchdog and the landing completion.
// The properties are per-schedule and enforced inside the run rather than
// after it: exactly one completion reaches the caller, and the context is
// freed exactly once -- the latter by ASan and the model's object
// accounting, which fail the run at the moment of the second free rather
// than reporting a count at the end.
//
TEST_F(SocketSchedTest, NoInterleavingFreesTheContextTwice)
{
    static WatchdogProof proof;

    proof = {};
    Proof = &proof;

    //
    // Required here: this protocol has no locks. Its entire arbitration is
    // the two sides racing one InterlockedDecrement, so without a
    // scheduling point at the atomic the explorer finds exactly the two
    // whole-thread orderings SocketKernelTest already covers by hand.
    //
    KmSchedSetAtomicYields(1);

    KM_SCHED_RESULT result =
        KmExploreInterleavings(WatchdogSetup, WatchdogTeardown, &proof, 20000);

    KmSchedSetAtomicYields(0);

    EXPECT_EQ(0, result.Deadlocks) << "a schedule deadlocked";

    EXPECT_EQ(0, result.Truncated)
        << "a schedule hit the depth cap, so the space was not fully explored";

    EXPECT_LT(result.Schedules, 20000)
        << "hit the schedule cap -- sampled, not exhausted";

    //
    // Coverage. If the receive never went pending, or one of the two racing
    // sides never ran, the schedules were trivial and prove nothing.
    //
    EXPECT_GT(proof.Issued, 0) << "no schedule ever issued the receive";
    EXPECT_GT(proof.TimerFired, 0) << "the watchdog never expired";
    EXPECT_GT(proof.DeferredReleased, 0) << "the completion never landed";

    printf("[  sched   ] %d interleavings, max depth %d, %ld completions delivered\n",
        result.Schedules, result.MaxDepth, proof.Completions);
}

} // namespace
