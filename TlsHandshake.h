#pragma once

//
// Needs PKSOCKET (Socket.h) -- include Driver.h then Socket.h before
// this header, matching Socket.h's own convention of relying on
// caller-established include order rather than self-including its
// prerequisites.
//
// Drives a full TLS 1.3 handshake (ClientHello through client Finished)
// over an already-connected KSOCKET, using the same async WSK primitives
// (SendWskAsync/ReceiveWskAsync) and completion-chain idioms as the HTTP
// client (Client.c) -- including its stack-safety discipline for
// synchronous completion chains (see HttpIssueReceive's comment there
// for why that matters). This is the kernel-specific I/O-driving glue;
// the actual protocol logic (message construction/parsing, key
// schedule, crypto) lives entirely in Tls.c.
//

typedef VOID(*PBLORG_TLS_HANDSHAKE_COMPLETION)(NTSTATUS Status, PVOID CallerContext);

//
// Called only for a freshly-connected socket (Socket->Tls.State ==
// TlsHandshakeNotStarted on entry -- a pooled/reused socket that already
// completed a handshake must skip straight past this, see HttpOnSocket).
//
// On success: Socket->Tls.State == TlsHandshakeComplete, with
// WriteKey/WriteIv (client) and ReadKey/ReadIv (server) set to the
// derived *application* traffic keys (sequence numbers reset to 0) --
// ready for wrapping ordinary HTTP send/receive through the record
// layer. On failure: Socket->Tls.State == TlsHandshakeFailed -- the
// caller must close this socket, never pool it.
//
VOID TlsStartHandshakeAsync(
    PKSOCKET Socket,
    PBLORG_TLS_HANDSHAKE_COMPLETION CompletionRoutine,
    PVOID CallerContext);

//
// Runtime-updatable certificate pin (SHA-256 of the leaf's DER-encoded
// SubjectPublicKeyInfo, matching the SPKI-pinning convention e.g. curl
// --pinnedpubkey uses -- not a compile-time constant, since the
// backend's cert is on a short public-CA rotation and a hardcoded pin
// would mean a full driver rebuild+resign+reload every renewal). Backed
// by an EX_PUSH_LOCK (kernel-only -- this is why these live in
// TlsHandshake.h/.c, not the portable Tls.h/.c, unlike everything else
// declared above).
//
// TlsSetPin: called once at DriverEntry (from a registry read) and again,
// any number of times, from the IOCTL_BLORGFS_SET_TLS_PIN handler
// (DevIoCtrl.c) -- both PASSIVE_LEVEL. Replaces the running pin
// atomically; a handshake concurrently checking the old pin either
// finishes checking against it or the new one, never a torn mix.
//
// TlsCheckPin: called from TlsHandshakeProcessFlightMessages (also
// PASSIVE_LEVEL) once the leaf certificate's SPKI span is known. Fails
// closed: returns FALSE both on an actual mismatch AND when no pin has
// ever been configured (TlsSetPin never called) -- a handshake must not
// be treated as authenticated just because pinning was never set up.
//
NTSTATUS TlsSetPin(const UCHAR Pin[TLS_HASH_LEN]);
BOOLEAN TlsCheckPin(const UCHAR* Spki, ULONG SpkiLen);
