#pragma once

//
// Scriptable implementation of Socket.h / TlsHandshake.h.
//
// It substitutes the implementation, not the interface: the declarations
// and the KSOCKET layout come from the driver's own Socket.h, so a field
// added there is a field this sees. An earlier version declared its own
// KSOCKET, which had already drifted -- it was missing PoolEntry and
// carried three fields the real one does not have.
//
// What makes it a test tool rather than a stub is the script. A scenario
// declares what the peer does -- these bytes, then a short read, then a
// reset -- and whether each step completes inline or later. Both matter:
// inline completion is what builds the deep synchronous chains the real
// client's stack-expansion logic exists for, and only a script can
// produce the hostile shapes (a lying Content-Length, a record split
// across three receives, an alert mid-flight) that a well-behaved server
// never will.
//

#include "..\Driver.h"
#include "..\Socket.h"
#include "..\TlsHandshake.h"

#ifdef __cplusplus
extern "C" {
#endif

///////////////////////////////////////////////////////////////////////////
// Peer scripting
///////////////////////////////////////////////////////////////////////////

//
// One scripted step of peer behaviour. A receive consumes steps until it
// has satisfied its caller or hit a step that ends the conversation.
//
typedef enum _SANDBOX_STEP_KIND
{
    //
    // Deliver up to Length bytes from Data. A receive takes as much as
    // it asked for and leaves the rest for the next one, so a single
    // Deliver can feed many receives -- or, with a short Length, force a
    // response to arrive in pieces and exercise the client's
    // reassembly.
    //
    SandboxStepDeliver = 0,

    // Complete with zero bytes: the peer closed. The idle-close race.
    SandboxStepClose,

    // Complete with an error status, e.g. a reset or a timeout.
    SandboxStepFail,

    //
    // Deliver nothing and leave the request outstanding. Models a peer
    // that accepts and never answers -- in the driver the watchdog kills
    // this; here the scenario asserts the request is still parked.
    //
    SandboxStepStall
} SANDBOX_STEP_KIND;

typedef struct _SANDBOX_STEP
{
    SANDBOX_STEP_KIND Kind;
    const unsigned char* Data;
    SIZE_T Length;
    NTSTATUS Status;

    //
    // TRUE completes the operation inline, before the issuing call
    // returns -- which is what chains synchronous completions deep enough
    // to reach the stack-expansion logic. FALSE defers it to the
    // scenario's drain, modelling a genuinely asynchronous completion.
    //
    BOOLEAN Inline;
} SANDBOX_STEP;

typedef struct SANDBOX_PEER
{
    const SANDBOX_STEP* Steps;
    SIZE_T StepCount;
    SIZE_T StepIndex;

    // Offset within the current Deliver step.
    SIZE_T StepOffset;

    // Bytes the client sent, for asserting on the request it built.
    unsigned char SentBuffer[8192];
    SIZE_T SentLength;

    BOOLEAN Stalled;
} SANDBOX_PEER;

//
// Installs the script every subsequently acquired socket replays.
// Sockets acquired from the pool keep their own position in it, so a
// keep-alive retry sees a *fresh* peer -- which is exactly the shape the
// driver's retry-on-reused-connection path assumes.
//
VOID SandboxInitialize(VOID);
VOID SandboxCleanup(VOID);

VOID SandboxSetPeerScript(const SANDBOX_STEP* Steps, SIZE_T StepCount);

// Fails the next N socket acquisitions, for the connect-failure paths.
VOID SandboxFailNextAcquires(ULONG Count);

// Runs deferred (non-inline) completions until none remain.
VOID SandboxDrainCompletions(VOID);

// Counters a scenario asserts on.
ULONG SandboxSocketsCreated(VOID);
ULONG SandboxSocketsClosed(VOID);
ULONG SandboxSocketsPooled(VOID);
ULONG SandboxSocketsLive(VOID);

VOID SandboxSocketsReset(VOID);

// The bytes the client wrote on its most recent connection.
const unsigned char* SandboxLastRequest(SIZE_T* Length);

#ifdef __cplusplus
}
#endif
