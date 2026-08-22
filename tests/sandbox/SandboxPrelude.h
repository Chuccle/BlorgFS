#pragma once

//
// The single kernel substitution for every sandbox target.
//
// Driver.h includes exactly this in place of <ntifs.h>, <ntstrsafe.h>,
// <wdmsec.h> and <wsk.h> when BLORGFS_SANDBOX_BUILD is defined. Nothing
// else in the driver changes: every .c file still says #include
// "Driver.h", and every driver type, constant and static assertion is the
// one the shipping build uses.
//
// That is deliberate and it is the most important property of this
// sandbox. Earlier versions declared their own FCB, their own
// DIRECTORY_INFO, their own CHECK_PADDING macros and their own
// READ_AHEAD_GRANULARITY, once per target. Every one of those was a copy
// that could drift from the driver without anything failing -- and a test
// that passes against a drifted copy is worse than no test, because it
// reports confidence it has not earned. The rule now is: if the driver
// defines it, the sandbox uses the driver's definition.
//
// The sandbox projects also define BLORGFS_KERNEL_BUILD (the shim is
// kernel-shaped, so Tls.h and Statistics.h should take their kernel
// branches) and PADDING_CHECKS (so Structs.h's real layout assertions are
// compiled rather than stubbed out).
//
// Layering, innermost first:
//
//   KernelModel.h   IRQL, lock order, DPCs, virtual-clock timers,
//                   quiescence accounting -- the rules, enforced.
//   NtShim.h        The NT API surface expressed over that model:
//                   pool, IRPs, MDLs, spin locks, events, work items.
//   NtShimSync.c    Push locks, ERESOURCEs, lookaside lists, Unicode.
//   FsRtlShim.h     The handful of FsRtl/MM types that must be
//                   substituted because they belong to the kernel.
//   WskModel.h      A scriptable WSK provider, so the real Socket.c runs.
//

//
// winbase.h defines a serial-port control block as struct _DCB / DCB,
// unconditionally and with no NOxxx guard to switch it off. The driver's
// directory node is also DCB, and in the kernel build nothing collides
// because winbase.h is not there. Renaming the SDK's copy across the
// Windows headers is the only substitution that leaves the driver's own
// name intact -- and the driver's name is the one that must not move,
// because Structs.h is the single definition both builds compile.
// Nothing in the sandbox opens a COM port; LPDCB still names the
// renamed struct for the comm prototypes that take it.
//
// KernelModel.h is what pulls in winsock2.h and windows.h, in that order,
// so the rename has to wrap it rather than a windows.h include of our own.
//
#define _DCB _WIN32_SERIAL_DCB
#define DCB WIN32_SERIAL_DCB

#include "KernelModel.h"

#undef DCB
#undef _DCB

#include "NtShim.h"
#include "FsRtlShim.h"
#include "IoShim.h"
#include "FileInfoShim.h"
#include "DispatchModel.h"
#include "WskModel.h"

//
// ntstrsafe.h's surface, which Driver.h would otherwise bring in. Only
// the two the driver actually calls; both are implemented in NtShim.c
// over the CRT's safe-string functions.
//
// (Declared in NtShim.h alongside the other Rtl helpers.)
//
