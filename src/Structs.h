#pragma once

//
// Core on-wire/in-memory structures for directory and file metadata, the
// FCB/DCB/CCB file-context objects and their device extensions, and the
// path-existence cache interface. CHECK_PADDING_* macros statically assert
// that structs below have no hidden compiler-inserted padding.
//

#ifdef PADDING_CHECKS

#include <assert.h>  // For static_assert
#include <stddef.h>  // For offsetof

#define CHECK_PADDING_BETWEEN(STRUCT_NAME, FIELD1, FIELD2)                                                                    \
static_assert(offsetof(STRUCT_NAME, FIELD2) == offsetof(STRUCT_NAME, FIELD1) + sizeof(C_CAST(STRUCT_NAME*, 0)->FIELD1), \
    "Padding detected between " #FIELD1 " and " #FIELD2 " members")

#define CHECK_PADDING_END(STRUCT_NAME, LAST_FIELD)                                                                     \
static_assert(sizeof(STRUCT_NAME) == offsetof(STRUCT_NAME, LAST_FIELD) + sizeof(C_CAST(STRUCT_NAME*, 0)->LAST_FIELD), \
    "Padding detected between " #LAST_FIELD " and end of struct")

#else

#define CHECK_PADDING_BETWEEN(STRUCT_NAME, FIELD1, FIELD2)

#define CHECK_PADDING_END(STRUCT_NAME, LAST_FIELD)

#endif

///////////////////////////////////////////////////////////
/////////// Structures for QueryFile operation ////////////
///////////////////////////////////////////////////////////

// Metadata for a single file/directory entry (size, timestamps, type).
typedef struct _DIRECTORY_ENTRY_METADATA
{
    ULONG64 Size;              // File size in bytes
    ULONG64 CreationTime;      // Creation time, NT FILETIME
    ULONG64 LastAccessedTime;  // Last access time, NT FILETIME
    ULONG64 LastModifiedTime;  // Last write time, NT FILETIME
    BOOLEAN IsDirectory;       // Nonzero if this entry is a directory
    UCHAR   Reserved[7];       // Pad to 8-byte alignment
} DIRECTORY_ENTRY_METADATA, * PDIRECTORY_ENTRY_METADATA;

CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY_METADATA, Size, CreationTime);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY_METADATA, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY_METADATA, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY_METADATA, LastModifiedTime, IsDirectory);
CHECK_PADDING_BETWEEN(DIRECTORY_ENTRY_METADATA, IsDirectory, Reserved);
CHECK_PADDING_END(DIRECTORY_ENTRY_METADATA, Reserved);

///////////////////////////////////////////////////////////
//////// Structures for ListDirectory operation ///////////
///////////////////////////////////////////////////////////

#define MAX_NAME_LEN 260

// A single file entry in a directory listing.
typedef struct _DIRECTORY_FILE_METADATA
{
    ULONG64 Size;              // File size in bytes
    ULONG64 CreationTime;      // Creation time, NT FILETIME
    ULONG64 LastAccessedTime;  // Last access time, NT FILETIME
    ULONG64 LastModifiedTime;  // Last write time, NT FILETIME
    SIZE_T  NameLength;        // Length of Name in characters
    WCHAR   Name[MAX_NAME_LEN];// File name
} DIRECTORY_FILE_METADATA, * PDIRECTORY_FILE_METADATA;

CHECK_PADDING_BETWEEN(DIRECTORY_FILE_METADATA, Size, CreationTime);
CHECK_PADDING_BETWEEN(DIRECTORY_FILE_METADATA, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_FILE_METADATA, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_FILE_METADATA, LastModifiedTime, NameLength);
CHECK_PADDING_BETWEEN(DIRECTORY_FILE_METADATA, NameLength, Name);
CHECK_PADDING_END(DIRECTORY_FILE_METADATA, Name);

// A single subdirectory entry in a directory listing.
typedef struct _DIRECTORY_SUBDIR_METADATA
{
    ULONG64 CreationTime;      // Creation time, NT FILETIME
    ULONG64 LastAccessedTime;  // Last access time, NT FILETIME
    ULONG64 LastModifiedTime;  // Last write time, NT FILETIME
    SIZE_T  NameLength;        // Length of Name in characters
    WCHAR   Name[MAX_NAME_LEN];// Directory name
} DIRECTORY_SUBDIR_METADATA, * PDIRECTORY_SUBDIR_METADATA;

CHECK_PADDING_BETWEEN(DIRECTORY_SUBDIR_METADATA, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_SUBDIR_METADATA, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(DIRECTORY_SUBDIR_METADATA, LastModifiedTime, NameLength);
CHECK_PADDING_BETWEEN(DIRECTORY_SUBDIR_METADATA, NameLength, Name);
CHECK_PADDING_END(DIRECTORY_SUBDIR_METADATA, Name);

//
// Header for a variable-length buffer holding a directory's file and
// subdirectory entries, packed contiguously after this struct.
//
typedef struct _DIRECTORY_INFO
{
    SIZE_T FilesOffset;   // Offset from start of this struct to first file entry
    SIZE_T SubDirsOffset; // Offset from start of this struct to first subdir entry
    SIZE_T FileCount;     // Number of DIRECTORY_FILE_METADATA entries
    SIZE_T SubDirCount;   // Number of DIRECTORY_SUBDIR_METADATA entries
} DIRECTORY_INFO, * PDIRECTORY_INFO;

CHECK_PADDING_BETWEEN(DIRECTORY_INFO, FilesOffset, SubDirsOffset);
CHECK_PADDING_BETWEEN(DIRECTORY_INFO, SubDirsOffset, FileCount);
CHECK_PADDING_BETWEEN(DIRECTORY_INFO, FileCount, SubDirCount);
CHECK_PADDING_END(DIRECTORY_INFO, SubDirCount);

//
// Returns a pointer to the Index'th subdirectory entry packed after this
// DIRECTORY_INFO, or NULL if Index is out of range.
//
inline PDIRECTORY_SUBDIR_METADATA BlorgGetSubDirEntry(PDIRECTORY_INFO DirInfo, SIZE_T Index)
{
    if (Index >= DirInfo->SubDirCount)
    {
        return NULL;
    }

    return C_CAST(PDIRECTORY_SUBDIR_METADATA,
        C_CAST(PUCHAR, DirInfo) +
        DirInfo->SubDirsOffset +
        (Index * sizeof(DIRECTORY_SUBDIR_METADATA))
        );
}

//
// Returns a pointer to the Index'th file entry packed after this
// DIRECTORY_INFO, or NULL if Index is out of range.
//
inline PDIRECTORY_FILE_METADATA BlorgGetFileEntry(PDIRECTORY_INFO DirInfo, SIZE_T Index)
{
    if (Index >= DirInfo->FileCount)
    {
        return NULL;
    }

    return C_CAST(PDIRECTORY_FILE_METADATA,
        C_CAST(PUCHAR, DirInfo) +
        DirInfo->FilesOffset +
        (Index * sizeof(DIRECTORY_FILE_METADATA))
        );
}

/////////////////////////////////////////////
/////// Structures for Read operation ///////
/////////////////////////////////////////////

//
// Holds file data read from the backend (or, for zero-copy MDL reads, just
// the transferred byte count with BodyBuffer/BaseAddress left NULL).
//
typedef struct _FILE_BUFFER
{
    PCHAR BodyBuffer;      // Allocated buffer backing the read, or NULL
    SIZE_T BodyBufferSize; // Size of BodyBuffer in bytes
    PCHAR BaseAddress;     // Start of the requested data within BodyBuffer, or NULL
} FILE_BUFFER, * PFILE_BUFFER;

CHECK_PADDING_BETWEEN(FILE_BUFFER, BodyBuffer, BodyBufferSize);
CHECK_PADDING_BETWEEN(FILE_BUFFER, BodyBufferSize, BaseAddress);
CHECK_PADDING_END(FILE_BUFFER, BaseAddress);

/////////////////////////////////////////////
///////FILE CONTEXT SECTION//////////////////
/////////////////////////////////////////////

// 0x8000 - 0xBFFF  reserved for 3rd party file systems
#define BLORGFS_FCB_SIGNATURE 0x8008
#define BLORGFS_DCB_SIGNATURE 0xB00B
#define BLORGFS_ROOT_DCB_SIGNATURE 0xBEEF
#define BLORGFS_VCB_SIGNATURE 0xB055
#define BLORGFS_CCB_SIGNATURE 0xBE55

#define GET_NODE_TYPE(Nodeptr) (*(C_CAST(USHORT*, Nodeptr)))

//
// Non-paged portion of a file/directory node: cache/MM section pointers and
// the synchronization objects FSRTL_ADVANCED_FCB_HEADER requires to live in
// non-paged memory.
//
typedef struct _NON_PAGED_NODE
{
    //
    // Set by MM/Cache Manager; the file object's SectionObject field points
    // here once the filesystem wires it up on open/create.
    //
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HdrFastMutex;         // Header synchronization (FastMutex variant)
    ERESOURCE  HdrResource;          // Header synchronization (Resource variant)
    ERESOURCE  HdrPagingIoResource;  // Serializes paging I/O against the header
} NON_PAGED_NODE, * PNON_PAGED_NODE;

CHECK_PADDING_BETWEEN(NON_PAGED_NODE, SectionObjectPointers, HdrFastMutex);
CHECK_PADDING_BETWEEN(NON_PAGED_NODE, HdrFastMutex, HdrResource);
CHECK_PADDING_BETWEEN(NON_PAGED_NODE, HdrResource, HdrPagingIoResource);
CHECK_PADDING_END(NON_PAGED_NODE, HdrPagingIoResource);

//
// Fields shared by every file-context node (FCB/DCB); embedded as the first
// member of both via DUMMYSTRUCTNAME so a COMMON_CONTEXT* aliases either.
//
typedef struct _COMMON_CONTEXT
{
    FSRTL_ADVANCED_FCB_HEADER Header; // Cache Manager / FsRtl header
    PNON_PAGED_NODE NonPaged;         // Non-paged sync objects for Header

    LIST_ENTRY Links;      // Linkage in the parent DCB's ChildrenList

    //
    // Node-table bucket linkage - Flink == NULL means the node
    // is unpublished: unreachable by the lock-free open path, so it can
    // carry no pins and may be freed inline under the VCB resource.
    // Linked/unlinked only under the owning bucket's push lock exclusive.
    //
    LIST_ENTRY TableLink;

    //
    // Deferred-reap list linkage, owned by the reap worker while
    // OnReapList != 0. A node is pushed at most once at a time: the
    // OnReapList claim (interlocked 0 -> 1) gates every push.
    //
    SINGLE_LIST_ENTRY ReapLink;

    UNICODE_STRING  FullPath; // Full path of this node

    PDEVICE_OBJECT VolumeDeviceObject; // Owning volume device object

    struct _DCB* ParentDcb; // Parent directory, or NULL for the root

    SHARE_ACCESS ShareAccess; // Share access state for open handles

    //
    // Transient references from the lock-free open path: taken under the
    // node's bucket push lock by BlorgNodeTableLookupPin, dropped by
    // BlorgNodeUnpin. A nonzero pin count blocks retirement; it never
    // affects share-access/oplock first-open semantics (that is RefCount).
    //
    LONG PinCount;

    ULONG64 CreationTime;     // Creation time, NT FILETIME
    ULONG64 LastAccessedTime; // Last access time, NT FILETIME
    ULONG64 LastModifiedTime; // Last write time, NT FILETIME

    LONG64 RefCount; // Open-handle count, updated via Interlocked ops

    LONG OnReapList; // Interlocked claim: nonzero while queued for the reap worker

    //
    // Node-table bucket index for FullPath, stamped once at node creation
    // (BlorgCreateFCB/BlorgCreateDCB) and immutable after, like FullPath
    // itself. Lets the hot count-drop paths (BlorgNodeUnpin/
    // BlorgNodeDereference), publish, retire, and the reap worker index
    // their bucket directly instead of re-hashing the full path on every
    // drop. Occupies what was the struct's 8-byte tail-alignment pad.
    //
    ULONG TableBucketIndex;
} COMMON_CONTEXT, * PCOMMON_CONTEXT;

CHECK_PADDING_BETWEEN(COMMON_CONTEXT, Header, NonPaged);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, NonPaged, Links);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, Links, TableLink);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, TableLink, ReapLink);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, ReapLink, FullPath);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, ShareAccess, PinCount);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, PinCount, CreationTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, LastModifiedTime, RefCount);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, RefCount, OnReapList);
CHECK_PADDING_BETWEEN(COMMON_CONTEXT, OnReapList, TableBucketIndex);
CHECK_PADDING_END(COMMON_CONTEXT, TableBucketIndex);


//
// One concurrent reader position on a file: where its last read ended and
// how many contiguous reads in a row it has issued. An FCB carries a small
// fixed array of these so interleaved readers (two streams on one file)
// each keep their own streak instead of resetting a single shared one --
// see ReadClaimStream (Read.c) for the claim/replace policy.
//
// These outlived the prefetch ring they were built for. They feed the
// ReadsSequential statistic, which is what established that this driver's
// paging reads are ~100% sequential, and an on-disk hot cache will want the
// same signal to decide what to admit (see the design note in README.md).
//
typedef struct _READ_STREAM_TRACKER
{
    ULONG64 End;    // End offset of this stream's last read
    ULONG64 Streak; // Consecutive contiguous reads observed
} READ_STREAM_TRACKER;

CHECK_PADDING_BETWEEN(READ_STREAM_TRACKER, End, Streak);
CHECK_PADDING_END(READ_STREAM_TRACKER, Streak);

//
// Sized so the whole tracker array is a single cache line: the per-read
// scan in ReadClaimStream touches exactly one line whichever tracker it
// lands on.
//
#define READ_STREAM_TRACKER_COUNT 4

//
// Per-file context node. Extends COMMON_CONTEXT with file locking and
// read-pattern tracking.
//
typedef struct _FCB BLORGFS_COMMON_CONTEXT_BASE
{
    BLORGFS_COMMON_CONTEXT_MEMBER
    FILE_LOCK       FileLock;       // Byte-range lock state
    PVOID           LazyWriteThread;// Thread performing lazy write, if any

    //
    // Read-pattern trackers, touched only at PASSIVE_LEVEL from the
    // paging-read dispatch path. A read starting where a tracker's last
    // read ended extends that tracker's streak; anything else claims the
    // coldest tracker for a new or seeked stream. Concurrent readers race
    // these plain fields harmlessly -- a lost update costs detection
    // accuracy, never correctness, which is why they carry no lock.
    //
    READ_STREAM_TRACKER Streams[READ_STREAM_TRACKER_COUNT]; // Per-stream sequentiality state

    //
    // Adaptive read-ahead granularity state, and why it lives on the FCB
    // rather than per file object.
    //
    // Cc splits this finer than it first appears. The shared cache map and
    // the section live on SECTION_OBJECT_POINTERS, which is per FCB, but
    // read-ahead state -- including the mask CcSetReadAheadGranularity
    // writes -- lives in FILE_OBJECT.PrivateCacheMap, which is per handle.
    // That is how Cc runs independent sequential detection for two handles
    // reading one file at different offsets.
    //
    // The state stays here anyway, and the deciding reason is the shrink
    // rule rather than the growth one. Read-ahead issued for one handle
    // fills the shared cache, and a second handle consumes those pages with
    // no paging read at all, so fetched-against-consumed is only coherent
    // where the cache is shared: split per handle, the first reader looks
    // wasteful and the second looks free. Amplification is what the shrink
    // rule exists to catch, and moving it per handle would make it worse.
    //
    // Growth is self-correcting across handles. A handle Cc has not been
    // told about issues reads sized by its own mask, so the
    // honoured-against-current test in ReadAdaptGranularity fails for it
    // and it does not grow -- it keeps the granularity it started with,
    // which is the safe direction: a player sharing a file with a copy is
    // left alone.
    //
    // What is NOT covered is the shrink direction. If a copy has grown this
    // FCB to 2 MB and a demuxer on the same file then votes shrink, the new
    // value is half of 2 MB and is set on the demuxer's handle, which was
    // sitting at the starting granule -- so a shrink vote raises it. That
    // needs two handles with opposite patterns on one file to reach, and
    // fixing it properly needs per-file-object state, which means a CCB for
    // file opens that this driver does not currently create.
    //
    // Windowed rather than cumulative: the counters reset at every
    // evaluation so the policy tracks what a reader is doing now, not what
    // it averaged since open. A player that seeks from streaming into
    // picking has to be able to change the answer.
    //
    // Unlocked for the same reason the trackers above are: concurrent
    // readers can lose an increment, which costs a delayed adaptation and
    // never correctness.
    //
    // Ordered widest-first so the ULONG does not sit before a ULONG64 and
    // introduce implicit padding; Reserved closes the tail explicitly
    // rather than widening ReadAheadGranularity to hide it.
    ULONG64 ReadAheadFetchedBytes;  // Paging bytes fetched this window
    ULONG64 ReadAheadConsumedBytes; // Bytes the application asked for this window

    //
    // When the last application-visible read on this file completed, so the
    // next one's arrival can be charged the gap. Zero until the first read
    // finishes, which is why the first read on a file records no idle
    // sample rather than one measured from an unrelated origin.
    //
    LONG64  ReadIdleLastEndQpc;

    //
    // This window's split between the consumer not asking and the driver
    // serving, in performance-counter ticks. Kept as ticks rather than
    // microseconds because only their ratio is ever read, and the ratio is
    // the same in either unit.
    //
    // Reset with the byte counters at every evaluation, for the same
    // reason: the policy acts on what a reader is doing now.
    //
    ULONG64 ReadIdleTicks;
    ULONG64 ReadBusyTicks;

    ULONG   ReadAheadGranularity;   // What Cc was last told, 0 = never set

    //
    // Consecutive windows voting the same way: positive to shrink, negative
    // to grow, zero when the last window was undecided. The policy acts
    // only on agreement, because a single window's ratio is not evidence.
    // Read-ahead runs ahead of consumption by construction, so within one
    // window fetched can exceed consumed even in a steady state that
    // averages 1.0 -- which made a one-window policy flap 170 times on a
    // sequential read whose pattern never changed.
    //
    LONG    ReadAheadAgreement;

    //
    // Largest paging read Cc has issued on this file during the current
    // window, which is how far it is willing to honour the granule.
    //
    // Cc does not read ahead in whatever size it is told. Measured, it caps
    // around 1.1 MB however high the granularity goes: the fetch count for
    // one 465 MB file was 418, 413, 421 and 412 at ceilings of 2, 4, 8 and
    // 16 MB. Growth past that point changes nothing it can act on, and this
    // is what lets the policy notice.
    //
    ULONG   ReadMaxPagingBytes;

    //
    // Closes the tail explicitly rather than widening a neighbour, which is
    // this file's rule for satisfying CHECK_PADDING_END.
    //
    ULONG   ReadAheadReserved;
} FCB, * PFCB;

CHECK_PADDING_BETWEEN(FCB, Header, NonPaged);
CHECK_PADDING_BETWEEN(FCB, NonPaged, Links);
CHECK_PADDING_BETWEEN(FCB, Links, TableLink);
CHECK_PADDING_BETWEEN(FCB, TableLink, ReapLink);
CHECK_PADDING_BETWEEN(FCB, ReapLink, FullPath);
CHECK_PADDING_BETWEEN(FCB, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(FCB, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(FCB, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(FCB, ShareAccess, PinCount);
CHECK_PADDING_BETWEEN(FCB, PinCount, CreationTime);
CHECK_PADDING_BETWEEN(FCB, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(FCB, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(FCB, LastModifiedTime, RefCount);
CHECK_PADDING_BETWEEN(FCB, RefCount, OnReapList);
CHECK_PADDING_BETWEEN(FCB, OnReapList, TableBucketIndex);
CHECK_PADDING_BETWEEN(FCB, TableBucketIndex, FileLock);
CHECK_PADDING_BETWEEN(FCB, FileLock, LazyWriteThread);
CHECK_PADDING_BETWEEN(FCB, LazyWriteThread, Streams);
CHECK_PADDING_BETWEEN(FCB, Streams, ReadAheadFetchedBytes);
CHECK_PADDING_BETWEEN(FCB, ReadAheadFetchedBytes, ReadAheadConsumedBytes);
CHECK_PADDING_BETWEEN(FCB, ReadAheadConsumedBytes, ReadIdleLastEndQpc);
CHECK_PADDING_BETWEEN(FCB, ReadIdleLastEndQpc, ReadIdleTicks);
CHECK_PADDING_BETWEEN(FCB, ReadIdleTicks, ReadBusyTicks);
CHECK_PADDING_BETWEEN(FCB, ReadBusyTicks, ReadAheadGranularity);
CHECK_PADDING_BETWEEN(FCB, ReadAheadGranularity, ReadAheadAgreement);
CHECK_PADDING_BETWEEN(FCB, ReadAheadAgreement, ReadMaxPagingBytes);
CHECK_PADDING_BETWEEN(FCB, ReadMaxPagingBytes, ReadAheadReserved);
CHECK_PADDING_END(FCB, ReadAheadReserved);

//
// Per-directory context node. Extends COMMON_CONTEXT with child linkage and
// a shared directory-listing cache.
//
typedef struct _DCB BLORGFS_COMMON_CONTEXT_BASE
{
    BLORGFS_COMMON_CONTEXT_MEMBER
    LIST_ENTRY ChildrenList; // Head of this directory's child node list

    //
    // Directory listing shared by every open handle on this directory.
    // Populated once (under Header.Resource) and reused until the last
    // handle closes, when the DCB and listing are freed together
    // (BlorgFreeFileContext). Borrowed (not owned) by per-handle CCBs via
    // CCB.Entries. Write-once: published NULL -> non-NULL exactly once
    // via WritePointerRelease (DirCtrlComplete) and never replaced.
    // BlorgVolumeCreate reads it with ReadPointerAcquire holding only the
    // VCB resource -- not this DCB's -- so it is the release/acquire pair,
    // not a common lock, that orders the listing's contents before the
    // pointer on weakly-ordered architectures (ARM64).
    //
    PDIRECTORY_INFO CachedListing;
} DCB, * PDCB;

CHECK_PADDING_BETWEEN(DCB, Header, NonPaged);
CHECK_PADDING_BETWEEN(DCB, NonPaged, Links);
CHECK_PADDING_BETWEEN(DCB, Links, TableLink);
CHECK_PADDING_BETWEEN(DCB, TableLink, ReapLink);
CHECK_PADDING_BETWEEN(DCB, ReapLink, FullPath);
CHECK_PADDING_BETWEEN(DCB, FullPath, VolumeDeviceObject);
CHECK_PADDING_BETWEEN(DCB, VolumeDeviceObject, ParentDcb);
CHECK_PADDING_BETWEEN(DCB, ParentDcb, ShareAccess);
CHECK_PADDING_BETWEEN(DCB, ShareAccess, PinCount);
CHECK_PADDING_BETWEEN(DCB, PinCount, CreationTime);
CHECK_PADDING_BETWEEN(DCB, CreationTime, LastAccessedTime);
CHECK_PADDING_BETWEEN(DCB, LastAccessedTime, LastModifiedTime);
CHECK_PADDING_BETWEEN(DCB, LastModifiedTime, RefCount);
CHECK_PADDING_BETWEEN(DCB, RefCount, OnReapList);
CHECK_PADDING_BETWEEN(DCB, OnReapList, TableBucketIndex);
CHECK_PADDING_BETWEEN(DCB, TableBucketIndex, ChildrenList);
CHECK_PADDING_BETWEEN(DCB, ChildrenList, CachedListing);
CHECK_PADDING_END(DCB, CachedListing);

// Per-handle context for an open directory search.
typedef struct _CCB
{
    ULONG NodeTypeCode;   // Node type identifier
    ULONG NodeByteSize;   // sizeof(CCB)
    ULONGLONG Flags;      // CCB_FLAG_* bits
    UINT64 CurrentIndex;  // Next entry index to return for this handle
    UNICODE_STRING SearchPattern; // Wildcard/name filter for this search
    PDIRECTORY_INFO Entries;      // Borrowed listing (see DCB.CachedListing)
} CCB, * PCCB;

#define CCB_FLAG_MATCH_ALL 0x0001

CHECK_PADDING_BETWEEN(CCB, NodeTypeCode, NodeByteSize);
CHECK_PADDING_BETWEEN(CCB, NodeByteSize, Flags);
CHECK_PADDING_BETWEEN(CCB, Flags, CurrentIndex);
CHECK_PADDING_BETWEEN(CCB, CurrentIndex, SearchPattern);
CHECK_PADDING_BETWEEN(CCB, SearchPattern, Entries);
CHECK_PADDING_END(CCB, Entries);

typedef FCB VCB;
typedef PFCB PVCB;

//
// All three set their out-pointer to NULL on entry and assign it only on
// the success return, so a caller that checked the status has a non-NULL
// node without re-testing. Stated in SAL rather than left to the comment:
// the walk in BlorgInsertByPath descends into a freshly created DCB with no
// further NULL check, and PREfast can only take that as proven from the
// annotation.
//
_Success_(return >= 0)
NTSTATUS BlorgCreateFCB(_Outptr_result_nullonfailure_ FCB** Fcb, CSHORT NodeType, const UNICODE_STRING* Name, const DEVICE_OBJECT* VolumeDeviceObject, ULONGLONG Size);

_Success_(return >= 0)
NTSTATUS BlorgCreateDCB(_Outptr_result_nullonfailure_ DCB** Dcb, CSHORT NodeType, const UNICODE_STRING* Name, const DEVICE_OBJECT* VolumeDeviceObject);

_Success_(return >= 0)
NTSTATUS BlorgCreateCCB(_Outptr_result_nullonfailure_ CCB** Ccb, const DEVICE_OBJECT* VolumeDeviceObject);
VOID BlorgFreeFileContext(PVOID Context, const DEVICE_OBJECT* VolumeDeviceObject);
VOID BlorgReapEmptyAncestorDcbs(PDCB Dcb, const DEVICE_OBJECT* VolumeDeviceObject);

PCOMMON_CONTEXT BlorgSearchByPath(const DCB* RootDcb, const UNICODE_STRING* Path);
NTSTATUS BlorgInsertByPath(PDCB RootDcb, const UNICODE_STRING* Path, const DIRECTORY_ENTRY_METADATA* DirEntryInfo, const DEVICE_OBJECT* VolumeDeviceObject, PCOMMON_CONTEXT* Out);

//
//  Node table (Structs.c): sharded push-locked hash keyed by FullPath,
//  mapping a path to its resident FCB/DCB so the open hot path resolves
//  in one bucket probe with no VCB acquire and no tree walk. Lifetime
//  protocol: lookups pin under the bucket lock; retirement (the only
//  free of a published node) happens on the deferred reap worker under
//  VCB exclusive + bucket exclusive with both counts re-checked, so a
//  held pin or handle always blocks the free. See the protocol note
//  above BlorgNodeTableLookupPin in Structs.c.
//
NTSTATUS BlorgNodeTableInit(PDEVICE_OBJECT VolumeDeviceObject);
VOID BlorgNodeTableTeardown(VOID);
PCOMMON_CONTEXT BlorgNodeTableLookupPin(const UNICODE_STRING* Path);
VOID BlorgNodeTablePublish(PCOMMON_CONTEXT Node);
VOID BlorgNodeUnpin(PCOMMON_CONTEXT Node);
VOID BlorgNodeDereference(PCOMMON_CONTEXT Node);
VOID BlorgNodeDeferReap(PCOMMON_CONTEXT Node);

//
// Defers an idle node (no handles, no pins) to the reap worker, taking the
// node's bucket lock shared around the idle test. For callers that hold no
// lock over the node's counts -- the failed-open arms in Create.c, whose
// bare RefCount read could otherwise be stale in the direction that
// strands a node forever.
//
VOID BlorgNodeDeferReapIfIdle(PCOMMON_CONTEXT Node);

//
//  Full-path resolution cache (PathCache.c). Memoizes create-time existence
//  results (exists+metadata / not-found) by full path, decoupled from node
//  lifetime. Sharded and push-locked for concurrent access.
//
typedef enum _PATH_CACHE_RESULT
{
    PathCacheMiss = 0,
    PathCacheExists,
    PathCacheNotFound
} PATH_CACHE_RESULT;

VOID BlorgPathCacheInit(VOID);
VOID BlorgPathCacheCleanup(VOID);
PATH_CACHE_RESULT BlorgPathCacheLookup(const UNICODE_STRING* Path, PDIRECTORY_ENTRY_METADATA Meta);
VOID BlorgPathCacheInsertExists(const UNICODE_STRING* Path, const DIRECTORY_ENTRY_METADATA* Meta);
VOID BlorgPathCacheInsertNotFound(const UNICODE_STRING* Path);

//
//  Invalidation. TTL keeps us eventually-consistent with the backing store
//  changing out of band; these drop entries early when we learn of a change
//  ourselves. Wire Invalidate/InvalidatePrefix to rename/delete and
//  directory-listing refresh once mutating SetInformation lands;
//  InvalidateAll is the O(1) wholesale flush for backend reconnect / remount.
//
VOID BlorgPathCacheInvalidate(const UNICODE_STRING* Path);
VOID BlorgPathCacheInvalidatePrefix(const UNICODE_STRING* Dir);
VOID BlorgPathCacheInvalidateAll(VOID);

/////////////////////////////////////////////
///////DEVICE EXTENSION SECTION//////////////
/////////////////////////////////////////////

//
// Device extension for the volume device object (VDO): lookaside lists for
// node allocation, the root directory, the VCB, and directory-change
// notification state.
//
typedef struct BLORGFS_VDO_DEVICE_EXTENSION
{
    NPAGED_LOOKASIDE_LIST NonPagedNodeLookasideList; // Allocates NON_PAGED_NODE
    PAGED_LOOKASIDE_LIST FcbLookasideList; // Allocates FCB
    PAGED_LOOKASIDE_LIST DcbLookasideList; // Allocates DCB
    PAGED_LOOKASIDE_LIST CcbLookasideList; // Allocates CCB
    PDCB RootDcb;       // Root directory node
    PVCB Vcb;            // Volume control block
    PNOTIFY_SYNC NotifySync; // Directory-change notification sync object
    LIST_ENTRY NotifyList;   // List of pending change-notification IRPs
} BLORGFS_VDO_DEVICE_EXTENSION, * PBLORGFS_VDO_DEVICE_EXTENSION;

CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, NonPagedNodeLookasideList, FcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, FcbLookasideList, DcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, DcbLookasideList, CcbLookasideList);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, CcbLookasideList, RootDcb);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, RootDcb, Vcb);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, Vcb, NotifySync);
CHECK_PADDING_BETWEEN(BLORGFS_VDO_DEVICE_EXTENSION, NotifySync, NotifyList);
CHECK_PADDING_END(BLORGFS_VDO_DEVICE_EXTENSION, NotifyList);

//
// The disk and file-system device objects carry no extension at all. Each
// used to hold one -- a type tag, plus (on the FSDO) the mounted volume
// pointer -- and both of those moved: the tag is gone entirely, replaced
// by BlorgDeviceKind's pointer comparison below, and the volume pointer
// lives in global (Driver.h) with the other two device pointers. Nothing
// was left, so both are now created with a zero-length extension.
//

//
// Reinterprets a volume device object's DeviceExtension as the VDO
// extension type. Caller is responsible for the object actually being a VDO.
//
inline PBLORGFS_VDO_DEVICE_EXTENSION BlorgGetVolumeDeviceExtension(const DEVICE_OBJECT* VolumeDeviceObject)
{
    return C_CAST(PBLORGFS_VDO_DEVICE_EXTENSION, VolumeDeviceObject->DeviceExtension);
}
