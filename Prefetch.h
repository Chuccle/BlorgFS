#pragma once

//
// Per-FCB sequential read prefetcher: declares the ring that keeps
// PREFETCH_DEPTH range GETs in flight ahead of a detected sequential
// read stream, so a paging read finds its chunk already resident (copy,
// no RTT) or in flight (park on it, partial RTT) instead of paying a
// full serialized HTTP round trip per chunk. Steady-state cost per
// chunk drops from RTT + transfer to max(transfer, (RTT + transfer) /
// DEPTH).
//
// IRQL/issuance rules:
//  - BlorgPrefetchServeRead / detach run at PASSIVE_LEVEL only (they are
//    called from the paging-read dispatch path / FCB teardown). All HTTP
//    fetch *issuance* happens there too: HttpBuildRequest's unicode->UTF8
//    conversion touches paged code, so fetches are never issued from the
//    DISPATCH-level fetch completions -- the pipeline is topped up on
//    each subsequent paging read instead.
//  - Fetch completions run at <= DISPATCH_LEVEL and touch only the ring
//    (NonPagedPoolNx) and the parked IRP -- never the FCB, which lives in
//    paged pool.
//  - Ring->Lock is a leaf lock: nothing else is ever acquired under it,
//    and no fetch is issued and no 256 KB copy performed while holding it
//    (buffers are detached from their slot under the lock and copied
//    outside it).
//
// Lifetime: RefCount = 1 for the FCB attachment + 1 per in-flight fetch.
// BlorgPrefetchDetach drops the attachment reference; whoever moves the
// count to zero frees the ring, so a late completion can never touch a
// freed ring. Generation invalidates stale in-flight data after a seek
// (completions compare their issue-time generation and discard), while
// parked waiters are always delivered regardless of generation -- a
// waiter parked on a slot wants exactly that slot's range.
//
// Pipeline keep-full rule: every serve outcome that consumes or bypasses
// ring coverage (hit, park, miss+re-aim) tops the pipeline back up before
// returning. Parking pumps too -- in the RTT-bound regime every read
// parks, and without a pump there the slots freed by completed waiters
// accumulated empty until the ring drained entirely and the stream paid a
// full re-aim burst every DEPTH chunks. The park-path pump runs BEFORE
// the waiter is published (BlorgPrefetchServeRead re-checks the slot
// after pumping): once the waiter is visible its completion can finish
// the IRP, and with it the FCB and the ring's attachment reference can
// go away -- the pump must never run after that handoff.
//
// Re-aim policy (multi-stream): the ring is a single pipeline, so two
// established streams on one file must not alternately steal it -- each
// steal discards fetched-ahead data the other stream was about to
// consume. A miss from a streaked reader only re-aims the ring if no
// serve has consumed ring coverage (hit or park) within the last
// PREFETCH_REAIM_IDLE_SERVES serve calls (FCB.StreamClock vs
// Ring->LastConsumeClock): an actively-consumed ring is left aimed where
// it is and the missing stream degrades to direct fetches, while a true
// seek (the ring's only consumer left) goes idle within two serves and
// re-aims almost immediately.
//
// Footprint scaling: a ring starts at PREFETCH_MIN_DEPTH slots and
// deepens by one per parked read up to PREFETCH_DEPTH (a park means the
// reader caught the pipeline, so it is too shallow for this stream's
// RTT x consumption rate; a low-RTT link that never parks stays
// shallow), with the extra slots' buffers allocated lazily by the next
// pump. The number of concurrent rings is capped driver-wide by system
// size (MmQuerySystemSize, Prefetch.c); at the cap a new stream gets no
// ring and degrades to direct fetches. The cap applies at arm time
// only -- a live ring feeding a reader is never shrunk or detached by
// the budget, so arming stream N+1 cannot stutter streams 1..N.
//

#define PREFETCH_DEPTH 8

//
// Slots a fresh ring starts with (and the count PrefetchCreateRing
// allocates buffers for up front). Two covers detection cost: the ring
// arms mid-stream, and the first post-arm read parking is what signals
// real depth demand -- growth to PREFETCH_DEPTH takes one park per slot,
// well inside a media player's startup buffering.
//
#define PREFETCH_MIN_DEPTH 2

//
// Derived from READ_AHEAD_GRANULARITY (Driver.h), not a standalone
// constant: Cc clusters its read-ahead miss into a paging IRP sized at
// 2x the granularity it was configured with, and slot lookup is an
// exact Length-fits check, so PREFETCH_CHUNK must track whatever that
// actual paging-read size is. Deriving it here means a granularity
// change can't silently desync the two the way a second hardcoded
// constant did.
//
#define PREFETCH_CHUNK (2 * READ_AHEAD_GRANULARITY)

//
// Reads-in-a-row (each starting where the last ended) before the ring is
// armed. 2 = second sequential read starts prefetching: one probe read
// costs nothing, but a real stream is caught at its second chunk.
//
#define PREFETCH_ARM_STREAK 2

//
// Serve calls without a ring hit/park before a streaked miss may re-aim
// the pipeline -- see the re-aim policy note above. 2 tolerates a second
// interleaved stream serving between two of the owner's consumes, while a
// post-seek re-aim (no consumer left, clock advancing every serve) waits
// at most this many direct-fetched reads.
//
#define PREFETCH_REAIM_IDLE_SERVES 2

// States for PREFETCH_SLOT_HOT.State.
enum
{
    PrefetchSlotEmpty = 0,    // unused, available for a new fetch
    PrefetchSlotInFlight = 1, // fetch issued, not yet complete
    PrefetchSlotReady = 2,    // fetch complete, buffer holds valid data
};

//
// Hot per-slot state for one prefetch fetch/buffer, scanned as a flat
// array under Ring->Lock on every paging read of a streaming file.
//
typedef struct _PREFETCH_SLOT_HOT
{
    ULONG64 RangeOffset;   // file offset this slot covers
    LONG    State;         // PrefetchSlot* (written under Ring->Lock)
    ULONG   Length;        // valid bytes (tail chunk may be short)
} PREFETCH_SLOT_HOT;

typedef struct _PREFETCH_RING PREFETCH_RING;

//
// Completion context for one in-flight fetch. One per slot, embedded in
// the ring -- no per-fetch allocations.
//
typedef struct _PREFETCH_FETCH_CTX
{
    PREFETCH_RING* Ring;    // ring that owns this fetch
    ULONG SlotIndex;        // which slot the fetch fills
    ULONG Generation;       // ring generation at issue time; stale means
                            //
                            // data predates a seek and is discarded
                            // unless a waiter is parked on it
                            //
} PREFETCH_FETCH_CTX;

//
// Per-FCB prefetch ring: PREFETCH_DEPTH slots of fetched/in-flight file
// data plus the state needed to issue, serve, and reclaim them.
//
struct _PREFETCH_RING
{
    // Hot: two contiguous cache lines at DEPTH 8, see PREFETCH_SLOT_HOT.
    PREFETCH_SLOT_HOT Hot[PREFETCH_DEPTH];

    KSPIN_LOCK Lock;              // guards all fields below and Hot[]
    ULONG64    NextFetchOffset;   // refill cursor (next unfetched chunk)
    ULONG64    LastConsumeClock;  // FCB.StreamClock at the last hit/park; feeds the re-aim recency test
    ULONG      Generation;        // bumped on seek; invalidates in-flight data issued under an older generation
    LONG       RefCount;          // FCB attachment + in-flight fetches

    //
    // Slots the pump may fill, PREFETCH_MIN_DEPTH..PREFETCH_DEPTH.
    // Bumped (under Lock) by a park in BlorgPrefetchServeRead; the next
    // pump allocates the newly admitted slots' buffers.
    //
    ULONG      DepthLimit;

    ULONG      Reserved;          // explicit pad keeping Buffers 8-aligned

    //
    // Cold: touched on hit-copy / issue / completion, not by the scan.
    // Buffers are PREFETCH_CHUNK NonPagedPoolNx, reused for every fetch
    // (their MDLs likewise built once) -- steady-state streaming
    // allocates nothing. The first PREFETCH_MIN_DEPTH are allocated with
    // the ring; the rest lazily by the pump as DepthLimit grows. A NULL
    // Buffers[i] with a non-NULL BufferMdls[i] means slot i's buffer is
    // temporarily detached for an outside-the-lock copy (the pump skips
    // such slots); NULL BufferMdls[i] means slot i has never been
    // allocated.
    //
    PCHAR Buffers[PREFETCH_DEPTH];       // per-slot fetch buffer
    PMDL  BufferMdls[PREFETCH_DEPTH];    // MDL describing each buffer
    PIRP  Waiters[PREFETCH_DEPTH];       // paging IRP parked on an in-flight slot, if any
    PREFETCH_FETCH_CTX FetchCtx[PREFETCH_DEPTH]; // completion context per slot
};

CHECK_PADDING_BETWEEN(PREFETCH_RING, Hot, Lock);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Lock, NextFetchOffset);
CHECK_PADDING_BETWEEN(PREFETCH_RING, NextFetchOffset, LastConsumeClock);
CHECK_PADDING_BETWEEN(PREFETCH_RING, LastConsumeClock, Generation);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Generation, RefCount);
CHECK_PADDING_BETWEEN(PREFETCH_RING, RefCount, DepthLimit);
CHECK_PADDING_BETWEEN(PREFETCH_RING, DepthLimit, Reserved);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Reserved, Buffers);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Buffers, BufferMdls);
CHECK_PADDING_BETWEEN(PREFETCH_RING, BufferMdls, Waiters);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Waiters, FetchCtx);
CHECK_PADDING_END(PREFETCH_RING, FetchCtx);

//
// Serve a paging read from the prefetch ring, PASSIVE_LEVEL only.
// Returns:
//   STATUS_SUCCESS   - data copied into Irp->MdlAddress and
//                      Irp->IoStatus.Information set; caller returns this
//                      and the dispatch wrapper completes the IRP.
//   STATUS_PENDING   - read parked on an in-flight fetch; the fetch
//                      completion copies and completes the IRP (same
//                      no-IoMarkIrpPending discipline as the existing
//                      inline async paging path).
//   STATUS_NOT_FOUND - no coverage; caller issues its own fetch as
//                      before. Also updates sequential detection and tops
//                      the pipeline back up as a side effect.
//
NTSTATUS BlorgPrefetchServeRead(struct _FCB* Fcb, PIRP Irp, ULONG64 Offset, ULONG Length);

//
// Detach and release the FCB's ring (FCB teardown, PASSIVE_LEVEL). Safe
// with fetches still in flight: the ring is freed by the last reference.
//
VOID BlorgPrefetchDetach(struct _FCB* Fcb);
