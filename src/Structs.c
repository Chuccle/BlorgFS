#include "Driver.h"

//
//  Allocation/teardown for FCB/DCB/CCB node objects (lookaside-list based),
//  plus the in-memory path tree: name lookup, path search, and insertion
//  of new nodes along a path.
//

//
// Defined with the node table below; used at node creation to stamp
// COMMON_CONTEXT.TableBucketIndex.
//
static ULONG NodeTableBucketIndexFor(const UNICODE_STRING* Path);

//
// Defined below BlorgNodeDeferReapIfIdle, which is its only caller outside
// Unpin/Dereference's own idle-test path.
//
static VOID NodeDeferReapIfIdleLocked(PCOMMON_CONTEXT Node);

//
//  Allocates and initializes an FCB (file node) from the volume's
//  lookaside lists: non-paged header resources, paged node, copied name,
//  and advanced header/oplock setup. Zeroes exactly
//  nonPagedLookaside->L.Size bytes -- the same field
//  ExAllocateFromNPagedLookasideList's own SAL contract already ties the
//  returned buffer's writable size to -- instead of the separate (if by
//  construction equal, see ExInitializeNPagedLookasideList in Driver.c)
//  sizeof(NON_PAGED_NODE) literal, so PREfast can verify this directly
//  rather than needing to trust that the two never drift apart.
//
_Success_(return >= 0)
NTSTATUS BlorgCreateFCB(_Outptr_result_nullonfailure_ FCB** Fcb, CSHORT NodeType, const UNICODE_STRING* Name, const DEVICE_OBJECT* VolumeDeviceObject, ULONGLONG Size)
{
    *Fcb = NULL;

    PNPAGED_LOOKASIDE_LIST nonPagedLookaside = &BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList;
    PNON_PAGED_NODE nonPaged = ExAllocateFromNPagedLookasideList(nonPagedLookaside);

    if (!nonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nonPaged, nonPagedLookaside->L.Size);

    PFCB fcb = ExAllocateFromPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList);

    if (!fcb)
    {
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(fcb, sizeof(FCB));

    if (Name && (0 < Name->Length))
    {
        PWCHAR nameBuffer = ExAllocatePoolZero(PagedPool, Name->Length, 'FCB');

        if (!nameBuffer)
        {
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
            ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

        fcb->FullPath.Buffer = nameBuffer;
        fcb->FullPath.Length = Name->Length;
        fcb->FullPath.MaximumLength = Name->Length;
    }

    NTSTATUS result = ExInitializeResourceLite(&nonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        if (fcb->FullPath.Buffer)
        {
            ExFreePool(fcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&nonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&nonPaged->HdrResource);
        if (fcb->FullPath.Buffer)
        {
            ExFreePool(fcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    ExInitializeFastMutex(&nonPaged->HdrFastMutex);

    FsRtlInitializeFileLock(&fcb->FileLock, NULL, NULL);

    FsRtlInitializeOplock(&fcb->Header.Oplock);

    FsRtlSetupAdvancedHeader(&fcb->Header, &nonPaged->HdrFastMutex);

    fcb->Header.NodeTypeCode = NodeType;
    fcb->Header.NodeByteSize = sizeof(FCB);
    fcb->Header.FileSize.QuadPart = fcb->Header.AllocationSize.QuadPart = fcb->Header.ValidDataLength.QuadPart = Size;
    fcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    fcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    fcb->Header.Resource = &nonPaged->HdrResource;
    fcb->Header.PagingIoResource = &nonPaged->HdrPagingIoResource;

    fcb->NonPaged = nonPaged;
    fcb->VolumeDeviceObject = C_CAST(PDEVICE_OBJECT, VolumeDeviceObject);
    fcb->TableBucketIndex = NodeTableBucketIndexFor(&fcb->FullPath);

    *Fcb = fcb;

    return STATUS_SUCCESS;
}

//
// Allocates and initializes a DCB (directory node) from the volume's
// lookaside lists: non-paged header resources, paged node, copied name,
// and advanced header/oplock setup. Mirrors BlorgCreateFCB's allocation
// and rollback ordering.
//
_Success_(return >= 0)
NTSTATUS BlorgCreateDCB(_Outptr_result_nullonfailure_ DCB** Dcb, CSHORT NodeType, const UNICODE_STRING* Name, const DEVICE_OBJECT* VolumeDeviceObject)
{
    *Dcb = NULL;

    PNPAGED_LOOKASIDE_LIST nonPagedLookaside = &BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList;
    PNON_PAGED_NODE nonPaged = ExAllocateFromNPagedLookasideList(nonPagedLookaside);

    if (!nonPaged)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nonPaged, nonPagedLookaside->L.Size);

    PDCB dcb = ExAllocateFromPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList);

    if (!dcb)
    {
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(dcb, sizeof(DCB));

    InitializeListHead(&dcb->ChildrenList);

    PWCHAR nameBuffer = ExAllocatePoolUninitialized(PagedPool, Name->Length, 'DCB');

    if (!nameBuffer)
    {
        ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(nameBuffer, Name->Buffer, Name->Length);

    dcb->FullPath.Buffer = nameBuffer;
    dcb->FullPath.Length = Name->Length;
    dcb->FullPath.MaximumLength = Name->Length;

    NTSTATUS result = ExInitializeResourceLite(&nonPaged->HdrResource);

    if (!NT_SUCCESS(result))
    {
        if (dcb->FullPath.Buffer)
        {
            ExFreePool(dcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    result = ExInitializeResourceLite(&nonPaged->HdrPagingIoResource);

    if (!NT_SUCCESS(result))
    {
        ExDeleteResourceLite(&nonPaged->HdrResource);
        if (dcb->FullPath.Buffer)
        {
            ExFreePool(dcb->FullPath.Buffer);
        }
        ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
        ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, nonPaged);
        return result;
    }

    ExInitializeFastMutex(&nonPaged->HdrFastMutex);

    FsRtlInitializeOplock(&dcb->Header.Oplock);

    FsRtlSetupAdvancedHeader(&dcb->Header, &nonPaged->HdrFastMutex);

    dcb->Header.NodeTypeCode = NodeType;
    dcb->Header.NodeByteSize = sizeof(DCB);
    dcb->Header.IsFastIoPossible = FastIoIsQuestionable;
    dcb->Header.ValidDataLength.QuadPart = MAXLONGLONG;
    dcb->Header.Resource = &nonPaged->HdrResource;
    dcb->Header.PagingIoResource = &nonPaged->HdrPagingIoResource;

    dcb->NonPaged = nonPaged;
    dcb->VolumeDeviceObject = C_CAST(PDEVICE_OBJECT, VolumeDeviceObject);
    dcb->TableBucketIndex = NodeTableBucketIndexFor(&dcb->FullPath);

    *Dcb = dcb;

    return STATUS_SUCCESS;
}

//
// Allocates and zero-initializes a CCB (per-open context) from the
// volume's CCB lookaside list.
//
_Success_(return >= 0)
NTSTATUS BlorgCreateCCB(_Outptr_result_nullonfailure_ CCB** Ccb, const DEVICE_OBJECT* VolumeDeviceObject)
{
    *Ccb = NULL;

    PCCB ccb = ExAllocateFromPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->CcbLookasideList);

    if (!ccb)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ccb, sizeof(CCB));

    ccb->NodeTypeCode = BLORGFS_CCB_SIGNATURE;
    ccb->NodeByteSize = sizeof(CCB);
    ccb->SearchPattern.Buffer = NULL;

    *Ccb = ccb;

    return STATUS_SUCCESS;
}

#define DEALLOCATE_COMMON_CONTEXT(ctx)                                                                                               \
do                                                                                                                                   \
{                                                                                                                                    \
    PCOMMON_CONTEXT commonContext = ctx;                                                                                             \
    FsRtlTeardownPerStreamContexts(&commonContext->Header);                                                                          \
    ExDeleteResourceLite(&commonContext->NonPaged->HdrPagingIoResource);                                                             \
    ExDeleteResourceLite(&commonContext->NonPaged->HdrResource);                                                                     \
    ExFreeToNPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->NonPagedNodeLookasideList, commonContext->NonPaged);  \
}                                                                                                                                    \
while(0)

//
// Tears down and frees an FCB/DCB/VCB/root-DCB/CCB node back to its
// lookaside list, dispatching on node-type signature since the four
// common-context kinds share teardown but differ in extra per-type state
// (file locks, oplocks, cached listing, search pattern). For a CCB,
// ccb->Entries is a borrowed pointer into the DCB's CachedListing (owned
// and freed by the DCB), so it is NOT freed here.
//
void BlorgFreeFileContext(PVOID Context, const DEVICE_OBJECT* VolumeDeviceObject)
{
    switch (GET_NODE_TYPE(Context))
    {
        case BLORGFS_FCB_SIGNATURE:
        {
            PFCB fcb = Context;
            FsRtlUninitializeFileLock(&fcb->FileLock);
            FsRtlUninitializeOplock(&fcb->Header.Oplock);
            DEALLOCATE_COMMON_CONTEXT(Context);
            ExFreePool(fcb->FullPath.Buffer);
            RemoveEntryList(&(fcb->Links));
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, fcb);
            break;
        }
        case BLORGFS_DCB_SIGNATURE:
        {
            PDCB dcb = Context;
            FsRtlUninitializeOplock(&dcb->Header.Oplock);
            DEALLOCATE_COMMON_CONTEXT(Context);
            BlorgFreeHttpDirectoryInfo(dcb->CachedListing);
            ExFreePool(dcb->FullPath.Buffer);
            RemoveEntryList(&(dcb->Links));
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
            break;
        }
        case BLORGFS_VCB_SIGNATURE:
        {
            PVCB vcb = Context;
            FsRtlUninitializeFileLock(&vcb->FileLock);
            FsRtlUninitializeOplock(&vcb->Header.Oplock);
            DEALLOCATE_COMMON_CONTEXT(Context);
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->FcbLookasideList, vcb);
            break;
        }
        case BLORGFS_ROOT_DCB_SIGNATURE:
        {
            PDCB dcb = Context;
            FsRtlUninitializeOplock(&dcb->Header.Oplock);
            DEALLOCATE_COMMON_CONTEXT(Context);
            BlorgFreeHttpDirectoryInfo(dcb->CachedListing);
            ExFreePool(dcb->FullPath.Buffer);
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->DcbLookasideList, dcb);
            break;
        }
        case BLORGFS_CCB_SIGNATURE:
        {
            PCCB ccb = Context;
            if (ccb->SearchPattern.Buffer)
            {
                RtlFreeUnicodeString(&ccb->SearchPattern);
                RtlZeroMemory(&ccb->SearchPattern, sizeof(UNICODE_STRING));
            }
            ExFreeToPagedLookasideList(&BlorgGetVolumeDeviceExtension(VolumeDeviceObject)->CcbLookasideList, ccb);
            break;
        }
    }
}

//
//  Node table: sharded push-locked hash keyed by FullPath, mapping a path
//  to its resident FCB/DCB. The open hot path resolves in one bucket probe
//  under a shared push lock -- no VCB resource, no tree walk. The FCB/DCB
//  tree (ChildrenList/ParentDcb) survives purely for hierarchy work
//  (insert, ancestor reap, teardown), all of it under the VCB resource
//  exclusive, which therefore never appears on a warm open or a non-final
//  close.
//
//  Lifetime protocol -- every rule below exists so no agent can ever
//  dereference a freed node:
//
//   - A node becomes findable only via BlorgNodeTablePublish, called after
//     its first successful open (VCB exclusive held). An unpublished node
//     (TableLink.Flink == NULL) is unreachable by the lock-free path, so
//     it carries no pins and may be freed inline under the VCB resource.
//
//   - Lookups pin (PinCount, interlocked) while holding the bucket lock
//     shared; retirement checks and unlinks under the same bucket lock
//     exclusive, so a pin is either visible to the retirer or the pinner
//     is blocked until the node is unlinked (and then misses).
//
//   - All count drops (BlorgNodeUnpin / BlorgNodeDereference) happen under
//     the bucket lock shared. The only free of a published node happens on
//     the reap worker under bucket exclusive, so a dropper's node pointer
//     stays valid for the duration of its bucket hold.
//
//   - A node reaches the reap worker at most once at a time: the
//     OnReapList interlocked claim gates every push. The worker owns the
//     claim for popped nodes and either frees (counts zero, children
//     empty, unlinked) or drops the claim (in use, pinned, or children
//     remain). Every drop has a later transition that re-claims: the
//     last dereference, the last unpin, or the ancestor walk -- the
//     worker never retries a node itself, so it cannot spin on one.
//
//  Lock order: VCB resource, then bucket push lock. The reap-list spin
//  lock nests inside either. Bucket locks and node resources are never
//  held together.
//
//  Atomics discipline -- split by lock ownership. RefCount, PinCount, and
//  OnReapList have writers under different locks (opens mutate RefCount
//  under the node resource, closes under the bucket lock shared, pins
//  under the bucket lock), so no single lock owns any counter -- every
//  mutation is Interlocked*, and every cross-thread read goes through
//  ReadNoFence*. A read's exactness therefore never comes from the read
//  itself; it comes from the context each call site states: the bucket
//  lock exclusive (excludes all droppers), the VCB resource exclusive
//  (excludes the worker), or the reader's own immediately preceding
//  interlocked op (full barrier).
//
//  NodeReap.Queued/ShuttingDown are the opposite case, and are plain:
//  NodeReap.Lock owns every write to both (kick claim/rollback, worker
//  gate-clear, teardown latch), so atomics would only misstate that
//  ownership. Their check-and-act pairs live inside single locked
//  sections; the two accesses outside the lock are safe by direction --
//  teardown's ReadAcquire poll of Queued (a missed clear delays its exit,
//  never shortens it) and kick's latch read, which sits inside the same
//  locked section as its claim.
//

#define NODE_TABLE_BUCKETS 256u   // power of two

//
// One shard of the node table: an independently locked bucket of nodes.
// Sized and aligned to exactly one 64-byte cache line so contention on
// one bucket's push lock never falsely shares a line with its neighbours
// (unpadded, ~2.7 buckets shared each line).
//
typedef struct DECLSPEC_ALIGN(CACHE_LINE_SIZE) _NODE_TABLE_BUCKET
{
    EX_PUSH_LOCK Lock; // guards List membership; shared for lookup, exclusive for publish/retire
    LIST_ENTRY   List; // COMMON_CONTEXT.TableLink chain
    UCHAR        Reserved[40]; // explicit pad to the 64-byte line
} NODE_TABLE_BUCKET;

//
// One bucket per cache line. This is an absolute-size claim, so it holds
// only where EX_PUSH_LOCK is the kernel's pointer-sized push lock; a build
// that substitutes a fatter one simply gets larger buckets.
//
C_ASSERT(sizeof(EX_PUSH_LOCK) != sizeof(PVOID) ||
         CACHE_LINE_SIZE == sizeof(NODE_TABLE_BUCKET));

//
// Deferred-reap state: idle nodes (no handles, no pins) queue here and a
// work item retires them in batches under one VCB exclusive acquire --
// the FastFat DelayedClose shape, unified with the table so the worker is
// the single freer of published nodes.
//
typedef struct _NODE_REAP_STATE
{
    //
    // Push lock, not a spin lock: List links ReapLink fields embedded in
    // FCB/DCB nodes, which come from a paged lookaside list (Driver.c). A
    // spin lock would raise to DISPATCH_LEVEL for the push/pop and fault
    // touching that paged memory. Every caller (BlorgNodeDeferReap under
    // a bucket push lock shared, the delayed work item, and teardown)
    // already runs at PASSIVE, so nothing here needs IRQL raised.
    //
    EX_PUSH_LOCK      Lock;         // guards List
    SINGLE_LIST_ENTRY List;         // claimed nodes awaiting the worker
    PIO_WORKITEM      WorkItem;     // preallocated at volume create

    //
    // Interlocked gate: worker queued/running. NOT lock-owned -- unlike
    // the demoted Queued/ShuttingDown below, its writers span locks that
    // never nest (claimers hold bucket or VCB; the worker tail holds
    // NodeReap alone), so the atomic is load-bearing here.
    //
    LONG              Queued;

    LONG              ShuttingDown; // Latch: NodeReap.Lock owns every write; see BlorgNodeTableTeardown.
} NODE_REAP_STATE;

static NODE_TABLE_BUCKET NodeTable[NODE_TABLE_BUCKETS];
static NODE_REAP_STATE NodeReap;

static IO_WORKITEM_ROUTINE NodeReapWorker;

//
// Hashes Path (case-insensitive, matching BlorgArePathComponentsEqual's compare)
// to its bucket index. Same construction as PathCacheBucketIndex, including
// the manual fallback for RtlHashUnicodeString's argument-validation
// failures. Run once per node at creation (the result is stamped into
// COMMON_CONTEXT.TableBucketIndex) and once per by-path lookup
// (BlorgNodeTableLookupPin); every other bucket access indexes off the
// stamp instead of re-hashing.
//
static ULONG NodeTableBucketIndexFor(const UNICODE_STRING* Path)
{
    ULONG hash = 0;

    if (!NT_SUCCESS(RtlHashUnicodeString(Path, TRUE, HASH_STRING_ALGORITHM_DEFAULT, &hash)))
    {
        for (USHORT i = 0; i < Path->Length / sizeof(WCHAR); i++)
        {
            hash = (hash * 131u) + RtlUpcaseUnicodeChar(Path->Buffer[i]);
        }
    }

    return hash & (NODE_TABLE_BUCKETS - 1u);
}

//
// Queues the reap worker if it isn't already queued/running. The Queued
// gate makes kicks idempotent; the worker re-checks the list after
// clearing the gate, closing the push-while-running race.
//
// The claim and the ShuttingDown check are ONE locked decision under
// NodeReap.Lock -- the same baton every pusher and the drain hold --
// which linearizes this against teardown's latch-set the way the
// README's claim-under-the-baton rule requires. The previous shape
// claimed Queued with a bare CAS and then read ShuttingDown bare: two
// independently-atomic words with no ordering edge between the read
// and the other side's write, which on weakly ordered silicon is a
// TOCTOU window whose loss queues the work item teardown has already
// freed. Under the lock there is nothing to miss: either this kick ran
// before teardown's locked section and its queue is legitimate, or it
// runs after and observes the latch.
//
// Both flags are plain rather than interlocked for the same reason:
// NodeReap.Lock owns every write to them, so atomics would only obscure
// that ownership while costing a LOCK prefix.
//
// IRQL: every caller here is PASSIVE-guaranteed and holds a critical
// region -- closes via BlorgNodeDeferReap (inside FsRtlEnterFileSystem),
// the worker tail via its own KeEnterCriticalRegion after
// FsRtlExitFileSystem. Nothing on a DISPATCH completion chain reaches it.
// The push lock's <=APC requirement is therefore met on every path.
//
static VOID NodeReapKick(VOID)
{
    ExAcquirePushLockExclusive(&NodeReap.Lock);

    if (NodeReap.Queued)
    {
        ExReleasePushLockExclusive(&NodeReap.Lock);
        return;
    }

    if (ReadNoFence(&NodeReap.ShuttingDown))
    {
        ExReleasePushLockExclusive(&NodeReap.Lock);
        return;
    }

    NodeReap.Queued = TRUE;

    ExReleasePushLockExclusive(&NodeReap.Lock);

    IoQueueWorkItem(NodeReap.WorkItem, NodeReapWorker, DelayedWorkQueue, NULL);
}

//
// Claims Node for the reap list and queues the worker; a lost claim means
// the node is already queued. Caller must guarantee Node cannot be freed
// across this call: either it holds the VCB resource exclusive (blocking
// the worker), or it holds the node's bucket lock (blocking the worker's
// free of this node).
//
VOID BlorgNodeDeferReap(PCOMMON_CONTEXT Node)
{
    if (InterlockedCompareExchange(&Node->OnReapList, TRUE, FALSE))
    {
        return;
    }

    ExAcquirePushLockExclusive(&NodeReap.Lock);
    PushEntryList(&NodeReap.List, &Node->ReapLink);
    ExReleasePushLockExclusive(&NodeReap.Lock);

    NodeReapKick();
}

//
// Idle test + deferral under the node's own bucket lock shared. This is
// the entry point for callers that hold no lock over the node's counts
// (the failed-open arms in BlorgVolumeCreate): RefCount's writers hold the
// node resource and the bucket respectively, so a bare read outside this
// lock can be arbitrarily stale in BOTH directions -- a stale zero defers
// harmlessly (the worker re-checks), but a stale nonzero skips the defer
// and strands a node nothing will ever re-test. Under the bucket lock the
// decision shares its baton with every closer's drop.
//
// Callers holding the VCB resource exclusive are already inside the
// documented VCB -> bucket order; taking it shared here nests with every
// other bucket-shared holder.
//
VOID BlorgNodeDeferReapIfIdle(PCOMMON_CONTEXT Node)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[Node->TableBucketIndex];

    ExAcquirePushLockShared(&bucket->Lock);
    NodeDeferReapIfIdleLocked(Node);
    ExReleasePushLockShared(&bucket->Lock);
}

//
// Idle test + deferral, called with Node's bucket lock held (any mode).
// Unfenced count reads are exact enough here: the caller's own interlocked
// drop orders them, a racing pinner/opener makes this a false negative at
// worst (its own drop re-runs the same test), and the worker revalidates
// both counts under the bucket lock exclusive before freeing anything.
//
static VOID NodeDeferReapIfIdleLocked(PCOMMON_CONTEXT Node)
{
    if (0 == ReadNoFence64(&Node->RefCount) && 0 == ReadNoFence(&Node->PinCount))
    {
        BlorgNodeDeferReap(Node);
    }
}

//
// Atomic retirement gate for the synchronous reap paths (ancestor walk,
// failed cold open), called under the VCB resource exclusive: under the
// bucket lock exclusive, checks both counts, takes the OnReapList claim,
// and unlinks the node from its bucket. TRUE means the caller now owns
// the node and must free it. FALSE means it is in use, or already queued
// to the worker (which will retire it once its children are gone).
//
static BOOLEAN NodeTableTryRetire(PCOMMON_CONTEXT Node)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[Node->TableBucketIndex];
    BOOLEAN retired = FALSE;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&bucket->Lock);

    if (0 == ReadNoFence64(&Node->RefCount) &&
        0 == ReadNoFence(&Node->PinCount) &&
        !InterlockedCompareExchange(&Node->OnReapList, TRUE, FALSE))
    {
        if (Node->TableLink.Flink)
        {
            RemoveEntryList(&Node->TableLink);
            Node->TableLink.Flink = NULL;
        }

        retired = TRUE;
    }

    ExReleasePushLockExclusive(&bucket->Lock);
    KeLeaveCriticalRegion();

    return retired;
}

//
// Publishes Node into the table, making it findable by the lock-free open
// path. Called after the node's first successful open, under the VCB
// resource exclusive; idempotent for re-opens of an already-published node.
//
VOID BlorgNodeTablePublish(PCOMMON_CONTEXT Node)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[Node->TableBucketIndex];
    ExAcquirePushLockExclusive(&bucket->Lock);

    if (NULL == Node->TableLink.Flink)
    {
        InsertTailList(&bucket->List, &Node->TableLink);
    }

    ExReleasePushLockExclusive(&bucket->Lock);
}

//
// Resolves Path to its resident node in one bucket probe, pinning the node
// (under the bucket lock, so retirement cannot miss the pin) before
// returning it. Returns NULL on a miss; the caller falls back to the cold
// path. The pin must be dropped with BlorgNodeUnpin.
//
PCOMMON_CONTEXT BlorgNodeTableLookupPin(const UNICODE_STRING* Path)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[NodeTableBucketIndexFor(Path)];
    PCOMMON_CONTEXT found = NULL;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&bucket->Lock);

    for (PLIST_ENTRY entry = bucket->List.Flink; entry != &bucket->List; entry = entry->Flink)
    {
        PCOMMON_CONTEXT node = CONTAINING_RECORD(entry, COMMON_CONTEXT, TableLink);

        if (node->FullPath.Length == Path->Length &&
            RtlEqualUnicodeString(&node->FullPath, Path, TRUE))
        {
            InterlockedIncrement(&node->PinCount);
            found = node;
            break;
        }
    }

    ExReleasePushLockShared(&bucket->Lock);
    KeLeaveCriticalRegion();

    return found;
}

//
// Drops a lookup pin. Runs under the bucket lock shared so the node stays
// alive for the idle test (the worker's free needs this bucket exclusive),
// and defers the node to the reap worker when this pin was the last
// reference of any kind. This idle test is load-bearing twice over: it
// un-strands a parked node whose closer's reap attempt was blocked by
// this very pin, and it is the retry for a node the worker popped while
// pinned and dropped (the worker never retries a node itself).
//
VOID BlorgNodeUnpin(PCOMMON_CONTEXT Node)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[Node->TableBucketIndex];

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&bucket->Lock);

    InterlockedDecrement(&Node->PinCount);
    NodeDeferReapIfIdleLocked(Node);

    ExReleasePushLockShared(&bucket->Lock);
    KeLeaveCriticalRegion();
}

//
// Drops one open-handle reference (IRP_MJ_CLOSE for FCBs and non-root
// DCBs). Same bucket-shared discipline as BlorgNodeUnpin: the final
// dereference defers the node to the reap worker instead of taking the
// VCB resource exclusive on the close path -- non-final closes touch
// nothing global at all.
//
VOID BlorgNodeDereference(PCOMMON_CONTEXT Node)
{
    NODE_TABLE_BUCKET* bucket = &NodeTable[Node->TableBucketIndex];

    ExAcquirePushLockShared(&bucket->Lock);

    InterlockedDecrement64(&Node->RefCount);
    NodeDeferReapIfIdleLocked(Node);

    ExReleasePushLockShared(&bucket->Lock);
}

//
// Initializes the table buckets and the reap state, and preallocates the
// worker's IO_WORKITEM against the volume device object so a reap kick can
// never fail on allocation. Called from BlorgCreateVolumeDeviceObject.
//
NTSTATUS BlorgNodeTableInit(PDEVICE_OBJECT VolumeDeviceObject)
{
    for (ULONG i = 0; i < NODE_TABLE_BUCKETS; i++)
    {
        ExInitializePushLock(&NodeTable[i].Lock);
        InitializeListHead(&NodeTable[i].List);
    }

    ExInitializePushLock(&NodeReap.Lock);
    NodeReap.List.Next = NULL;
    NodeReap.Queued = 0;
    NodeReap.ShuttingDown = 0;
    NodeReap.WorkItem = IoAllocateWorkItem(VolumeDeviceObject);

    return NodeReap.WorkItem ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

//
// Volume teardown: suppresses further kicks, waits out an in-flight
// worker pass, then discards the queue (the nodes themselves are freed by
// FreeFileContextTree immediately after) and resets the buckets. Runs at
// PASSIVE after the FSP queue is drained, so no new pushes can race it.
//
VOID BlorgNodeTableTeardown(VOID)
{
    //
    // Latch teardown under the same lock the kicks claim through: a kick
    // that runs after this section observes ShuttingDown set and rolls
    // back, so nothing can queue the work item this function is about to
    // free. (See NODE_REAP_STATE for why the pairing lives under the
    // lock rather than in fence reasoning.)
    //
    if (NodeReap.ShuttingDown)
    {
        return;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&NodeReap.Lock);
    NodeReap.ShuttingDown = TRUE;
    ExReleasePushLockExclusive(&NodeReap.Lock);
    KeLeaveCriticalRegion();

    LARGE_INTEGER interval = { .QuadPart = -10LL * 10 * 1000 };

    while (ReadAcquire(&NodeReap.Queued))
    {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&NodeReap.Lock);
    PSINGLE_LIST_ENTRY entry = NodeReap.List.Next;
    NodeReap.List.Next = NULL;
    ExReleasePushLockExclusive(&NodeReap.Lock);
    KeLeaveCriticalRegion();

    while (entry)
    {
        PCOMMON_CONTEXT node = CONTAINING_RECORD(entry, COMMON_CONTEXT, ReapLink);
        entry = entry->Next;
        InterlockedExchange(&node->OnReapList, FALSE);
    }

    if (NodeReap.WorkItem)
    {
        IoFreeWorkItem(NodeReap.WorkItem);
        NodeReap.WorkItem = NULL;
    }

    for (ULONG i = 0; i < NODE_TABLE_BUCKETS; i++)
    {
        InitializeListHead(&NodeTable[i].List);
    }
}

//
// Walks upward from Dcb freeing each ancestor DCB that is now empty,
// unopened, and unpinned, stopping at the first node still in use, one
// already queued to the reap worker (NodeTableTryRetire loses the claim;
// the worker finishes the job), or the root (whose signature fails the
// type check). Shared by the reap worker, the failed-create paths
// (BlorgVolumeCreate / BlorgInsertByPath), and teardown -- a failed create
// never receives a close, so nothing else reaps its intermediates. Caller
// must hold the VCB resource exclusive: that is what makes the
// ChildrenList check and the tree unlink atomic against inserts, while
// NodeTableTryRetire makes the count checks atomic against the lock-free
// open path.
//
void BlorgReapEmptyAncestorDcbs(PDCB Dcb, const DEVICE_OBJECT* VolumeDeviceObject)
{
    while ((BLORGFS_DCB_SIGNATURE == GET_NODE_TYPE(Dcb)) &&
           IsListEmpty(&Dcb->ChildrenList) &&
           NodeTableTryRetire(C_CAST(PCOMMON_CONTEXT, Dcb)))
    {
        PDCB parentDcb = Dcb->ParentDcb;

        BlorgFreeFileContext(Dcb, VolumeDeviceObject);

        Dcb = parentDcb;
    }
}

//
// Deferred-reap worker: drains the queue and retires every idle node in
// one VCB-exclusive batch. Per node, under its bucket lock exclusive: an
// idle node (no handles, no pins, no children) is unlinked from the
// table and freed -- with its newly childless ancestors reaped -- outside
// the bucket lock but inside the VCB hold; anything else drops its claim
// and leaves the queue. Dropping rather than retrying is what keeps the
// worker from spinning on a transient state (a pin can legitimately wait
// out an HTTP round trip behind Cc read-ahead holding the node
// resource): every drop has a later transition that re-claims -- the
// last dereference (BlorgNodeDereference), the last unpin
// (BlorgNodeUnpin's idle test runs after every pin drop), or a parent's
// last child retirement (the ancestor walk). The claim hand-off is
// race-free because the drop happens under the bucket lock exclusive
// while every count drop and its idle test run under it shared. The
// gate-clear/recheck tail closes the race with pushes that arrived while
// the worker was running: both the clear and the List recheck run under
// NodeReap.Lock -- the same lock every pusher holds across its push --
// so a push is either visible to the recheck or the pusher's own kick
// call (made after its locked push) finds the gate free and queues the
// worker itself. No push can be missed by both.
//
static VOID NodeReapWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    PVCB vcb = BlorgGetVolumeDeviceExtension(DeviceObject)->Vcb;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&NodeReap.Lock);
    PSINGLE_LIST_ENTRY entry = NodeReap.List.Next;
    NodeReap.List.Next = NULL;
    ExReleasePushLockExclusive(&NodeReap.Lock);
    KeLeaveCriticalRegion();

    if (entry)
    {
        FsRtlEnterFileSystem();
        ExAcquireResourceExclusiveLite(vcb->Header.Resource, TRUE);

        while (entry)
        {
            PCOMMON_CONTEXT node = CONTAINING_RECORD(entry, COMMON_CONTEXT, ReapLink);
            entry = entry->Next;

            NODE_TABLE_BUCKET* bucket = &NodeTable[node->TableBucketIndex];
            BOOLEAN freeNode = FALSE;

            ExAcquirePushLockExclusive(&bucket->Lock);

            if ((0 == ReadNoFence64(&node->RefCount)) &&
                (0 == ReadNoFence(&node->PinCount)) &&
                ((BLORGFS_DCB_SIGNATURE != GET_NODE_TYPE(node)) ||
                 IsListEmpty(&C_CAST(PDCB, node)->ChildrenList)))
            {
                if (node->TableLink.Flink)
                {
                    RemoveEntryList(&node->TableLink);
                    node->TableLink.Flink = NULL;
                }

                freeNode = TRUE;
            }
            else
            {
                InterlockedExchange(&node->OnReapList, FALSE);
            }

            ExReleasePushLockExclusive(&bucket->Lock);

            if (freeNode)
            {
                PDCB parentDcb = node->ParentDcb;
                PDEVICE_OBJECT volumeDeviceObject = node->VolumeDeviceObject;

                BlorgFreeFileContext(node, volumeDeviceObject);
                BlorgReapEmptyAncestorDcbs(parentDcb, volumeDeviceObject);
            }
        }

        ExReleaseResourceLite(vcb->Header.Resource);
        FsRtlExitFileSystem();
    }

    //
    // Gate-clear and recheck under the reap lock: pushes publish List.Next
    // under this same lock, so a push that landed before this section is
    // visible to the recheck, and one that lands after finds Queued clear
    // and queues the worker through its own kick. (The previous barrier
    // argument for these two bare operations is what the baton replaces.)
    //
    // KeEnterCriticalRegion of its own: FsRtlExitFileSystem above already
    // released the dispatch region, and NodeReapKick's own push-lock
    // acquisition needs the <=APC guarantee.
    //
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&NodeReap.Lock);
    NodeReap.Queued = FALSE;
    BOOLEAN pending = (NULL != NodeReap.List.Next);
    ExReleasePushLockExclusive(&NodeReap.Lock);

    if (pending)
    {
        NodeReapKick();
    }

    KeLeaveCriticalRegion();
}

//
// Returns the final path component (substring, not a copy) of Path,
// skipping one trailing separator if present.
//
static UNICODE_STRING GetLastComponent(const UNICODE_STRING* Path)
{
    UNICODE_STRING lastComponent = { 0 };

    if (Path == NULL || Path->Length == 0 || Path->Buffer == NULL)
    {
        return lastComponent;
    }

    PWCHAR pathEnd = Path->Buffer + (Path->Length / sizeof(WCHAR)) - 1;
    PWCHAR current = pathEnd;

    if (*current == L'\\' && current > Path->Buffer)
    {
        current--;
        pathEnd = current;
    }

    while (current >= Path->Buffer)
    {
        if (*current == L'\\')
        {
            break;
        }
        current--;
    }

    PWCHAR start = (current < Path->Buffer) ? Path->Buffer : current + 1;

    USHORT componentLength = C_CAST(USHORT, (pathEnd - start + 1) * sizeof(WCHAR));

    if (componentLength > 0)
    {
        lastComponent.Length = componentLength;
        lastComponent.MaximumLength = componentLength;
        lastComponent.Buffer = start;
    }

    return lastComponent;
}

//
// Case-insensitive equality check for a single path component. Length
// check first as a cheap short-circuit before the NT string compare.
//
inline static BOOLEAN BlorgArePathComponentsEqual(const UNICODE_STRING* Component1, const UNICODE_STRING* Component2)
{
    if (Component1->Length != Component2->Length)
    {
        return FALSE;
    }

    return RtlEqualUnicodeString(Component1, Component2, TRUE);
}

//
// Linear scan of ParentDcb's immediate children for one whose last path
// component matches Name.
//
inline static PCOMMON_CONTEXT BlorgSearchByName(const DCB* ParentDcb, const UNICODE_STRING* Name)
{
    PCOMMON_CONTEXT child = NULL;
    UNICODE_STRING lastComponent;

    for (PLIST_ENTRY entry = ParentDcb->ChildrenList.Flink;
        entry != &ParentDcb->ChildrenList;
        entry = entry->Flink)
    {
        child = CONTAINING_RECORD(entry, COMMON_CONTEXT, Links);
        lastComponent = GetLastComponent(&child->FullPath);

        if (BlorgArePathComponentsEqual(Name, &lastComponent))
        {
            return child;
        }
    }

    return NULL;
}

//
// Walks the in-memory path tree from ParentDcb one component at a time
// (via FsRtlDissectName), descending into child DCBs. Returns NULL if any
// component is missing, or if an FCB is reached before the path is
// exhausted (a file can't have children).
//
PCOMMON_CONTEXT BlorgSearchByPath(const DCB* ParentDcb, const UNICODE_STRING* Path)
{
    const DCB* currentDcb = ParentDcb;
    UNICODE_STRING remainingPath = *Path;
    UNICODE_STRING component, nextRemainingPart, lastComponent;
    PCOMMON_CONTEXT child = NULL;
    PCOMMON_CONTEXT matchingChild = NULL;

    while (0 < remainingPath.Length)
    {
        FsRtlDissectName(remainingPath, &component, &nextRemainingPart);

        matchingChild = NULL;

        for (PLIST_ENTRY entry = currentDcb->ChildrenList.Flink;
            entry != &currentDcb->ChildrenList;
            entry = entry->Flink)
        {
            ASSERT(entry);

            child = CONTAINING_RECORD(entry, COMMON_CONTEXT, Links);
            lastComponent = GetLastComponent(&child->FullPath);

            if (BlorgArePathComponentsEqual(&component, &lastComponent))
            {
                matchingChild = child;
                break;
            }
        }

        if (!matchingChild)
        {
            return NULL;
        }

        if (BLORGFS_FCB_SIGNATURE == GET_NODE_TYPE(matchingChild))
        {
            return (nextRemainingPart.Length == 0) ? matchingChild : NULL;
        }

        currentDcb = C_CAST(PDCB, matchingChild);
        remainingPath = nextRemainingPart;
    }

    return C_CAST(PCOMMON_CONTEXT, currentDcb);
}

//
// Ensures every component of Path exists in the tree under ParentDcb,
// creating intermediate DCBs and a terminal FCB or DCB (per
// DirEntryInfo->IsDirectory) for any components not already present.
// *Out is the newly created terminal node, or NULL if the full path
// already existed. On a node-creation failure partway down, any
// intermediate DCBs created earlier in this walk that remain empty and
// unopened are reaped before returning, so a failed insert cannot strand
// zero-ref nodes in the tree. Caller must hold the VCB resource exclusive.
//
// An intermediate component that resolves to a resident FILE is rejected
// with STATUS_OBJECT_PATH_NOT_FOUND rather than descended into -- the
// same answer BlorgSearchByPath gives for the same shape, and the reason that
// check cannot be left to the caller: only DCB has a ChildrenList, so
// treating an FCB as the next directory walks a list head that overlaps
// the FCB's FILE_LOCK and dereferences whatever it holds. It is
// reachable without a hostile backend: a path that was a file when its
// FCB was created and is a directory by the time a child of it is
// opened, with the stale FCB still resident (open, or queued to the reap
// worker), lands here with a well-formed create for a real backend path.
// A terminal component matching an FCB is NOT this case -- the walk ends
// there with *Out NULL, the ordinary "already existed" result.
//
// currentDcb is non-NULL at every use: it starts as ParentDcb and is only
// ever reassigned from a node the loop just proved to be a directory, or
// from a BlorgCreateDCB the loop just proved succeeded (see the
// _Outptr_result_nullonfailure_ contract on it). PREfast cannot follow
// that across the loop's back edge -- it keeps the "may be NULL" fact
// from the NEXT iteration's freshly searched `existing` and applies it to
// the currentDcb aliased from the PREVIOUS one -- hence the single
// suppressed C28182 below, which is that stale alias and not a reachable
// dereference.
//
NTSTATUS BlorgInsertByPath(PDCB ParentDcb, const UNICODE_STRING* Path, const DIRECTORY_ENTRY_METADATA* DirEntryInfo, const DEVICE_OBJECT* VolumeDeviceObject, PCOMMON_CONTEXT* Out)
{
    *Out = NULL;
    UNICODE_STRING remainingPath = *Path;
    UNICODE_STRING firstPart, remainingPart;
    PDCB currentDcb = ParentDcb;
    PCOMMON_CONTEXT lastCreated = NULL;

    while (0 < remainingPath.Length)
    {
        FsRtlDissectName(remainingPath, &firstPart, &remainingPart);

        BOOLEAN isLastComponent = (0 == remainingPart.Length);
        PCOMMON_CONTEXT existing = BlorgSearchByName(currentDcb, &firstPart);

        if (existing)
        {
            if (isLastComponent)
            {
                break;
            }

            if ((BLORGFS_DCB_SIGNATURE != GET_NODE_TYPE(existing)) &&
                (BLORGFS_ROOT_DCB_SIGNATURE != GET_NODE_TYPE(existing)))
            {
                BlorgReapEmptyAncestorDcbs(currentDcb, VolumeDeviceObject);
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }

            currentDcb = C_CAST(PDCB, existing);
            remainingPath = remainingPart;
            continue;
        }

        NTSTATUS status;

        if (isLastComponent && !DirEntryInfo->IsDirectory)
        {
            PFCB newFcb;

            status = BlorgCreateFCB(&newFcb, BLORGFS_FCB_SIGNATURE, Path, VolumeDeviceObject, DirEntryInfo->Size);

            if (!NT_SUCCESS(status))
            {
                BlorgReapEmptyAncestorDcbs(currentDcb, VolumeDeviceObject);
                return status;
            }

            newFcb->LastAccessedTime = DirEntryInfo->LastAccessedTime;
            newFcb->LastModifiedTime = DirEntryInfo->LastModifiedTime;
            newFcb->CreationTime = DirEntryInfo->CreationTime;

            newFcb->ParentDcb = currentDcb;
            InsertTailList(&currentDcb->ChildrenList, &newFcb->Links);

            lastCreated = C_CAST(PCOMMON_CONTEXT, newFcb);
            break;
        }

        USHORT partialLength = isLastComponent
            ? Path->Length
            : C_CAST(USHORT, Path->Length - (remainingPart.Length + sizeof(WCHAR)));

        UNICODE_STRING partialPath =
        {
            .Length = partialLength,
            .MaximumLength = partialLength,
            .Buffer = Path->Buffer
        };

        PDCB newDcb;

        status = BlorgCreateDCB(&newDcb, BLORGFS_DCB_SIGNATURE, &partialPath, VolumeDeviceObject);

        if (!NT_SUCCESS(status))
        {
            BlorgReapEmptyAncestorDcbs(currentDcb, VolumeDeviceObject);
            return status;
        }

        newDcb->ParentDcb = currentDcb;

#pragma warning(suppress: 28182)
        InsertTailList(&currentDcb->ChildrenList, &newDcb->Links);

        lastCreated = C_CAST(PCOMMON_CONTEXT, newDcb);

        if (isLastComponent)
        {
            break;
        }

        currentDcb = newDcb;
        remainingPath = remainingPart;
    }

    *Out = lastCreated;
    return STATUS_SUCCESS;
}
