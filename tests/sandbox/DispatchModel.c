//
// The kernel surface the IRP dispatch translation units call out to.
//
// Everything here is modelled only as far as a dispatch test can observe
// it. That is a deliberate line, and it is worth being explicit about
// where it falls: these are Cc, FsRtl, Ob, Se and the share-access
// package -- Windows components with their own state machines and their
// own tests. Reimplementing them would mean this sandbox tests Microsoft's
// code rather than BlorgFS's, and any divergence between the
// reimplementation and the real thing would show up as a passing test
// against a driver that breaks on a real machine.
//
// What IS real is the dispatch logic under test: which status a handler
// returns for a given IRP, which information class it fills, how it
// validates lengths, what it does when a node is missing. Those are
// BlorgFS's decisions, and they run here unmodified.
//
// Share access is the one place with actual behaviour, because
// IoCheckShareAccess deciding yes or no is exactly what the create path
// branches on -- a stub that always granted access would make every
// sharing test vacuous.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

///////////////////////////////////////////////////////////////////////////
// Cache manager
///////////////////////////////////////////////////////////////////////////

VOID CcInitializeCacheMap(PFILE_OBJECT F, PCC_FILE_SIZES S, BOOLEAN P, PCACHE_MANAGER_CALLBACKS C, PVOID Ctx)
{
    (void)F; (void)S; (void)P; (void)C; (void)Ctx;
}

BOOLEAN CcUninitializeCacheMap(PFILE_OBJECT F, PLARGE_INTEGER T, PVOID E)
{
    (void)F; (void)T; (void)E;
    return TRUE;
}

VOID CcSetReadAheadGranularity(PFILE_OBJECT F, ULONG G) { (void)F; (void)G; }
VOID CcSetAdditionalCacheAttributes(PFILE_OBJECT F, BOOLEAN NoRa, BOOLEAN NoWb) { (void)F; (void)NoRa; (void)NoWb; }

//
// A cache miss at PASSIVE with Wait=FALSE is what Cc uses to tell the
// caller "this would block, come back on a thread that can wait" -- Read.c
// answers by reposting to the FSP worker pool, which is the entire reason
// that pool exists (see FspWorkQueue.c's header comment). An always-TRUE
// stub makes that branch structurally unreachable: no dispatch test could
// ever exercise it, whatever it wrote, because the driver would never see
// the FALSE that triggers it. ShimForceNextCcCopyReadMiss makes it
// reachable on demand instead of leaving it permanently dead.
//
static volatile LONG CcCopyReadForceMiss = 0;

VOID ShimForceNextCcCopyReadMiss(VOID)
{
    InterlockedExchange(&CcCopyReadForceMiss, 1);
}

BOOLEAN CcCopyReadEx(PFILE_OBJECT F, PLARGE_INTEGER O, ULONG L, BOOLEAN W, PVOID B, PIO_STATUS_BLOCK S, PETHREAD T)
{
    (void)F; (void)O; (void)L; (void)B; (void)T;

    if (W && InterlockedCompareExchange(&CcCopyReadForceMiss, 0, 1))
    {
        return FALSE;
    }

    if (S)
    {
        S->Status = STATUS_SUCCESS;
        S->Information = 0;
    }

    return TRUE;
}

VOID CcMdlRead(PFILE_OBJECT F, PLARGE_INTEGER O, ULONG L, PMDL* M, PIO_STATUS_BLOCK S)
{
    (void)F; (void)O; (void)L;

    if (M)
    {
        *M = NULL;
    }

    if (S)
    {
        S->Status = STATUS_SUCCESS;
        S->Information = 0;
    }
}

VOID CcFlushCache(PVOID S, PLARGE_INTEGER O, ULONG L, PIO_STATUS_BLOCK St)
{
    (void)S; (void)O; (void)L;

    if (St)
    {
        St->Status = STATUS_SUCCESS;
    }
}

///////////////////////////////////////////////////////////////////////////
// Share access
///////////////////////////////////////////////////////////////////////////

//
// Real, because the create path's answer depends on it. The counting is
// the documented algorithm: a request is refused when it wants an access
// an existing opener did not share, or shares less than an existing
// opener already holds.
//
VOID IoSetShareAccess(ACCESS_MASK Desired, ULONG Share, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess)
{
    (void)FileObject;

    ShareAccess->OpenCount = 1;
    ShareAccess->Readers = (Desired & FILE_READ_DATA) ? 1 : 0;
    ShareAccess->Writers = (Desired & FILE_WRITE_DATA) ? 1 : 0;
    ShareAccess->Deleters = (Desired & DELETE) ? 1 : 0;
    ShareAccess->SharedRead = (Share & FILE_SHARE_READ) ? 1 : 0;
    ShareAccess->SharedWrite = (Share & FILE_SHARE_WRITE) ? 1 : 0;
    ShareAccess->SharedDelete = (Share & FILE_SHARE_DELETE) ? 1 : 0;
}

NTSTATUS IoCheckShareAccess(ACCESS_MASK Desired, ULONG Share, PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess, BOOLEAN Update)
{
    (void)FileObject;

    const BOOLEAN wantsRead = (BOOLEAN)((Desired & FILE_READ_DATA) != 0);
    const BOOLEAN wantsWrite = (BOOLEAN)((Desired & FILE_WRITE_DATA) != 0);
    const BOOLEAN wantsDelete = (BOOLEAN)((Desired & DELETE) != 0);

    if ((wantsRead && ShareAccess->SharedRead < ShareAccess->OpenCount) ||
        (wantsWrite && ShareAccess->SharedWrite < ShareAccess->OpenCount) ||
        (wantsDelete && ShareAccess->SharedDelete < ShareAccess->OpenCount) ||
        (ShareAccess->Readers != 0 && !(Share & FILE_SHARE_READ)) ||
        (ShareAccess->Writers != 0 && !(Share & FILE_SHARE_WRITE)) ||
        (ShareAccess->Deleters != 0 && !(Share & FILE_SHARE_DELETE)))
    {
        return STATUS_SHARING_VIOLATION;
    }

    if (Update)
    {
        ShareAccess->OpenCount++;
        ShareAccess->Readers += wantsRead ? 1 : 0;
        ShareAccess->Writers += wantsWrite ? 1 : 0;
        ShareAccess->Deleters += wantsDelete ? 1 : 0;
        ShareAccess->SharedRead += (Share & FILE_SHARE_READ) ? 1 : 0;
        ShareAccess->SharedWrite += (Share & FILE_SHARE_WRITE) ? 1 : 0;
        ShareAccess->SharedDelete += (Share & FILE_SHARE_DELETE) ? 1 : 0;
    }

    return STATUS_SUCCESS;
}

VOID IoRemoveShareAccess(PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess)
{
    (void)FileObject;

    if (ShareAccess->OpenCount > 0)
    {
        ShareAccess->OpenCount--;
    }
}

///////////////////////////////////////////////////////////////////////////
// FsRtl
///////////////////////////////////////////////////////////////////////////

//
// STATUS_PENDING here means the oplock package took ownership of the IRP
// and will re-drive the caller from BlorgOplockComplete -- the single most
// dangerous status this file returns, per Create.c's own comment above
// OpenExistingFcb. An always-SUCCESS stub makes every oplock-pending path
// in Create.c (three call sites) and Read.c permanently unreachable: not
// merely untested today, but incapable of ever being tested against this
// stub. ShimForceNextOplockCheck makes the branch reachable.
//
static volatile LONG OplockForcedStatus = STATUS_SUCCESS;

VOID ShimForceNextOplockCheck(NTSTATUS Status)
{
    InterlockedExchange(&OplockForcedStatus, Status);
}

NTSTATUS FsRtlCheckOplock(POPLOCK O, PIRP I, PVOID C, PVOID W, PVOID P)
{
    (void)O; (void)I; (void)C; (void)W; (void)P;
    return InterlockedExchange(&OplockForcedStatus, STATUS_SUCCESS);
}

BOOLEAN FsRtlOplockIsFastIoPossible(POPLOCK O) { (void)O; return TRUE; }
BOOLEAN FsRtlOplockIsSharedRequest(PIRP I) { (void)I; return TRUE; }

NTSTATUS FsRtlOplockBreakH(POPLOCK O, PIRP I, ULONG F, PVOID C, PVOID Cb, PVOID P)
{
    (void)O; (void)I; (void)F; (void)C; (void)Cb; (void)P;
    return STATUS_SUCCESS;
}

BOOLEAN FsRtlFastUnlockAll(PFILE_LOCK L, PFILE_OBJECT F, PEPROCESS P, PVOID C)
{
    (void)L; (void)F; (void)P; (void)C;
    return TRUE;
}

BOOLEAN FsRtlCheckLockForReadAccess(PFILE_LOCK L, PIRP I) { (void)L; (void)I; return TRUE; }

VOID FsRtlNotifyCleanup(PNOTIFY_SYNC S, PLIST_ENTRY L, PVOID C) { (void)S; (void)L; (void)C; }

VOID FsRtlNotifyFullChangeDirectory(
    PNOTIFY_SYNC S, PLIST_ENTRY L, PVOID C, PSTRING F, BOOLEAN W, BOOLEAN I,
    ULONG Filter, PIRP Irp, PVOID Cb, PVOID Sub)
{
    (void)S; (void)L; (void)C; (void)F; (void)W; (void)I;
    (void)Filter; (void)Irp; (void)Cb; (void)Sub;
}

//
// Name matching is real: BlorgFS passes the caller's wildcard straight
// through, so a directory query test that could not distinguish "*.mkv"
// from "*" would not be testing the filter at all. This handles the
// wildcards the driver can actually receive.
//
BOOLEAN FsRtlIsNameInExpression(PUNICODE_STRING Expression, PUNICODE_STRING Name, BOOLEAN IgnoreCase, PWCH Upcase)
{
    (void)Upcase;

    const USHORT expLen = (USHORT)(Expression->Length / sizeof(WCHAR));
    const USHORT nameLen = (USHORT)(Name->Length / sizeof(WCHAR));

    USHORT e = 0;
    USHORT n = 0;
    USHORT starE = 0xFFFF;
    USHORT starN = 0;

    while (n < nameLen)
    {
        WCHAR ec = (e < expLen) ? Expression->Buffer[e] : 0;
        WCHAR nc = Name->Buffer[n];

        if (IgnoreCase)
        {
            ec = RtlUpcaseUnicodeChar(ec);
            nc = RtlUpcaseUnicodeChar(nc);
        }

        if (e < expLen && (ec == L'*' || ec == DOS_STAR))
        {
            starE = e++;
            starN = n;
        }
        else if (e < expLen && (ec == nc || ec == L'?' || ec == DOS_QM))
        {
            e++;
            n++;
        }
        else if (starE != 0xFFFF)
        {
            e = (USHORT)(starE + 1);
            n = ++starN;
        }
        else
        {
            return FALSE;
        }
    }

    while (e < expLen && (Expression->Buffer[e] == L'*' || Expression->Buffer[e] == DOS_STAR))
    {
        e++;
    }

    return (BOOLEAN)(e == expLen);
}

BOOLEAN FsRtlAreNamesEqual(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN IgnoreCase, PCWCH Upcase)
{
    (void)Upcase;
    return RtlEqualUnicodeString(A, B, IgnoreCase);
}

///////////////////////////////////////////////////////////////////////////
// I/O, objects and the rest
///////////////////////////////////////////////////////////////////////////

BOOLEAN IoIsOperationSynchronous(PIRP Irp)
{
    return (BOOLEAN)(Irp->Flags & IRP_SYNCHRONOUS_API);
}

PEPROCESS IoGetRequestorProcess(PIRP Irp) { (void)Irp; return PsGetCurrentProcess(); }

VOID IoAcquireVpbSpinLock(PKIRQL Irql) { if (Irql) { *Irql = 0; } }
VOID IoReleaseVpbSpinLock(KIRQL Irql) { (void)Irql; }

NTSTATUS BlorgCreateVolumeDeviceObject(PDRIVER_OBJECT D, PDEVICE_OBJECT* V)
{
    (void)D;

    if (V)
    {
        *V = NULL;
    }

    return STATUS_NOT_IMPLEMENTED;
}

BOOLEAN ExIsResourceAcquiredExclusiveLite(PERESOURCE R) { (void)R; return TRUE; }
VOID ExConvertExclusiveToSharedLite(PERESOURCE R) { (void)R; }

VOID ObReferenceObject(PVOID O) { (void)O; }
VOID ObDereferenceObject(PVOID O) { (void)O; }

NTSTATUS ObReferenceObjectByHandle(HANDLE H, ACCESS_MASK A, POBJECT_TYPE T, KPROCESSOR_MODE M, PVOID* O, PVOID I)
{
    (void)A; (void)T; (void)M; (void)I;

    if (O)
    {
        *O = NULL;
    }

    //
    // A NULL handle must fail, matching real Windows -- callers (e.g.
    // FspWorkQueue.c's StopWorkQueueThreads, when PsCreateSystemThread's
    // own no-op stub leaves ThreadHandle[i] NULL) rely on the failure to
    // skip a wait on an object that was never created. An unconditional
    // success here previously produced a NULL PVOID* that
    // KeWaitForSingleObject then dereferenced.
    //
    if (!H)
    {
        return STATUS_INVALID_HANDLE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS ZwClose(HANDLE H) { (void)H; return STATUS_SUCCESS; }

NTSTATUS PsCreateSystemThread(PHANDLE T, ULONG A, PVOID Ob, HANDLE P, PVOID C, PVOID S, PVOID Ctx)
{
    (void)A; (void)Ob; (void)P; (void)C; (void)S; (void)Ctx;

    if (T)
    {
        *T = NULL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS PsTerminateSystemThread(NTSTATUS S) { return S; }

PVOID KeGetCurrentThread(VOID) { return PsGetCurrentThread(); }
LONG KeSetBasePriorityThread(PVOID T, LONG P) { (void)T; (void)P; return 0; }

ULONG KeGetCurrentProcessorIndex(VOID) { return 0; }
ULONG KeQueryActiveProcessorCountEx(USHORT G) { (void)G; return 1; }
ULONG KeQueryMaximumProcessorCountEx(USHORT G) { (void)G; return 1; }

//
// KeQueryPerformanceCounter's backing counter ticks once per call, not
// with wall-clock time, so a TTL measured in seconds of real interrupt
// time is unreachable by simply calling it in a loop. ShimAdvanceInterruptTime
// lets a test jump the clock directly instead.
//
static volatile LONG64 InterruptTimeOffset = 0;

ULONG64 KeQueryInterruptTime(VOID)
{
    return (ULONG64)KeQueryPerformanceCounter(NULL).QuadPart + (ULONG64)InterruptTimeOffset;
}

VOID ShimAdvanceInterruptTime(ULONG64 Ticks100ns)
{
    InterlockedExchangeAdd64(&InterruptTimeOffset, (LONG64)Ticks100ns);
}

//
// A real wait, same rationale as KeWaitForSingleObject's (NtShim.c): the
// signal that satisfies FspWorkQueue.c's BlorgFspDispatch loop -- work posted,
// or shutdown requested -- comes from a genuinely different thread, so a
// stub that does not actually block cannot distinguish "no work yet" from
// "terminating" and cannot be driven by a real producer/consumer test.
// Object[] entries are PKEVENT, the same struct KeInitializeEvent/
// KeSetEvent use, so this waits on their real Win32 handles directly.
//
// Bounded rather than INFINITE for the same reason KeWaitForSingleObject
// is: a driver bug that never signals should fail the test with a
// diagnostic, not hang the suite.
//
NTSTATUS KeWaitForMultipleObjects(
    ULONG Count, PVOID Object[], WAIT_TYPE Type, KWAIT_REASON Reason,
    KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Timeout, PVOID WaitBlock)
{
    (void)Reason; (void)Mode; (void)Alertable; (void)Timeout; (void)WaitBlock;

    KmRequireIrqlAtMost(PASSIVE_LEVEL, "KeWaitForMultipleObjects");

    if (KmSchedActive())
    {
        //
        // Same hazard as KeWaitForSingleObject under the executor: a
        // real wait parks the only host thread. Fail loudly rather than
        // hang.
        //
        KmReportViolation(KmViolationLifetime,
            "KeWaitForMultipleObjects under systematic exploration");
        return STATUS_TIMEOUT;
    }

    HANDLE handles[MAXIMUM_WAIT_OBJECTS];

    for (ULONG i = 0; i < Count; ++i)
    {
        PKEVENT event = (PKEVENT)Object[i];
        handles[i] = event->Handle;
    }

    DWORD result = WaitForMultipleObjects(Count, handles, (WaitAll == Type), 30000);

    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + Count)
    {
        return STATUS_WAIT_0 + (result - WAIT_OBJECT_0);
    }

    KmReportViolation(KmViolationLifetime,
        "KeWaitForMultipleObjects timed out -- a signal never arrived");

    return STATUS_TIMEOUT;
}

VOID KdBreakPoint(VOID) { }

//
// ProbeForRead is the driver's own validation of a user buffer, so it has
// to reject what the kernel rejects rather than wave everything through:
// a handler that forgot to check a length must fail here, not silently
// pass a dispatch test.
//
VOID ProbeForRead(PVOID Address, SIZE_T Length, ULONG Alignment)
{
    if (0 == Length)
    {
        return;
    }

    if (!Address || (Alignment && (((ULONG_PTR)Address) & (Alignment - 1))))
    {
        KmReportViolation(KmViolationLifetime, "ProbeForRead on a misaligned or null user buffer");
    }
}

///////////////////////////////////////////////////////////////////////////
// Rtl
///////////////////////////////////////////////////////////////////////////

VOID RtlInitUnicodeString(PUNICODE_STRING Destination, PCWSTR Source)
{
    Destination->Buffer = (PWSTR)Source;
    Destination->Length = 0;
    Destination->MaximumLength = 0;

    if (Source)
    {
        SIZE_T chars = 0;

        while (Source[chars])
        {
            chars++;
        }

        Destination->Length = (USHORT)(chars * sizeof(WCHAR));
        Destination->MaximumLength = (USHORT)(Destination->Length + sizeof(WCHAR));
    }
}

BOOLEAN RtlPrefixUnicodeString(PCUNICODE_STRING Prefix, PCUNICODE_STRING String, BOOLEAN IgnoreCase)
{
    if (Prefix->Length > String->Length)
    {
        return FALSE;
    }

    const USHORT chars = (USHORT)(Prefix->Length / sizeof(WCHAR));

    for (USHORT i = 0; i < chars; i++)
    {
        WCHAR a = Prefix->Buffer[i];
        WCHAR b = String->Buffer[i];

        if (IgnoreCase)
        {
            a = RtlUpcaseUnicodeChar(a);
            b = RtlUpcaseUnicodeChar(b);
        }

        if (a != b)
        {
            return FALSE;
        }
    }

    return TRUE;
}

NTSTATUS RtlUpcaseUnicodeString(PUNICODE_STRING Destination, PCUNICODE_STRING Source, BOOLEAN Allocate)
{
    if (Allocate)
    {
        Destination->Buffer = NULL;
        Destination->Length = 0;
        Destination->MaximumLength = 0;

        if (0 == Source->Length)
        {
            return STATUS_SUCCESS;
        }

        PWCH buffer = ExAllocatePoolUninitialized(PagedPool, Source->Length, 'pUlR');

        if (!buffer)
        {
            return STATUS_NO_MEMORY;
        }

        const USHORT chars = (USHORT)(Source->Length / sizeof(WCHAR));

        for (USHORT i = 0; i < chars; i++)
        {
            buffer[i] = RtlUpcaseUnicodeChar(Source->Buffer[i]);
        }

        Destination->Buffer = buffer;
        Destination->Length = Source->Length;
        Destination->MaximumLength = Source->Length;

        return STATUS_SUCCESS;
    }

    if (Destination->MaximumLength < Source->Length)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    const USHORT chars = (USHORT)(Source->Length / sizeof(WCHAR));

    for (USHORT i = 0; i < chars; i++)
    {
        Destination->Buffer[i] = RtlUpcaseUnicodeChar(Source->Buffer[i]);
    }

    Destination->Length = Source->Length;

    return STATUS_SUCCESS;
}

NTSTATUS RtlCreateSecurityDescriptor(PVOID D, ULONG R) { (void)D; (void)R; return STATUS_SUCCESS; }
NTSTATUS RtlSetDaclSecurityDescriptor(PVOID D, BOOLEAN P, PVOID A, BOOLEAN Def) { (void)D; (void)P; (void)A; (void)Def; return STATUS_SUCCESS; }
NTSTATUS RtlSetOwnerSecurityDescriptor(PVOID D, PSID O, BOOLEAN Def) { (void)D; (void)O; (void)Def; return STATUS_SUCCESS; }
NTSTATUS RtlSetGroupSecurityDescriptor(PVOID D, PSID G, BOOLEAN Def) { (void)D; (void)G; (void)Def; return STATUS_SUCCESS; }

NTSTATUS RtlAbsoluteToSelfRelativeSD(PVOID A, PVOID S, PULONG L)
{
    (void)A; (void)S;

    if (L)
    {
        *L = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS SeQuerySecurityDescriptorInfo(PULONG I, PVOID D, PULONG L, PVOID* O)
{
    (void)I; (void)D; (void)O;

    if (L)
    {
        *L = 0;
    }

    return STATUS_SUCCESS;
}
