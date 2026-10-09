//
// BlorgTlsStartHandshakeAsync for targets that do not compile TlsHandshake.c.
//
// Client.c calls it from HttpKick, so it must resolve; but a target that
// links the real TlsHandshake.c must not also link this. Same split as
// NoClientStub.c and NoStatisticsStub.c.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "SandboxSocket.h"

//
// These scenarios drive the plaintext client (SandboxInitialize leaves
// global.TlsEnabled FALSE), so this exists to satisfy the one call site in
// HttpKick and to keep the contract Client.c is written against: the
// completion runs, and the socket is left in a state the caller can act on.
// It is deliberately not a TLS implementation -- the real handshake is
// covered against RFC 8448 vectors by TlsHandshakeTest, which drives
// TlsHandshake.c directly. A scenario that set TlsEnabled would be testing
// this stub, so nothing here should grow until the peer script can speak
// records -- except a failure, which is all a scenario needs to drive what
// Client.c does when a handshake does not complete, and a count of the
// handshakes started and of those started above PASSIVE_LEVEL, which the
// real one's CNG calls do not allow.
//
static ULONG FailHandshakesRemaining;
static NTSTATUS FailHandshakesStatus;
static ULONG HandshakesStarted;
static ULONG HandshakesAbovePassive;

VOID SandboxFailNextHandshakesWith(ULONG Count, NTSTATUS Status)
{
    FailHandshakesRemaining = Count;
    FailHandshakesStatus = Status;
}

VOID SandboxResetHandshakes(VOID)
{
    FailHandshakesRemaining = 0;
    FailHandshakesStatus = STATUS_SUCCESS;
    HandshakesStarted = 0;
    HandshakesAbovePassive = 0;
}

ULONG SandboxHandshakesStarted(VOID)
{
    return HandshakesStarted;
}

ULONG SandboxHandshakesAbovePassive(VOID)
{
    return HandshakesAbovePassive;
}

VOID BlorgTlsStartHandshakeAsync(
    PKSOCKET Socket,
    PBLORG_TLS_HANDSHAKE_COMPLETION CompletionRoutine,
    PVOID CallerContext)
{
    HandshakesStarted++;

    if (PASSIVE_LEVEL < KeGetCurrentIrql())
    {
        HandshakesAbovePassive++;
    }

    if (0 < FailHandshakesRemaining)
    {
        FailHandshakesRemaining--;
        Socket->Tls.State = TlsHandshakeFailed;
        CompletionRoutine(FailHandshakesStatus, CallerContext);
        return;
    }

    Socket->Tls.State = TlsHandshakeComplete;

    CompletionRoutine(STATUS_SUCCESS, CallerContext);
}

