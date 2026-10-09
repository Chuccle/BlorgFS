#include "Driver.h"

//
//  IRP_MJ_CREATE handling: access/share-access checks, oplock breaks on
//  sharing violations, path resolution against in-memory FCB/DCB tree,
//  path-cache and parent-listing lookups, and the async network fallback
//  (BlorgHttpGetFileInformation) that re-drives the create via the FSP
//  once remote metadata is available.
//

//
//  The access this read-only volume grants, as one set rather than two
//  predicates that each restated it.
//
//  MAXIMUM_ALLOWED belongs here, not outside. It is not a request for write
//  access -- it means "grant whatever I am entitled to" -- and the
//  entitlement is decided before either check below runs: the devices are
//  FILE_DEVICE_SECURE_OPEN, so the I/O manager resolves MAXIMUM_ALLOWED
//  against the device security descriptor and sets the handle's granted
//  access from that. These checks are the second, independent gate.
//
//  Note FILE_READ_DATA and FILE_LIST_DIRECTORY are the same bit, as are
//  FILE_EXECUTE and FILE_TRAVERSE; both spellings are listed because both
//  are what a caller writes, and neither is redundant to a reader.
//
#define BLORGFS_READ_ONLY_ACCESS (DELETE |    \
    READ_CONTROL |                            \
    WRITE_OWNER |                             \
    WRITE_DAC |                               \
    SYNCHRONIZE |                             \
    ACCESS_SYSTEM_SECURITY |                  \
    FILE_READ_DATA |                          \
    FILE_READ_EA |                            \
    FILE_WRITE_EA |                           \
    FILE_READ_ATTRIBUTES |                    \
    FILE_WRITE_ATTRIBUTES |                   \
    FILE_EXECUTE |                            \
    FILE_LIST_DIRECTORY |                     \
    FILE_TRAVERSE |                           \
    MAXIMUM_ALLOWED)

//
//  What a directory grants on top of that: adding or removing a child is a
//  normal directory operation, not a data write in the file sense. This
//  difference is the entire reason the two checks below are separate.
//
#define BLORGFS_DIRECTORY_CHILD_ACCESS (FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD)

//
//  Rejects a desired-access mask this read-only volume cannot honour.
//
//  Deliberately no KdBreakPoint() here or in CreateCheckDirectoryAccess/the
//  disposition check below. Refusing a write on a read-only volume is an
//  expected, well-defined outcome, not an anomaly: Explorer, Defender and
//  SearchHost probe files with write masks unprompted, and anything that
//  registers a library or saves settings (Steam, game launchers) opens for
//  write as a matter of course. KdBreakPoint() is an `int 3` in Debug
//  builds, so trapping here halted the entire guest -- indistinguishable
//  from VM/VIX flakiness, and the cause of many "frozen VM" incidents.
//  See deploy/DEBUGGING.md.
//
//  Both checks used to run a second, wider mask first -- the set of bits
//  this volume understands at all -- gated on an IsReadOnly parameter that
//  every one of the four call sites passed TRUE. That made the wider mask
//  unreachable in two senses at once: the parameter was never FALSE, and
//  the read-only set it guarded is a strict subset of it, so no mask could
//  be rejected by the first test that the second did not reject anyway. It
//  cost two verbatim copies of a sixteen-flag list whose only job was to be
//  kept in step with a list that already decided every answer.
//
//  ReadOnlyAccessMaskIsDecidedBitByBit in
//  tests\sandbox\CreateDirectoryTest.cpp drives all 32 bits through a real
//  open against the sets above. Both predicates reject on "any bit outside
//  the permitted set", which is monotone in bits, so deciding every single
//  bit correctly decides every combination -- that test was written against
//  the two-mask version and passed unchanged here, which is what says the
//  collapse changed no answer.
//
static inline BOOLEAN CreateCheckFileAccess(const ACCESS_MASK* DesiredAccess)
{
    if (FlagOn(*DesiredAccess, ~BLORGFS_READ_ONLY_ACCESS))
    {
        return FALSE;
    }

    return TRUE;
}

//
//  Same check for directories, widened by the child-mutation bits.
//
static inline BOOLEAN CreateCheckDirectoryAccess(const ACCESS_MASK* DesiredAccess)
{
    if (FlagOn(*DesiredAccess, ~(BLORGFS_READ_ONLY_ACCESS | BLORGFS_DIRECTORY_CHILD_ACCESS)))
    {
        return FALSE;
    }

    return TRUE;
}

//
//  Establishes or checks share access for an open: the first handle to a
//  node seeds the SHARE_ACCESS state via IoSetShareAccess, every
//  subsequent handle is validated against it via IoCheckShareAccess.
//
static inline NTSTATUS CreateApplyShareAccess(PFILE_OBJECT FileObject, const ACCESS_MASK* DesiredAccess, USHORT ShareAccess, PSHARE_ACCESS Sa, BOOLEAN FirstOpen)
{
    if (FirstOpen)
    {
        IoSetShareAccess(*DesiredAccess, ShareAccess, FileObject, Sa);
        return STATUS_SUCCESS;
    }

    return IoCheckShareAccess(*DesiredAccess, ShareAccess, FileObject, Sa, TRUE);
}

//
//  Called from the open-existing paths when CreateApplyShareAccess returns a sharing
//  violation. A handle (RH/RWH) oplock holder may be keeping the file open for
//  caching; break it so the holder closes and the conflict can resolve when the
//  FSP re-drives this create -- without this a handle oplock locks the
//  conflicting opener out permanently. Mirrors fastfat's FsRtlOplockBreakH on
//  the sharing-violation path (create.c). Must be called under the node
//  resource. Returns STATUS_PENDING if the break was posted (the IRP now
//  belongs to the oplock package), the break error, or -- when there is no
//  handle oplock to break -- the original sharing status. Honors
//  FILE_COMPLETE_IF_OPLOCKED: that caller explicitly asked not to trigger a
//  break, so we just hand its sharing violation back.
//
static inline NTSTATUS CreateBreakHandleOplockOnSharingViolation(POPLOCK Oplock, PIRP Irp, NTSTATUS ShareStatus)
{
    if ((STATUS_SHARING_VIOLATION != ShareStatus) ||
        FlagOn(IoGetCurrentIrpStackLocation(Irp)->Parameters.Create.Options, FILE_COMPLETE_IF_OPLOCKED))
    {
        return ShareStatus;
    }

    NTSTATUS breakStatus = FsRtlOplockBreakH(Oplock, Irp, 0, NULL, BlorgOplockComplete, BlorgOplockPrePostIrp);

    return (STATUS_SUCCESS == breakStatus) ? ShareStatus : breakStatus;
}

//
//  Opens a handle to an already-resident FCB: checks access, breaks any
//  conflicting oplock, bumps RefCount, and applies share access -- all
//  under the Fcb resource so the oplock break is atomic with the
//  RefCount/share-access update (see CreateBreakHandleOplockOnSharingViolation).
//  That atomicity is the point: OplockRequest grants under this same
//  resource using RefCount as OpenCount, so without it a grant could slip
//  into the gap between the break and the bump and hand out an oplock this
//  open never broke. The oplock package is internally thread-safe, but that
//  protects only the OPLOCK structure, not this open-count invariant.
//  FsRtlCheckOplock is called unconditionally (it fast-returns SUCCESS with
//  no oplock). On a pending break it posts the IRP and returns
//  STATUS_PENDING; BlorgOplockComplete re-queues it to the FSP, which re-drives
//  this create from the top, releasing the resource so the re-drive starts
//  from a clean slate. On success wires FsContext/Vpb/SectionObjectPointer
//  onto the file object.
//  Clears the FCB's read-ahead idle stamp on every open, which is not
//  bookkeeping but a correctness fix. An FCB outlives its handles -- a
//  closed file is parked on the delayed close list and revived on re-open
//  -- so a stale stamp charges the first read of a new session the gap
//  since the last read of the previous one. Measured, that was a single
//  402-second sample reported as a consumer idling 99.7% of a run lasting
//  seconds. Cleared on every open rather than only the first: a second
//  handle arriving mid-session costs one lost sample, where keeping the
//  stamp risks a fabricated one, and the statistics block's standard is
//  that a counter may be lossy and may never be invented.
//
static inline NTSTATUS CreateOpenExistingFcb(PIRP Irp, PFILE_OBJECT FileObject, const ACCESS_MASK* DesiredAccess, USHORT ShareAccess, PFCB Fcb)
{
    if (!CreateCheckFileAccess(DesiredAccess))
    {
        return STATUS_ACCESS_DENIED;
    }

    ASSERT((0 < Fcb->PinCount) ||
           ExIsResourceAcquiredExclusiveLite(BlorgGetVolumeDeviceExtension(Fcb->VolumeDeviceObject)->Vcb->Header.Resource));

    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);

    NTSTATUS oplockStatus = FsRtlCheckOplock(&Fcb->Header.Oplock, Irp, NULL, BlorgOplockComplete, BlorgOplockPrePostIrp);

    if (STATUS_SUCCESS != oplockStatus)
    {
        ExReleaseResourceLite(Fcb->Header.Resource);
        return oplockStatus;
    }

    const BOOLEAN firstOpen = (1 == InterlockedIncrement64(&Fcb->RefCount));

    Fcb->ReadIdleLastEndQpc = 0;

    NTSTATUS result = CreateApplyShareAccess(FileObject, DesiredAccess, ShareAccess, &Fcb->ShareAccess, firstOpen);

    if (!NT_SUCCESS(result))
    {
        InterlockedDecrement64(&Fcb->RefCount);
        result = CreateBreakHandleOplockOnSharingViolation(&Fcb->Header.Oplock, Irp, result);
        ExReleaseResourceLite(Fcb->Header.Resource);
        return result;
    }

    ExReleaseResourceLite(Fcb->Header.Resource);

#pragma warning(suppress: 28175)
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = NULL;
    FileObject->SectionObjectPointer = &Fcb->NonPaged->SectionObjectPointers;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

//
//  Whether a resident FCB may answer an open as it is. An FCB outlives its
//  handles while Cc or Mm still reference its file object, so without this
//  a file changed on the server reopened with the size and pages of the
//  copy read before it. The FCB follows the path cache's rule, which the
//  TTL and the change feed keep honest, so it inherits both without
//  invalidation plumbing of its own. Its own stamp answers first, with no
//  lock taken: nothing invalidated since its metadata was read, and that
//  read younger than the cache's lifetime. Any invalidation anywhere fails
//  the stamp, so it then asks the path cache, and trusts the FCB while the
//  entry there has the same size and write time. Anything else sends the
//  open down the cold path for current metadata, which CreateFcbRefresh
//  applies.
//
//  A file some handle still has open is trusted as it is. Its handles share
//  one FCB and one cache, and changing the size under them would tear the
//  view a reader is in the middle of, so the refresh waits for the first
//  open after the last cleanup: NFS's close-to-open rule.
//  ShareAccess.OpenCount is that count: IoSetShareAccess counts only opens
//  with data access, the only ones that can read the cache, and each
//  cleanup removes one. Read here without the FCB resource it is a hint;
//  CreateFcbRefresh re-reads it under the resource before acting on it.
//
static BOOLEAN CreateFcbIsCurrent(PFCB Fcb)
{
    if ((0 != ReadNoFence(C_CAST(LONG*, &Fcb->ShareAccess.OpenCount))) ||
        BlorgPathCacheTicketCurrent(&Fcb->MetaTicket))
    {
        return TRUE;
    }

    DIRECTORY_ENTRY_METADATA cached;

    if (PathCacheExists != BlorgPathCachePeek(&Fcb->FullPath, &cached))
    {
        return FALSE;
    }

    return !cached.IsDirectory &&
           (C_CAST(LONGLONG, cached.Size) == Fcb->Header.FileSize.QuadPart) &&
           (cached.LastModifiedTime == Fcb->LastModifiedTime);
}

//
//  Applies current metadata, read under Ticket, to a resident FCB on the
//  cold path, under the VCB resource exclusive and before CreateOpenExistingFcb.
//  When the size and write time match, which is the usual case, only the
//  stamp moves: the FCB is known current as of Ticket. Otherwise what Cc
//  and Mm hold of the old contents is dropped and the FCB takes the new
//  size and times. Dropping them needs no handle open, no user-mapped view
//  left (a view outlives its handle) and no image section (a running
//  executable). When any of those holds the old pages the FCB keeps the
//  old copy, which is still internally consistent, and is stamped anyway:
//  every open would otherwise take the VCB resource exclusive to fail the
//  same purge, so the next attempt waits for the stamp to lapse, a
//  lifetime or the next invalidation on.
//
//  The sizes change under the paging resource as well, which is what Mm
//  reads them under. A shared cache map can outlive the purge until Cc's
//  lazy teardown reaches it, so it is told the new sizes through the file
//  object being opened, which is about to point at this FCB anyway.
//
static VOID CreateFcbRefresh(PFCB Fcb, const DIRECTORY_ENTRY_METADATA* Meta, const PATH_CACHE_TICKET* Ticket, PFILE_OBJECT FileObject)
{
    PSECTION_OBJECT_POINTERS sections = &Fcb->NonPaged->SectionObjectPointers;

    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);

    if ((C_CAST(LONGLONG, Meta->Size) == Fcb->Header.FileSize.QuadPart) &&
        (Meta->LastModifiedTime == Fcb->LastModifiedTime))
    {
        Fcb->MetaTicket = *Ticket;
        ExReleaseResourceLite(Fcb->Header.Resource);
        return;
    }

    if ((0 != Fcb->ShareAccess.OpenCount) ||
        !MmFlushImageSection(sections, MmFlushForWrite) ||
        !CcPurgeCacheSection(sections, NULL, 0, UNINITIALIZE_CACHE_MAPS))
    {
        BLORGFS_STAT_INC(FcbRefreshesDeferred);
        Fcb->MetaTicket = *Ticket;
        ExReleaseResourceLite(Fcb->Header.Resource);
        return;
    }

    ExAcquireResourceExclusiveLite(Fcb->Header.PagingIoResource, TRUE);
    Fcb->Header.FileSize.QuadPart = Fcb->Header.AllocationSize.QuadPart = C_CAST(LONGLONG, Meta->Size);
    Fcb->CreationTime = Meta->CreationTime;
    Fcb->LastAccessedTime = Meta->LastAccessedTime;
    Fcb->LastModifiedTime = Meta->LastModifiedTime;
    ExReleaseResourceLite(Fcb->Header.PagingIoResource);

    Fcb->MetaTicket = *Ticket;

    FileObject->SectionObjectPointer = sections;

    if (CcIsFileCached(FileObject))
    {
        CcSetFileSizes(FileObject, C_CAST(PCC_FILE_SIZES, &Fcb->Header.AllocationSize));
    }

    BLORGFS_STAT_INC(FcbRefreshes);
    ExReleaseResourceLite(Fcb->Header.Resource);
}

//
//  Opens a handle to an already-resident non-root DCB: same pattern as
//  CreateOpenExistingFcb (access check, oplock break, RefCount bump, share
//  access, all under the Dcb resource), but also allocates the CCB used
//  for this directory handle's enumeration/notify state. The oplock break
//  happens before the CCB is allocated so a pending break leaks nothing;
//  the resource is released on every early-out path.
//
static inline NTSTATUS CreateOpenExistingDcb(PIRP Irp, PFILE_OBJECT FileObject, const ACCESS_MASK* DesiredAccess, USHORT ShareAccess, PDCB Dcb, const DEVICE_OBJECT* VolumeDeviceObject)
{
    if (!CreateCheckDirectoryAccess(DesiredAccess))
    {
        return STATUS_ACCESS_DENIED;
    }

    ASSERT((0 < Dcb->PinCount) ||
           ExIsResourceAcquiredExclusiveLite(BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->Vcb->Header.Resource));

    ExAcquireResourceExclusiveLite(Dcb->Header.Resource, TRUE);

    NTSTATUS oplockStatus = FsRtlCheckOplock(&Dcb->Header.Oplock, Irp, NULL, BlorgOplockComplete, BlorgOplockPrePostIrp);

    if (STATUS_SUCCESS != oplockStatus)
    {
        ExReleaseResourceLite(Dcb->Header.Resource);
        return oplockStatus;
    }

    PCCB pCcb;

    NTSTATUS result = BlorgCreateCCB(&pCcb, VolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ExReleaseResourceLite(Dcb->Header.Resource);
        return result;
    }

    const BOOLEAN firstOpen = (1 == InterlockedIncrement64(&Dcb->RefCount));

    result = CreateApplyShareAccess(FileObject, DesiredAccess, ShareAccess, &Dcb->ShareAccess, firstOpen);

    if (!NT_SUCCESS(result))
    {
        InterlockedDecrement64(&Dcb->RefCount);
        result = CreateBreakHandleOplockOnSharingViolation(&Dcb->Header.Oplock, Irp, result);
        ExReleaseResourceLite(Dcb->Header.Resource);
        BlorgFreeFileContext(pCcb, VolumeDeviceObject);
        return result;
    }

    ExReleaseResourceLite(Dcb->Header.Resource);

#pragma warning(suppress: 28175)
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Dcb;
    FileObject->FsContext2 = pCcb;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

//
//  Opens a handle to the volume object itself (empty file name, no
//  related file object). No oplock handling -- the VCB has no oplock --
//  just access check, RefCount bump, and share access under the VCB
//  resource.
//
static inline NTSTATUS CreateOpenVcb(PIRP Irp, PFILE_OBJECT FileObject, const ACCESS_MASK* DesiredAccess, USHORT ShareAccess, PVCB Vcb)
{
    if (!CreateCheckFileAccess(DesiredAccess))
    {
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceExclusiveLite(Vcb->Header.Resource, TRUE);

    const BOOLEAN firstOpen = (1 == InterlockedIncrement64(&Vcb->RefCount));

    NTSTATUS result = CreateApplyShareAccess(FileObject, DesiredAccess, ShareAccess, &Vcb->ShareAccess, firstOpen);

    if (!NT_SUCCESS(result))
    {
        InterlockedDecrement64(&Vcb->RefCount);
        ExReleaseResourceLite(Vcb->Header.Resource);
        return result;
    }

    ExReleaseResourceLite(Vcb->Header.Resource);

#pragma warning(suppress: 28175)
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Vcb;
    FileObject->FsContext2 = NULL;
    FileObject->SectionObjectPointer = &Vcb->NonPaged->SectionObjectPointers;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

//
//  Opens a handle to the root directory: same pattern as CreateOpenExistingDcb,
//  minus the pin/VCB assert -- the root is never table-resident and never
//  reaped, so reaching it via the file object is always safe.
//
static inline NTSTATUS CreateOpenRootDcb(PIRP Irp, PFILE_OBJECT FileObject, const ACCESS_MASK* DesiredAccess, USHORT ShareAccess, PDCB Dcb, const DEVICE_OBJECT* VolumeDeviceObject)
{
    if (!CreateCheckDirectoryAccess(DesiredAccess))
    {
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceExclusiveLite(Dcb->Header.Resource, TRUE);

    NTSTATUS oplockStatus = FsRtlCheckOplock(&Dcb->Header.Oplock, Irp, NULL, BlorgOplockComplete, BlorgOplockPrePostIrp);

    if (STATUS_SUCCESS != oplockStatus)
    {
        ExReleaseResourceLite(Dcb->Header.Resource);
        return oplockStatus;
    }

    PCCB pCcb;

    NTSTATUS result = BlorgCreateCCB(&pCcb, VolumeDeviceObject);

    if (!NT_SUCCESS(result))
    {
        ExReleaseResourceLite(Dcb->Header.Resource);
        return result;
    }

    const BOOLEAN firstOpen = (1 == InterlockedIncrement64(&Dcb->RefCount));

    result = CreateApplyShareAccess(FileObject, DesiredAccess, ShareAccess, &Dcb->ShareAccess, firstOpen);

    if (!NT_SUCCESS(result))
    {
        InterlockedDecrement64(&Dcb->RefCount);
        result = CreateBreakHandleOplockOnSharingViolation(&Dcb->Header.Oplock, Irp, result);
        ExReleaseResourceLite(Dcb->Header.Resource);
        BlorgFreeFileContext(pCcb, VolumeDeviceObject);
        return result;
    }

    ExReleaseResourceLite(Dcb->Header.Resource);

#pragma warning(suppress: 28175)
    FileObject->Vpb = global.DiskDeviceObject->Vpb;
    FileObject->FsContext = Dcb;
    FileObject->FsContext2 = pCcb;
    Irp->IoStatus.Information = FILE_OPENED;

    return STATUS_SUCCESS;
}

typedef struct _CREATE_NET_CONTEXT
{
    PIRP              Irp;
    UNICODE_STRING    Path;   // owned copy, NonPagedPoolNx
    PATH_CACHE_TICKET Ticket; // taken immediately before the lookup is issued
} CREATE_NET_CONTEXT, * PCREATE_NET_CONTEXT;

//
//  Async completion for the BlorgHttpGetFileInformation lookup issued from
//  BlorgVolumeCreate. Memoizes the result in the path cache (only a
//  definitive not-found, never a transient failure, and neither if an
//  invalidation overtook the request -- see PATH_CACHE_TICKET), stashes the metadata
//  on the IRP, and re-queues it with NET_DONE set so BlorgVolumeCreate
//  resumes from the top with the result already in hand. If the re-queue
//  fails (FSP threads tearing down), the stash is freed and the create is
//  failed with the re-queue status rather than leaking the stash.
//
static VOID CreateComplete(NTSTATUS Status, const DIRECTORY_ENTRY_METADATA* FileInfo, PVOID CallerContext)
{
    PCREATE_NET_CONTEXT netCtx = CallerContext;
    PIRP irp = netCtx->Irp;

    if (!NT_SUCCESS(Status))
    {
        BLORGFS_LOG("Create net result FAILED %08x\n", Status);

        if (STATUS_OBJECT_NAME_NOT_FOUND == Status)
        {
            BlorgPathCacheInsertNotFound(&netCtx->Path, &netCtx->Ticket);
        }

        ExFreePool(netCtx->Path.Buffer);
        ExFreePool(netCtx);
        BlorgCompleteRequest(irp, Status, IO_DISK_INCREMENT);
        return;
    }

    BLORGFS_LOG("Create net result OK (dir=%u size=%llu)\n", FileInfo->IsDirectory, FileInfo->Size);

    BlorgPathCacheInsertExists(&netCtx->Path, FileInfo, &netCtx->Ticket);

    PCREATE_NET_RESULT stash = ExAllocatePoolUninitialized(NonPagedPoolNx, sizeof(CREATE_NET_RESULT), 'CRET');

    if (!stash)
    {
        ExFreePool(netCtx->Path.Buffer);
        ExFreePool(netCtx);
        BlorgCompleteRequest(irp, STATUS_INSUFFICIENT_RESOURCES, IO_DISK_INCREMENT);
        return;
    }

    stash->Meta = *FileInfo;
    stash->Ticket = netCtx->Ticket;
    irp->Tail.Overlay.DriverContext[1] = stash;

    BlorgSetIrpContextFlag(irp, IRP_CONTEXT_FLAG_NET_DONE);

    NTSTATUS requeue = BlorgFsdRequeueRequest(irp);

    ExFreePool(netCtx->Path.Buffer);
    ExFreePool(netCtx);

    if (STATUS_PENDING != requeue)
    {
        ExFreePool(stash);
        irp->Tail.Overlay.DriverContext[1] = NULL;
        BlorgCompleteRequest(irp, requeue, IO_DISK_INCREMENT);
    }
}

//
//  Splits a full path into its parent directory portion and final
//  component (leaf) by locating the last backslash. Both outputs alias
//  Path's buffer -- no allocation or copy.
//
static BOOLEAN CreateSplitPathLeaf(const UNICODE_STRING* Path, PUNICODE_STRING ParentPath, PUNICODE_STRING Leaf)
{
    if (!Path->Buffer || Path->Length < sizeof(WCHAR))
    {
        return FALSE;
    }

    LONG chars = C_CAST(LONG, Path->Length / sizeof(WCHAR));
    LONG sep = -1;

    for (LONG i = chars - 1; i >= 0; i--)
    {
        if (L'\\' == Path->Buffer[i])
        {
            sep = i;
            break;
        }
    }

    if (sep < 0)
    {
        Leaf->Buffer = Path->Buffer;
        Leaf->Length = Path->Length;
        Leaf->MaximumLength = Path->Length;
        ParentPath->Buffer = Path->Buffer;
        ParentPath->Length = 0;
        ParentPath->MaximumLength = 0;
        return TRUE;
    }

    Leaf->Buffer = Path->Buffer + sep + 1;
    Leaf->Length = C_CAST(USHORT, (chars - (sep + 1)) * sizeof(WCHAR));
    Leaf->MaximumLength = Leaf->Length;

    ParentPath->Buffer = Path->Buffer;
    ParentPath->Length = C_CAST(USHORT, sep * sizeof(WCHAR));
    ParentPath->MaximumLength = ParentPath->Length;

    return 0 != Leaf->Length;
}

//
//  Looks up a single entry by name within a cached parent-directory
//  listing, checking files then subdirectories, and fills Out with its
//  metadata on a match.
//
static BOOLEAN CreateFindEntryByName(PDIRECTORY_INFO Listing, const UNICODE_STRING* Name, PDIRECTORY_ENTRY_METADATA Out)
{
    for (SIZE_T i = 0; i < Listing->FileCount; i++)
    {
        PDIRECTORY_FILE_METADATA file = BlorgGetFileEntry(Listing, i);

        if (!file)
        {
            break;
        }

        UNICODE_STRING entryName;
        entryName.Buffer = file->Name;
        entryName.Length = C_CAST(USHORT, file->NameLength * sizeof(WCHAR));
        entryName.MaximumLength = entryName.Length;

        if (RtlEqualUnicodeString(&entryName, Name, TRUE))
        {
            Out->Size = file->Size;
            Out->CreationTime = file->CreationTime;
            Out->LastAccessedTime = file->LastAccessedTime;
            Out->LastModifiedTime = file->LastModifiedTime;
            Out->IsDirectory = FALSE;
            return TRUE;
        }
    }

    for (SIZE_T i = 0; i < Listing->SubDirCount; i++)
    {
        PDIRECTORY_SUBDIR_METADATA sub = BlorgGetSubDirEntry(Listing, i);

        if (!sub)
        {
            break;
        }

        UNICODE_STRING entryName;
        entryName.Buffer = sub->Name;
        entryName.Length = C_CAST(USHORT, sub->NameLength * sizeof(WCHAR));
        entryName.MaximumLength = entryName.Length;

        if (RtlEqualUnicodeString(&entryName, Name, TRUE))
        {
            Out->Size = 0;
            Out->CreationTime = sub->CreationTime;
            Out->LastAccessedTime = sub->LastAccessedTime;
            Out->LastModifiedTime = sub->LastModifiedTime;
            Out->IsDirectory = TRUE;
            return TRUE;
        }
    }

    return FALSE;
}

//
//  Core IRP_MJ_CREATE handler for the volume device: resolves the target
//  path against the node table (one bucket probe under a shared push
//  lock, no VCB resource -- see the protocol note in Structs.c), falling
//  back to the path cache, then a parent's cached listing, then an async
//  network lookup (re-driven through the FSP via NET_DONE) before taking
//  the VCB resource exclusive for the cold tree work and handing off to
//  the appropriate Open*/OpenExisting* helper. Designed to re-run
//  top-to-bottom on each FSP pass, consuming any stashed async result at
//  entry so no early return can leak it. The warm path holds only the
//  node's pin (taken by BlorgNodeTableLookupPin, dropped by
//  BlorgNodeUnpin after the open helper returns); the unpin defers the
//  node to the reap worker when a failed or filtered open leaves it with
//  no handles. The path cache absorbs the shell's repeated probe storm
//  the same way it always has; the parent-listing probe takes a reference
//  to the parent's snapshot from the listing cache by path -- fresh only,
//  since a miss in it is answered as not-found outright -- and needs no
//  node at all, so it also answers for a parent nobody has open. Results
//  it and the network lookup produce are inserted with the ticket taken
//  before the read, so neither re-caches what an invalidation in between
//  removed (PATH_CACHE_TICKET). If neither table nor path cache nor listing
//  resolves the path, existence is verified on the remote store: on the
//  first pass the lookup is issued asynchronously and CreateComplete
//  stashes the result on the IRP and re-queues it with NET_DONE set, so
//  on the second pass the result is already in hand and this function
//  falls through to the existence checks and tree insert without another
//  network round trip. NET_DONE and its stash are consumed together as a
//  single shot at entry (BlorgClearIrpContextFlag): the same IRP can be
//  re-driven a third time by an oplock break (BlorgOplockComplete re-queues
//  it), and a still-set NET_DONE with the stash already freed would
//  dereference the NULLed DriverContext[1]. A re-drive after consumption
//  re-resolves normally -- node table, then the path cache this pass
//  already seeded. The async completion needs its own copy of the
//  resolved path (to seed the path cache), since filePath is freed before
//  the completion runs. Once BlorgHttpGetFileInformation is issued,
//  STATUS_PENDING means CreateComplete owns the IRP and netCtx and
//  frees both; any other (synchronous) result means the completion never
//  ran, so netCtx is freed here and the FSP worker loop completes the IRP
//  with the returned status. The cold path re-searches the tree under the
//  VCB resource exclusive, since another thread may have inserted the
//  node after the warm miss. Only a fully successful cold open publishes
//  the node into the table -- STATUS_SUCCESS exactly, not NT_SUCCESS,
//  which would also pass an oplock-pended STATUS_PENDING: a pended open
//  has taken no reference, so its node is left for the re-driven create
//  to re-resolve, and if it is a pre-existing zero-handle node it is
//  deferred to the reap worker (safe even though the re-drive may revive
//  it first: the worker re-checks both counts under the bucket lock, and
//  the parked IRP holds no node pointer, so a worker that wins re-creates
//  nothing stale) rather than stranded parked forever should the break
//  fail and the create never be re-driven. A failed open of a node
//  inserted by this pass reaps it
//  and any now-empty intermediate DCBs inline (fresh nodes are
//  unpublished -- unreachable by the lock-free path, so no pin can exist
//  and the free is safe under the exclusive hold), while a failed open of
//  a pre-existing zero-handle node is deferred to the reap worker, which
//  re-checks pins under the bucket lock before freeing.
//
//  A resident FCB answers on the warm path only while CreateFcbIsCurrent
//  holds. Otherwise its open resolves like a miss, and CreateFcbRefresh
//  brings the FCB up to date on the cold path before opening it.
//
//  A relative open (RelatedFileObject set -- OBJECT_ATTRIBUTES.RootDirectory
//  at the Nt layer) is the one shape where the full path has to be built
//  here rather than taken from FileObject->FileName, and both halves of
//  that join have a trap in them. The separator is conditional: the parent
//  handle's own name already ends in one when it is the root, and appending
//  a second produces a path ("\\leaf") that matches nothing the equivalent
//  absolute open resolves to. The destination offsets are byte offsets into
//  a PWCH, so they are cast rather than added to the pointer -- pointer
//  arithmetic on UNICODE_STRING.Buffer scales by sizeof(WCHAR) and would
//  place the leaf at twice its offset, past the end of the block for any
//  parent deeper than the root. joinedLength is computed in a ULONG because
//  the two USHORT lengths plus a separator can exceed what a UNICODE_STRING
//  can describe; a path that long is rejected rather than truncated into a
//  buffer smaller than what is about to be copied into it (STATUS_OBJECT_NAME_INVALID,
//  matching the leading-separator rejection just above it).
//
//  The KdBreakPoint() on the final fallthrough is deliberate and is the
//  only one left in the driver -- every other one was removed because it
//  trapped on outcomes that are normal (see CreateCheckFileAccess above and
//  deploy/DEBUGGING.md). This one is different: reaching the bottom of this
//  function means the create matched no case at all, which is a state the
//  logic above says cannot happen. If it ever does, stopping in the
//  debugger with the IRP still in hand is worth far more than the
//  STATUS_INVALID_DEVICE_REQUEST that follows it. Anyone grepping for
//  KdBreakPoint and finding one survivor should read this rather than
//  assume it was missed.
//
//  CreateHits is raised on the path-cache and cached-listing hits, which is
//  what FAT_STATISTICS means by the name: a create answered from what the
//  driver already had, with no request to the backend.
//
NTSTATUS BlorgVolumeCreate(PIRP Irp, PIO_STACK_LOCATION IrpSp, PDEVICE_OBJECT VolumeDeviceObject)
{
    struct OwnedString
    {
        BOOLEAN IsAllocated;
        UNICODE_STRING String;
    };

    PFILE_OBJECT fileObject = IrpSp->FileObject;
    PFILE_OBJECT relatedFileObject = fileObject->RelatedFileObject;

    struct OwnedString filePath = { 0 };
    PDCB parentDcb = BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb;
    ULONG options = IrpSp->Parameters.Create.Options;
    USHORT shareAccess = IrpSp->Parameters.Create.ShareAccess;
    UCHAR createDisposition = (options >> 24) & 0x000000ff;
    PACCESS_MASK desiredAccess = &IrpSp->Parameters.Create.SecurityContext->DesiredAccess;

    ULONG_PTR irpFlags = C_CAST(ULONG_PTR, Irp->Tail.Overlay.DriverContext[0]);
    DIRECTORY_ENTRY_METADATA dirEntInfo = { 0 };
    PATH_CACHE_TICKET resolvedTicket = { 0 };
    BOOLEAN haveDirEntInfo = FALSE;

    if (BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_NET_DONE))
    {
        BlorgClearIrpContextFlag(Irp, IRP_CONTEXT_FLAG_NET_DONE);
        ClearFlag(irpFlags, IRP_CONTEXT_FLAG_NET_DONE);

        PCREATE_NET_RESULT stash = Irp->Tail.Overlay.DriverContext[1];

        if (stash)
        {
            dirEntInfo = stash->Meta;

            if (!dirEntInfo.NoStore)
            {
                resolvedTicket = stash->Ticket;
            }

            ExFreePool(stash);
            Irp->Tail.Overlay.DriverContext[1] = NULL;
            haveDirEntInfo = TRUE;
        }
    }

    if (FILE_OPEN != createDisposition && FILE_OPEN_IF != createDisposition)
    {
        return STATUS_ACCESS_DENIED;
    }

    if (!relatedFileObject)
    {
        if (0 == fileObject->FileName.Length)
        {
            return CreateOpenVcb(Irp, fileObject, desiredAccess, shareAccess, BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->Vcb);
        }
        
        filePath.String = fileObject->FileName;
    }
    else
    {
        if ((0 < fileObject->FileName.Length) &&
            (L'\\' == fileObject->FileName.Buffer[0]))
        {
            return STATUS_OBJECT_NAME_INVALID;
        }

        if ((BLORGFS_DCB_SIGNATURE != GET_NODE_TYPE(relatedFileObject->FsContext))
            && (BLORGFS_ROOT_DCB_SIGNATURE != GET_NODE_TYPE(relatedFileObject->FsContext)))
        {
            return STATUS_INVALID_PARAMETER;
        }

        parentDcb = relatedFileObject->FsContext;

        USHORT parentLength = relatedFileObject->FileName.Length;

        BOOLEAN parentEndsWithSeparator = (0 < parentLength) &&
            (L'\\' == relatedFileObject->FileName.Buffer[(parentLength / sizeof(WCHAR)) - 1]);

        USHORT separatorLength = parentEndsWithSeparator ? 0 : C_CAST(USHORT, sizeof(WCHAR));

        ULONG joinedLength = parentLength + C_CAST(ULONG, separatorLength) + fileObject->FileName.Length;

        if (MAXUSHORT < joinedLength)
        {
            return STATUS_OBJECT_NAME_INVALID;
        }

        filePath.String.Buffer = ExAllocatePoolUninitialized(PagedPool, joinedLength, 'CRET');

        if (!filePath.String.Buffer)
        {
            return STATUS_NO_MEMORY;
        }

        filePath.IsAllocated = TRUE;

        RtlCopyMemory(filePath.String.Buffer, relatedFileObject->FileName.Buffer, parentLength);

        if (separatorLength)
        {
            filePath.String.Buffer[parentLength / sizeof(WCHAR)] = L'\\';
        }

        RtlCopyMemory(
            C_CAST(PUCHAR, filePath.String.Buffer) + parentLength + separatorLength,
            fileObject->FileName.Buffer,
            fileObject->FileName.Length);

        filePath.String.Length = C_CAST(USHORT, joinedLength);
        filePath.String.MaximumLength = C_CAST(USHORT, joinedLength);
    }

    if (((sizeof(WCHAR) * 2) <= filePath.String.Length) &&
        (L'\\' == filePath.String.Buffer[(filePath.String.Length / sizeof(WCHAR)) - 1]))
    {
        filePath.String.Length -= sizeof(WCHAR);
    }

    if (sizeof(WCHAR) == filePath.String.Length && L'\\' == filePath.String.Buffer[0])
    {
        if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
        {
            if (filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }

            return STATUS_FILE_IS_A_DIRECTORY;
        }

        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }

        return CreateOpenRootDcb(Irp, fileObject, desiredAccess, shareAccess, parentDcb, VolumeDeviceObject);
    }

    BLORGFS_PRINT(" ->NormalisedFileName             = %wZ\n", &filePath.String);

    PVCB vcb = BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->Vcb;

    PCOMMON_CONTEXT desiredNode = BlorgNodeTableLookupPin(&filePath.String);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                NTSTATUS result;

                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    result = STATUS_FILE_IS_A_DIRECTORY;
                }
                else
                {
                    result = CreateOpenExistingDcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PDCB, desiredNode), VolumeDeviceObject);
                }

                BlorgNodeUnpin(desiredNode);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                if (!CreateFcbIsCurrent(C_CAST(PFCB, desiredNode)))
                {
                    BlorgNodeUnpin(desiredNode);
                    break;
                }

                NTSTATUS result;

                if (BooleanFlagOn(options, FILE_DIRECTORY_FILE))
                {
                    result = STATUS_NOT_A_DIRECTORY;
                }
                else
                {
                    result = CreateOpenExistingFcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PFCB, desiredNode));
                }

                BlorgNodeUnpin(desiredNode);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
        }
    }

    if (!haveDirEntInfo)
    {
        BlorgPathCacheTakeTicket(&resolvedTicket);

        DIRECTORY_ENTRY_METADATA cached;
        PATH_CACHE_RESULT pc = BlorgPathCacheLookupDated(&filePath.String, &cached, &resolvedTicket);

        if (PathCacheExists == pc)
        {
            BLORGFS_STAT_INC(CreateHits);
            BLORGFS_LOG("Create path-cache HIT (exists): %wZ\n", &filePath.String);
            dirEntInfo = cached;
            haveDirEntInfo = TRUE;
        }
        else if (PathCacheNotFound == pc)
        {
            BLORGFS_LOG("Create path-cache HIT (not found): %wZ\n", &filePath.String);

            if (filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }

            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    if (!haveDirEntInfo)
    {
        UNICODE_STRING parentPath, leaf;

        if (CreateSplitPathLeaf(&filePath.String, &parentPath, &leaf))
        {
            if (0 == parentPath.Length)
            {
                parentPath = BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->RootDcb->FullPath;
            }

            PATH_CACHE_TICKET ticket;
            BlorgPathCacheTakeTicket(&ticket);

            PDIRECTORY_INFO listing = BlorgPathCacheLookupListing(&parentPath, FALSE, NULL, NULL, &ticket);

            if (listing)
            {
                BOOLEAN found = CreateFindEntryByName(listing, &leaf, &dirEntInfo);
                BlorgReleaseDirectoryInfo(listing);

                if (found)
                {
                    BLORGFS_STAT_INC(CreateHits);
                    BLORGFS_LOG("Create listing HIT (exists): %wZ\n", &filePath.String);
                    BlorgPathCacheInsertExists(&filePath.String, &dirEntInfo, &ticket);
                    resolvedTicket = ticket;
                    haveDirEntInfo = TRUE;
                }
                else
                {
                    BLORGFS_LOG("Create listing HIT (not found): %wZ\n", &filePath.String);
                    BlorgPathCacheInsertNotFound(&filePath.String, &ticket);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
            }
        }
    }

    if (!haveDirEntInfo)
    {
        if (!BooleanFlagOn(irpFlags, IRP_CONTEXT_FLAG_IN_FSP))
        {
            BLORGFS_PRINT("BlorgVolumeCreate: Enqueue to Fsp\n");

            if (filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }

            return BlorgFsdPostRequest(Irp, IrpSp);
        }

        BLORGFS_LOG("Create cache MISS -> network: %wZ\n", &filePath.String);

        PCREATE_NET_CONTEXT netCtx = ExAllocatePoolZero(NonPagedPoolNx, sizeof(CREATE_NET_CONTEXT), 'CRET');

        if (!netCtx)
        {
            if (filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        netCtx->Path.Buffer = ExAllocatePoolUninitialized(NonPagedPoolNx, filePath.String.Length, 'CRET');

        if (!netCtx->Path.Buffer)
        {
            ExFreePool(netCtx);
            if (filePath.IsAllocated)
            {
                ExFreePool(filePath.String.Buffer);
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(netCtx->Path.Buffer, filePath.String.Buffer, filePath.String.Length);
        netCtx->Path.Length = filePath.String.Length;
        netCtx->Path.MaximumLength = filePath.String.Length;
        netCtx->Irp = Irp;
        BlorgPathCacheTakeTicket(&netCtx->Ticket);

        NTSTATUS issueResult = BlorgHttpGetFileInformation(&filePath.String, CreateComplete, netCtx);

        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }

        if (STATUS_PENDING != issueResult)
        {
            ExFreePool(netCtx->Path.Buffer);
            ExFreePool(netCtx);
        }

        return issueResult;
    }

    NTSTATUS result;

    if (dirEntInfo.IsDirectory && BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
    {
        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }

        return STATUS_FILE_IS_A_DIRECTORY;
    }
    else if (!dirEntInfo.IsDirectory && BooleanFlagOn(options, FILE_DIRECTORY_FILE))
    {
        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }

        return STATUS_NOT_A_DIRECTORY;
    }

    ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);

    desiredNode = BlorgSearchByPath(parentDcb, &filePath.String);

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_NON_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_FILE_IS_A_DIRECTORY;
                }

                result = CreateOpenExistingDcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PDCB, desiredNode), VolumeDeviceObject);

                if (STATUS_SUCCESS == result)
                {
                    BlorgNodeTablePublish(desiredNode);
                }
                else
                {
                    BlorgNodeDeferReapIfIdle(desiredNode);
                }

                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                if (BooleanFlagOn(options, FILE_DIRECTORY_FILE))
                {
                    ExReleaseResourceLite(vcb->Header.Resource);

                    if (filePath.IsAllocated)
                    {
                        ExFreePool(filePath.String.Buffer);
                    }

                    return STATUS_NOT_A_DIRECTORY;
                }

                CreateFcbRefresh(C_CAST(PFCB, desiredNode), &dirEntInfo, &resolvedTicket, fileObject);

                result = CreateOpenExistingFcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PFCB, desiredNode));

                if (STATUS_SUCCESS == result)
                {
                    BlorgNodeTablePublish(desiredNode);
                }
                else
                {
                    BlorgNodeDeferReapIfIdle(desiredNode);
                }

                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
        }
    }

    result = BlorgInsertByPath(parentDcb, &filePath.String, &dirEntInfo, VolumeDeviceObject, &desiredNode);

    if (!NT_SUCCESS(result))
    {
        ExReleaseResourceLite(vcb->Header.Resource);

        if (filePath.IsAllocated)
        {
            ExFreePool(filePath.String.Buffer);
        }
        
        return result;
    }

    if (desiredNode)
    {
        switch (GET_NODE_TYPE(desiredNode))
        {
            case BLORGFS_DCB_SIGNATURE:
            {
                result = CreateOpenExistingDcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PDCB, desiredNode), VolumeDeviceObject);

                if (STATUS_SUCCESS == result)
                {
                    BlorgNodeTablePublish(desiredNode);
                }
                else
                {
                    PDCB orphanParentDcb = desiredNode->ParentDcb;
                    BlorgFreeFileContext(desiredNode, VolumeDeviceObject);
                    BlorgReapEmptyAncestorDcbs(orphanParentDcb, VolumeDeviceObject);
                }

                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
            case BLORGFS_FCB_SIGNATURE:
            {
                C_CAST(PFCB, desiredNode)->MetaTicket = resolvedTicket;

                result = CreateOpenExistingFcb(Irp, fileObject, desiredAccess, shareAccess, C_CAST(PFCB, desiredNode));

                if (STATUS_SUCCESS == result)
                {
                    BlorgNodeTablePublish(desiredNode);
                }
                else
                {
                    PDCB orphanParentDcb = desiredNode->ParentDcb;
                    BlorgFreeFileContext(desiredNode, VolumeDeviceObject);
                    BlorgReapEmptyAncestorDcbs(orphanParentDcb, VolumeDeviceObject);
                }

                ExReleaseResourceLite(vcb->Header.Resource);

                if (filePath.IsAllocated)
                {
                    ExFreePool(filePath.String.Buffer);
                }

                return result;
            }
        }
    }

    ExReleaseResourceLite(vcb->Header.Resource);

    if (filePath.IsAllocated)
    {
        ExFreePool(filePath.String.Buffer);
    }

    KdBreakPoint();
    return STATUS_INVALID_DEVICE_REQUEST;
}

//
//  IRP_MJ_CREATE handler for the disk device object: no real open
//  semantics, just reports success.
//
static NTSTATUS CreateDisk(PIRP Irp)
{
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

//
//  IRP_MJ_CREATE handler for the file system device object: no real open
//  semantics, just reports success.
//
static NTSTATUS CreateFileSystem(PIRP Irp)
{
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

//
//  IRP_MJ_CREATE dispatch entry: sets up the IRP context/top-level state,
//  dispatches by device type, and completes the IRP unless the volume
//  path returned STATUS_PENDING (async network lookup or FSP requeue in
//  flight).
//
//  SuccessfulCreates/FailedCreates are counted here rather than at each
//  return inside BlorgVolumeCreate: this is the one place a create's final
//  status is known, and a create that pends is counted by whichever
//  completion finishes it rather than twice.
//
//  The default case is unreachable through the I/O manager today -- only
//  this driver's three device objects carry its major table -- but the
//  cases above complete inside themselves, so a fourth kind would strand
//  the IRP rather than fail it. Hence the unconditional completion.
//
NTSTATUS BlorgCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS result = STATUS_INVALID_DEVICE_REQUEST;

    BOOLEAN topLevel = BlorgIsIrpTopLevel(Irp);

    FsRtlEnterFileSystem();
    switch (BlorgDeviceKind(DeviceObject))
    {
        case BlorgDeviceVolume:
        {
            BlorgSetupIrpContext(Irp, TRUE);
            result = BlorgVolumeCreate(Irp, irpSp, DeviceObject);
            if (STATUS_PENDING != result)
            {
                if (NT_SUCCESS(result))
                {
                    BLORGFS_STAT_INC(SuccessfulCreates);
                }
                else
                {
                    BLORGFS_STAT_INC(FailedCreates);
                }

                BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            }
            break;
        }
        case BlorgDeviceDisk:
        {
            result = CreateDisk(Irp);
            BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
        case BlorgDeviceFileSystem:
        {
            result = CreateFileSystem(Irp);
            BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }

        default:
        {
            BlorgCompleteRequest(Irp, result, IO_DISK_INCREMENT);
            break;
        }
    }
    FsRtlExitFileSystem();

    if (topLevel)
    {
        IoSetTopLevelIrp(NULL);
    }

    return result;
}