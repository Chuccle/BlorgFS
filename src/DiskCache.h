#pragma once

//
// The disk cache: 64 KB blocks of file content kept in one preallocated
// file on a local volume, so a block the Windows cache has dropped is read
// back from local disk instead of fetched again (DiskCache.c). Off unless
// the DiskCacheMb registry value sizes it.
//
// A block is stored under the file's path and the version its bytes belong
// to -- size and modification time, as the server's entity tag names them
// -- and served only to a read of that same version, so a file that
// changes on the server stops matching rather than serving old bytes. The
// index is not kept across a driver load: the file is reused, its contents
// are not.
//

#define DISK_CACHE_BLOCK_SHIFT 16
#define DISK_CACHE_BLOCK_SIZE  (1UL << DISK_CACHE_BLOCK_SHIFT)

//
// Largest cache the index is sized for. Each block costs its slot in
// nonpaged pool (DISK_CACHE_SLOT, plus a hash head and a ghost tag), about
// 80 bytes, so 16 GB holds 20 MB of pool.
//
#define DISK_CACHE_MAX_MB      16384u

#define DISK_CACHE_NO_SLOT     MAXULONG

//
// Tags a set of the ghost table holds. A direct-mapped table lost about a
// fifth of a 4096-block file to pairs of blocks evicting each other's tag
// on every pass, so neither was ever admitted; with four ways and half as
// many sets as chains, a set overflows only past several times as many
// distinct misses as the cache has slots.
//
#define DISK_CACHE_GHOST_WAYS 4

//
// Where the cache file lives unless the DiskCachePath registry value says
// otherwise. ProgramData is on the system volume and always exists; the
// BlorgFS directory under it is created with the file.
//
#define DISK_CACHE_DEFAULT_PATH L"\\??\\C:\\ProgramData\\BlorgFS\\BlockCache.bin"

//
// What a block is stored under. File is two independent hashes of the
// path, case-folded, so two files share a key only if both collide and
// their versions agree too.
//
typedef struct _DISK_CACHE_KEY
{
    ULONG64 File[2];      // Two independent hashes of the case-folded path
    ULONG64 Size;         // File size the block's bytes belong to
    ULONG64 ModifiedTime; // Modification time they belong to, 100-ns ticks since 1601
    ULONG64 Block;        // Offset in the file, in blocks
} DISK_CACHE_KEY, * PDISK_CACHE_KEY;

CHECK_PADDING_BETWEEN(DISK_CACHE_KEY, File, Size);
CHECK_PADDING_BETWEEN(DISK_CACHE_KEY, Size, ModifiedTime);
CHECK_PADDING_BETWEEN(DISK_CACHE_KEY, ModifiedTime, Block);
CHECK_PADDING_END(DISK_CACHE_KEY, Block);

typedef enum _DISK_CACHE_SLOT_STATE
{
    DiskCacheSlotFree,    // Holds nothing; reserved slots come from here first
    DiskCacheSlotFilling, // Reserved for Key, its write not yet done; never served
    DiskCacheSlotValid    // Holds Key's bytes on disk
} DISK_CACHE_SLOT_STATE;

typedef struct _DISK_CACHE_SLOT
{
    DISK_CACHE_KEY Key;   // What the slot holds or is being filled with
    ULONG Next;           // Next slot in the same hash chain, or DISK_CACHE_NO_SLOT
    LONG Pins;            // Reads and the fill in flight against the slot; never reused while nonzero
    UCHAR State;          // DISK_CACHE_SLOT_STATE
    UCHAR Referenced;     // Served since the clock hand last passed it
    UCHAR Reserved[6];    // explicit tail padding
} DISK_CACHE_SLOT, * PDISK_CACHE_SLOT;

CHECK_PADDING_BETWEEN(DISK_CACHE_SLOT, Key, Next);
CHECK_PADDING_BETWEEN(DISK_CACHE_SLOT, Next, Pins);
CHECK_PADDING_BETWEEN(DISK_CACHE_SLOT, Pins, State);
CHECK_PADDING_BETWEEN(DISK_CACHE_SLOT, State, Referenced);
CHECK_PADDING_BETWEEN(DISK_CACHE_SLOT, Referenced, Reserved);
CHECK_PADDING_END(DISK_CACHE_SLOT, Reserved);

//
// Which slot of the cache file holds which block (DiskCacheIndex.c). Slot i
// is the file's bytes [i * DISK_CACHE_BLOCK_SIZE, (i + 1) * ...). Every
// field is guarded by Lock, which is a spin lock because reads are served
// and fills admitted from fetch completions at DISPATCH_LEVEL.
//
typedef struct _DISK_CACHE_INDEX
{
    KSPIN_LOCK Lock;
    PDISK_CACHE_SLOT Slots; // SlotCount entries, NonPagedPoolNx
    PULONG Heads;           // Hash chain heads, HashMask + 1 of them
    PULONG64 Ghosts;        // Tags of blocks missed once, DISK_CACHE_GHOST_WAYS per set, newest first
    ULONG SlotCount;
    ULONG HashMask;         // Heads are a power of two long
    ULONG Hand;             // Next slot the clock looks at for a victim
    ULONG GhostMask;        // Ghost sets are a power of two long
} DISK_CACHE_INDEX, * PDISK_CACHE_INDEX;

CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, Lock, Slots);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, Slots, Heads);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, Heads, Ghosts);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, Ghosts, SlotCount);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, SlotCount, HashMask);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, HashMask, Hand);
CHECK_PADDING_BETWEEN(DISK_CACHE_INDEX, Hand, GhostMask);
CHECK_PADDING_END(DISK_CACHE_INDEX, GhostMask);

typedef enum _DISK_CACHE_ADMIT
{
    DiskCacheReserved,    // A slot is reserved for the block: write it, then commit
    DiskCacheHeld,        // Already held or being filled; nothing to write
    DiskCacheFirstMiss,   // Not admitted until it is missed again
    DiskCacheNoVictim     // Every slot is pinned
} DISK_CACHE_ADMIT;

//
// The index, which has no I/O of its own. Allocates the arrays for
// SlotCount slots, or fails leaving Index empty; Cleanup frees them and
// tolerates an empty index.
//
NTSTATUS BlorgDiskCacheIndexInitialize(PDISK_CACHE_INDEX Index, ULONG SlotCount);
VOID BlorgDiskCacheIndexCleanup(PDISK_CACHE_INDEX Index);

//
// Pins every block Key names from Key->Block to LastBlock, in order, into
// Slots, but only if every one of them is held: TRUE with all pinned, or
// FALSE with none. A pinned slot is not reused until unpinned, so its bytes
// stay Key's while a read is in flight against them. <= DISPATCH_LEVEL.
//
BOOLEAN BlorgDiskCacheIndexPinRange(PDISK_CACHE_INDEX Index, const DISK_CACHE_KEY* Key, ULONG64 LastBlock, PULONG Slots);
VOID BlorgDiskCacheIndexUnpin(PDISK_CACHE_INDEX Index, ULONG Slot);

//
// Reserves a slot to fill with Key's block, pinned, unless the block is
// already held, has not been missed before (second-miss admission: a block
// read once costs no write), or no slot is free of pins. The fill ends with
// Commit, which makes the slot servable if Written and frees it otherwise,
// and drops the pin. <= DISPATCH_LEVEL.
//
DISK_CACHE_ADMIT BlorgDiskCacheIndexReserve(PDISK_CACHE_INDEX Index, const DISK_CACHE_KEY* Key, PULONG Slot);
VOID BlorgDiskCacheIndexCommit(PDISK_CACHE_INDEX Index, ULONG Slot, BOOLEAN Written);

//
// The I/O side (DiskCache.c). Initialize opens or creates the cache file at
// Path and sizes it for SizeMb, at PASSIVE_LEVEL from DriverEntry; zero, or
// any failure, leaves the cache off and every call below a no-op. Cleanup
// waits for the cache's own I/O and closes the file, from DriverUnload.
//
NTSTATUS BlorgDiskCacheInitialize(const UNICODE_STRING* Path, ULONG SizeMb);
VOID BlorgDiskCacheCleanup(VOID);

//
// Records which file and version Node's reads are for, from its FCB, so a
// fetch completion at DISPATCH_LEVEL can key what it admits without the
// paged FCB, and sets Key to that file and version for the read at hand,
// all zero while the cache is off. The read looks itself up under Key, not
// under what Node records: another read of the same node, one whose handle
// predates a refresh of the FCB (Create.c), can record its own version
// there in between, and only costs the fills of the reads beside it.
// PASSIVE_LEVEL, before every read that may fetch or hit.
//
VOID BlorgDiskCacheNoteFile(PNON_PAGED_NODE Node, const UNICODE_STRING* Path, ULONG64 Size, ULONG64 ModifiedTime, PDISK_CACHE_KEY Key);

//
// Serves a non-cached read of Length bytes at Offset into Irp->MdlAddress
// from the cache, if every block it covers is held for the version Key
// names (BlorgDiskCacheNoteFile). TRUE means the read is under way and
// Completion will be called once with the IRP, its outcome and the Valid
// it was given, at <= DISPATCH_LEVEL; FALSE means nothing was started and
// the caller fetches as usual. Offset and Length must be page multiples;
// Valid is how much of the read lies before end of file. PASSIVE_LEVEL:
// BlorgVolumeRead issues a paging read inline only at PASSIVE_LEVEL and
// posts any other to the FSP.
//
typedef VOID DISK_CACHE_READ_COMPLETION(PIRP Irp, NTSTATUS Status, ULONG Valid);
typedef DISK_CACHE_READ_COMPLETION* PDISK_CACHE_READ_COMPLETION;

BOOLEAN BlorgDiskCacheRead(PIRP Irp, const DISK_CACHE_KEY* Key, ULONG64 Offset, ULONG Length, ULONG Valid, PDISK_CACHE_READ_COMPLETION Completion);

//
// Whether the cache is taking fills. A fetch made while it is lands in a
// buffer of the driver's own (BlorgHttpGetFile) rather than straight in
// the reader's pages: those may hold Mm's dummy page, which a mapped
// read's cluster puts in place of pages already resident, or a user
// buffer the application changes under the read, and the cache must keep
// the bytes the server sent, not what those pages hold afterwards.
//
BOOLEAN BlorgDiskCacheLive(VOID);

//
// Offers what a fetch just received into FileBuffer's body to the cache:
// Length bytes of Node's file at Offset, belonging to the version
// FileBuffer's entity tag names. Every whole block in it -- or the last
// block of the file, if the fetch reached end of file -- is copied and
// queued to be written, if admitted. Nothing is kept unless that version
// is the one Node's reads are for, nor from a fetch received straight into
// the reader's pages. A full write backlog or a failed allocation drops the
// rest of the body, each block counted as dropped: nothing this completion
// could do for the next block would succeed where that one failed.
// <= DISPATCH_LEVEL, from a fetch completion.
//
VOID BlorgDiskCacheAdmit(PNON_PAGED_NODE Node, const FILE_BUFFER* FileBuffer, ULONG64 Offset, ULONG Length);
