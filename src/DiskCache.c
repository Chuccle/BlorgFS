#include "Driver.h"

//
// The disk cache's I/O: the cache file, reads served from it, and fetched
// blocks written into it. DiskCache.h says what it is for and what a block
// is stored under; DiskCacheIndex.c decides which slot holds what.
//
// Reads
// ---------------------------------------------------------------------
// A non-cached read is served from the file only when every block it
// covers is held for the version the open's FCB names, pinned for as long
// as the read is in flight. It is issued as one IRP per run of blocks that
// sit in consecutive slots, each into a partial MDL of the read's own, so
// nothing is copied. The IRPs are this driver's own, sent straight to the
// cache file's device and finished by a completion routine, which needs no
// APC from the thread that issued them: the read path can run with APCs
// disabled (a paging read under a page fault), and a synchronous Zw call
// would wait on one. A read the cache cannot finish is given back to the
// caller to fetch.
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
// age out.
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

//
// One read being served from the cache, shared by the IRPs it was split
// into.
//
typedef struct _DISK_CACHE_READ
{
    PIRP Irp;                               // The read being served
    PDISK_CACHE_READ_COMPLETION Completion; // Called once, when the last IRP is done
    volatile LONG Outstanding;              // IRPs in flight, plus one the issuer holds
    volatile LONG Status;                   // First failure, or STATUS_SUCCESS
    volatile LONG64 Bytes;                  // Bytes the IRPs reported read
    ULONG Span;                             // Bytes asked of the cache file in all
    ULONG Valid;                            // The caller's Valid, handed back
    ULONG SlotCount;
    ULONG Reserved;                         // explicit padding
    ULONG Slots[DISK_CACHE_MAX_READ_BLOCKS]; // Pinned, one per block, in file order
} DISK_CACHE_READ, * PDISK_CACHE_READ;

CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Irp, Completion);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Completion, Outstanding);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Outstanding, Status);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Status, Bytes);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Bytes, Span);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Span, Valid);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Valid, SlotCount);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, SlotCount, Reserved);
CHECK_PADDING_BETWEEN(DISK_CACHE_READ, Reserved, Slots);
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
// Drops one hold on Read; the last unpins its blocks and hands the IRP back
// through its completion. A read that came back short failed. <=
// DISPATCH_LEVEL.
//
static VOID DiskCacheReadSettle(PDISK_CACHE_READ Read)
{
    if (0 != InterlockedDecrement(&Read->Outstanding))
    {
        return;
    }

    for (ULONG i = 0; i < Read->SlotCount; ++i)
    {
        BlorgDiskCacheIndexUnpin(&DiskCache.Index, Read->Slots[i]);
    }

    NTSTATUS status = Read->Status;

    if (NT_SUCCESS(status) && Read->Bytes != Read->Span)
    {
        status = STATUS_UNEXPECTED_IO_ERROR;
    }

    if (NT_SUCCESS(status))
    {
        BLORGFS_STAT_INC(DiskCacheHits);
        BLORGFS_STAT_ADD(DiskCacheHitBytes, Read->Valid);
    }
    else
    {
        BLORGFS_STAT_INC(DiskCacheReadFailures);
    }

    PIRP irp = Read->Irp;
    PDISK_CACHE_READ_COMPLETION completion = Read->Completion;
    const ULONG valid = Read->Valid;

    ExFreePool(Read);
    DiskCacheLeave();

    completion(irp, status, valid);
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
        InterlockedCompareExchange(&read->Status, Irp->IoStatus.Status, STATUS_SUCCESS);
        DiskCacheLost();
    }

    IoFreeMdl(Irp->MdlAddress);
    IoFreeIrp(Irp);

    DiskCacheReadSettle(read);

    return STATUS_MORE_PROCESSING_REQUIRED;
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
// tail past end of file the fill wrote as zeros. If an IRP cannot be sent
// once some already are, the read is failed through Completion rather than
// handed back, since those are writing into its buffer.
//
BOOLEAN BlorgDiskCacheRead(PIRP Irp, const DISK_CACHE_KEY* Key, ULONG64 Offset, ULONG Length, ULONG Valid, PDISK_CACHE_READ_COMPLETION Completion)
{
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

    if (!BlorgDiskCacheIndexPinRange(&DiskCache.Index, &key, last, read->Slots))
    {
        ExFreePool(read);
        DiskCacheLeave();
        return FALSE;
    }

    read->Irp = Irp;
    read->Completion = Completion;
    read->Outstanding = 1;
    read->Status = STATUS_SUCCESS;
    read->Span = span;
    read->Valid = Valid;
    read->SlotCount = blocks;

    PUCHAR base = MmGetMdlVirtualAddress(Irp->MdlAddress);
    ULONG issued = 0;

    while (issued < span)
    {
        const ULONG64 at = Offset + issued;
        const ULONG i = C_CAST(ULONG, (at >> DISK_CACHE_BLOCK_SHIFT) - first);
        ULONG run = 1;

        while (i + run < blocks && read->Slots[i + run] == read->Slots[i] + run)
        {
            run++;
        }

        const ULONG64 runEnd = (first + i + run) << DISK_CACHE_BLOCK_SHIFT;
        const ULONG length = C_CAST(ULONG, min(runEnd - at, C_CAST(ULONG64, span - issued)));
        const ULONG64 diskOffset = (C_CAST(ULONG64, read->Slots[i]) << DISK_CACHE_BLOCK_SHIFT) + (at & (DISK_CACHE_BLOCK_SIZE - 1));

        PMDL mdl = IoAllocateMdl(base + issued, length, FALSE, FALSE, NULL);
        NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

        if (mdl)
        {
            IoBuildPartialMdl(Irp->MdlAddress, mdl, base + issued, length);
            InterlockedIncrement(&read->Outstanding);

            status = DiskCacheIssue(IRP_MJ_READ, mdl, length, diskOffset, DiskCacheReadDone, read);

            if (!NT_SUCCESS(status))
            {
                InterlockedDecrement(&read->Outstanding);
                IoFreeMdl(mdl);
            }
        }

        if (!NT_SUCCESS(status))
        {
            if (0 == issued)
            {
                for (ULONG j = 0; j < blocks; ++j)
                {
                    BlorgDiskCacheIndexUnpin(&DiskCache.Index, read->Slots[j]);
                }

                ExFreePool(read);
                DiskCacheLeave();
                return FALSE;
            }

            InterlockedCompareExchange(&read->Status, status, STATUS_SUCCESS);
            break;
        }

        issued += length;
    }

    DiskCacheReadSettle(read);

    return TRUE;
}

BOOLEAN BlorgDiskCacheLive(VOID)
{
    return 0 != ReadNoFence(&DiskCache.Live);
}

VOID BlorgDiskCacheAdmit(PNON_PAGED_NODE Node, const FILE_BUFFER* FileBuffer, ULONG64 Offset, ULONG Length)
{
    DISK_CACHE_KEY key;
    const UCHAR* source = C_CAST(const UCHAR*, FileBuffer->BodyBuffer);

    if (!ReadNoFence(&DiskCache.Live) || !source || 0 == Length || !DiskCacheKeyOf(Node, &key))
    {
        return;
    }

    if (!FileBuffer->HasVersion || FileBuffer->VersionSize != key.Size || FileBuffer->VersionTime != key.ModifiedTime)
    {
        BLORGFS_STAT_INC(DiskCacheStale);
        return;
    }

    const ULONG64 end = Offset + Length;

    if (end > key.Size)
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

        RtlCopyMemory(buffer, source + (start - Offset), held);
        RtlZeroMemory(C_CAST(PUCHAR, buffer) + held, DISK_CACHE_BLOCK_SIZE - held);

        fill->Buffer = buffer;
        fill->Mdl = mdl;
        fill->Slot = slot;

        DiskCacheQueueFill(fill);
    }
}
