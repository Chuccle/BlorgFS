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
// Most scenarios drive the plaintext client (SandboxInitialize leaves
// global.TlsEnabled FALSE), so this exists to satisfy the one call site in
// HttpKick and to keep the contract Client.c is written against: the
// completion runs, and the socket is left in a state the caller can act on.
// It is deliberately not a TLS implementation -- the real handshake is
// covered against RFC 8448 vectors by TlsHandshakeTest, which drives
// TlsHandshake.c directly. What it adds is what a scenario needs around
// one: a failure, to drive what Client.c does when a handshake does not
// complete; a count of the handshakes started and of those started above
// PASSIVE_LEVEL, which the real one's CNG calls do not allow; and traffic
// keys. A completed handshake leaves them on the socket and imports their
// handles, as the real key schedule does, so the client's record layer
// runs for real: a request is sealed and sent, and a scenario scripts the
// records that come back. The keys are all zero. The import needs the CNG
// providers a scenario opens with BlorgTlsGlobalInit; without them it is
// refused and the handles stay NULL, which a scenario that never sends a
// record does not notice.
//
static ULONG FailHandshakesRemaining;
static NTSTATUS FailHandshakesStatus;
static ULONG HandshakesStarted;
static ULONG HandshakesAbovePassive;

static UCHAR ClientWriteKey[TLS_KEY_LEN];
static UCHAR ClientWriteIv[TLS_IV_LEN];
static UCHAR ServerWriteKey[TLS_KEY_LEN];
static UCHAR ServerWriteIv[TLS_IV_LEN];

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

//
// The client writes with the client's key and reads with the server's,
// both from sequence zero, as after a real handshake.
//
static VOID SandboxInstallTrafficKeys(PTLS_CONNECTION_STATE Tls)
{
    RtlCopyMemory(Tls->WriteKey, ClientWriteKey, TLS_KEY_LEN);
    RtlCopyMemory(Tls->WriteIv, ClientWriteIv, TLS_IV_LEN);
    RtlCopyMemory(Tls->ReadKey, ServerWriteKey, TLS_KEY_LEN);
    RtlCopyMemory(Tls->ReadIv, ServerWriteIv, TLS_IV_LEN);
    Tls->WriteSeq = 0;
    Tls->ReadSeq = 0;

    (void)BlorgTlsImportKeyHandle(Tls->WriteKey, &Tls->WriteKeyHandle);
    (void)BlorgTlsImportKeyHandle(Tls->ReadKey, &Tls->ReadKeyHandle);
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

    SandboxInstallTrafficKeys(&Socket->Tls);
    Socket->Tls.State = TlsHandshakeComplete;

    CompletionRoutine(STATUS_SUCCESS, CallerContext);
}

