#pragma once

//
// Always-on filesystem statistics.
//
// Two surfaces over one set of counters:
//
//  - The standard one. FSCTL_FILESYSTEM_GET_STATISTICS and its _EX
//    variant, answered in the documented FILESYSTEM_STATISTICS /
//    FAT_STATISTICS shape, so `fsutil fsinfo statistics B:` and anything
//    else that already speaks this protocol works against this volume
//    with no bespoke tooling. FILESYSTEM_STATISTICS_TYPE_FAT is the
//    honest choice of the four documented types: this driver has no
//    NTFS/ReFS metadata concepts to report, and FAT_STATISTICS's
//    create/non-cached-read counters map cleanly onto what it does.
//
//  - The driver-specific one. IOCTL_BLORGFS_QUERY_STATISTICS, carrying
//    the counters no standard type has a field for: prefetch ring
//    hit/park/miss, chunk-fetch latency distribution, connection pool
//    behaviour, TLS record throughput. This is what turns "playback
//    stutters" into a number, and what the perf harness samples.
//
// Storage, and why it is shaped this way
// ---------------------------------------------------------------------
// One BLORGFS_STATISTICS entry per processor, each padded out to a
// multiple of 64 bytes, exactly as FILESYSTEM_STATISTICS's
// "SizeOfCompleteStructure must be a multiple of 64 bytes" contract
// implies. Two consequences, both deliberate:
//
//  - A CPU updates only its own entry, so counters are plain += rather
//    than interlocked. The read path takes these on every paging read;
//    a single global counter line would be dirtied by every core at once
//    and the cache-line ping-pong would cost more than the fetch being
//    measured.
//
//  - A thread preempted mid-increment and rescheduled elsewhere can lose
//    an update. That is the accepted trade for the above, and it is what
//    every in-box filesystem does with these counters: they are advisory
//    aggregates read over windows of thousands of operations, not ledger
//    entries. Anything that must be exact does not belong in this block.
//
// Gauges are the one exception. A value that goes up and down (fetches
// in flight) cannot be split across per-CPU entries, because the
// decrement can land on a different CPU than the increment and the sum
// would drift permanently. Those few live in BLORGFS_STATISTICS_GLOBAL
// and are interlocked; there is one increment per 512 KB fetch, so the
// shared line costs nothing at that rate.
//
// Compiled into two places, same arrangement as Tls.h:
//  - BlorgFS.vcxproj (kernel driver), which defines BLORGFS_KERNEL_BUILD
//    and includes this after Driver.h, so ntifs.h's FILESYSTEM_STATISTICS
//    and the kernel base types are already visible.
//  - PerfHarness.vcxproj (usermode console harness), a plain Win32
//    project that needs <windows.h>/<winioctl.h> first.
// Everything the two share must stay a plain POD of fixed-width fields.
//

#ifndef BLORGFS_KERNEL_BUILD
#include <windows.h>
#include <winioctl.h>
#endif

//
// Symbolic link the FSDO is published under so usermode can reach the
// vendor IOCTLs as \\.\BlorgFS. The device itself lives at
// BLORGFS_FSDO_STRING (Driver.h) in the object-namespace root, which
// CreateFile cannot name on its own. The standard statistics FSCTLs need
// none of this -- they go to the volume handle (\\.\B:) like any other
// filesystem.
//
#define BLORGFS_FSDO_SYMLINK_STRING L"\\DosDevices\\BlorgFS"
#define BLORGFS_FSDO_USERMODE_PATH  L"\\\\.\\BlorgFS"

//
// Chunk-fetch latency histogram, base-2 microsecond buckets: bucket i
// covers [2^(i-1), 2^i) microseconds, bucket 0 catches everything under
// 1 us, and the last bucket catches everything at or above 2^(N-2).
// A distribution rather than a mean because the number that decides
// whether playback stutters is the tail: a 99th-percentile 400 ms chunk
// is a visible glitch that a healthy mean hides completely.
//
// Sized so the top bucket sits above SOCKET_RECEIVE_TIMEOUT_MS (Socket.c,
// 30 s): 2^25 us = 33.5 s, so N - 2 = 25 and N = 27. That is the stated
// intent -- resolve everything from a warm local fetch up to a request
// dying on the receive watchdog -- and it only holds if the arithmetic is
// done in microseconds.
//
// This was 16, on a comment claiming sixteen buckets reached "~16 s". They
// reach 2^14 us, which is 16 MILLISECONDS -- a thousandfold error that put
// the top bucket below the median real fetch. Measured against the live
// backend, 33542 of 33821 samples (99.2%) landed in it, so p50, p90 and
// p99 all reported the same saturated bound and the histogram conveyed
// nothing at exactly the tail it exists to show.
//
#define BLORGFS_STATISTICS_LATENCY_BUCKETS 27

//
// Cache-line size the per-processor table is strided and aligned to. Also
// the multiple FILESYSTEM_STATISTICS requires SizeOfCompleteStructure to
// be, so one constant serves both: the wire contract and the false-sharing
// property are asking for the same number for the same reason.
//
#define BLORGFS_STATISTICS_LINE 64

//
// Internal per-processor counter block. Held in ULONG64 throughout,
// including for the fields that are reported as ULONG through the
// non-EX FSCTL: a media stream moves 4 GB in minutes, so 32-bit byte
// counters wrap almost immediately, and clamping at the reporting
// boundary keeps the wrap out of the source of truth. The first two
// groups mirror FILESYSTEM_STATISTICS_EX and FAT_STATISTICS field for
// field so the standard output can be built by straight assignment.
//
typedef struct _BLORGFS_STATISTICS
{
    // --- FILESYSTEM_STATISTICS(_EX) ------------------------------------
    ULONG64 UserFileReads;
    ULONG64 UserFileReadBytes;
    ULONG64 UserDiskReads;
    ULONG64 UserFileWrites;
    ULONG64 UserFileWriteBytes;
    ULONG64 UserDiskWrites;
    ULONG64 MetaDataReads;
    ULONG64 MetaDataReadBytes;
    ULONG64 MetaDataDiskReads;
    ULONG64 MetaDataWrites;
    ULONG64 MetaDataWriteBytes;
    ULONG64 MetaDataDiskWrites;

    // --- FAT_STATISTICS ------------------------------------------------
    ULONG64 CreateHits;
    ULONG64 SuccessfulCreates;
    ULONG64 FailedCreates;
    ULONG64 NonCachedReads;
    ULONG64 NonCachedReadBytes;
    ULONG64 NonCachedWrites;
    ULONG64 NonCachedWriteBytes;
    ULONG64 NonCachedDiskReads;
    ULONG64 NonCachedDiskWrites;

    // --- read dispatch, BlorgFS-specific -------------------------------
    ULONG64 ReadsCached;                 // served through Cc (copy or MDL)
    ULONG64 ReadsPagingInline;           // paging read issued on the calling thread
    ULONG64 ReadsPosted;                 // non-cached read posted to the FSP queue
    ULONG64 ReadsSequential;             // started exactly where a previous read ended
    ULONG64 ReadsEndOfFile;              // rejected at or past EOF

    // --- prefetch ring -------------------------------------------------
    ULONG64 PrefetchRingsArmed;
    ULONG64 PrefetchRingsRefused;        // arm refused; only the unload drain does this now, so a nonzero value means an arm raced dismount
    ULONG64 PrefetchHits;                // chunk already resident, copy only
    ULONG64 PrefetchParks;               // chunk in flight, read parked on it
    ULONG64 PrefetchMisses;              // no coverage, caller fetches directly
    ULONG64 PrefetchReaims;              // pipeline re-pointed after a seek
    ULONG64 PrefetchDepthGrowths;        // a park admitted one more slot
    ULONG64 PrefetchFetchesIssued;
    ULONG64 PrefetchFetchesFailed;
    ULONG64 PrefetchStaleDiscards;       // completed under a superseded generation
    ULONG64 PrefetchBytesServed;         // delivered out of ring buffers

    // --- HTTP chunk fetches --------------------------------------------
    ULONG64 FetchesIssued;               // direct (non-prefetch) chunk fetches
    ULONG64 FetchesCompleted;
    ULONG64 FetchesFailed;
    ULONG64 FetchBytes;
    ULONG64 FetchLatencySumUs;
    ULONG64 FetchLatencyMaxUs;
    ULONG64 FetchLatencyBuckets[BLORGFS_STATISTICS_LATENCY_BUCKETS];

    // --- metadata requests ---------------------------------------------
    ULONG64 DirInfoRequests;
    ULONG64 DirInfoFailures;
    ULONG64 DirInfoLatencySumUs;
    ULONG64 FileInfoRequests;
    ULONG64 FileInfoFailures;
    ULONG64 FileInfoLatencySumUs;

    // --- path cache ------------------------------------------------------
    ULONG64 PathCacheHits;
    ULONG64 PathCacheMisses;

    // --- connections -----------------------------------------------------
    ULONG64 ConnectionsPooled;           // acquire satisfied from the keep-alive pool
    ULONG64 ConnectionsFresh;            // acquire needed a new TCP connect
    ULONG64 ConnectionsFailed;
    ULONG64 ConnectionsReleasedToPool;
    ULONG64 ConnectionsClosedPoolFull;
    ULONG64 KeepAliveRetries;            // reused socket was dead, retried fresh
    ULONG64 SocketTimeouts;              // a per-operation watchdog fired

    // --- TLS -------------------------------------------------------------
    ULONG64 HandshakesStarted;
    ULONG64 HandshakesCompleted;
    ULONG64 HandshakesFailed;
    ULONG64 HandshakeLatencySumUs;
    ULONG64 HandshakeLatencyMaxUs;
    ULONG64 TlsRecordsDecrypted;
    ULONG64 TlsBytesDecrypted;
    ULONG64 TlsBulkReceives;             // wire receives feeding the accumulator

    //
    // Appended out of its topical group on purpose: this block is
    // append-only (see the query handler in DevIoCtrl.c), so a new counter
    // goes at the end even when it belongs with the prefetch counters
    // above.
    //
    // Reads that missed the ring while some slot's range did cover them.
    // The name predates the lookup becoming a containment test and now
    // overstates what it finds: the miss scan and the serve scan share
    // PrefetchSlotCovers, so a covered-but-unserved read is almost always
    // one thing -- the slot is in flight and another reader is already
    // parked on it, one waiter per slot.
    //
    // That makes this a contention counter, not a coverage one: two
    // readers on the same file chasing the same chunk, the second paying a
    // full round trip for bytes already on the wire. Read it against
    // PrefetchParks. See PrefetchCountNearMiss in Prefetch.c.
    //
    ULONG64 PrefetchNearMisses;

    //
    // Re-aims the idle test asked for and the pipeline-window test vetoed,
    // because the read being served still fell inside the range the ring
    // was actively fetching. Re-aiming there is pure loss: it bumps
    // Generation, discards in-flight chunks, and so makes the next reads
    // miss as well.
    //
    // The idle test alone cannot see this. It is gated on
    // streak >= PREFETCH_ARM_STREAK and a real seek resets the streak to 1,
    // so every re-aim fires on a currently-sequential stream -- either the
    // second read after a seek, where the ring genuinely points elsewhere,
    // or a reader that merely outran its own pipeline. Measured against the
    // live backend before the window test existed, 234 of 234 re-aims were
    // the second kind.
    //
    // This counts the fix doing its job, so it is expected to be nonzero;
    // what must stay at zero is wasted work, which shows up as
    // PrefetchStaleDiscards and as fetched bytes exceeding file size.
    //
    ULONG64 PrefetchReaimsSuppressed;

    //
    // Ring lifetime. Detached counts rings whose attachment reference was
    // dropped; Freed counts rings whose reference count then reached zero
    // and handed a slot back to the driver-wide budget.
    //
    // They exist because the budget was observed sitting at its cap while
    // every later stream was refused, and no existing counter could say
    // whether rings were never released or merely released late. They are
    // released late: the answer only showed up as Detached and Freed rising
    // together once a workload finished, because a ring outlives the handle
    // by however long the cache manager and MM keep the file object.
    //
    // Read them against PrefetchRingsArmed and the live gauge. Armed well
    // ahead of Freed with the gauge pinned at the cap is budget starvation,
    // and it is invisible without these two.
    //
    ULONG64 PrefetchRingsDetached;
    ULONG64 PrefetchRingsFreed;

    //
    // Times a pump asked the chunk pool for transfer memory and the
    // driver-wide chunk budget had none left to give.
    //
    // This is the counter that replaces PrefetchRingsRefused as the
    // pressure signal, and it means something materially different. A
    // refusal cost a stream its whole pipeline; a starvation costs one
    // slot of depth on one pump pass, and the next pass retries. So a
    // nonzero value here is not a fault -- it is the budget doing its job
    // -- and only a rate high enough to keep rings pinned near
    // PREFETCH_MIN_DEPTH indicates the cap is genuinely too low for the
    // offered concurrency.
    //
    ULONG64 PrefetchChunkStarvations;

    //
    // A file-read fetch split at the moment its response headers land:
    // TTFB is issue-to-headers, body is headers-to-last-byte, and Samples
    // counts the fetches that got far enough to contribute both.
    //
    // One issue-to-completion number cannot distinguish a server that is
    // slow to answer from a driver that is slow to take delivery, and that
    // ambiguity is currently the largest open question in this driver: a
    // 512 KB range GET measures ~20 ms end to end from usermode inside the
    // same guest, against ~98 ms here, with only two fetches in flight and
    // no contention to explain it. These two counters say which half.
    //
    // Compare against the usermode probe's split for the same request:
    // TTFB p50 5.4 ms, body p50 14 ms from the guest.
    //
    //
    // Issue-to-send-start: socket acquisition from the pool, any connect,
    // and any bounce to PASSIVE to build the request -- everything before
    // the request reaches the wire. A usermode client's TTFB does not
    // include this, so it is the first thing to subtract before comparing
    // the two.
    //
    ULONG64 FetchPreSendSumUs;
    ULONG64 FetchPreSendMaxUs;

    //
    // Send-start to send-completion, then send-completion to headers. The
    // second is the only piece directly comparable to a usermode client's
    // TTFB; the first is ours alone.
    //
    ULONG64 FetchSendSumUs;
    ULONG64 FetchSendMaxUs;
    ULONG64 FetchWaitSumUs;
    ULONG64 FetchWaitMaxUs;

    ULONG64 FetchTtfbSumUs;
    ULONG64 FetchTtfbMaxUs;
    ULONG64 FetchBodySumUs;
    ULONG64 FetchBodyMaxUs;
    ULONG64 FetchSplitSamples;

} BLORGFS_STATISTICS, * PBLORGFS_STATISTICS;

//
// Counters that cannot be split per-processor: a gauge's increment and
// decrement can run on different CPUs, so summing per-CPU entries would
// drift permanently. Interlocked, and cheap at one update per fetch.
//
typedef struct _BLORGFS_STATISTICS_GLOBAL
{
    LONG64 FetchesActive;
    LONG64 FetchesActivePeak;
    LONG64 PrefetchRingsLive;

    //
    // Chunks the prefetcher has committed, owned and pooled alike -- its
    // actual transfer footprint, capped by PrefetchMaxChunks.
    // PrefetchRingsLive no longer implies a footprint (an idle ring holds
    // no chunks), so this is the number to read for memory, and the two
    // together say how widely the pool is being shared out.
    //
    // NOT maintained here like the gauges around it, and reading it off
    // BlorgStatisticsGauges in driver or test code gets a permanent zero.
    // The prefetcher already counts chunks to enforce its budget, so this
    // is filled from that counter at snapshot time; call
    // BlorgPrefetchChunksLive() for a live value.
    //
    // Left as the prefetcher's own counter rather than folded in here
    // because it is an allocation budget, not a statistic: a statistics
    // reset must not be able to corrupt it.
    //
    LONG64 PrefetchChunksLive;
} BLORGFS_STATISTICS_GLOBAL, * PBLORGFS_STATISTICS_GLOBAL;

//
// Wire format for IOCTL_BLORGFS_QUERY_STATISTICS: the summed per-CPU
// counters, the gauges, and enough timebase to turn counts into rates.
// Version is checked by the driver so a stale harness fails loudly
// instead of misreading a struct whose tail moved.
//
#define BLORGFS_STATISTICS_VERSION 3

typedef struct _BLORGFS_STATISTICS_RESPONSE
{
    ULONG Version;                       // BLORGFS_STATISTICS_VERSION
    ULONG SizeOfStruct;                  // sizeof(BLORGFS_STATISTICS_RESPONSE)
    ULONG ProcessorCount;                // entries that were summed
    ULONG Reserved;                      // explicit pad to an 8-byte granule

    //
    // QueryPerformanceCounter frequency, the stamp taken at the last
    // reset, and the stamp taken while filling this response. Reported
    // rather than assumed so the harness computes rates over exactly the
    // window the driver observed, not over its own guess at when the
    // reset landed.
    //
    LONG64 QpcFrequency;
    LONG64 EpochQpc;
    LONG64 NowQpc;

    BLORGFS_STATISTICS Totals;
    BLORGFS_STATISTICS_GLOBAL Gauges;

} BLORGFS_STATISTICS_RESPONSE, * PBLORGFS_STATISTICS_RESPONSE;

//
//  Vendor IOCTLs on the FSDO. 0x800 is IOCTL_BLORGFS_SET_TLS_PIN
//  (DevIoCtrl.c); these continue that range. Device type is
//  FILE_DEVICE_UNKNOWN, not FILE_DEVICE_FILE_SYSTEM: the I/O manager
//  routes any IOCTL whose CTL_CODE device type is FILE_DEVICE_FILE_SYSTEM
//  as IRP_MJ_FILE_SYSTEM_CONTROL (an FSCTL) instead of
//  IRP_MJ_DEVICE_CONTROL, regardless of driver intent, so these would
//  never reach BlorgFsdoDeviceControl -- they would fall into FsCtrl.c's
//  unhandled-FSCTL default and return STATUS_INVALID_DEVICE_REQUEST every
//  time, which is exactly the failure this shape produced before the
//  device type was corrected.
//
//  QUERY is FILE_READ_ACCESS so an unelevated harness can sample a
//  running system -- the FSDO SDDL grants World GR. RESET is
//  FILE_WRITE_ACCESS because zeroing the window is destructive to anyone
//  else sampling concurrently, so it takes the same admin-only access the
//  pin update does.
//
#define IOCTL_BLORGFS_QUERY_STATISTICS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_BLORGFS_RESET_STATISTICS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#ifdef BLORGFS_KERNEL_BUILD

//
// Per-processor table and the gauges. BlorgStatisticsTable is NULL until
// BlorgStatisticsInitialize succeeds; every accessor tolerates that, so a
// statistics allocation failure costs the numbers and nothing else.
//
extern PBLORGFS_STATISTICS BlorgStatisticsTable;
extern ULONG BlorgStatisticsEntryStride;
extern ULONG BlorgStatisticsProcessorCount;
extern BLORGFS_STATISTICS_GLOBAL BlorgStatisticsGauges;

//
// This processor's counter block, or NULL if statistics are unavailable.
// The returned pointer is only valid for the current processor and only
// until this thread is rescheduled -- which is exactly the looseness the
// per-CPU design accepts (see the header comment). Callers take it, bump
// a field, and drop it; they must not cache it.
//
PBLORGFS_STATISTICS BlorgStatisticsForCurrentProcessor(VOID);

//
// Bumps one counter on the current processor's block by Value. The
// Field argument is the BLORGFS_STATISTICS member name, so a site reads
// as BLORGFS_STAT_ADD(PrefetchHits, 1) with no pointer bookkeeping and
// no cost when the table failed to allocate.
//
#define BLORGFS_STAT_ADD(Field, Value)                            \
    do {                                                          \
        PBLORGFS_STATISTICS statsBlock_ =                         \
            BlorgStatisticsForCurrentProcessor();                 \
        if (statsBlock_)                                          \
        {                                                         \
            statsBlock_->Field += (Value);                        \
        }                                                         \
    } while (0)

#define BLORGFS_STAT_INC(Field) BLORGFS_STAT_ADD(Field, 1)

//
// Records one completed operation's latency: adds to Sum, raises Max if
// this sample beats it, and bins it into Buckets when Buckets is
// non-NULL (the metadata paths pass NULL -- a sum is enough there and a
// histogram would be noise). ElapsedQpc is a raw
// QueryPerformanceCounter delta; the microsecond conversion happens here
// so no call site carries the frequency around. Runs on the per-CPU
// block, so it is as loose as every other counter here.
//
VOID BlorgStatisticsRecordLatency(
    ULONG64* Sum,
    ULONG64* Max,
    ULONG64* Buckets,
    LONG64 ElapsedQpc);

//
// Raw QueryPerformanceCounter stamp for the latency sites.
//
LONG64 BlorgStatisticsNow(VOID);

//
// Interlocked gauge helpers (see BLORGFS_STATISTICS_GLOBAL). Increment
// also carries the running peak forward.
//
VOID BlorgStatisticsGaugeIncrement(LONG64 volatile* Gauge, LONG64 volatile* Peak);
VOID BlorgStatisticsGaugeDecrement(LONG64 volatile* Gauge);

//
// Allocates the per-processor table (sized for the maximum processor
// count, so processor hot-add cannot walk off the end), captures the QPC
// frequency, and stamps the first epoch. Called once from DriverEntry.
// Failure is non-fatal and leaves every counter site a no-op.
//
NTSTATUS BlorgStatisticsInitialize(VOID);

// Frees the per-processor table. Called once from DriverUnload.
VOID BlorgStatisticsCleanup(VOID);

//
// Sums every processor's block into Out and stamps the timebase, for
// IOCTL_BLORGFS_QUERY_STATISTICS.
//
VOID BlorgStatisticsQuery(PBLORGFS_STATISTICS_RESPONSE Out);

// Zeroes every counter and restamps the epoch, starting a fresh window.
VOID BlorgStatisticsReset(VOID);

//
// Fills OutputBuffer in the documented FILESYSTEM_STATISTICS (Extended ==
// FALSE) or FILESYSTEM_STATISTICS_EX (Extended == TRUE) layout: one
// 64-byte-aligned entry per processor, each the common header followed by
// FAT_STATISTICS. Returns STATUS_SUCCESS when the whole array fit,
// STATUS_BUFFER_OVERFLOW when only part of it did (with *BytesReturned
// set to what was written, matching what callers of these FSCTLs expect),
// or STATUS_BUFFER_TOO_SMALL when not even one header fits.
//
NTSTATUS BlorgStatisticsFillFsctlBuffer(
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN Extended,
    PULONG BytesReturned);

#endif // BLORGFS_KERNEL_BUILD
