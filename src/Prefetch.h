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
//    (a chunk is taken out of its slot under the lock and copied outside
//    it). The chunk pool's own lock is a second leaf, never nested with
//    this one -- every acquire and release of a chunk happens with
//    Ring->Lock dropped -- so the two can be reasoned about separately
//    and there is no lock order between them to get wrong.
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
// A Ready slot serves MANY reads, not one. A hit copies out the bytes it
// wants and the slot stays Ready until a read consumes through its tail
// (slotOffset + Length reaching Hot[i].Length), at which point the chunk
// goes back to the pool.
//
// Retiring the slot on the first hit is what it used to do, and it was
// throwing most of every chunk away: PREFETCH_CHUNK is 512 KB, a clustered
// paging read is around 128 KB, so roughly three quarters of each fetched
// chunk was discarded -- and the very next sequential read, whose bytes
// were sitting in that discarded chunk, missed. Measured at 16 streams:
// 487 MB prefetched to serve 95 MB (19.5%), with an 81.9% miss rate.
//
// The chunk is still detached from its slot for the copy (slot to Empty,
// Chunks[i] to NULL) rather than copied under the lock, because the copy
// is ~128 KB at DISPATCH and Ring->Lock is a leaf that must not be held
// across it. A surviving slot is reinstated afterwards, but only if the
// slot is still Empty, still owns no chunk, and the generation has not
// moved -- a re-aim or a pump reservation during the copy wins, and the
// chunk goes back to the pool instead. That check is what keeps a
// reinstate from resurrecting a slot the ring has already moved past.
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
// every byte it wanted.
//
// PrefetchNearMisses (Statistics.h) was the counter that measured the cost
// of that exact match, and containment retired the case it counted. It
// still runs, and now finds the one other way a covered slot goes unserved:
// the slot is in flight with another reader already parked on it. Its name
// no longer describes it -- see PrefetchCountNearMiss in Prefetch.c before
// reading anything into the number.
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
// shallow).
//
// The driver-wide budget is denominated in chunks, not rings
// (PrefetchMaxChunks, Prefetch.c), because a chunk is where the memory
// actually is: a ring is a few hundred bytes of struct, a chunk is
// PREFETCH_CHUNK of NonPagedPoolNx. Slots take chunks from the shared
// pool as they reserve and hand them back the moment they are drained,
// so a stream holds transfer memory only for the fetches it currently
// has outstanding, and a stream that has read to EOF holds none.
//
// Budgeting the memory rather than the rings is what makes the scaling
// graceful. Rings are unbounded, so every stream arms; exhausting the
// chunk pool costs a stream depth, not its pipeline. Under pressure N
// streams each run shallower and all keep prefetching, where a ring cap
// would have given the first few full depth and the rest nothing at all.
//

#define PREFETCH_DEPTH 8

//
// Slots a fresh ring starts with. Two covers detection cost: the ring
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
// One PREFETCH_CHUNK of transfer memory plus the MDL describing it, the
// unit the driver-wide prefetch budget is denominated in.
//
// Buffer is NonPagedPoolNx and Mdl is built over it once, at allocation,
// so a chunk changing hands costs a list pop and nothing else -- no pool
// call, no MDL build, and legal at DISPATCH_LEVEL, which is where fetch
// completions release them.
//
// Link is live only while the chunk sits on the free list; a chunk held
// by a slot is reachable solely through that slot's Chunks[i], so the
// list head can never see a chunk two slots believe they own.
//
typedef struct _PREFETCH_CHUNK_BLOCK
{
    SINGLE_LIST_ENTRY Link;   // free-list linkage; meaningless while owned by a slot
    PCHAR             Buffer; // PREFETCH_CHUNK bytes of NonPagedPoolNx
    PMDL              Mdl;    // MDL over Buffer, built at allocation and never rebuilt

} PREFETCH_CHUNK_BLOCK;

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
    // pump tries to take chunks for the newly admitted slots, and leaves
    // them Empty for a later pump if the pool has none to give.
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
    // A slot owns a chunk exactly while it holds data or is fetching it
    // -- Chunks[i] is non-NULL if and only if Hot[i].State is InFlight or
    // Ready. An Empty slot owns nothing, so an idle ring costs its struct
    // and no chunk memory at all, which is what lets the driver-wide
    // budget be spent on the streams actually reading rather than
    // reserved by the ones that have finished.
    //
    // Chunks come from and return to the driver-wide free list
    // (PrefetchChunkAcquire / PrefetchChunkRelease in Prefetch.c), so
    // steady-state streaming still allocates nothing: a chunk released by
    // a slot that just served its bytes is the chunk the next reservation
    // picks up. The pool is what recycles them, not the ring.
    //
    PREFETCH_CHUNK_BLOCK* Chunks[PREFETCH_DEPTH];  // chunk backing slot i, or NULL if the slot is Empty
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
CHECK_PADDING_BETWEEN(PREFETCH_RING, PumpWorkItem, Chunks);
CHECK_PADDING_BETWEEN(PREFETCH_RING, Chunks, Waiters);
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

//
// Returns the driver-wide chunk pool's free memory to the system. Called
// by BlorgPrefetchDrain at unload; separately callable at any quiescent
// point, since it reclaims only chunks no slot owns.
//
VOID BlorgPrefetchReleaseChunkPool(VOID);

//
// Chunks the prefetcher currently has committed, owned and pooled alike.
// Read by the statistics snapshot; see BlorgPrefetchChunksLive.
//
LONG BlorgPrefetchChunksLive(VOID);

//
// The driver-wide chunk budget for this machine size. Exposed because it
// bounds concurrent prefetch fetches, which is what the keep-alive socket
// pool has to be able to hold warm (Socket.c) -- sizing the two
// independently is how the pool ended up below peak concurrency.
//
LONG BlorgPrefetchMaxChunks(VOID);
