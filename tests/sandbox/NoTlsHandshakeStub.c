//
// TlsStartHandshakeAsync for targets that do not compile TlsHandshake.c.
//
// Client.c calls it from HttpKick, so it must resolve; but a target that
// links the real TlsHandshake.c must not also link this. Same split as
// NoPrefetchStub.c and NoClientStub.c.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"
#include "..\..\src\Socket.h"
#include "..\..\src\TlsHandshake.h"

//
// These scenarios drive the plaintext client (SandboxInitialize leaves
// global.TlsEnabled FALSE), so this exists to satisfy the one call site in
// HttpKick and to keep the contract Client.c is written against: the
// completion runs, and the socket is left in a state the caller can act on.
// It is deliberately not a TLS implementation -- the real handshake is
// covered against RFC 8448 vectors by TlsHandshakeTest, which drives
// TlsHandshake.c directly. A scenario that set TlsEnabled would be testing
// this stub, so nothing here should grow until the peer script can speak
// records.
//
VOID TlsStartHandshakeAsync(
    PKSOCKET Socket,
    PBLORG_TLS_HANDSHAKE_COMPLETION CompletionRoutine,
    PVOID CallerContext)
{
    Socket->Tls.State = TlsHandshakeComplete;

    CompletionRoutine(STATUS_SUCCESS, CallerContext);
}

