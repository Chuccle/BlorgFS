#include "Driver.h"

//
// Follows the server's change feed (server-rs's /get_changes) and turns
// what it reports into invalidations, so the path and listing caches can
// trust an entry for minutes instead of seconds.
//
// One system thread per mounted volume holds one long-poll open at a time.
// The server answers when something changes, or after its hold time with an
// empty batch, and every answer names the generation to ask from next. The
// feed is live from the first reset the server answers with until the first
// poll that fails; while it is live, PathCache.c applies its long lifetime
// (BlorgPathCacheFollowFeed), and while it is not, the short TTL is all
// there is, exactly as before the feed existed.
//
// What makes trusting the long lifetime correct is the server's half of the
// contract: a client that has applied generation G and then reads anything
// sees every change up to G, because the server refuses to vouch for a load
// a generation overtook and marks it no-store (Client.c,
// HttpForbidsStoring), and the path cache never keeps those. The driver's
// half is the ticket protocol (BlorgPathCacheTakeTicket): an invalidation
// applied here refuses every insert whose read was issued before it, so a
// fetch racing a reported change cannot re-cache what it replaced.
//
// What a batch invalidates:
//
//  - Modified: the path's own entry, its listing if it is a directory, and
//    its parent's listing, which quotes its size and times
//    (BlorgPathCacheInvalidate).
//  - Created or removed: everything at and beneath the path, and its
//    parent's listing (BlorgPathCacheInvalidatePrefix). A removed directory
//    takes its subtree with it, and a created one may replace a cached
//    "not found" for anything beneath it.
//  - Past CHANGE_FEED_PRECISE_STRUCTURAL_MAX created or removed paths, the
//    whole cache instead: each prefix drop sweeps every bucket, and a
//    batch that large (an unpacked archive, a moved tree) is cheaper to
//    answer wholesale.
//
// Every path is also reported to the directory-change notification package,
// so an Explorer window on a directory refreshes when the server's copy
// changes, rather than never.
//
// Nothing here is on an I/O path. The thread is the only caller of
// BlorgHttpGetChanges, and everything it calls runs at PASSIVE_LEVEL.
//

//
// Most created or removed paths one batch invalidates one by one; past this,
// the batch flushes the whole cache (see above).
//
#define CHANGE_FEED_PRECISE_STRUCTURAL_MAX 64u

//
// How long to wait before polling again after a failed poll: from the
// minimum, doubling per consecutive failure up to the maximum. A server that
// is down or predates the feed (and so answers 404) costs one request per
// half minute.
//
#define CHANGE_FEED_BACKOFF_MIN_MS 1000u
#define CHANGE_FEED_BACKOFF_MAX_MS 30000u

#define CHANGE_FEED_MS_TO_100NS(ms) (C_CAST(LONG64, ms) * 10LL * 1000LL)

//
// The follower's state. Epoch and Since are the thread's alone. Status and
// Batch are written by the poll's completion and read by the thread after
// Done is signalled, which orders them.
//
typedef struct _CHANGE_FEED_STATE
{
    HANDLE         Thread;     // the follower thread, NULL when not started
    PDEVICE_OBJECT Volume;     // the volume whose notify list changes are reported into
    KEVENT         Stop;       // signalled to make the thread exit
    KEVENT         Done;       // signalled by the poll completion
    ULONG64        Epoch;      // server process being followed, 0 for none
    ULONG64        Since;      // generation applied so far
    PCHANGE_BATCH  Batch;      // the completed poll's batch, handed to the thread
    NTSTATUS       Status;     // the completed poll's status
    ULONG          BackoffMs;  // wait before the next poll; 0 after a success
} CHANGE_FEED_STATE;

static CHANGE_FEED_STATE ChangeFeed;

//
// The final component's offset within Path, in bytes: what
// FsRtlNotifyFullReportChange is told separates the directory from the
// name. Zero for the root itself, which has no parent to report into.
//
static USHORT ChangeFeedNameOffset(const UNICODE_STRING* Path)
{
    USHORT chars = Path->Length / sizeof(WCHAR);

    while (chars > 0 && L'\\' != Path->Buffer[chars - 1])
    {
        chars--;
    }

    return (chars * sizeof(WCHAR) == Path->Length) ? 0 : C_CAST(USHORT, chars * sizeof(WCHAR));
}

//
// Reports each change to the volume's directory-change notification
// package, which completes the NOTIFY_CHANGE_DIRECTORY IRPs watching its
// parent (DirCtrl.c). The kind of a created or removed path is not known
// here, so both name filters are reported, as a file and as a directory.
//
static VOID ChangeFeedNotify(const CHANGE_BATCH* Batch)
{
    if (!ChangeFeed.Volume)
    {
        return;
    }

    PBLORGFS_VDO_DEVICE_EXTENSION devExt = BlorgGetVolumeDeviceExtension(ChangeFeed.Volume);

    for (SIZE_T i = 0; i < Batch->Count; ++i)
    {
        const CHANGE_ENTRY* entry = &Batch->Entries[i];
        USHORT nameOffset = ChangeFeedNameOffset(&entry->Path);

        if (0 == nameOffset)
        {
            continue;
        }

        ULONG filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
        ULONG action = FILE_ACTION_ADDED;

        if (ChangeModified == entry->Kind)
        {
            filter = FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
            action = FILE_ACTION_MODIFIED;
        }
        else if (ChangeRemoved == entry->Kind)
        {
            action = FILE_ACTION_REMOVED;
        }

        FsRtlNotifyFullReportChange(
            devExt->NotifySync,
            &devExt->NotifyList,
            C_CAST(PSTRING, &entry->Path),
            nameOffset,
            NULL,
            NULL,
            filter,
            action,
            NULL);
    }
}

//
// Invalidates what one batch names; see the file header for what each kind
// drops and why.
//
static VOID ChangeFeedApply(const CHANGE_BATCH* Batch)
{
    SIZE_T structural = 0;

    for (SIZE_T i = 0; i < Batch->Count; ++i)
    {
        if (ChangeModified != Batch->Entries[i].Kind)
        {
            structural++;
        }
    }

    if (structural > CHANGE_FEED_PRECISE_STRUCTURAL_MAX)
    {
        BLORGFS_STAT_INC(ChangeFeedFlushes);
        BlorgPathCacheInvalidateAll();
    }
    else
    {
        for (SIZE_T i = 0; i < Batch->Count; ++i)
        {
            const CHANGE_ENTRY* entry = &Batch->Entries[i];

            if (ChangeModified == entry->Kind)
            {
                BlorgPathCacheInvalidate(&entry->Path);
            }
            else
            {
                BlorgPathCacheInvalidatePrefix(&entry->Path);
            }
        }
    }

    BLORGFS_STAT_ADD(ChangeFeedPaths, Batch->Count);

    ChangeFeedNotify(Batch);
}

//
// A batch from a server process other than the one being followed is acted
// on as a reset whatever it says: the generation it carries is not
// comparable with the one applied so far. The server sends a reset in that
// case anyway; this does not rely on it.
//
VOID BlorgChangeFeedReceive(NTSTATUS Status, _In_opt_ PCHANGE_BATCH Batch)
{
    if (!NT_SUCCESS(Status) || !Batch)
    {
        BLORGFS_STAT_INC(ChangeFeedFailures);
        BlorgFreeChangeBatch(Batch);

        ChangeFeed.Epoch = 0;
        ChangeFeed.Since = 0;
        BlorgPathCacheFollowFeed(FALSE);
        return;
    }

    if (Batch->Reset || Batch->Epoch != ChangeFeed.Epoch)
    {
        BLORGFS_STAT_INC(ChangeFeedResets);
        BlorgPathCacheFollowFeed(TRUE);
    }
    else
    {
        ChangeFeedApply(Batch);
    }

    ChangeFeed.Epoch = Batch->Epoch;
    ChangeFeed.Since = Batch->Generation;

    BlorgFreeChangeBatch(Batch);
}

//
// The poll's completion, at PASSIVE_LEVEL on a work item: hands the outcome
// to the thread waiting on Done.
//
static VOID ChangeFeedOnAnswer(NTSTATUS Status, PCHANGE_BATCH Batch, PVOID CallerContext)
{
    UNREFERENCED_PARAMETER(CallerContext);

    ChangeFeed.Status = Status;
    ChangeFeed.Batch = Batch;

    KeSetEvent(&ChangeFeed.Done, IO_NO_INCREMENT, FALSE);
}

//
// The follower: poll, act on the answer, repeat; after a failure, wait out
// the backoff first. A poll in flight is always waited for, never
// abandoned, because its completion writes into this file's state -- so a
// stop takes effect once the server answers, at most its hold time later.
// Waiting on Stop with the backoff as the timeout is what lets a stop cut a
// backoff short.
//
static VOID ChangeFeedThread(_In_ PVOID StartContext)
{
    UNREFERENCED_PARAMETER(StartContext);

    LARGE_INTEGER noWait;
    noWait.QuadPart = 0;

    while (STATUS_TIMEOUT == KeWaitForSingleObject(&ChangeFeed.Stop, Executive, KernelMode, FALSE, &noWait))
    {
        KeClearEvent(&ChangeFeed.Done);
        ChangeFeed.Batch = NULL;

        BLORGFS_STAT_INC(ChangeFeedPolls);

        NTSTATUS status = BlorgHttpGetChanges(ChangeFeed.Epoch, ChangeFeed.Since, ChangeFeedOnAnswer, NULL);

        if (STATUS_PENDING == status)
        {
            KeWaitForSingleObject(&ChangeFeed.Done, Executive, KernelMode, FALSE, NULL);
            status = ChangeFeed.Status;
        }

        BlorgChangeFeedReceive(status, ChangeFeed.Batch);
        ChangeFeed.Batch = NULL;

        if (NT_SUCCESS(status))
        {
            ChangeFeed.BackoffMs = 0;
            continue;
        }

        ULONG doubled = ChangeFeed.BackoffMs * 2;

        ChangeFeed.BackoffMs = (0 == ChangeFeed.BackoffMs)
            ? CHANGE_FEED_BACKOFF_MIN_MS
            : ((doubled > CHANGE_FEED_BACKOFF_MAX_MS) ? CHANGE_FEED_BACKOFF_MAX_MS : doubled);

        LARGE_INTEGER backoff;
        backoff.QuadPart = -CHANGE_FEED_MS_TO_100NS(ChangeFeed.BackoffMs);

        KeWaitForSingleObject(&ChangeFeed.Stop, Executive, KernelMode, FALSE, &backoff);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

//
// Starts at the end of volume creation, once the volume's notify package is
// initialised; see Driver.c. Not thread-safe against a concurrent Stop, and
// need not be: both follow the volume's lifecycle.
//
VOID BlorgChangeFeedStart(PDEVICE_OBJECT VolumeDeviceObject)
{
    if (!global.ChangeFeed || ChangeFeed.Thread)
    {
        return;
    }

    ChangeFeed.Volume = VolumeDeviceObject;
    ChangeFeed.Epoch = 0;
    ChangeFeed.Since = 0;
    ChangeFeed.BackoffMs = 0;

    KeInitializeEvent(&ChangeFeed.Stop, NotificationEvent, FALSE);
    KeInitializeEvent(&ChangeFeed.Done, NotificationEvent, FALSE);

    NTSTATUS result = PsCreateSystemThread(&ChangeFeed.Thread, DELETE | SYNCHRONIZE, NULL, NULL, NULL, ChangeFeedThread, NULL);

    if (!NT_SUCCESS(result))
    {
        BLORGFS_LOG("BlorgChangeFeedStart() - PsCreateSystemThread failed: 0x%X; caches stay on their TTL\n", result);
        ChangeFeed.Thread = NULL;
        ChangeFeed.Volume = NULL;
    }
}

//
// Signals the thread and reaps it, as StopWorkQueueThreads does
// (FspWorkQueue.c), then takes the feed down so nothing outlives the volume
// on the long lifetime. The feed is taken down even with no thread to reap,
// so a stop always leaves the caches on their short TTL.
//
VOID BlorgChangeFeedStop(VOID)
{
    if (ChangeFeed.Thread)
    {
        KeSetEvent(&ChangeFeed.Stop, IO_NO_INCREMENT, FALSE);

        PVOID thread;

        if (NT_SUCCESS(ObReferenceObjectByHandle(ChangeFeed.Thread, SYNCHRONIZE, *PsThreadType, KernelMode, &thread, NULL)))
        {
            KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(thread);
        }

        ZwClose(ChangeFeed.Thread);
        ChangeFeed.Thread = NULL;
    }

    ChangeFeed.Volume = NULL;

    BlorgPathCacheFollowFeed(FALSE);
}
