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
//    fetch *issuance* happens at PASSIVE too: HttpBuildRequest's
//    unicode->UTF8 conversion touches paged code, so fetches are never
//    issued directly from the DISPATCH-level fetch completions. The
//    pipeline is topped up two ways: on each paging read (serve path),
//    and by a per-ring PASSIVE work item that a successful fetch
//    completion queues whenever it empties a slot -- so a slot freed by
//    a delivered waiter is refilled immediately instead of waiting for
//    the next read to arrive (see the pump work-item fields below).
//  - Fetch completions run at <= DISPATCH_LEVEL and touch only the ring
//    (NonPagedPoolNx) and the parked IRP -- never the FCB, which lives in
//    paged pool. The pump work item honors the same rule: everything a
//    pump needs from the file (Path, FileSize) is snapshotted into the
//    ring at creation, so no pump -- read-path or work-item -- ever
//    races FCB teardown.
//  - Ring->Lock is a leaf lock: nothing else is ever acquired under it,
//    and no fetch is issued and no 256 KB copy performed while holding it
//    (buffers are detached from their slot under the lock and copied
//    outside it).
//
// Lifetime: RefCount = 1 for the FCB attachment + 1 per in-flight fetch
// + 1 while a pump work item is queued or running.
// BlorgPrefetchDetach drops the attachment reference; whoever moves the
// count to zero frees the ring, so a late completion (or pump worker)
// can never touch a freed ring. Generation invalidates stale in-flight
// data after a seek
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
// Slot lookup is a CONTAINMENT test: a read is served from slot i whenever
// [Offset, Offset + Length) lies inside [Hot[i].RangeOffset,
// RangeOffset + Hot[i].Length). It does not have to start on the slot
// boundary, so the hit path and the park path both carry an offset into
// the slot buffer (WaiterSlotOffsets for a park) rather than assuming the
// read wants the head of it.
//
// It was an exact-offset match until 2026-08-22, which made the ring
// useful only while the reader stayed in phase with the chunk grid -- any
// drift turned into a permanent stream of misses rather than a gradual
// falloff, because a read one byte off a boundary missed a slot holding
// every byte it wanted. PrefetchNearMisses (Statistics.h) counts reads
// that a containment test would have served and an exact one would not, so
// the cost of narrowing this again is measurable rather than argued.
//
// Sizing PREFETCH_CHUNK to the real paging-read size still matters for
// fetch efficiency, but it is no longer load-bearing for correctness of
// coverage the way it was under exact matching.
//
// The test is also the memory-safety boundary for the copy it admits, so
// PrefetchSlotCovers (Prefetch.c) is written to have no expression that can
// wrap for any input, rather than being correct only for offsets a sane
// caller would produce. Read.c rejects negative offsets at dispatch, but
// this must hold without that check having run.
//
// Pinned by InteriorOffsetCoveredByASlotIsServedFromWithinIt,
// InteriorParkIsDeliveredFromItsOffsetWithinTheSlot and
// WrappingOffsetIsNeverServedFromASlot in
// tests\sandbox\PrefetchKernelTest.cpp. The first two check the delivered
// bytes, not just the status: a containment lookup that forgot to offset
// the copy would still report success while handing back the wrong data,
// which is worse than the miss it replaced. The third drives the wrapping
// offsets past every slot base the ring could hold.
//
// Re-aim window test: an idle ring is only re-aimed if the read that
// missed falls OUTSIDE the range the ring is actively fetching, i.e.
// outside [NextFetchOffset - DepthLimit * PREFETCH_CHUNK,
// NextFetchOffset). A reader inside that window has not seeked -- it has
// simply outrun its own pipeline -- and re-aiming there is pure loss: it
// bumps Generation, discards the in-flight chunks, and so makes the
// following reads miss as well, which trips the idle test again. That
// feedback loop was measured at 234 of 234 re-aims on a single sequential
// stream, with fetched bytes running 1.78x file size.
//
// The window test rather than a plain "is the cursor ahead of the reader"
// test, because a BACKWARD seek also leaves the cursor ahead, and there
// re-aiming is exactly right. Inside the window means the pipeline is
// aimed correctly and the answer is to pump; outside it -- ahead of the
// cursor, or behind the window after a backward seek -- means the
// coverage is genuinely useless and the ring must be re-pointed.
//
// BackwardSeekStillReaimsEvenThoughTheCursorIsAhead in
// tests\sandbox\PrefetchKernelTest.cpp fails if this is ever simplified to
// the naive "cursor is ahead, do nothing" form, which would strand a
// backward-seeking reader on direct fetches forever.
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

    //
    // QPC stamp taken as the fetch is issued, so the completion can fold
    // this fetch into the same latency histogram the direct-fetch path
    // feeds (Statistics.h). A prefetch fetch has no IRP to hang the stamp
    // off the way BlorgVolumeRead uses DriverContext[2], and its latency
    // is the more interesting of the two: it is what decides whether the
    // pipeline can stay ahead of the reader.
    //
    LONG64 IssueQpc;

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
    LONG       RefCount;          // FCB attachment + in-flight fetches + queued/running pump work item

    //
    // Slots the pump may fill, PREFETCH_MIN_DEPTH..PREFETCH_DEPTH.
    // Bumped (under Lock) by a park in BlorgPrefetchServeRead; the next
    // pump allocates the newly admitted slots' buffers.
    //
    ULONG      DepthLimit;

    //
    // Completion-driven pump. A fetch completion that empties a slot
    // (waiter delivered, or a stale-generation discard) queues
    // PumpWorkItem so the slot is refilled at PASSIVE right away
    // instead of on the next paging read; failed fetches deliberately
    // do NOT queue it (the next read's pump is the retry, which keeps a
    // dead server from driving a fetch-fail-refetch storm).
    //
    //  - PumpQueued: interlocked dedup flag -- at most one queued work
    //    item at a time; cleared by the worker before it pumps, so a
    //    completion during the pump re-queues rather than being lost.
    //  - Detached (under Lock): set by BlorgPrefetchDetach so a
    //    late-running worker stops issuing fetches for a closed file.
    //    Purely an economy measure, not a lifetime one: a pump that
    //    races the flag touches only ring-owned state, and any fetches
    //    it issues hold ring references and complete into slots nothing
    //    will ever read -- wasted wire bytes, never a use-after-free.
    //
    LONG         PumpQueued;
    BOOLEAN      Detached;
    
    UCHAR        Reserved[7];  // explicit pad to 8-byte struct granule

    PIO_WORKITEM PumpWorkItem;

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

    //
    // Bytes Waiters[i] actually asked for, recorded at park time. Not
    // recoverable from the IRP at completion: Parameters.Read.Length is
    // the untrimmed request, while the read parked with the length
    // BlorgTrimReadToFileSize clipped to the FCB's current file size, and
    // the two differ for a read straddling EOF. Reading the untrimmed
    // value back would over-report IoStatus.Information whenever the
    // slot's fetch (sized off the ring's file-size snapshot) is longer
    // than what the reader may legitimately see.
    //
    ULONG WaiterLengths[PREFETCH_DEPTH];

    //
    // Where inside slot i's buffer Waiters[i]'s bytes begin, recorded at
    // park time. Nonzero whenever the read was admitted by containment
    // rather than by starting exactly on the slot boundary, so the
    // completion cannot assume the waiter wants the head of the buffer.
    //
    // The slot's Length at park time is the length the fetch was ISSUED
    // for; a short response can still come back, so the completion clamps
    // against what actually arrived rather than trusting the containment
    // test that admitted the park.
    //
    ULONG WaiterSlotOffsets[PREFETCH_DEPTH];
    PREFETCH_FETCH_CTX FetchCtx[PREFETCH_DEPTH]; // completion context per slot

    //
    // Ring-owned snapshot of the file identity the pump needs: an owned
    // NonPagedPoolNx copy of the FCB's FullPath and the file size at
    // ring-arm time. This is what lets every pump -- read-path and work
    // item alike -- run without dereferencing the FCB, so no pump can
    // ever race FCB teardown. The size snapshot only bounds how far the
    // pipeline fetches ahead; the FS is read-only against its backend,
    // and even a stale value merely means a tail chunk the reads then
    // fetch directly.
    //
    UNICODE_STRING Path;
    ULONG64        FileSize;
};

CHECK_PADDING_BETWEEN(PREFETCH_RING, Hot, Lock);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Lock, NextFetchOffset);
CHECK_PADDING_BETWEEN(PREFETCH_RING, NextFetchOffset, LastConsumeClock);
CHECK_PADDING_BETWEEN(PREFETCH_RING, LastConsumeClock, Generation);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Generation, RefCount);
CHECK_PADDING_BETWEEN(PREFETCH_RING, RefCount, DepthLimit);
CHECK_PADDING_BETWEEN(PREFETCH_RING, DepthLimit, PumpQueued);
CHECK_PADDING_BETWEEN(PREFETCH_RING, PumpQueued, Detached);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Detached, Reserved);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Reserved, PumpWorkItem);
CHECK_PADDING_BETWEEN(PREFETCH_RING, PumpWorkItem, Buffers);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Buffers, BufferMdls);
CHECK_PADDING_BETWEEN(PREFETCH_RING, BufferMdls, Waiters);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Waiters, WaiterLengths);
CHECK_PADDING_BETWEEN(PREFETCH_RING, WaiterLengths, WaiterSlotOffsets);
CHECK_PADDING_BETWEEN(PREFETCH_RING, WaiterSlotOffsets, FetchCtx);
CHECK_PADDING_BETWEEN(PREFETCH_RING, FetchCtx, Path);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Path, FileSize);
CHECK_PADDING_END(PREFETCH_RING, FileSize);

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
//                      the pipeline back up as a side effect. The miss is
//                      counted by the caller rather than here, because
//                      this returns STATUS_NOT_FOUND from several places
//                      (no ring yet, streak too short, lost the publish
//                      race) and the number worth having is the one that
//                      makes hits + parks + misses add up to the paging
//                      reads served.
//
NTSTATUS BlorgPrefetchServeRead(struct _FCB* Fcb, PIRP Irp, ULONG64 Offset, ULONG Length);

//
// Detach and release the FCB's ring (FCB teardown, PASSIVE_LEVEL). Safe
// with fetches still in flight: the ring is freed by the last reference.
//
VOID BlorgPrefetchDetach(struct _FCB* Fcb);

//
// Initializes the prefetcher's drain gate. Called once from DriverEntry,
// before any ring can be created.
//
VOID BlorgPrefetchInitialize(VOID);

//
// Blocks until every prefetch ring is gone -- which means no fetch is in
// flight and no pump work item is queued or running -- and refuses to arm
// any new ring. PASSIVE_LEVEL only, called once from DriverUnload before
// the device objects are torn down, since a queued pump work item is
// queued against the filesystem device object.
//
VOID BlorgPrefetchDrain(VOID);
