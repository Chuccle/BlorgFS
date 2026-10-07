#include "Driver.h"

//
// The disk cache's I/O: the cache file, reads served from it, and fetched
// blocks written into it. DiskCache.h says what it is for and what a block
// is stored under; DiskCacheIndex.c decides which slot holds what.
//
// Reads
// ---------------------------------------------------------------------
// A non-cached read is served from the file as far as the blocks it covers
// are held for the version the open's FCB names, each pinned for as long as
// the read is in flight. Held blocks are read with one IRP per run that
// sits in consecutive slots, each into a partial MDL of the read's own, so
// nothing is copied. The IRPs are this driver's own, sent straight to the
// cache file's device and finished by a completion routine, which needs no
// APC from the thread that issued them: the read path can run with APCs
// disabled (a paging read under a page fault), and a synchronous Zw call
// would wait on one.
//
// The blocks it does not hold are fetched, one ranged GET per run of them,
// into the client's buffer as any fetch is while the cache is live, offered
// to the cache, and copied into the read's pages. The clock leaves what it
// keeps scattered through a file larger than the cache, so serving a read
// only when every block was held served almost nothing there while the
// blocks went on being written; simulated on a file 1.5 times the cache,
// re-read, it served 0% of reads against 40% of blocks held. A read that
// fetches is admitted to the fair-share scheduler (Read.c) before it
// starts, as one fetch of its whole length, and its caller says how many
// runs it may fetch: each is a request of its own, and every request
// counts against the fair share's fetch limit. A cache file read failing,
// or a run fetched at another version than the held blocks, fails the
// read back to its caller to be fetched whole; a fetch failing fails it.
//
// Fills
// ---------------------------------------------------------------------
// A fetch completion offers what it received. Each whole block in it that
// the index admits is copied into a block of nonpaged pool there and then,
// before the reader's IRP is completed and its pages go back to whoever
// owns them, and queued. A file system can only be called at <= APC_LEVEL,
// and a fetch completes at DISPATCH_LEVEL, so the writes are issued from a
// work item. Copies waiting for their write are bounded by
// DISK_CACHE_FILL_BACKLOG; past it, fetched blocks are simply not kept.
//
// Versions
// ---------------------------------------------------------------------
// A fill is keyed by the version the open's FCB names, and admitted only
// if the response's entity tag names that same version: a fetch that read
// a file the server has since replaced carries the new tag and is not kept
// under the old key. A read is keyed the same way, so once the FCB is
// refreshed to a new version (Create.c) its old blocks stop matching and
// age out. A read in flight across such a refresh keeps the key it pinned
// its blocks under, and a run it fetches must be of that version too.
//
// The file
// ---------------------------------------------------------------------
// Opened for non-buffered I/O, so the cache file does not also occupy the
// Windows cache it exists to stand behind, and sized once to hold every
// slot. Its DACL admits only SYSTEM and Administrators: the bytes in it are
// files read through this volume, and once the volume carries its own ACLs
// a copy readable by anyone would leak them. Slots are first handed out in
// file order, so the first fills extend what NTFS has written in order
// rather than making it zero a gap ahead of each.
//
// The file and its directory are opened without following reparse points,
// and refused unless SYSTEM or Administrators own them. ProgramData lets any
// user create a directory, so one could be waiting at the configured path
// with a link in it or a file whose DACL the user chose; this driver would
// then be writing as SYSTEM wherever the user pointed it. A store that fails
// the check is refused, not repaired, since repairing races the user. A
// store on this driver's own volume is refused too: its I/O would come back
// into this driver.
//
// Nothing in the file is trusted across loads. The index starts empty, so
// a block is served only after this load wrote it, and the file is opened
// without sharing, so nothing else can change it in between.
//
// Losing the file
// ---------------------------------------------------------------------
// Without FILE_SHARE_DELETE nobody can open the file to delete or rename
// it while the cache holds it, and NTFS will not delete or rename the
// directory around an open file. What can still take it away is its volume
// going: a removable disk pulled, or a forced dismount. Every I/O on it
// then fails, so the first failed read or write turns the cache off for the
// rest of the load (DiskCacheLost): a read it failed is fetched instead, as
// any failed read is, and nothing further is sent to a file that is gone.
// A disk cache is disposable, so a disk error that leaves the file in place
// is treated the same way rather than told apart from it.
//
// Teardown
// ---------------------------------------------------------------------
// Busy counts the cache's own work in flight -- each read, each fill from
// admission to the end of its write, and the fill worker -- plus one for
// the cache being live. Cleanup clears Live, which stops new work, drops
// that one, and waits for the count to reach zero before closing the file.
// Work already counted may count more (a fill counts the worker it starts),
// which a rundown reference could not do once teardown began.
//

#define DISK_CACHE_TAG 'cDPB'

//
// Bytes of fetched blocks copied but not yet written, past which a fetched
// block is not kept. A slow or busy disk then costs hit rate, not memory.
//
#define DISK_CACHE_FILL_BACKLOG (32ull * 1024 * 1024)

//
// Most blocks one read may cover: 4 MB, far beyond any paging read Cc or
// MM issues. A larger read is fetched.
//
#define DISK_CACHE_MAX_READ_BLOCKS 64

//
// One block on its way into the cache: a copy of what a fetch received,
// and the slot reserved for it.
//
typedef struct _DISK_CACHE_FILL
{
    LIST_ENTRY Link; // In DiskCache.Fills until the worker issues its write
    PVOID Buffer;    // DISK_CACHE_BLOCK_SIZE bytes, NonPagedPoolNx, page aligned
    PMDL Mdl;        // Describes Buffer
    ULONG Slot;      // Reserved and pinned for this block
    ULONG Reserved;  // explicit tail padding
} DISK_CACHE_FILL, * PDISK_CACHE_FILL;

CHECK_PADDING_BETWEEN(DISK_CACHE_FILL, Link, Buffer);
CHECK_PADDING_BETWEEN(DISK_CACHE_FILL, Buffer, Mdl);
CHECK_PADDING_BETWEEN(DISK_CACHE_FILL, Mdl, Slot);
CHECK_PADDING_BETWEEN(DISK_CACHE_FILL, Slot, Reserved);
CHECK_PADDING_END(DISK_CACHE_FILL, Reserved);

struct _DISK_CACHE_READ;

//
// Why a read the cache began must be fetched whole after all: a part read
// from the cache file failed or came back short, or a fetched run is of
// another version than the held blocks beside it, which the read would
// otherwise return mixed.
//
#define DISK_CACHE_READ_FAILED 0x1
#define DISK_CACHE_READ_STALE  0x2

//
// One run of a read's blocks the cache did not hold, being fetched.
//
typedef struct _DISK_CACHE_READ_FETCH
{
    struct _DISK_CACHE_READ* Read; // The read this run belongs to
    PUCHAR Target;                 // Where in the read's mapped buffer the body is copied
    ULONG64 Offset;                // File offset of the run
    ULONG Length;                  // Bytes asked of the server
    ULONG Reserved;                // explicit tail padding
} DISK_CACHE_READ_FETCH, * PDISK_CACHE_READ_FETCH;

CHECK_PADDING_BETWEEN(DISK_CACHE_READ_FETCH, Read, Target);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ_FETCH, Target, Offset);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ_FETCH, Offset, Length);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ_FETCH, Length, Reserved);
CHECK_PADDING_END(DISK_CACHE_READ_FETCH, Reserved);

//
// One read being served from the cache, shared by the IRPs and fetches it
// was split into.
//
typedef struct _DISK_CACHE_READ
{
    PIRP Irp;                               // The read being served
    PDISK_CACHE_READ_COMPLETION Completion; // Called once, when the last part is done
    DISK_CACHE_KEY Key;                     // The file and version the held blocks were pinned under; every fetched run must be of it
    volatile LONG Outstanding;              // Parts in flight, plus one the issuer holds
    volatile LONG Status;                   // First failure, or STATUS_SUCCESS
    volatile LONG64 Bytes;                  // Bytes the parts reported read
    ULONG Span;                             // Bytes asked of the cache file and the server in all
    ULONG DiskSpan;                         // Of those, bytes before end of file asked of the cache file
    ULONG Valid;                            // The caller's Valid, handed back
    ULONG SlotCount;
    ULONG FetchCount;
    volatile LONG Refetch;                  // DISK_CACHE_READ_* reasons it must be fetched whole instead
    DISK_CACHE_READ_FETCH Fetches[DISK_CACHE_MAX_READ_FETCHES];
    ULONG Slots[DISK_CACHE_MAX_READ_BLOCKS]; // Pinned, one per block in file order, or DISK_CACHE_NO_SLOT where fetched
} DISK_CACHE_READ, * PDISK_CACHE_READ;

CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Irp, Completion);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Completion, Key);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Key, Outstanding);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Outstanding, Status);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Status, Bytes);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Bytes, Span);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Span, DiskSpan);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, DiskSpan, Valid);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Valid, SlotCount);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, SlotCount, FetchCount);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, FetchCount, Refetch);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Refetch, Fetches);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Fetches, Slots);
CHECK_PADDING_END(DISK_CACHE_READ, Slots);

static struct
{
    DISK_CACHE_INDEX Index;
    HANDLE Handle;               // Kernel handle to the cache file
    PFILE_OBJECT FileObject;     // Referenced from Handle; every IRP names it
    PDEVICE_OBJECT Device;       // Top of the cache file's volume stack
    KSPIN_LOCK FillLock;
    LIST_ENTRY Fills;            // Copied blocks waiting for the worker, under FillLock
    BOOLEAN FillWorkerQueued;    // The worker is queued or running, under FillLock
    volatile LONG64 FillBytes;   // Bytes of fills copied and not yet written
    volatile LONG Live;          // Nonzero while new reads and fills may start
    volatile LONG Busy;          // Work in flight, plus one while Live; see the file header
    KEVENT Idle;                 // Set when Busy reaches zero
} DiskCache;

static IO_WORKITEM_ROUTINE DiskCacheFillWorker;
static IO_COMPLETION_ROUTINE DiskCacheReadDone;
static IO_COMPLETION_ROUTINE DiskCacheFillDone;

// Counts one more piece of work, unless the cache is not live.
static BOOLEAN DiskCacheEnter(VOID)
{
    if (!ReadNoFence(&DiskCache.Live))
    {
        return FALSE;
    }

    InterlockedIncrement(&DiskCache.Busy);

    if (!ReadNoFence(&DiskCache.Live))
    {
        if (0 == InterlockedDecrement(&DiskCache.Busy))
        {
            KeSetEvent(&DiskCache.Idle, IO_NO_INCREMENT, FALSE);
        }

        return FALSE;
    }

    return TRUE;
}

static VOID DiskCacheLeave(VOID)
{
    if (0 == InterlockedDecrement(&DiskCache.Busy))
    {
        KeSetEvent(&DiskCache.Idle, IO_NO_INCREMENT, FALSE);
    }
}

//
// Turns the cache off after an I/O on its file failed; see "Losing the
// file". Work already counted finishes, and the file is closed at unload
// as usual. <= DISPATCH_LEVEL.
//
static VOID DiskCacheLost(VOID)
{
    InterlockedExchange(&DiskCache.Live, 0);
}

//
// The key a fetch for Node is admitted under, as the last read noted it,
// with Block left zero, or FALSE if BlorgDiskCacheNoteFile has not run for
// it yet. File[0] is published last
// and is never zero once set, so seeing it means File[1] is there too.
//
static BOOLEAN DiskCacheKeyOf(PNON_PAGED_NODE Node, PDISK_CACHE_KEY Key)
{
    Key->File[0] = C_CAST(ULONG64, ReadAcquire64(C_CAST(volatile LONG64*, &Node->DiskCacheFile[0])));

    if (0 == Key->File[0])
    {
        return FALSE;
    }

    Key->File[1] = Node->DiskCacheFile[1];
    Key->Size = Node->DiskCacheSize;
    Key->ModifiedTime = Node->DiskCacheModifiedTime;
    Key->Block = 0;

    return TRUE;
}

//
// Sends one read or write of Length bytes at DiskOffset in the cache file,
// described by Mdl, to the cache file's device. Routine runs with Context
// when it is done and owns the IRP from then: it frees it and returns
// STATUS_MORE_PROCESSING_REQUIRED. A failure means nothing was sent.
// <= APC_LEVEL.
//
static NTSTATUS DiskCacheIssue(UCHAR MajorFunction, PMDL Mdl, ULONG Length, ULONG64 DiskOffset, PIO_COMPLETION_ROUTINE Routine, PVOID Context)
{
    PIRP irp = IoAllocateIrp(DiskCache.Device->StackSize, FALSE);

    if (!irp)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->MdlAddress = Mdl;
    irp->UserBuffer = MmGetMdlVirtualAddress(Mdl);
    irp->Flags = IRP_NOCACHE | ((IRP_MJ_READ == MajorFunction) ? IRP_READ_OPERATION : IRP_WRITE_OPERATION);
    irp->RequestorMode = KernelMode;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->Tail.Overlay.OriginalFileObject = DiskCache.FileObject;

    PIO_STACK_LOCATION irpSp = IoGetNextIrpStackLocation(irp);
    irpSp->MajorFunction = MajorFunction;
    irpSp->FileObject = DiskCache.FileObject;

    if (IRP_MJ_READ == MajorFunction)
    {
        irpSp->Parameters.Read.Length = Length;
        irpSp->Parameters.Read.ByteOffset.QuadPart = C_CAST(LONGLONG, DiskOffset);
    }
    else
    {
        irpSp->Parameters.Write.Length = Length;
        irpSp->Parameters.Write.ByteOffset.QuadPart = C_CAST(LONGLONG, DiskOffset);
    }

    IoSetCompletionRoutine(irp, Routine, Context, TRUE, TRUE, TRUE);

    IoCallDriver(DiskCache.Device, irp);

    return STATUS_SUCCESS;
}

//
// Drops one hold on Read; the last unpins its blocks, marking them served
// if the read succeeded, and hands the IRP back through its completion,
// saying whether it is to be fetched whole. A read that came back short
// failed. A fetch that failed fails the read outright: fetching it whole
// would ask the same server again. <= DISPATCH_LEVEL.
//
static VOID DiskCacheReadSettle(PDISK_CACHE_READ Read)
{
    if (0 != InterlockedDecrement(&Read->Outstanding))
    {
        return;
    }

    NTSTATUS status = Read->Status;
    LONG refetch = Read->Refetch;

    if (NT_SUCCESS(status) && 0 == refetch && Read->Bytes != Read->Span)
    {
        refetch |= DISK_CACHE_READ_FAILED;
    }

    if (0 != refetch)
    {
        status = STATUS_UNEXPECTED_IO_ERROR;
    }

    for (ULONG i = 0; i < Read->SlotCount; ++i)
    {
        if (DISK_CACHE_NO_SLOT != Read->Slots[i])
        {
            BlorgDiskCacheIndexUnpin(&DiskCache.Index, Read->Slots[i], NT_SUCCESS(status));
        }
    }

    if (FlagOn(refetch, DISK_CACHE_READ_FAILED))
    {
        BLORGFS_STAT_INC(DiskCacheReadFailures);
    }

    if (NT_SUCCESS(status))
    {
        if (0 == Read->FetchCount)
        {
            BLORGFS_STAT_INC(DiskCacheHits);
        }
        else
        {
            BLORGFS_STAT_INC(DiskCachePartialHits);
        }

        BLORGFS_STAT_ADD(DiskCacheHitBytes, Read->DiskSpan);
    }

    PIRP irp = Read->Irp;
    PDISK_CACHE_READ_COMPLETION completion = Read->Completion;
    const ULONG valid = Read->Valid;
    const ULONG fetches = Read->FetchCount;

    ExFreePool(Read);
    DiskCacheLeave();

    completion(irp, status, valid, fetches, 0 != refetch);
}

static NTSTATUS DiskCacheReadDone(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PDISK_CACHE_READ read = Context;

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        InterlockedAdd64(&read->Bytes, C_CAST(LONG64, Irp->IoStatus.Information));
    }
    else
    {
        InterlockedOr(&read->Refetch, DISK_CACHE_READ_FAILED);
        DiskCacheLost();
    }

    IoFreeMdl(Irp->MdlAddress);
    IoFreeIrp(Irp);

    DiskCacheReadSettle(read);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Whether what FileBuffer received belongs to the version Key names, as
// its entity tag says.
//
static BOOLEAN DiskCacheIsVersion(const DISK_CACHE_KEY* Key, const FILE_BUFFER* FileBuffer)
{
    return FileBuffer->HasVersion && FileBuffer->VersionSize == Key->Size && FileBuffer->VersionTime == Key->ModifiedTime;
}

static VOID DiskCacheFill(const DISK_CACHE_KEY* Key, const UCHAR* Source, ULONG64 Offset, ULONG Length);

//
// Completion for one fetched run of a partly held read, on the WSK
// completion chain at <= DISPATCH_LEVEL. The body arrived in the client's
// buffer, for the reason BlorgDiskCacheLive gives; it is copied into the
// read's pages and offered to the cache before the read is settled. A body
// of another version than the one the read's held blocks were pinned for
// is not copied, and the read is fetched whole instead. That is checked
// against the read's own key, not the node's: the node's moves on when its
// FCB is refreshed, and the check must not depend on whether the cache is
// still live, since the held blocks were read either way.
//
static VOID DiskCacheReadFetched(NTSTATUS Status, PFILE_BUFFER FileBuffer, PVOID CallerContext)
{
    PDISK_CACHE_READ_FETCH fetch = CallerContext;
    PDISK_CACHE_READ read = fetch->Read;

    if (NT_SUCCESS(Status))
    {
        const ULONG received = C_CAST(ULONG, min(FileBuffer->BodyBufferSize, C_CAST(SIZE_T, fetch->Length)));

        BLORGFS_STAT_INC(FetchesCompleted);
        BLORGFS_STAT_ADD(FetchBytes, received);

        if (DiskCacheIsVersion(&read->Key, FileBuffer))
        {
            RtlCopyMemory(fetch->Target, FileBuffer->BodyBuffer, received);
            InterlockedAdd64(&read->Bytes, C_CAST(LONG64, received));
            DiskCacheFill(&read->Key, C_CAST(const UCHAR*, FileBuffer->BodyBuffer), fetch->Offset, received);
        }
        else
        {
            BLORGFS_STAT_INC(DiskCacheStale);
            InterlockedOr(&read->Refetch, DISK_CACHE_READ_STALE);
        }

        BlorgFreeHttpFile(FileBuffer);
    }
    else
    {
        BLORGFS_STAT_INC(FetchesFailed);
        InterlockedCompareExchange(&read->Status, Status, STATUS_SUCCESS);
    }

    DiskCacheReadSettle(read);
}

//
// Gives back one block of the fill backlog. A fill charges its block
// before anything else, and the charge and the test against
// DISK_CACHE_FILL_BACKLOG are one interlocked add, since completions on
// several processors could otherwise all pass the test before any added.
//
static VOID DiskCacheUnchargeFill(VOID)
{
    InterlockedAdd64(&DiskCache.FillBytes, -C_CAST(LONG64, DISK_CACHE_BLOCK_SIZE));
}

//
// Ends a fill: its slot becomes servable if it was written and is freed if
// not, and the copy goes. <= DISPATCH_LEVEL.
//
static VOID DiskCacheFillSettle(PDISK_CACHE_FILL Fill, BOOLEAN Written)
{
    BlorgDiskCacheIndexCommit(&DiskCache.Index, Fill->Slot, Written);

    if (Written)
    {
        BLORGFS_STAT_INC(DiskCacheFills);
    }
    else
    {
        BLORGFS_STAT_INC(DiskCacheFillFailures);
    }

    DiskCacheUnchargeFill();

    IoFreeMdl(Fill->Mdl);
    ExFreePool(Fill->Buffer);
    ExFreePool(Fill);

    DiskCacheLeave();
}

static NTSTATUS DiskCacheFillDone(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    const BOOLEAN written = NT_SUCCESS(Irp->IoStatus.Status) && DISK_CACHE_BLOCK_SIZE == Irp->IoStatus.Information;

    if (!written)
    {
        DiskCacheLost();
    }

    IoFreeIrp(Irp);

    DiskCacheFillSettle(Context, written);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

//
// Issues the write of every queued fill, until the queue is empty, or
// gives each up once the cache is off. Runs from a work item allocated by
// whoever queued it, which it frees first, and counted in Busy from then
// until it returns.
//
static VOID DiskCacheFillWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    IoFreeWorkItem(Context);

    for (;;)
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&DiskCache.FillLock, &oldIrql);

        if (IsListEmpty(&DiskCache.Fills))
        {
            DiskCache.FillWorkerQueued = FALSE;
            KeReleaseSpinLock(&DiskCache.FillLock, oldIrql);
            break;
        }

        PDISK_CACHE_FILL fill = CONTAINING_RECORD(RemoveHeadList(&DiskCache.Fills), DISK_CACHE_FILL, Link);

        KeReleaseSpinLock(&DiskCache.FillLock, oldIrql);

        const ULONG64 diskOffset = C_CAST(ULONG64, fill->Slot) << DISK_CACHE_BLOCK_SHIFT;

        if (!ReadNoFence(&DiskCache.Live) ||
            !NT_SUCCESS(DiskCacheIssue(IRP_MJ_WRITE, fill->Mdl, DISK_CACHE_BLOCK_SIZE, diskOffset, DiskCacheFillDone, fill)))
        {
            DiskCacheFillSettle(fill, FALSE);
        }
    }

    DiskCacheLeave();
}

//
// Queues Fill for writing and starts the worker if it is not already
// going. If no work item can be had for it, everything queued is given up
// rather than left for a worker that may never come. <= DISPATCH_LEVEL.
//
static VOID DiskCacheQueueFill(PDISK_CACHE_FILL Fill)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&DiskCache.FillLock, &oldIrql);

    InsertTailList(&DiskCache.Fills, &Fill->Link);

    const BOOLEAN start = !DiskCache.FillWorkerQueued;
    DiskCache.FillWorkerQueued = TRUE;

    KeReleaseSpinLock(&DiskCache.FillLock, oldIrql);

    if (!start)
    {
        return;
    }

    PIO_WORKITEM workItem = IoAllocateWorkItem(global.FileSystemDeviceObject);

    if (workItem)
    {
        InterlockedIncrement(&DiskCache.Busy);
        IoQueueWorkItem(workItem, DiskCacheFillWorker, DelayedWorkQueue, workItem);
        return;
    }

    LIST_ENTRY abandoned;
    InitializeListHead(&abandoned);

    KeAcquireSpinLock(&DiskCache.FillLock, &oldIrql);

    while (!IsListEmpty(&DiskCache.Fills))
    {
        InsertTailList(&abandoned, RemoveHeadList(&DiskCache.Fills));
    }

    DiskCache.FillWorkerQueued = FALSE;

    KeReleaseSpinLock(&DiskCache.FillLock, oldIrql);

    while (!IsListEmpty(&abandoned))
    {
        DiskCacheFillSettle(CONTAINING_RECORD(RemoveHeadList(&abandoned), DISK_CACHE_FILL, Link), FALSE);
    }
}

//
// Whether SYSTEM or Administrators own the file Handle is open on.
//
static BOOLEAN DiskCacheOwnedByAdmins(HANDLE Handle)
{
    ULONG descriptorBuffer[64];
    ULONG needed = 0;
    PSID owner = NULL;
    BOOLEAN defaulted = FALSE;

    if (!NT_SUCCESS(ZwQuerySecurityObject(Handle, OWNER_SECURITY_INFORMATION, descriptorBuffer, sizeof(descriptorBuffer), &needed)))
    {
        return FALSE;
    }

    if (!NT_SUCCESS(RtlGetOwnerSecurityDescriptor(descriptorBuffer, &owner, &defaulted)) || !owner)
    {
        return FALSE;
    }

    return RtlEqualSid(owner, SeExports->SeLocalSystemSid) || RtlEqualSid(owner, SeExports->SeAliasAdminsSid);
}

//
// Opens or creates Path (a directory when Options says so) with a DACL that
// admits only SYSTEM and Administrators, for a file that does not exist
// yet. An existing one keeps its own, so it is refused unless SYSTEM or
// Administrators own it; a reparse point anywhere along Path is refused.
//
static NTSTATUS DiskCacheOpen(const UNICODE_STRING* Path, ACCESS_MASK Access, ULONG Options, PHANDLE Handle)
{
    ULONG aclBuffer[(sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE)) / sizeof(ULONG)];
    PACL acl = C_CAST(PACL, aclBuffer);
    SECURITY_DESCRIPTOR descriptor;

    NTSTATUS status = RtlCreateAcl(acl, sizeof(aclBuffer), ACL_REVISION);

    if (NT_SUCCESS(status))
    {
        status = RtlAddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, SeExports->SeLocalSystemSid);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlAddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, SeExports->SeAliasAdminsSid);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlCreateSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
    }

    if (NT_SUCCESS(status))
    {
        status = RtlSetDaclSecurityDescriptor(&descriptor, TRUE, acl, FALSE);
    }

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    descriptor.Control |= SE_DACL_PROTECTED;

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, C_CAST(PUNICODE_STRING, Path), OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, NULL, &descriptor);

    IO_STATUS_BLOCK ioStatus;

    status = ZwCreateFile(
        Handle,
        Access | READ_CONTROL | SYNCHRONIZE,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        0,
        FILE_OPEN_IF,
        Options | FILE_OPEN_REPARSE_POINT,
        NULL,
        0);

    if (NT_SUCCESS(status) && !DiskCacheOwnedByAdmins(*Handle))
    {
        ZwClose(*Handle);
        *Handle = NULL;
        status = STATUS_ACCESS_DENIED;
    }

    return status;
}

//
// Creates the directory Path is in, if it is missing. Only the last level:
// the default lives under ProgramData, which exists.
//
static NTSTATUS DiskCacheCreateParent(const UNICODE_STRING* Path)
{
    UNICODE_STRING parent = *Path;

    while (parent.Length > 0 && L'\\' != parent.Buffer[(parent.Length / sizeof(WCHAR)) - 1])
    {
        parent.Length -= sizeof(WCHAR);
    }

    if (parent.Length <= sizeof(WCHAR))
    {
        return STATUS_OBJECT_PATH_INVALID;
    }

    parent.Length -= sizeof(WCHAR);
    parent.MaximumLength = parent.Length;

    HANDLE directory;
    NTSTATUS status = DiskCacheOpen(&parent, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, &directory);

    if (NT_SUCCESS(status))
    {
        ZwClose(directory);
    }

    return status;
}

NTSTATUS BlorgDiskCacheInitialize(const UNICODE_STRING* Path, ULONG SizeMb)
{
    KeInitializeSpinLock(&DiskCache.FillLock);
    InitializeListHead(&DiskCache.Fills);
    KeInitializeEvent(&DiskCache.Idle, NotificationEvent, FALSE);

    if (0 == SizeMb)
    {
        return STATUS_SUCCESS;
    }

    if (SizeMb > DISK_CACHE_MAX_MB)
    {
        SizeMb = DISK_CACHE_MAX_MB;
    }

    const ULONG slotCount = SizeMb << (20 - DISK_CACHE_BLOCK_SHIFT);

    NTSTATUS status = BlorgDiskCacheIndexInitialize(&DiskCache.Index, slotCount);

    if (NT_SUCCESS(status))
    {
        status = DiskCacheCreateParent(Path);
    }

    if (NT_SUCCESS(status))
    {
        status = DiskCacheOpen(
            Path,
            FILE_READ_DATA | FILE_WRITE_DATA,
            FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING | FILE_RANDOM_ACCESS,
            &DiskCache.Handle);
    }

    if (!NT_SUCCESS(status))
    {
        DiskCache.Handle = NULL;
        BlorgDiskCacheIndexCleanup(&DiskCache.Index);
        return status;
    }

    FILE_END_OF_FILE_INFORMATION endOfFile;
    endOfFile.EndOfFile.QuadPart = C_CAST(LONGLONG, C_CAST(ULONG64, slotCount) << DISK_CACHE_BLOCK_SHIFT);

    IO_STATUS_BLOCK ioStatus;
    status = ZwSetInformationFile(DiskCache.Handle, &ioStatus, &endOfFile, sizeof(endOfFile), FileEndOfFileInformation);

    if (STATUS_PENDING == status)
    {
        status = ZwWaitForSingleObject(DiskCache.Handle, FALSE, NULL);

        if (NT_SUCCESS(status))
        {
            status = ioStatus.Status;
        }
    }

    if (NT_SUCCESS(status))
    {
        status = ObReferenceObjectByHandle(DiskCache.Handle, FILE_READ_DATA | FILE_WRITE_DATA, *IoFileObjectType, KernelMode, C_CAST(PVOID*, &DiskCache.FileObject), NULL);
    }

    if (NT_SUCCESS(status) && DiskCache.FileObject->DeviceObject == global.DiskDeviceObject)
    {
        ObDereferenceObject(DiskCache.FileObject);
        DiskCache.FileObject = NULL;
        status = STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(status))
    {
        ZwClose(DiskCache.Handle);
        DiskCache.Handle = NULL;
        BlorgDiskCacheIndexCleanup(&DiskCache.Index);
        return status;
    }

    DiskCache.Device = IoGetRelatedDeviceObject(DiskCache.FileObject);
    DiskCache.Busy = 1;
    WriteRelease(&DiskCache.Live, 1);

    return STATUS_SUCCESS;
}

VOID BlorgDiskCacheCleanup(VOID)
{
    if (!DiskCache.Handle)
    {
        return;
    }

    WriteRelease(&DiskCache.Live, 0);
    DiskCacheLeave();

    KeWaitForSingleObject(&DiskCache.Idle, Executive, KernelMode, FALSE, NULL);

    ObDereferenceObject(DiskCache.FileObject);
    ZwClose(DiskCache.Handle);
    BlorgDiskCacheIndexCleanup(&DiskCache.Index);

    DiskCache.FileObject = NULL;
    DiskCache.Handle = NULL;
}

//
// Two independent 64-bit hashes of the case-folded path: FNV-1a, and a
// rotate-multiply over the same characters. Computed once per node; File[0]
// is forced nonzero, since zero means not computed yet.
//
VOID BlorgDiskCacheNoteFile(PNON_PAGED_NODE Node, const UNICODE_STRING* Path, ULONG64 Size, ULONG64 ModifiedTime, PDISK_CACHE_KEY Key)
{
    RtlZeroMemory(Key, sizeof(*Key));

    if (!ReadNoFence(&DiskCache.Live))
    {
        return;
    }

    LONG64 file = ReadAcquire64(C_CAST(volatile LONG64*, &Node->DiskCacheFile[0]));

    if (0 == file)
    {
        ULONG64 first = 0xCBF29CE484222325ull;
        ULONG64 second = 0x9E3779B97F4A7C15ull;

        for (USHORT i = 0; i < Path->Length / sizeof(WCHAR); ++i)
        {
            const WCHAR c = RtlUpcaseUnicodeChar(Path->Buffer[i]);

            first = (first ^ c) * 0x100000001B3ull;
            second = (second + c) * 0xBF58476D1CE4E5B9ull;
            second = (second << 23) | (second >> 41);
        }

        file = C_CAST(LONG64, first | 1);
        Node->DiskCacheFile[1] = second;
        WriteRelease64(C_CAST(volatile LONG64*, &Node->DiskCacheFile[0]), file);
    }

    Node->DiskCacheSize = Size;
    Node->DiskCacheModifiedTime = ModifiedTime;

    Key->File[0] = C_CAST(ULONG64, file);
    Key->File[1] = Node->DiskCacheFile[1];
    Key->Size = Size;
    Key->ModifiedTime = ModifiedTime;
}

//
// What is read from the file is the valid bytes rounded up to a page, whose
// tail past end of file the fill wrote as zeros; what is fetched stops at
// the valid bytes, as a whole fetch would. A held part is read into a
// partial MDL of the read's own; a fetched one lands in the client's buffer
// and is copied into the read's mapped pages on completion. If a part
// cannot be started once some already are, the read is failed through
// Completion rather than handed back, since those are writing into its
// buffer.
//
static NTSTATUS DiskCacheReadPart(PDISK_CACHE_READ Read, const UNICODE_STRING* Path, ULONG64 Offset, ULONG64 At, ULONG Length, ULONG Slot)
{
    PMDL source = Read->Irp->MdlAddress;
    NTSTATUS status;

    if (DISK_CACHE_NO_SLOT == Slot)
    {
        PUCHAR mapped = MmGetSystemAddressForMdlSafe(source, NormalPagePriority | MdlMappingNoExecute);

        if (!mapped)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        PDISK_CACHE_READ_FETCH fetch = &Read->Fetches[Read->FetchCount++];

        fetch->Read = Read;
        fetch->Target = mapped + (At - Offset);
        fetch->Offset = At;
        fetch->Length = Length;

        InterlockedIncrement(&Read->Outstanding);
        BLORGFS_STAT_INC(FetchesIssued);

        status = BlorgHttpGetFile(Path, C_CAST(SIZE_T, At), Length, DiskCacheReadFetched, fetch);

        if (STATUS_PENDING == status)
        {
            return STATUS_SUCCESS;
        }

        BLORGFS_STAT_INC(FetchesFailed);
        InterlockedDecrement(&Read->Outstanding);
        return status;
    }

    PUCHAR va = C_CAST(PUCHAR, MmGetMdlVirtualAddress(source)) + (At - Offset);
    PMDL mdl = IoAllocateMdl(va, Length, FALSE, FALSE, NULL);

    if (!mdl)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoBuildPartialMdl(source, mdl, va, Length);
    InterlockedIncrement(&Read->Outstanding);

    const ULONG64 diskOffset = (C_CAST(ULONG64, Slot) << DISK_CACHE_BLOCK_SHIFT) + (At & (DISK_CACHE_BLOCK_SIZE - 1));

    status = DiskCacheIssue(IRP_MJ_READ, mdl, Length, diskOffset, DiskCacheReadDone, Read);

    if (!NT_SUCCESS(status))
    {
        InterlockedDecrement(&Read->Outstanding);
        IoFreeMdl(mdl);
    }

    return status;
}

//
// Runs of blocks a read pinned by BlorgDiskCacheIndexPinHeld would have to
// fetch, at most DISK_CACHE_MAX_READ_FETCHES.
//
static ULONG DiskCacheRunsToFetch(const ULONG* Slots, ULONG Count)
{
    ULONG runs = 0;

    for (ULONG i = 0; i < Count && runs < DISK_CACHE_MAX_READ_FETCHES; ++i)
    {
        if (DISK_CACHE_NO_SLOT == Slots[i] && (0 == i || DISK_CACHE_NO_SLOT != Slots[i - 1]))
        {
            runs++;
        }
    }

    return runs;
}

BOOLEAN BlorgDiskCacheRead(PIRP Irp, const DISK_CACHE_KEY* Key, const UNICODE_STRING* Path, ULONG64 Offset, ULONG Length, ULONG Valid, PULONG Fetches, PDISK_CACHE_READ_COMPLETION Completion)
{
    const ULONG allowed = *Fetches;

    *Fetches = 0;

    if (!ReadNoFence(&DiskCache.Live) || !Irp->MdlAddress || 0 == Valid || Valid > Length)
    {
        return FALSE;
    }

    if (0 != ((Offset | Length) & (PAGE_SIZE - 1)) || 0 != MmGetMdlByteOffset(Irp->MdlAddress))
    {
        return FALSE;
    }

    DISK_CACHE_KEY key = *Key;

    const ULONG span = (Valid + (PAGE_SIZE - 1)) & ~C_CAST(ULONG, PAGE_SIZE - 1);
    const ULONG64 first = Offset >> DISK_CACHE_BLOCK_SHIFT;
    const ULONG64 last = (Offset + span - 1) >> DISK_CACHE_BLOCK_SHIFT;
    const ULONG blocks = C_CAST(ULONG, last - first + 1);

    if (blocks > DISK_CACHE_MAX_READ_BLOCKS || !DiskCacheEnter())
    {
        return FALSE;
    }

    PDISK_CACHE_READ read = ExAllocatePoolZero(NonPagedPoolNx, sizeof(DISK_CACHE_READ), DISK_CACHE_TAG);

    if (!read)
    {
        DiskCacheLeave();
        return FALSE;
    }

    key.Block = first;

    const ULONG held = BlorgDiskCacheIndexPinHeld(&DiskCache.Index, &key, last, read->Slots);

    ULONG served = 0;

    if (0 != held)
    {
        if (0 == allowed && held != blocks)
        {
            *Fetches = DiskCacheRunsToFetch(read->Slots, blocks);
        }
        else
        {
            served = BlorgDiskCacheIndexPlanRead(&DiskCache.Index, read->Slots, blocks, held, allowed);
        }
    }

    if (0 == served)
    {
        for (ULONG i = 0; i < blocks; ++i)
        {
            if (DISK_CACHE_NO_SLOT != read->Slots[i])
            {
                BlorgDiskCacheIndexUnpin(&DiskCache.Index, read->Slots[i], FALSE);
            }
        }

        ExFreePool(read);
        DiskCacheLeave();
        return FALSE;
    }

    read->Irp = Irp;
    read->Completion = Completion;
    read->Key = key;
    read->Outstanding = 1;
    read->Status = STATUS_SUCCESS;
    read->Valid = Valid;
    read->SlotCount = blocks;

    ULONG issued = 0;

    while (issued < span)
    {
        const ULONG64 at = Offset + issued;
        const ULONG i = C_CAST(ULONG, (at >> DISK_CACHE_BLOCK_SHIFT) - first);
        const ULONG slot = read->Slots[i];
        ULONG run = 1;

        while (i + run < blocks &&
               ((DISK_CACHE_NO_SLOT == slot) ? (DISK_CACHE_NO_SLOT == read->Slots[i + run]) : (read->Slots[i + run] == slot + run)))
        {
            run++;
        }

        const ULONG64 runEnd = (first + i + run) << DISK_CACHE_BLOCK_SHIFT;
        const ULONG stop = (DISK_CACHE_NO_SLOT == slot) ? Valid : span;
        const ULONG length = C_CAST(ULONG, min(runEnd - at, C_CAST(ULONG64, stop - issued)));

        const NTSTATUS status = DiskCacheReadPart(read, Path, Offset, at, length, slot);

        if (!NT_SUCCESS(status))
        {
            if (0 == issued)
            {
                for (ULONG j = 0; j < blocks; ++j)
                {
                    if (DISK_CACHE_NO_SLOT != read->Slots[j])
                    {
                        BlorgDiskCacheIndexUnpin(&DiskCache.Index, read->Slots[j], FALSE);
                    }
                }

                ExFreePool(read);
                DiskCacheLeave();
                return FALSE;
            }

            InterlockedCompareExchange(&read->Status, status, STATUS_SUCCESS);
            break;
        }

        read->Span += length;

        if (DISK_CACHE_NO_SLOT != slot && issued < Valid)
        {
            read->DiskSpan += min(length, Valid - issued);
        }

        if (DISK_CACHE_NO_SLOT == slot && stop == issued + length)
        {
            break;
        }

        issued += length;
    }

    *Fetches = read->FetchCount;

    DiskCacheReadSettle(read);

    return TRUE;
}

BOOLEAN BlorgDiskCacheLive(VOID)
{
    return 0 != ReadNoFence(&DiskCache.Live);
}

//
// Copies every whole block of Length bytes at Offset of Key's file in
// Source -- or the file's last block, if they reach end of file -- and
// queues it to be written, if the index admits it. Source is a body of the
// version Key names. <= DISPATCH_LEVEL, from a fetch completion.
//
static VOID DiskCacheFill(const DISK_CACHE_KEY* Key, const UCHAR* Source, ULONG64 Offset, ULONG Length)
{
    DISK_CACHE_KEY key = *Key;
    const ULONG64 end = Offset + Length;

    if (!Source || 0 == Length || end > key.Size)
    {
        return;
    }

    for (key.Block = (Offset + DISK_CACHE_BLOCK_SIZE - 1) >> DISK_CACHE_BLOCK_SHIFT; ; ++key.Block)
    {
        const ULONG64 start = key.Block << DISK_CACHE_BLOCK_SHIFT;
        const ULONG64 stop = min(start + DISK_CACHE_BLOCK_SIZE, key.Size);

        if (start >= stop || stop > end)
        {
            break;
        }

        if (InterlockedAdd64(&DiskCache.FillBytes, DISK_CACHE_BLOCK_SIZE) > C_CAST(LONG64, DISK_CACHE_FILL_BACKLOG))
        {
            DiskCacheUnchargeFill();
            BLORGFS_STAT_INC(DiskCacheDropped);
            continue;
        }

        if (!DiskCacheEnter())
        {
            DiskCacheUnchargeFill();
            break;
        }

        ULONG slot;
        const DISK_CACHE_ADMIT admit = BlorgDiskCacheIndexReserve(&DiskCache.Index, &key, &slot);

        if (DiskCacheReserved != admit)
        {
            if (DiskCacheFirstMiss == admit)
            {
                BLORGFS_STAT_INC(DiskCacheFirstMisses);
            }
            else if (DiskCacheNoVictim == admit)
            {
                BLORGFS_STAT_INC(DiskCacheDropped);
            }

            DiskCacheUnchargeFill();
            DiskCacheLeave();
            continue;
        }

        PDISK_CACHE_FILL fill = ExAllocatePoolZero(NonPagedPoolNx, sizeof(DISK_CACHE_FILL), DISK_CACHE_TAG);
        PVOID buffer = fill ? ExAllocatePoolUninitialized(NonPagedPoolNx, DISK_CACHE_BLOCK_SIZE, DISK_CACHE_TAG) : NULL;
        PMDL mdl = buffer ? IoAllocateMdl(buffer, DISK_CACHE_BLOCK_SIZE, FALSE, FALSE, NULL) : NULL;

        if (!mdl)
        {
            if (buffer)
            {
                ExFreePool(buffer);
            }

            if (fill)
            {
                ExFreePool(fill);
            }

            BlorgDiskCacheIndexCommit(&DiskCache.Index, slot, FALSE);
            BLORGFS_STAT_INC(DiskCacheDropped);
            DiskCacheUnchargeFill();
            DiskCacheLeave();
            break;
        }

        MmBuildMdlForNonPagedPool(mdl);

        const ULONG held = C_CAST(ULONG, stop - start);

        RtlCopyMemory(buffer, Source + (start - Offset), held);
        RtlZeroMemory(C_CAST(PUCHAR, buffer) + held, DISK_CACHE_BLOCK_SIZE - held);

        fill->Buffer = buffer;
        fill->Mdl = mdl;
        fill->Slot = slot;

        DiskCacheQueueFill(fill);
    }
}

BOOLEAN BlorgDiskCacheAdmit(PNON_PAGED_NODE Node, const FILE_BUFFER* FileBuffer, ULONG64 Offset, ULONG Length)
{
    DISK_CACHE_KEY key;

    if (!ReadNoFence(&DiskCache.Live) || !FileBuffer->BodyBuffer || 0 == Length || !DiskCacheKeyOf(Node, &key))
    {
        return TRUE;
    }

    if (!DiskCacheIsVersion(&key, FileBuffer))
    {
        BLORGFS_STAT_INC(DiskCacheStale);
        return FALSE;
    }

    DiskCacheFill(&key, C_CAST(const UCHAR*, FileBuffer->BodyBuffer), Offset, Length);

    return TRUE;
}
