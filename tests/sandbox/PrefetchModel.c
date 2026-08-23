//
// The two things Prefetch.c calls out to, modelled so a test controls
// when a fetch completes and can observe what happened to a parked read.
//
// The ring's invariants are all about ordering -- a fetch completing
// while a serve is mid-flight, a detach racing a pump, a teardown racing
// an in-flight fetch. None of those are reachable unless the test decides
// when completions happen, so the fetch issuer is a queue the test
// drains rather than something that answers immediately.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////
// IRP completion
///////////////////////////////////////////////////////////////////////////

//
// Parked reads are completed from a fetch completion at DISPATCH, and
// completing one twice is the classic bug in a park/publish protocol.
// The model used to intercept BlorgCompleteRequest to count that -- but
// BlorgCompleteRequest is a real inline in Util.h, so intercepting it meant
// the tests exercised the model's copy instead of the driver's. The
// counting now lives in the shim's IoCompleteRequest, which is where the
// invariant actually belongs: an IRP completed twice is an NT-level bug
// whatever completes it. These are thin readers over that.
//

static CRITICAL_SECTION FetchCs;
static long FetchCsInit = 0;

static void EnsureFetchCs(void)
{
    if (0 == InterlockedCompareExchange(&FetchCsInit, 1, 0))
    {
        InitializeCriticalSection(&FetchCs);
        InterlockedExchange(&FetchCsInit, 2);
    }

    while (2 != InterlockedCompareExchange(&FetchCsInit, 2, 2))
    {
        Sleep(0);
    }
}

int PrefetchModelCompletionCount(PIRP Irp)
{
    return (int)Irp->CompletionCount;
}

NTSTATUS PrefetchModelCompletionStatus(PIRP Irp)
{
    return Irp->CompletionCount ? Irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
}

SIZE_T PrefetchModelCompletionBytes(PIRP Irp)
{
    return (SIZE_T)Irp->IoStatus.Information;
}

///////////////////////////////////////////////////////////////////////////
// Fetch issuer
///////////////////////////////////////////////////////////////////////////

typedef struct _PREFETCH_FETCH
{
    PBLORG_FILEREAD_COMPLETION Routine;
    PVOID Context;
    PMDL TargetMdl;
    SIZE_T Offset;
    SIZE_T Length;
    struct _PREFETCH_FETCH* Next;
} PREFETCH_FETCH;

static PREFETCH_FETCH* FetchQueueHead = NULL;
static PREFETCH_FETCH* FetchQueueTail = NULL;

static volatile LONG FetchesIssued = 0;
static volatile LONG IssueFailuresPending = 0;

//
// Fill byte for a completed fetch, so a test can prove the bytes a read
// received came from the slot it parked on rather than from another.
//
static unsigned char FetchFillByte = 0xAB;

VOID PrefetchModelSetFillByte(unsigned char Value)
{
    FetchFillByte = Value;
}

VOID PrefetchModelFailNextIssues(LONG Count)
{
    InterlockedExchange(&IssueFailuresPending, Count);
}

LONG PrefetchModelFetchesIssued(VOID)
{
    return FetchesIssued;
}

LONG PrefetchModelFetchesPending(VOID)
{
    EnsureFetchCs();
    EnterCriticalSection(&FetchCs);

    LONG count = 0;

    for (PREFETCH_FETCH* fetch = FetchQueueHead; fetch; fetch = fetch->Next)
    {
        count++;
    }

    LeaveCriticalSection(&FetchCs);

    return count;
}

NTSTATUS BlorgHttpGetFileMdl(
    const UNICODE_STRING* Path,
    SIZE_T StartOffset,
    SIZE_T Length,
    PMDL TargetMdl,
    PBLORG_FILEREAD_COMPLETION CompletionRoutine,
    PVOID CallerContext)
{
    (void)Path;

    //
    // Issuance is documented PASSIVE-only: HttpBuildRequest touches paged
    // code. Enforcing it here is what would catch a fetch issued from a
    // completion, which is the mistake the pump work item exists to
    // prevent.
    //
    KmRequireIrqlAtMost(PASSIVE_LEVEL, "BlorgHttpGetFileMdl");

    if (InterlockedDecrement(&IssueFailuresPending) >= 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InterlockedExchange(&IssueFailuresPending, 0);
    InterlockedIncrement(&FetchesIssued);

    PREFETCH_FETCH* fetch = (PREFETCH_FETCH*)calloc(1, sizeof(PREFETCH_FETCH));

    fetch->Routine = CompletionRoutine;
    fetch->Context = CallerContext;
    fetch->TargetMdl = TargetMdl;
    fetch->Offset = StartOffset;
    fetch->Length = Length;

    EnsureFetchCs();
    EnterCriticalSection(&FetchCs);

    if (FetchQueueTail)
    {
        FetchQueueTail->Next = fetch;
    }
    else
    {
        FetchQueueHead = fetch;
    }

    FetchQueueTail = fetch;

    LeaveCriticalSection(&FetchCs);

    return STATUS_PENDING;
}

static PREFETCH_FETCH* TakeFetch(void)
{
    EnsureFetchCs();
    EnterCriticalSection(&FetchCs);

    PREFETCH_FETCH* fetch = FetchQueueHead;

    if (fetch)
    {
        FetchQueueHead = fetch->Next;

        if (!FetchQueueHead)
        {
            FetchQueueTail = NULL;
        }
    }

    LeaveCriticalSection(&FetchCs);

    return fetch;
}

//
// Completes one outstanding fetch, at DISPATCH_LEVEL, the way the WSK
// completion chain delivers them. Filling the slot buffer before the
// callback matters: the ring copies out of it inside the completion, so
// filling afterwards would let a bug that copies stale data pass.
//
static int PrefetchModelCompleteOne(PREFETCH_FETCH* Fetch, NTSTATUS Status)
{
    if (!Fetch)
    {
        return 0;
    }

    FILE_BUFFER buffer = { 0 };

    if (NT_SUCCESS(Status))
    {
        if (Fetch->TargetMdl && Fetch->TargetMdl->Base)
        {
            memset(Fetch->TargetMdl->Base, FetchFillByte, Fetch->Length);
        }

        buffer.BodyBufferSize = Fetch->Length;
    }

    unsigned char saved = KmGetIrql();
    KmSetIrql(DISPATCH_LEVEL);

    Fetch->Routine(Status, NT_SUCCESS(Status) ? &buffer : NULL, Fetch->Context);

    KmSetIrql(saved);

    free(Fetch);

    return 1;
}

int PrefetchModelCompleteNextFetch(NTSTATUS Status)
{
    return PrefetchModelCompleteOne(TakeFetch(), Status);
}

int PrefetchModelCompleteAllFetches(NTSTATUS Status)
{
    int completed = 0;

    for (;;)
    {
        PREFETCH_FETCH* fetch = TakeFetch();

        if (!fetch)
        {
            return completed;
        }

        completed += PrefetchModelCompleteOne(fetch, Status);
    }
}

//
// Completions can queue the pump work item, which issues more fetches,
// which complete and queue again. Settling means running that to a fixed
// point -- which is also how a test detects a pump that never stops.
//
int PrefetchModelSettle(NTSTATUS Status)
{
    int rounds = 0;

    for (;;)
    {
        int work = PrefetchModelCompleteAllFetches(Status);

        ShimDrainWorkItems();

        if (0 == work && 0 == PrefetchModelFetchesPending())
        {
            return rounds;
        }

        if (++rounds > 1000)
        {
            KmReportViolation(KmViolationLifetime,
                "prefetch pump did not settle after 1000 rounds -- runaway refill");
            return rounds;
        }
    }
}

VOID PrefetchModelReset(VOID)
{
    EnsureFetchCs();
    EnterCriticalSection(&FetchCs);

    while (FetchQueueHead)
    {
        PREFETCH_FETCH* next = FetchQueueHead->Next;
        free(FetchQueueHead);
        FetchQueueHead = next;
    }

    FetchQueueTail = NULL;

    LeaveCriticalSection(&FetchCs);

    FetchesIssued = 0;
    IssueFailuresPending = 0;
    FetchFillByte = 0xAB;
}
