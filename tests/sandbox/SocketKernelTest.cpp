//
// Kernel-behaviour tests for the real Socket.c, run against the rule
// model (KernelModel.h) with a scriptable WSK provider (WskModel.h).
//
// These cover the things review can only argue about:
//
//   Timeouts      A watchdog is armed before every operation and must fire
//                 when the peer never answers, cancel the IRP, and surface
//                 as STATUS_IO_TIMEOUT rather than STATUS_CANCELLED.
//   DPCs          The timeout runs as a DPC at DISPATCH_LEVEL, and the
//                 kernel's one-queued-instance rule has to hold.
//   Synchronisation  A DPC and a completion race to free one context; a
//                 refcount decides. Both orderings are executed here.
//   Quiescence    The pool must hand back what it took, and teardown must
//                 leave nothing live.
//   Reentrancy    A completion that runs inline, before the issuing call
//                 has returned, must not corrupt the issuing path.
//
// Every fixture asserts quiescence on teardown, so a leak in any of them
// fails the test that caused it rather than the next one to run.
//

#include <gtest/gtest.h>

#include <thread>

extern "C" {
#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
#include "Scheduler.h"

// Diagnostic read of the pump's budget word (Socket.c); used solely by
// PrewarmChainSurvivesCompletionRacingThePumpLoop.
ULONG BlorgPrewarmRemainingForDiagnostics(VOID);
}

namespace
{

//
// The driver's own timeouts, from Socket.c. Duplicated deliberately: if
// someone changes them there, the timing tests here should be re-read
// rather than silently keep passing against a stale assumption.
//
const long long kConnectTimeoutMs = 15000;
const long long kSendTimeoutMs = 15000;
const long long kReceiveTimeoutMs = 30000;

struct CompletionRecord
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Bytes = 0;
};

CompletionRecord LastCompletion;

void RecordCompletion(NTSTATUS Status, ULONG_PTR BytesTransferred, PVOID Context)
{
    (void)Context;

    LastCompletion.Calls++;
    LastCompletion.Status = Status;
    LastCompletion.Bytes = (SIZE_T)BytesTransferred;
}

struct AcquireRecord
{
    int Calls = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PKSOCKET Socket = nullptr;
    BOOLEAN Reused = FALSE;
};

AcquireRecord LastAcquire;

void RecordAcquire(NTSTATUS Status, PKSOCKET Socket, BOOLEAN Reused, PVOID Context)
{
    (void)Context;

    LastAcquire.Calls++;
    LastAcquire.Status = Status;
    LastAcquire.Socket = Socket;
    LastAcquire.Reused = Reused;
}

WSK_MODEL_BEHAVIOUR Behaviour(WSK_MODEL_COMPLETION completion,
                              NTSTATUS status = STATUS_SUCCESS,
                              SIZE_T bytes = 0,
                              const unsigned char* payload = nullptr,
                              SIZE_T payloadLength = 0)
{
    WSK_MODEL_BEHAVIOUR behaviour = {};
    behaviour.Completion = completion;
    behaviour.Status = status;
    behaviour.Bytes = bytes;
    behaviour.Payload = payload;
    behaviour.PayloadLength = payloadLength;
    return behaviour;
}

class SocketKernelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ShimReset();
        WskModelReset();

        LastCompletion = {};
        LastAcquire = {};

        ASSERT_EQ(STATUS_SUCCESS, BlorgInitialiseWskClient());
    }

    void TearDown() override
    {
        BlorgCleanupWskClient();

        //
        // Nothing may outlive a test. An IRP, MDL or pool block still live
        // here is a leak in the driver, not in the harness -- the model
        // counts only what the driver allocated through it.
        //
        KmAssertQuiescent("SocketKernelTest teardown");
    }

    //
    // A connected socket, the way the client obtains one. The record is
    // cleared first so the "exactly one callback" assertion holds for
    // this acquire rather than counting every acquire the test has made.
    //
    PKSOCKET AcquireSocket()
    {
        SOCKADDR_IN address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(80);

        LastAcquire = {};

        NTSTATUS status = BlorgAcquireReusableWskSocketAsync(
            (PSOCKADDR)&address, TRUE, RecordAcquire, nullptr);

        EXPECT_EQ(STATUS_PENDING, status);
        EXPECT_EQ(1, LastAcquire.Calls);
        EXPECT_TRUE(NT_SUCCESS(LastAcquire.Status));

        return LastAcquire.Socket;
    }
};

///////////////////////////////////////////////////////////////////////////
// HTTP timeout handling
///////////////////////////////////////////////////////////////////////////

//
// The case the watchdog exists for: a peer that accepts and then says
// nothing. Nothing but the timer can end this operation, and the caller
// must see STATUS_IO_TIMEOUT -- not STATUS_CANCELLED, which is what the
// IRP actually completes with. That translation is the part worth
// asserting; a caller that saw CANCELLED would have no way to tell a
// dead peer from a shutdown.
//
TEST_F(SocketKernelTest, ReceiveThatNeverCompletesTimesOut)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    WSK_MODEL_BEHAVIOUR never = Behaviour(WskModelNever);
    WskModelSetReceiveBehaviour(&never);

    unsigned char buffer[64] = {};

    NTSTATUS status = BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr);

    ASSERT_EQ(STATUS_PENDING, status);
    EXPECT_EQ(0, LastCompletion.Calls) << "an operation that never completed must not have called back";

    // Just short of the deadline: still nothing.
    KmAdvanceTime(kReceiveTimeoutMs - 1);
    WskModelPumpCancellations();

    EXPECT_EQ(0, LastCompletion.Calls) << "watchdog fired early";

    // Past it: the DPC runs, cancels the IRP, the transport completes it.
    int fired = KmAdvanceTime(2);
    EXPECT_EQ(1, fired) << "the receive watchdog did not fire";

    WskModelPumpCancellations();

    EXPECT_EQ(1, LastCompletion.Calls);
    EXPECT_EQ(STATUS_IO_TIMEOUT, LastCompletion.Status)
        << "a timed-out operation must surface as IO_TIMEOUT, not the "
           "STATUS_CANCELLED the IRP actually completed with";
    EXPECT_EQ(1u, WskModelCancelled());

    BlorgCloseWskSocketAsync(socket);
}

//
// The send path has its own, shorter deadline. Asserting the boundary
// rather than just "it eventually times out" is what would catch a send
// accidentally wired to the receive timeout.
//
TEST_F(SocketKernelTest, SendUsesItsOwnTimeout)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    WSK_MODEL_BEHAVIOUR never = Behaviour(WskModelNever);
    WskModelSetSendBehaviour(&never);

    unsigned char buffer[16] = {};

    ASSERT_EQ(STATUS_PENDING, BlorgSendWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr));

    KmAdvanceTime(kSendTimeoutMs - 1);
    WskModelPumpCancellations();
    EXPECT_EQ(0, LastCompletion.Calls);

    KmAdvanceTime(2);
    WskModelPumpCancellations();

    EXPECT_EQ(1, LastCompletion.Calls);
    EXPECT_EQ(STATUS_IO_TIMEOUT, LastCompletion.Status);

    BlorgCloseWskSocketAsync(socket);
}

//
// A completion that arrives normally must disarm the watchdog. If it did
// not, the timer would fire later and cancel an IRP that has already been
// freed -- which is why the model treats cancelling a freed IRP as a
// violation rather than a no-op.
//
TEST_F(SocketKernelTest, NormalCompletionDisarmsTheWatchdog)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    static const unsigned char payload[] = "hello";

    WSK_MODEL_BEHAVIOUR inlineOk =
        Behaviour(WskModelInline, STATUS_SUCCESS, sizeof(payload) - 1, payload, sizeof(payload) - 1);
    WskModelSetReceiveBehaviour(&inlineOk);

    unsigned char buffer[64] = {};

    ASSERT_EQ(STATUS_PENDING, BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr));

    EXPECT_EQ(1, LastCompletion.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastCompletion.Status);
    EXPECT_EQ(sizeof(payload) - 1, LastCompletion.Bytes);
    EXPECT_STREQ("hello", (const char*)buffer);

    //
    // Well past every deadline. A watchdog left armed would fire here and
    // touch a freed context; the model would report it.
    //
    int fired = KmAdvanceTime(kReceiveTimeoutMs * 4);
    EXPECT_EQ(0, fired) << "a completed operation left its watchdog armed";
    EXPECT_EQ(1, LastCompletion.Calls) << "completion delivered twice";

    BlorgCloseWskSocketAsync(socket);
}

///////////////////////////////////////////////////////////////////////////
// The DPC/completion race
///////////////////////////////////////////////////////////////////////////

//
// The interesting ordering: the timer expires and its DPC is queued, but a
// real completion lands first. Both then run, and both try to release the
// shared context; the refcount must let exactly one free it. If the
// protocol were wrong this is where a double free or a leak shows up, and
// the model detects both -- the guarded pool catches the double free, the
// quiescence assertion catches the leak.
//
TEST_F(SocketKernelTest, CompletionRacingAnExpiredTimerFreesTheContextOnce)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 4);
    WskModelSetReceiveBehaviour(&deferred);

    unsigned char buffer[64] = {};

    ASSERT_EQ(STATUS_PENDING, BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr));

    //
    // Push past the deadline. The timer fires and the DPC runs, cancelling
    // the IRP -- but the transport has not noticed yet, so the operation is
    // still outstanding with a cancel pending.
    //
    EXPECT_EQ(1, KmAdvanceTime(kReceiveTimeoutMs + 1));

    //
    // Now the real completion lands, ahead of the transport processing the
    // cancel. This is the race.
    //
    WskModelReleaseDeferred();

    EXPECT_EQ(1, LastCompletion.Calls) << "exactly one completion must reach the caller";

    // Anything the transport still had queued.
    WskModelPumpCancellations();

    EXPECT_EQ(1, LastCompletion.Calls) << "the cancel produced a second completion";

    BlorgCloseWskSocketAsync(socket);

    //
    // TearDown's quiescence assertion is the other half: if the DPC and the
    // completion each thought the other owned the free, the context leaks
    // and this test is what reports it.
    //
}

//
// The reverse ordering: the completion runs first and disarms, then the
// timer would have fired. KeCancelTimer returning TRUE is what tells the
// completion the DPC will never run, so it owns the free outright.
//
TEST_F(SocketKernelTest, CompletionBeforeExpiryOwnsTheFree)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 4);
    WskModelSetReceiveBehaviour(&deferred);

    unsigned char buffer[64] = {};

    ASSERT_EQ(STATUS_PENDING, BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr));

    KmAdvanceTime(kReceiveTimeoutMs / 2);

    WskModelReleaseDeferred();

    EXPECT_EQ(1, LastCompletion.Calls);
    EXPECT_EQ(STATUS_SUCCESS, LastCompletion.Status);

    EXPECT_EQ(0, KmAdvanceTime(kReceiveTimeoutMs * 2)) << "timer was not disarmed by the completion";

    BlorgCloseWskSocketAsync(socket);
}

//
// Many operations in flight, each with its own watchdog, all expiring at
// once. Beyond checking the arithmetic, this is what would catch a shared
// timer or a shared context between operations -- a mistake that looks
// fine with one request outstanding.
//
TEST_F(SocketKernelTest, ConcurrentWatchdogsAreIndependent)
{
    const int kOperations = 8;

    PKSOCKET sockets[kOperations] = {};
    unsigned char buffers[kOperations][32] = {};

    WSK_MODEL_BEHAVIOUR never = Behaviour(WskModelNever);
    WskModelSetReceiveBehaviour(&never);

    for (int i = 0; i < kOperations; ++i)
    {
        sockets[i] = AcquireSocket();
        ASSERT_NE(nullptr, sockets[i]);

        ASSERT_EQ(STATUS_PENDING,
            BlorgReceiveWskAsync(sockets[i], buffers[i], sizeof(buffers[i]), 0, RecordCompletion, nullptr));
    }

    EXPECT_EQ(0, LastCompletion.Calls);

    EXPECT_EQ(kOperations, KmAdvanceTime(kReceiveTimeoutMs + 1))
        << "every outstanding operation should have its own armed watchdog";

    WskModelPumpCancellations();

    EXPECT_EQ(kOperations, LastCompletion.Calls);
    EXPECT_EQ(STATUS_IO_TIMEOUT, LastCompletion.Status);

    for (int i = 0; i < kOperations; ++i)
    {
        BlorgCloseWskSocketAsync(sockets[i]);
    }
}

///////////////////////////////////////////////////////////////////////////
// Reentrancy
///////////////////////////////////////////////////////////////////////////

//
// An inline completion runs on the issuing thread before the issuing call
// returns, at DISPATCH. A completion routine that issues the next
// operation therefore recurses through the whole layer. The client relies
// on this working -- it is the fast path against a local server -- so the
// depth is driven deliberately rather than hoped for.
//
TEST_F(SocketKernelTest, InlineCompletionsNestWithoutCorruption)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    static const unsigned char payload[] = "xyz";

    WSK_MODEL_BEHAVIOUR inlineOk =
        Behaviour(WskModelInline, STATUS_SUCCESS, sizeof(payload) - 1, payload, sizeof(payload) - 1);
    WskModelSetReceiveBehaviour(&inlineOk);

    unsigned char buffer[16] = {};

    const int kDepth = 64;

    for (int i = 0; i < kDepth; ++i)
    {
        ASSERT_EQ(STATUS_PENDING, BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, RecordCompletion, nullptr));
    }

    EXPECT_EQ(kDepth, LastCompletion.Calls);
    EXPECT_EQ(kDepth, (int)WskModelReceives());

    BlorgCloseWskSocketAsync(socket);
}

//
// A completion routine runs at DISPATCH_LEVEL. Asserting it directly is
// cheap insurance: the driver's whole bounce design (Client.c's
// HttpMustBounceToPassive, the work-item hops) is built on this being
// true, and a harness that quietly delivered completions at PASSIVE would
// make every one of those tests vacuous.
//
TEST_F(SocketKernelTest, CompletionsRunAtDispatchLevel)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    static unsigned char observedIrql = 0xFF;

    struct Local
    {
        static void Completion(NTSTATUS Status, ULONG_PTR Bytes, PVOID Context)
        {
            (void)Status;
            (void)Bytes;
            (void)Context;
            observedIrql = KmGetIrql();
        }
    };

    WSK_MODEL_BEHAVIOUR inlineOk = Behaviour(WskModelInline, STATUS_SUCCESS, 0);
    WskModelSetReceiveBehaviour(&inlineOk);

    unsigned char buffer[8] = {};

    ASSERT_EQ(STATUS_PENDING, BlorgReceiveWskAsync(socket, buffer, sizeof(buffer), 0, Local::Completion, nullptr));

    EXPECT_EQ(DISPATCH_LEVEL, observedIrql);
    EXPECT_EQ(PASSIVE_LEVEL, KmGetIrql()) << "IRQL was not restored after the completion";

    BlorgCloseWskSocketAsync(socket);
}

///////////////////////////////////////////////////////////////////////////
// Connection pool and quiescence
///////////////////////////////////////////////////////////////////////////

//
// The local address is what fixes a WSK socket's address family, so it has
// to match the remote or the connect opens a socket that can never reach
// the peer it names. It was a fixed AF_INET wildcard, which made every
// IPv6 backend unreachable however the name resolved -- DriverEntry asks
// for AF_UNSPEC and the pool comparison already handles AF_INET6, so the
// only thing standing between this driver and an IPv6 server was these two
// disagreeing.
//
// Both families are driven, because pinning only the v6 case would let a
// fix that hardcoded AF_INET6 pass.
//
TEST_F(SocketKernelTest, LocalBindFamilyFollowsTheRemoteAddress)
{
    SOCKADDR_IN6 v6 = {};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(80);

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&v6, TRUE, RecordAcquire, nullptr));

    EXPECT_EQ(AF_INET6, WskModelLastRemoteFamily());
    EXPECT_EQ(AF_INET6, WskModelLastLocalFamily())
        << "an IPv6 remote was connected from an address of a different family";

    BlorgCloseWskSocketAsync(LastAcquire.Socket);

    SOCKADDR_IN v4 = {};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(80);

    LastAcquire = {};

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&v4, TRUE, RecordAcquire, nullptr));

    EXPECT_EQ(AF_INET, WskModelLastRemoteFamily());
    EXPECT_EQ(AF_INET, WskModelLastLocalFamily());

    BlorgCloseWskSocketAsync(LastAcquire.Socket);
}

//
// A released socket comes back on the next acquire, flagged Reused --
// which is what lets the client treat a failure on it as the idle-close
// race instead of a hard error. Getting the flag wrong would turn every
// keep-alive race into a user-visible failure.
//
TEST_F(SocketKernelTest, ReleasedSocketIsHandedBackAsReused)
{
    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, FALSE, RecordAcquire, nullptr));

    PKSOCKET first = LastAcquire.Socket;
    ASSERT_NE(nullptr, first);
    EXPECT_FALSE(LastAcquire.Reused) << "the first acquire cannot be a reuse";

    BlorgReleaseReusableWskSocket(first);

    LastAcquire = {};

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, FALSE, RecordAcquire, nullptr));

    EXPECT_EQ(first, LastAcquire.Socket) << "the pooled connection was not reused";
    EXPECT_TRUE(LastAcquire.Reused);
    EXPECT_EQ(1u, WskModelConnects()) << "a reuse must not open a second connection";

    BlorgCloseWskSocketAsync(LastAcquire.Socket);
}

//
// ForceFresh is what the retry path uses so a retry cannot land on a
// second stale connection. If it ever started reusing, the retry would be
// pointless and the failure would look intermittent.
//
TEST_F(SocketKernelTest, ForceFreshBypassesThePool)
{
    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, FALSE, RecordAcquire, nullptr));

    PKSOCKET pooled = LastAcquire.Socket;
    BlorgReleaseReusableWskSocket(pooled);

    LastAcquire = {};

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, TRUE, RecordAcquire, nullptr));

    EXPECT_NE(pooled, LastAcquire.Socket);
    EXPECT_FALSE(LastAcquire.Reused);
    EXPECT_EQ(2u, WskModelConnects());

    BlorgCloseWskSocketAsync(LastAcquire.Socket);

    // The pooled one is still the pool's; teardown drains it.
}

//
// Teardown has to actually drain. A pooled socket left behind is a leaked
// connection and, at unload, a leaked non-paged allocation -- the model's
// quiescence assertion in TearDown is what proves it does not happen.
//
TEST_F(SocketKernelTest, CleanupDrainsThePool)
{
    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    for (int i = 0; i < 4; ++i)
    {
        LastAcquire = {};
        ASSERT_EQ(STATUS_PENDING,
            BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, TRUE, RecordAcquire, nullptr));
        BlorgReleaseReusableWskSocket(LastAcquire.Socket);
    }

    EXPECT_GT(KmObjectsLive(KmObjectSocket), 0);

    BlorgCleanupWskSocketPool();

    EXPECT_EQ(0, KmObjectsLive(KmObjectSocket)) << "the pool did not drain";
}

//
// A failed connect must not leak the half-built socket, and must report
// the failure rather than handing back a socket that was never connected.
//
TEST_F(SocketKernelTest, FailedConnectReportsAndLeaksNothing)
{
    WSK_MODEL_BEHAVIOUR failing = Behaviour(WskModelInline, STATUS_CONNECTION_REFUSED);
    WskModelSetConnectBehaviour(&failing);

    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    ASSERT_EQ(STATUS_PENDING,
        BlorgAcquireReusableWskSocketAsync((PSOCKADDR)&address, TRUE, RecordAcquire, nullptr));

    EXPECT_EQ(1, LastAcquire.Calls);
    EXPECT_FALSE(NT_SUCCESS(LastAcquire.Status));
    EXPECT_EQ(nullptr, LastAcquire.Socket) << "a failed connect must not hand back a socket";
}

//
// Allocation failure at each position in turn. Every one must either
// report an error to the caller or complete normally -- never both, never
// neither -- and must leave nothing behind. These branches are otherwise
// unreachable, which makes them the least-exercised code in the driver.
//
TEST_F(SocketKernelTest, AllocationFailuresAreClean)
{
    for (LONG failAt = 0; failAt < 6; ++failAt)
    {
        SCOPED_TRACE(::testing::Message() << "failing allocation #" << failAt);

        ShimPoolFailAt(failAt);

        SOCKADDR_IN address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(80);

        LastAcquire = {};

        NTSTATUS status = BlorgAcquireReusableWskSocketAsync(
            (PSOCKADDR)&address, TRUE, RecordAcquire, nullptr);

        ShimPoolFailAt(-1);

        if (STATUS_PENDING == status)
        {
            EXPECT_EQ(1, LastAcquire.Calls);

            if (NT_SUCCESS(LastAcquire.Status) && LastAcquire.Socket)
            {
                BlorgCloseWskSocketAsync(LastAcquire.Socket);
            }
        }

        WskModelReleaseDeferred();
        WskModelPumpCancellations();
    }
}

///////////////////////////////////////////////////////////////////////////
// TLS receive accumulator
///////////////////////////////////////////////////////////////////////////

//
// BlorgEnsureTlsRecvBuffer's three resources (TlsRecvBuffer, TlsPlaintextScratch,
// TlsRecvMdl) must each be checked against its own allocation, not a
// neighbour's -- a failure allocating TlsRecvBuffer must be reported as
// such, not laundered through the TlsPlaintextScratch check, and must
// short-circuit before attempting TlsPlaintextScratch at all. Index 0 is
// TlsRecvBuffer's allocation; index 1 is TlsPlaintextScratch's, right
// behind it once TlsRecvBuffer has succeeded.
//
TEST_F(SocketKernelTest, EnsureTlsRecvBufferFailsResourcesIndependently)
{
    PKSOCKET socket = AcquireSocket();
    ASSERT_NE(nullptr, socket);

    ShimPoolFailAt(0);
    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, BlorgEnsureTlsRecvBuffer(socket));
    ShimPoolFailAt(-1);

    EXPECT_EQ(nullptr, socket->TlsRecvBuffer);
    EXPECT_EQ(nullptr, socket->TlsPlaintextScratch)
        << "scratch must not be attempted once TlsRecvBuffer itself failed";

    ShimPoolFailAt(1);
    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, BlorgEnsureTlsRecvBuffer(socket));
    ShimPoolFailAt(-1);

    EXPECT_NE(nullptr, socket->TlsRecvBuffer)
        << "TlsRecvBuffer succeeded on this call and must be kept for the retry";
    EXPECT_EQ(nullptr, socket->TlsPlaintextScratch);

    ASSERT_EQ(STATUS_SUCCESS, BlorgEnsureTlsRecvBuffer(socket));
    EXPECT_NE(nullptr, socket->TlsRecvBuffer);
    EXPECT_NE(nullptr, socket->TlsPlaintextScratch);
    ASSERT_NE(nullptr, socket->TlsRecvMdl);

    PUCHAR recvBuffer = socket->TlsRecvBuffer;
    PUCHAR scratch = socket->TlsPlaintextScratch;
    PMDL mdl = socket->TlsRecvMdl;

    EXPECT_EQ(STATUS_SUCCESS, BlorgEnsureTlsRecvBuffer(socket))
        << "a second call must be a no-op once the MDL already exists";
    EXPECT_EQ(recvBuffer, socket->TlsRecvBuffer);
    EXPECT_EQ(scratch, socket->TlsPlaintextScratch);
    EXPECT_EQ(mdl, socket->TlsRecvMdl);

    BlorgCloseWskSocketAsync(socket);
}

///////////////////////////////////////////////////////////////////////////
// Pre-warm pump and its teardown contract
///////////////////////////////////////////////////////////////////////////

//
// The chain must issue exactly its budget -- one connect per owed step,
// each issued only after the previous one completes, one outstanding at a
// time -- and terminate with nothing deferred. This pins the accounting the
// unload-race test below leans on: Remaining consumed by completions but
// never past zero, InFlight returning to zero with the idle event set.
//
TEST_F(SocketKernelTest, PrewarmChainIssuesExactlyItsBudgetAndTerminates)
{
    WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 0);
    WskModelSetConnectBehaviour(&deferred);

    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    BlorgPrewarmSocketPool((PSOCKADDR)&address, 3);

    EXPECT_EQ(1, WskModelDeferredCount())
        << "the pump keeps exactly one step outstanding";

    //
    // One release cascades: completing step N issues step N+1 inline
    // (deferred again), which this same drain then delivers, until the
    // budget is spent.
    //
    EXPECT_GT(WskModelReleaseDeferred(), 0);

    EXPECT_EQ(0, WskModelDeferredCount()) << "the chain did not terminate";
    EXPECT_EQ(3u, WskModelConnects());

    BlorgCleanupWskSocketPool();

    EXPECT_EQ(0, KmObjectsLive(KmObjectSocket)) << "the filled pool did not drain";
}

//
// The unload race: a pre-warm connect still outstanding when teardown runs,
// with the transport answering only after teardown began. Before teardown
// waited on the pump, that ordering resurrected a live socket into a pool
// whose owner had already been released -- leaked in usermode terms, and in
// the kernel a completion routine running against a deregistered provider
// out of an unloaded image.
//
// The verdict is deliberately timing-independent. Whether the cleanup
// thread has actually reached its wait when the release lands decides only
// WHICH path drains the socket -- woken wait, or socket already in the pool
// when the drain loop starts. What no ordering may produce is a live socket
// once both sides are done, which is exactly what the pre-fix code produced
// whenever the transport answered second.
//
//
// Same fixture, different name, because these two are sampling runs rather
// than unit tests: each drives thousands of iterations against real threads
// and settles by spinning, so they cost hundreds of milliseconds where
// every other test here costs single digits. The name is what the Fast
// tier filters on -- it excludes *StressTest.* and *SchedTest.* so the
// gate stays about a second, and -Tier Proof runs exactly those.
//
using SocketStressTest = SocketKernelTest;

TEST_F(SocketStressTest, TeardownDrainsAPrewarmConnectThatOutlivesIt)
{
    WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 0);
    WskModelSetConnectBehaviour(&deferred);

    SOCKADDR_IN address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);

    BlorgPrewarmSocketPool((PSOCKADDR)&address, 1);
    ASSERT_EQ(1, WskModelDeferredCount());

    std::thread cleaner(BlorgCleanupWskSocketPool);

    //
    // Head start for the cleaner so the blocked-in-wait path is the one
    // typically exercised rather than the arrived-early one. Not required
    // for the verdict -- see the comment above.
    //
    Sleep(100);

    WskModelReleaseDeferred();

    cleaner.join();

    EXPECT_EQ(0, WskModelDeferredCount());
    EXPECT_EQ(0, KmObjectsLive(KmObjectSocket))
        << "a pre-warm connect outlived teardown";
    EXPECT_GT(WskModelCloses(), 0u)
        << "the socket the late completion produced was never closed";
}

///////////////////////////////////////////////////////////////////////////
// The pump's lost-wakeup window
///////////////////////////////////////////////////////////////////////////
//
// A completion that lands while a pump loop is mid-flight cannot issue the
// next step itself -- that would nest one WSK dispatch frame inside the
// one that completed (see SocketPrewarmPump). It publishes a token and
// relies on the holder to pick it up before exiting; the holder's tail is
// what stands between a completion landing in the consume-to-clear window
// and a fill that stalls forever with steps still owed.
//
// This runs the two real participants -- a fill's issue loop and a
// completion draining the deferred connect -- as REAL threads, sampled
// across many iterations. Real threads were chosen over the systematic
// explorer deliberately: the fiber scheduler parks and resumes modelled
// threads while SocketPool.Lock is held mid-loop, and its serial drain
// can resume those out of pairing, which produced stall snapshots
// (remaining=1 with zero pending) that the protocol's accounting cannot
// produce on any single-threaded run of the identical flow. Sampling with
// OS threads keeps the publish/final-consume window under genuine
// preemption -- including the weak-memory ordering the SC explorer cannot
// model at all -- without that artifact.
//
struct PumpRaceProof
{
    SOCKADDR_IN Address;
    volatile long Stalls;
    volatile long Healthy;
    volatile long Stop;
};

unsigned int PumpRaceSeed = 0;

ULONG PumpRaceRandom()
{
    PumpRaceSeed = PumpRaceSeed * 1664525u + 1013904223u;
    return PumpRaceSeed >> 8;
}

void PumpRaceCompleter(PumpRaceProof* proof)
{
    //
    // Draining is jittered so completions land in every phase of the
    // fill: before the first issue, mid-issue, after the loop exited.
    // SwitchToThread rather than Sleep: a real preemption window at
    // scheduler granularity, without the 15 ms timer floor that would
    // make thousands of iterations a minutes-long test.
    //
    for (unsigned i = PumpRaceRandom() % 8; i > 0; --i)
    {
        SwitchToThread();
    }

    while (!ReadNoFence(&proof->Stop))
    {
        if (WskModelReleaseDeferred() == 0)
        {
            SwitchToThread();
        }
    }

    WskModelReleaseDeferred();
}

TEST_F(SocketStressTest, PrewarmChainSurvivesCompletionRacingThePumpLoop)
{
    PumpRaceProof proof = {};
    proof.Address.sin_family = AF_INET;
    proof.Address.sin_port = htons(80);
    PumpRaceSeed = 0xB10B;

    const int kIterations = 2000;

    for (int i = 0; i < kIterations && testing::Test::HasNonfatalFailure() == false; ++i)
    {
        WskModelReset();

        ASSERT_EQ(STATUS_SUCCESS, BlorgInitialiseWskClient());

        WSK_MODEL_BEHAVIOUR deferred = Behaviour(WskModelDeferred, STATUS_SUCCESS, 0);
        WskModelSetConnectBehaviour(&deferred);

        std::thread completer(PumpRaceCompleter, &proof);

        BlorgPrewarmSocketPool(C_CAST(const SOCKADDR*, &proof.Address), 2);

        //
        // Settle: spin until the budget word reaches zero (or the bound),
        // THEN verify. Waiting for an empty deferred queue first is wrong:
        // the fill may not have queued its connect yet, and draining
        // nothing leaves the budget unconsumed -- a harness artifact that
        // mimics exactly the stall under proof.
        //
        // The invariant is Remaining itself -- a pooled-socket handoff can
        // satisfy a step without opening a new connection, so
        // "connects == budget" is not sound; Remaining==0 is: every
        // decrement chains exactly one further issue or an explicit
        // refusal this scenario never arms.
        //
        int settle = 0;

        while (BlorgPrewarmRemainingForDiagnostics() > 0 && settle < 2000)
        {
            WskModelReleaseDeferred();
            SwitchToThread();
            ++settle;
        }

        ULONG remaining = BlorgPrewarmRemainingForDiagnostics();

        InterlockedExchange(&proof.Stop, 1);
        completer.join();
        InterlockedExchange(&proof.Stop, 0);

        WskModelReleaseDeferred();

        if (remaining == 0)
        {
            InterlockedIncrement(&proof.Healthy);
        }
        else
        {
            InterlockedIncrement(&proof.Stalls);

            printf("[  diag    ] stall (iteration %d): remaining=%lu\n",
                i, remaining);
        }

        BlorgCleanupWskClient();
    }

    //
    // Coverage: most iterations must have exercised a healthy fill, or
    // the assertion above passes vacuously.
    //
    EXPECT_GT(proof.Healthy, kIterations / 2)
        << "the racing hand-off was rarely exercised";

    EXPECT_EQ(0, proof.Stalls)
        << "a completion landed in the pump's publish window and the budget stalled";
}

} // namespace
