//
// Implementation of the NT substitute declared in NtShim.h. Anything with
// an IRQL contract routes through the model so the contract is checked
// rather than documented.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\Driver.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include <strsafe.h>

BOOLEAN ShimTraceEnabled = FALSE;


VOID KeEnterCriticalRegion(VOID)
{
    KmRequireIrqlAtMost(APC_LEVEL, "KeEnterCriticalRegion");
}

VOID KeLeaveCriticalRegion(VOID) {}

///////////////////////////////////////////////////////////////////////////
// Pool
///////////////////////////////////////////////////////////////////////////

#define SHIM_POOL_GUARD 0xA5A5A5A5A5A5A5A5ULL

typedef struct _SHIM_POOL_HEADER
{
    SIZE_T Size;
    ULONG Tag;
    ULONG Reserved;
    ULONG64 FrontGuard;
} SHIM_POOL_HEADER;

static volatile LONG PoolFailIndex = -1;
static volatile LONG PoolAllocationCounter = 0;

static PVOID WatchedBlock = NULL;
static volatile LONG* WatchedFlag = NULL;

VOID ShimWatchFree(PVOID Block, volatile LONG* Flag)
{
    WatchedBlock = Block;
    WatchedFlag = Flag;
}

static VOID NoteFree(PVOID Block)
{
    if (Block && Block == WatchedBlock && WatchedFlag)
    {
        InterlockedExchange((LONG*)WatchedFlag, 1);
    }
}

//
// A crashing sandbox binary must fail the run, not stop it. Without this
// a fault raises Windows Error Reporting and the process sits on a modal
// dialog until someone dismisses it -- which in a test loop means dozens
// of dialogs and no results.
//
static void ShimSuppressCrashDialogs(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

VOID ShimPoolFailAt(LONG Index)
{
    InterlockedExchange(&PoolFailIndex, Index);
    InterlockedExchange(&PoolAllocationCounter, 0);
}

SIZE_T ShimPoolOutstanding(VOID)
{
    return (SIZE_T)KmObjectsLive(KmObjectPool);
}

static PVOID ShimAllocate(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag, BOOLEAN Zero)
{
    //
    // Paged pool cannot be touched above APC_LEVEL, and allocating it at
    // DISPATCH is the bug that produces a paged-memory bugcheck later,
    // far from the cause. Checking at the allocation is what puts the
    // diagnostic on the offending line.
    //
    if (PagedPool == PoolType)
    {
        KmRequireIrqlAtMost(APC_LEVEL, "ExAllocatePool(PagedPool)");
    }
    else
    {
        KmRequireIrqlAtMost(DISPATCH_LEVEL, "ExAllocatePool(NonPagedPool)");
    }

    if (0 == NumberOfBytes)
    {
        return NULL;
    }

    LONG index = InterlockedIncrement(&PoolAllocationCounter) - 1;
    LONG failAt = PoolFailIndex;

    if (failAt >= 0 && index == failAt)
    {
        return NULL;
    }

    SHIM_POOL_HEADER* header =
        (SHIM_POOL_HEADER*)malloc(sizeof(SHIM_POOL_HEADER) + NumberOfBytes + sizeof(ULONG64));

    if (!header)
    {
        return NULL;
    }

    header->Size = NumberOfBytes;
    header->Tag = Tag;
    header->Reserved = 0;
    header->FrontGuard = SHIM_POOL_GUARD;

    unsigned char* body = (unsigned char*)(header + 1);

    //
    // Uninitialized pool is not zeroed in the kernel. Poisoning it makes
    // code that accidentally relies on zeroing fail here rather than
    // working by luck until a machine under memory pressure hands back a
    // dirty page.
    //
    memset(body, Zero ? 0 : 0xCD, NumberOfBytes);

    //
    // The tail guard is written unaligned on purpose -- it sits exactly at
    // the end of the caller's block, so a one-byte overrun trips it.
    //
    const ULONG64 backGuard = SHIM_POOL_GUARD;
    memcpy(body + NumberOfBytes, &backGuard, sizeof(backGuard));

    KmObjectCreated(KmObjectPool);

    return body;
}

PVOID ExAllocatePoolZero(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag)
{
    return ShimAllocate(PoolType, NumberOfBytes, Tag, TRUE);
}

PVOID ExAllocatePoolUninitialized(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag)
{
    return ShimAllocate(PoolType, NumberOfBytes, Tag, FALSE);
}

//
// Freed blocks are held poisoned rather than returned to the CRT, for a
// bounded window.
//
// The point is diagnosis. A use-after-free on memory the CRT has taken
// back faults, and a fault says only "something touched bad memory" from
// a stack that has already left the interesting code. Keeping the block
// mapped and full of 0xDD means the read returns a value instead, so the
// test that asserts on that value reports what actually went wrong -- a
// node used after it was freed -- at the line that used it.
//
// Bounded because an unbounded quarantine is a leak: systematic
// exploration replays a body thousands of times and would hold every
// block from every replay.
//
#define SHIM_QUARANTINE_DEPTH 512

static SHIM_POOL_HEADER* volatile Quarantine[SHIM_QUARANTINE_DEPTH];
static volatile LONG QuarantineNext = 0;

//
// Interlocked, because frees race. Claiming the slot with an atomic
// increment and swapping the occupant out with an atomic exchange is what
// stops two concurrent frees taking the same slot and both releasing the
// block they evicted -- a double free, which corrupts the CRT heap and
// surfaces as an intermittent crash somewhere else entirely.
//
static void QuarantineOrFree(SHIM_POOL_HEADER* Header)
{
    const LONG slot =
        ((ULONG)InterlockedIncrement(&QuarantineNext) - 1) % SHIM_QUARANTINE_DEPTH;

    SHIM_POOL_HEADER* evicted =
        (SHIM_POOL_HEADER*)InterlockedExchangePointer((PVOID volatile*)&Quarantine[slot], Header);

    if (evicted)
    {
        free(evicted);
    }
}

//
// Both guards are checked on the way out, so an overrun or an underrun is
// attributed to the block it corrupted rather than to whatever allocation
// happens to fail next. The body is poisoned before the underlying free so
// a read through a stale pointer sees an obviously wrong value instead of
// the object that used to be there.
//
VOID ExFreePool(PVOID P)
{
    if (!P)
    {
        return;
    }

    NoteFree(P);

    SHIM_POOL_HEADER* header = ((SHIM_POOL_HEADER*)P) - 1;

    if (SHIM_POOL_GUARD != header->FrontGuard)
    {
        KmReportViolation(KmViolationPool,
            "pool block underrun or double free (tag %.4s)", (const char*)&header->Tag);
        return;
    }

    ULONG64 backGuard = 0;
    memcpy(&backGuard, ((unsigned char*)P) + header->Size, sizeof(backGuard));

    if (SHIM_POOL_GUARD != backGuard)
    {
        KmReportViolation(KmViolationPool,
            "pool block overrun past %zu bytes (tag %.4s)", header->Size, (const char*)&header->Tag);
    }

    memset(P, 0xDD, header->Size);
    header->FrontGuard = 0;

    KmObjectDestroyed(KmObjectPool);

    QuarantineOrFree(header);
}

VOID ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PVOID Allocate, PVOID Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth)
{
    (void)Allocate;
    (void)Free;
    (void)Flags;
    Lookaside->L.Size = (ULONG)Size;
    Lookaside->L.Tag = Tag;
    Lookaside->L.Depth = Depth;
    Lookaside->Outstanding = 0;
}

VOID ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside)
{
    if (Lookaside->Outstanding != 0)
    {
        KmReportViolation(KmViolationLifetime,
            "lookaside list deleted with %ld entries outstanding", Lookaside->Outstanding);
    }
}

//
// Deliberately NOT a free list: every entry is a fresh guarded allocation.
// A real lookaside hands back recently-freed memory, which is precisely
// how a use-after-free stays invisible -- the stale pointer still points
// at a plausible object. Fresh blocks plus the guards turn that into an
// immediate, attributable failure.
//
PVOID ExAllocateFromNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside)
{
    PVOID entry = ExAllocatePoolUninitialized(NonPagedPoolNx, Lookaside->L.Size, Lookaside->L.Tag);

    if (entry)
    {
        InterlockedIncrement(&Lookaside->Outstanding);
    }

    return entry;
}

VOID ExFreeToNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PVOID Entry)
{
    InterlockedDecrement(&Lookaside->Outstanding);
    ExFreePool(Entry);
}

static MM_SYSTEMSIZE ShimSystemSize = MmLargeSystem;

MM_SYSTEMSIZE MmQuerySystemSize(VOID)
{
    return ShimSystemSize;
}

///////////////////////////////////////////////////////////////////////////
// Timers and DPCs
///////////////////////////////////////////////////////////////////////////

typedef struct _SHIM_DPC_BRIDGE
{
    PKDEFERRED_ROUTINE Routine;
    PVOID Context;
} SHIM_DPC_BRIDGE;

//
// KM_DPC carries a single void* context, but a KDEFERRED_ROUTINE wants
// both its own context and the DPC pointer. The bridge is stored in the
// KM_DPC's context slot and unpacked here.
//
static void ShimDpcTrampoline(KM_DPC* Dpc, void* Context, void* Arg1, void* Arg2)
{
    SHIM_DPC_BRIDGE* bridge = (SHIM_DPC_BRIDGE*)Context;

    bridge->Routine((PKDPC)Dpc, bridge->Context, Arg1, Arg2);
}

VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
    SHIM_DPC_BRIDGE* bridge = (SHIM_DPC_BRIDGE*)calloc(1, sizeof(SHIM_DPC_BRIDGE));

    bridge->Routine = Routine;
    bridge->Context = Context;

    KmInitializeDpc((KM_DPC*)Dpc, ShimDpcTrampoline, bridge);
}

VOID KeInitializeTimer(PKTIMER Timer)
{
    KmInitializeTimer((KM_TIMER*)Timer);
}

BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
{
    return KmSetTimer((KM_TIMER*)Timer, DueTime.QuadPart, (KM_DPC*)Dpc);
}

BOOLEAN KeCancelTimer(PKTIMER Timer)
{
    return KmCancelTimer((KM_TIMER*)Timer);
}

///////////////////////////////////////////////////////////////////////////
// Events
///////////////////////////////////////////////////////////////////////////

VOID KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State)
{
    Event->Handle = CreateEventW(NULL, (NotificationEvent == Type), State, NULL);
}

LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait)
{
    (void)Increment;
    (void)Wait;

    SetEvent(Event->Handle);
    return 0;
}

//
// A real wait, because the drain paths under test are genuinely
// cross-thread: the completion that satisfies the wait runs on the
// transport thread. Waiting for real is what makes a quiescence test
// mean something.
//
NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON Reason, KPROCESSOR_MODE Mode, BOOLEAN Alertable, PVOID Timeout)
{
    (void)Reason;
    (void)Mode;
    (void)Alertable;
    (void)Timeout;

    KmRequireIrqlAtMost(PASSIVE_LEVEL, "KeWaitForSingleObject");

    PKEVENT event = (PKEVENT)Object;

    //
    // Bounded rather than INFINITE: a driver bug that never signals
    // should fail the test with a diagnostic, not hang the suite until
    // someone kills it.
    //
    if (WAIT_OBJECT_0 != WaitForSingleObject(event->Handle, 30000))
    {
        KmReportViolation(KmViolationLifetime,
            "KeWaitForSingleObject timed out -- a drain never completed");
    }

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// MDLs
///////////////////////////////////////////////////////////////////////////

static volatile LONG MdlMappingFailPending = 0;

VOID ShimFailNextMdlMapping(VOID)
{
    InterlockedExchange(&MdlMappingFailPending, 1);
}

PMDL IoAllocateMdl(PVOID Base, ULONG Length, BOOLEAN Secondary, BOOLEAN ChargeQuota, PVOID Irp)
{
    (void)Secondary;
    (void)ChargeQuota;
    (void)Irp;

    PMDL mdl = (PMDL)calloc(1, sizeof(MDL));

    if (!mdl)
    {
        return NULL;
    }

    mdl->Base = Base;
    mdl->Length = Length;

    KmObjectCreated(KmObjectMdl);

    return mdl;
}

VOID IoFreeMdl(PMDL Mdl)
{
    if (!Mdl)
    {
        return;
    }

    if (Mdl->Locked)
    {
        KmReportViolation(KmViolationLifetime, "IoFreeMdl on an MDL whose pages are still locked");
    }

    KmObjectDestroyed(KmObjectMdl);
    free(Mdl);
}

VOID MmProbeAndLockPages(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG Operation)
{
    (void)AccessMode;
    (void)Operation;

    KmRequireIrqlAtMost(APC_LEVEL, "MmProbeAndLockPages");

    if (Mdl->Locked)
    {
        KmReportViolation(KmViolationLifetime, "MmProbeAndLockPages on an already-locked MDL");
    }

    Mdl->Locked = TRUE;
}

VOID MmUnlockPages(PMDL Mdl)
{
    if (!Mdl->Locked)
    {
        KmReportViolation(KmViolationLifetime, "MmUnlockPages on an MDL that is not locked");
        return;
    }

    Mdl->Locked = FALSE;
}

VOID MmBuildMdlForNonPagedPool(PMDL Mdl)
{
    Mdl->BuiltForNonPaged = TRUE;
}

PVOID MmGetSystemAddressForMdlSafe(PMDL Mdl, ULONG Priority)
{
    (void)Priority;

    if (InterlockedExchange(&MdlMappingFailPending, 0))
    {
        return NULL;
    }

    return Mdl ? Mdl->Base : NULL;
}

///////////////////////////////////////////////////////////////////////////
// IRPs
///////////////////////////////////////////////////////////////////////////

PIRP IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota)
{
    (void)StackSize;
    (void)ChargeQuota;

    PIRP irp = (PIRP)calloc(1, sizeof(IRP));

    if (irp)
    {
        KmObjectCreated(KmObjectIrp);
    }

    return irp;
}

VOID IoFreeIrp(PIRP Irp)
{
    if (!Irp)
    {
        return;
    }

    if (Irp->Freed)
    {
        KmReportViolation(KmViolationLifetime, "IoFreeIrp on an already-freed IRP -- double free");
        return;
    }

    if (Irp->Outstanding)
    {
        KmReportViolation(KmViolationLifetime,
            "IoFreeIrp while the transport still owns the IRP -- use after free ahead");
    }

    Irp->Freed = TRUE;
    KmObjectDestroyed(KmObjectIrp);

    free(Irp);
}

VOID IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context, BOOLEAN Success, BOOLEAN Error, BOOLEAN Cancel)
{
    (void)Success;
    (void)Error;
    (void)Cancel;

    Irp->CompletionRoutine = Routine;
    Irp->CompletionContext = Context;
}

VOID IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost)
{
    (void)PriorityBoost;

    Irp->CompletionCount++;

    if (Irp->Completed)
    {
        KmReportViolation(KmViolationLifetime, "IoCompleteRequest on an already-completed IRP");
        return;
    }

    Irp->Completed = TRUE;
    Irp->Outstanding = FALSE;

    if (Irp->CompletionRoutine)
    {
        Irp->CompletionRoutine(NULL, Irp, Irp->CompletionContext);
    }
}

//
// Cancelling an outstanding IRP is what a watchdog does; the transport
// notices and completes it STATUS_CANCELLED. Cancelling an
// already-completed one must be harmless -- the driver's timeout DPC can
// legitimately race a real completion and lose, and the model has to let
// that happen rather than making the race look like a bug.
//
BOOLEAN IoCancelIrp(PIRP Irp)
{
    if (Irp->Freed)
    {
        KmReportViolation(KmViolationLifetime, "IoCancelIrp on a freed IRP -- use after free");
        return FALSE;
    }

    Irp->Cancel = TRUE;

    return TRUE;
}

//
// In the kernel this ORs SL_PENDING_RETURNED into the current stack
// location. Here it just records the fact, so a test can assert that
// every asynchronously-completed request was marked -- the contract whose
// violation is invisible until a filter sits above the driver.
//
VOID IoMarkIrpPending(PIRP Irp)
{
    Irp->PendingReturned = TRUE;
}

///////////////////////////////////////////////////////////////////////////
// Rtl
///////////////////////////////////////////////////////////////////////////

BOOLEAN RtlEqualString(const STRING* String1, const STRING* String2, BOOLEAN CaseInSensitive)
{
    //
    // The real one is documented PASSIVE_LEVEL only. Client.c has a local
    // case-insensitive compare precisely because of that, so enforcing it
    // here keeps anyone from "simplifying" that away.
    //
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "RtlEqualString");

    if (String1->Length != String2->Length)
    {
        return FALSE;
    }

    for (USHORT i = 0; i < String1->Length; ++i)
    {
        char a = String1->Buffer[i];
        char b = String2->Buffer[i];

        if (CaseInSensitive)
        {
            if (a >= 'A' && a <= 'Z') { a += 'a' - 'A'; }
            if (b >= 'A' && b <= 'Z') { b += 'a' - 'A'; }
        }

        if (a != b)
        {
            return FALSE;
        }
    }

    return TRUE;
}

NTSTATUS RtlUnicodeStringToUTF8String(PUTF8_STRING Destination, const UNICODE_STRING* Source, BOOLEAN AllocateDestination)
{
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "RtlUnicodeStringToUTF8String");

    if (!AllocateDestination)
    {
        return STATUS_INVALID_PARAMETER;
    }

    int chars = (int)(Source->Length / sizeof(WCHAR));
    int needed = WideCharToMultiByte(CP_UTF8, 0, Source->Buffer, chars, NULL, 0, NULL, NULL);

    if (needed < 0 || needed > 0xFFFF)
    {
        return STATUS_INVALID_PARAMETER;
    }

    char* buffer = (char*)malloc((size_t)needed + 1);

    if (!buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (needed > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, Source->Buffer, chars, buffer, needed, NULL, NULL);
    }

    buffer[needed] = '\0';

    Destination->Buffer = buffer;
    Destination->Length = (USHORT)needed;
    Destination->MaximumLength = (USHORT)(needed + 1);

    return STATUS_SUCCESS;
}

VOID RtlFreeUTF8String(PUTF8_STRING Utf8String)
{
    free(Utf8String->Buffer);
    Utf8String->Buffer = NULL;
    Utf8String->Length = 0;
    Utf8String->MaximumLength = 0;
}

NTSTATUS RtlUTF8ToUnicodeN(PWSTR Destination, ULONG MaxBytes, PULONG ActualBytes, const CHAR* Source, ULONG SourceBytes)
{
    int chars = MultiByteToWideChar(CP_UTF8, 0, Source, (int)SourceBytes, Destination, (int)(MaxBytes / sizeof(WCHAR)));

    if (0 == chars && SourceBytes > 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (ActualBytes)
    {
        *ActualBytes = (ULONG)(chars * sizeof(WCHAR));
    }

    return STATUS_SUCCESS;
}

NTSTATUS RtlStringCbLengthA(const char* Source, SIZE_T MaxLength, SIZE_T* Length)
{
    return SUCCEEDED(StringCbLengthA(Source, MaxLength, Length)) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

//
// %wZ is kernel-only, so the format is rewritten as it is walked: each
// %wZ consumes a PUNICODE_STRING and expands to its UTF-8 bytes. Client.c
// builds every request line through this, so a wrong expansion here would
// invalidate every request-shape assertion in the suite.
//
NTSTATUS RtlStringCbPrintfA(char* Destination, SIZE_T DestinationBytes, const char* Format, ...)
{
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "RtlStringCbPrintfA(%wZ)");

    va_list args;
    va_start(args, Format);

    char* out = Destination;
    SIZE_T remaining = DestinationBytes;
    const char* cursor = Format;

    while (*cursor)
    {
        if (cursor[0] == '%' && cursor[1] == 'w' && cursor[2] == 'Z')
        {
            const UNICODE_STRING* value = va_arg(args, const UNICODE_STRING*);
            UTF8_STRING utf8;

            if (!NT_SUCCESS(RtlUnicodeStringToUTF8String(&utf8, value, TRUE)))
            {
                va_end(args);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            if (utf8.Length + 1u > remaining)
            {
                RtlFreeUTF8String(&utf8);
                va_end(args);
                return STATUS_BUFFER_OVERFLOW;
            }

            memcpy(out, utf8.Buffer, utf8.Length);
            out += utf8.Length;
            remaining -= utf8.Length;

            RtlFreeUTF8String(&utf8);
            cursor += 3;
            continue;
        }

        if (cursor[0] == '%')
        {
            char spec[16] = { 0 };
            int specLen = 0;

            spec[specLen++] = *cursor++;

            while (*cursor && specLen < 15 && !strchr("diouxXeEfgGaAcspn%", *cursor))
            {
                spec[specLen++] = *cursor++;
            }

            if (*cursor && specLen < 15)
            {
                spec[specLen++] = *cursor++;
            }

            int written = 0;

            if (0 == strcmp(spec, "%%"))
            {
                written = _snprintf_s(out, remaining, _TRUNCATE, "%%");
            }
            else if (strstr(spec, "s"))
            {
                written = _snprintf_s(out, remaining, _TRUNCATE, spec, va_arg(args, const char*));
            }
            else if (strstr(spec, "z") || strstr(spec, "I"))
            {
                written = _snprintf_s(out, remaining, _TRUNCATE, spec, va_arg(args, size_t));
            }
            else
            {
                written = _snprintf_s(out, remaining, _TRUNCATE, spec, va_arg(args, int));
            }

            if (written < 0)
            {
                va_end(args);
                return STATUS_BUFFER_OVERFLOW;
            }

            out += written;
            remaining -= (SIZE_T)written;
            continue;
        }

        if (remaining <= 1)
        {
            va_end(args);
            return STATUS_BUFFER_OVERFLOW;
        }

        *out++ = *cursor++;
        remaining--;
    }

    if (0 == remaining)
    {
        va_end(args);
        return STATUS_BUFFER_OVERFLOW;
    }

    *out = '\0';
    va_end(args);

    return STATUS_SUCCESS;
}

VOID ShimReset(VOID)
{
    ShimSuppressCrashDialogs();

    KmReset();
    ShimPoolFailAt(-1);
    ShimWatchFree(NULL, NULL);
    InterlockedExchange(&MdlMappingFailPending, 0);
}

///////////////////////////////////////////////////////////////////////////
// Work items
///////////////////////////////////////////////////////////////////////////

struct _IO_WORKITEM
{
    //
    // The device object the item was allocated against. The kernel hands
    // this back to the routine as its first argument, and BlorgFS's reap
    // worker uses it to reach the volume extension -- so passing NULL here
    // faults the worker on its first line.
    //
    PDEVICE_OBJECT DeviceObject;

    PIO_WORKITEM_ROUTINE Routine;
    PVOID Context;
    LONG Queued;
    struct _IO_WORKITEM* Next;
};

static PIO_WORKITEM WorkItemHead = NULL;
static PIO_WORKITEM WorkItemTail = NULL;
static CRITICAL_SECTION WorkItemCs;
static long WorkItemCsInit = 0;

static void EnsureWorkItemCs(void)
{
    if (0 == InterlockedCompareExchange(&WorkItemCsInit, 1, 0))
    {
        InitializeCriticalSection(&WorkItemCs);
        InterlockedExchange(&WorkItemCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&WorkItemCsInit, 2, 2))
    {
        Sleep(0);
    }
}

PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject)
{
    PIO_WORKITEM item = (PIO_WORKITEM)calloc(1, sizeof(IO_WORKITEM));

    if (item)
    {
        item->DeviceObject = DeviceObject;
        KmObjectCreated(KmObjectWorkItem);
    }

    return item;
}

VOID IoFreeWorkItem(PIO_WORKITEM IoWorkItem)
{
    if (!IoWorkItem)
    {
        return;
    }

    if (IoWorkItem->Queued)
    {
        KmReportViolation(KmViolationLifetime,
            "IoFreeWorkItem on a still-queued work item -- the worker will run on freed memory");
    }

    KmObjectDestroyed(KmObjectWorkItem);
    free(IoWorkItem);
}

//
// The kernel does not dedup work items; queueing one twice before it runs
// corrupts its list linkage. Drivers that need dedup do it themselves --
// the prefetch ring's PumpQueued flag exists exactly for this -- so the
// model reports a double queue rather than silently tolerating it, which
// is what would let a broken dedup flag pass.
//
VOID IoQueueWorkItem(PIO_WORKITEM IoWorkItem, PIO_WORKITEM_ROUTINE Routine, WORK_QUEUE_TYPE QueueType, PVOID Context)
{
    (void)QueueType;

    EnsureWorkItemCs();
    EnterCriticalSection(&WorkItemCs);

    if (IoWorkItem->Queued)
    {
        LeaveCriticalSection(&WorkItemCs);

        KmReportViolation(KmViolationLifetime,
            "IoQueueWorkItem on an already-queued work item -- caller's dedup is broken");
        return;
    }

    IoWorkItem->Routine = Routine;
    IoWorkItem->Context = Context;
    IoWorkItem->Queued = 1;
    IoWorkItem->Next = NULL;

    if (WorkItemTail)
    {
        WorkItemTail->Next = IoWorkItem;
    }
    else
    {
        WorkItemHead = IoWorkItem;
    }

    WorkItemTail = IoWorkItem;

    LeaveCriticalSection(&WorkItemCs);
}

ULONG ShimPendingWorkItems(VOID)
{
    EnsureWorkItemCs();
    EnterCriticalSection(&WorkItemCs);

    ULONG count = 0;

    for (PIO_WORKITEM item = WorkItemHead; item; item = item->Next)
    {
        count++;
    }

    LeaveCriticalSection(&WorkItemCs);

    return count;
}

//
// A work item runs at PASSIVE_LEVEL whatever IRQL queued it, and the
// driver's entire bounce design depends on that. Forcing it here is what
// makes a fetch issued from a work item legal and one issued from a
// completion illegal, which is the distinction the pump exists to draw.
//
ULONG ShimDrainWorkItems(VOID)
{
    ULONG ran = 0;

    for (;;)
    {
        EnsureWorkItemCs();
        EnterCriticalSection(&WorkItemCs);

        PIO_WORKITEM item = WorkItemHead;

        if (item)
        {
            WorkItemHead = item->Next;

            if (!WorkItemHead)
            {
                WorkItemTail = NULL;
            }

            item->Queued = 0;
            item->Next = NULL;
        }

        LeaveCriticalSection(&WorkItemCs);

        if (!item)
        {
            return ran;
        }

        unsigned char saved = KmGetIrql();
        KmSetIrql(PASSIVE_LEVEL);

        item->Routine(item->DeviceObject, item->Context);

        KmSetIrql(saved);
        ran++;
    }
}

///////////////////////////////////////////////////////////////////////////
// Statistics
///////////////////////////////////////////////////////////////////////////

//
// One block stands in for the per-processor table. The sandbox's threads
// share it, which is exactly what the driver's per-CPU design avoids --
// but the counters are advisory in both, and a torn count here would fail
// no assertion that matters.
//
// The kernel's trace sink. Gated on ShimTraceEnabled so a passing run is
// quiet and a failing one can be made loud without recompiling.
//
ULONG DbgPrintEx(ULONG ComponentId, ULONG Level, const char* Format, ...)
{
    (void)ComponentId;
    (void)Level;

    if (!ShimTraceEnabled)
    {
        return 0;
    }

    va_list args;
    va_start(args, Format);
    vprintf(Format, args);
    va_end(args);

    return 0;
}

///////////////////////////////////////////////////////////////////////////
// Test-built MDLs and the kernel stack budget
///////////////////////////////////////////////////////////////////////////

PMDL ShimCreateMdl(PVOID Base, SIZE_T Length)
{
    PMDL mdl = (PMDL)calloc(1, sizeof(MDL));

    if (mdl)
    {
        mdl->Base = Base;
        mdl->Length = Length;
    }

    return mdl;
}

VOID ShimFreeMdl(PMDL Mdl)
{
    free(Mdl);
}

static SIZE_T RemainingStack = 16 * 1024;
static BOOLEAN StackExpansionFailPending = FALSE;

SIZE_T IoGetRemainingStackSize(VOID)
{
    return RemainingStack;
}

VOID ShimSetRemainingStack(SIZE_T Bytes)
{
    RemainingStack = Bytes;
}

VOID ShimFailNextStackExpansion(VOID)
{
    StackExpansionFailPending = TRUE;
}

//
// The contract says the callout has NOT run when expansion fails and HAS
// run to completion when it succeeds. Client.c relies on both halves, so
// the budget is raised for the duration and restored after rather than
// this being a no-op wrapper that always runs the callout.
//
NTSTATUS KeExpandKernelStackAndCalloutEx(
    PEXPAND_STACK_CALLOUT Callout,
    PVOID Parameter,
    SIZE_T Size,
    BOOLEAN Wait,
    PVOID Context)
{
    (void)Wait;
    (void)Context;

    if (StackExpansionFailPending)
    {
        StackExpansionFailPending = FALSE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SIZE_T saved = RemainingStack;
    RemainingStack += Size;

    Callout(Parameter);

    RemainingStack = saved;

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////
// Cancel-safe queue
///////////////////////////////////////////////////////////////////////////

NTSTATUS IoCsqInitialize(
    PIO_CSQ Csq,
    IO_CSQ_INSERT_IRP* CsqInsertIrp,
    IO_CSQ_REMOVE_IRP* CsqRemoveIrp,
    IO_CSQ_PEEK_NEXT_IRP* CsqPeekNextIrp,
    IO_CSQ_ACQUIRE_LOCK* CsqAcquireLock,
    IO_CSQ_RELEASE_LOCK* CsqReleaseLock,
    IO_CSQ_COMPLETE_CANCELED_IRP* CsqCompleteCanceledIrp)
{
    Csq->CsqInsertIrp = CsqInsertIrp;
    Csq->CsqRemoveIrp = CsqRemoveIrp;
    Csq->CsqPeekNextIrp = CsqPeekNextIrp;
    Csq->CsqAcquireLock = CsqAcquireLock;
    Csq->CsqReleaseLock = CsqReleaseLock;
    Csq->CsqCompleteCanceledIrp = CsqCompleteCanceledIrp;

    return STATUS_SUCCESS;
}

//
// The kernel marks the IRP pending here, inside the queue lock, before the
// driver's insert callback runs. A driver that additionally calls
// IoMarkIrpPending around this is double-marking -- which is why the model
// does it rather than leaving it to the caller to imitate.
//
VOID IoCsqInsertIrp(PIO_CSQ Csq, PIRP Irp, PIO_CSQ_IRP_CONTEXT Context)
{
    KIRQL irql = 0;

    (void)Context;

    Csq->CsqAcquireLock(Csq, &irql);

    IoMarkIrpPending(Irp);
    Csq->CsqInsertIrp(Csq, Irp);

    Csq->CsqReleaseLock(Csq, irql);
}

PIRP IoCsqRemoveNextIrp(PIO_CSQ Csq, PVOID PeekContext)
{
    KIRQL irql = 0;

    Csq->CsqAcquireLock(Csq, &irql);

    PIRP irp = Csq->CsqPeekNextIrp(Csq, NULL, PeekContext);

    if (irp)
    {
        Csq->CsqRemoveIrp(Csq, irp);
    }

    Csq->CsqReleaseLock(Csq, irql);

    return irp;
}

PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp)
{
    return Irp->StackLocation;
}

static OBJECT_TYPE* PsThreadTypeObject = NULL;
POBJECT_TYPE* PsThreadType = &PsThreadTypeObject;

//
// A monotonic counter with a fixed frequency. Statistics.c divides by the
// frequency, so it must never be zero.
//
LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency)
{
    static LONG64 Ticks = 0;

    if (PerformanceFrequency)
    {
        PerformanceFrequency->QuadPart = 10000000;
    }

    LARGE_INTEGER now;
    now.QuadPart = InterlockedIncrement64(&Ticks);
    return now;
}

///////////////////////////////////////////////////////////////////////////
// Thread identity and top-level IRP
///////////////////////////////////////////////////////////////////////////

PETHREAD PsGetCurrentThread(VOID)
{
    return (PETHREAD)(ULONG_PTR)GetCurrentThreadId();
}

PEPROCESS PsGetCurrentProcess(VOID)
{
    return (PEPROCESS)(ULONG_PTR)GetCurrentProcessId();
}

//
// Top-level IRP is per-thread in the kernel and must be per-thread here:
// the recursion guards in Util.h and FspWorkQueue.h exist to tell a
// re-entrant call apart from a fresh request, and a shared global would
// make two sandbox threads see each other's re-entrancy.
//
static DWORD TopLevelIrpSlot = TLS_OUT_OF_INDEXES;
static long TopLevelIrpSlotInit = 0;

static DWORD TopLevelSlot(void)
{
    if (0 == InterlockedCompareExchange(&TopLevelIrpSlotInit, 1, 0))
    {
        TopLevelIrpSlot = TlsAlloc();
    }

    while (TLS_OUT_OF_INDEXES == TopLevelIrpSlot)
    {
        Sleep(0);
    }

    return TopLevelIrpSlot;
}

PIRP IoGetTopLevelIrp(VOID)
{
    return (PIRP)TlsGetValue(TopLevelSlot());
}

VOID IoSetTopLevelIrp(PIRP Irp)
{
    TlsSetValue(TopLevelSlot(), Irp);
}

//
// No byte-range lock is ever held in the sandbox, so every fast check
// succeeds. Modelling real ranges would be modelling FsRtl, not BlorgFS.
//
BOOLEAN FsRtlFastCheckLockForRead(
    PFILE_LOCK FileLock, PLARGE_INTEGER FileOffset, PLARGE_INTEGER Length,
    ULONG Key, PFILE_OBJECT FileObject, PVOID ProcessId)
{
    (void)FileLock; (void)FileOffset; (void)Length;
    (void)Key; (void)FileObject; (void)ProcessId;
    return TRUE;
}

BOOLEAN FsRtlFastCheckLockForWrite(
    PFILE_LOCK FileLock, PLARGE_INTEGER FileOffset, PLARGE_INTEGER Length,
    ULONG Key, PFILE_OBJECT FileObject, PVOID ProcessId)
{
    (void)FileLock; (void)FileOffset; (void)Length;
    (void)Key; (void)FileObject; (void)ProcessId;
    return TRUE;
}

PSE_EXPORTS SeExports = NULL;
