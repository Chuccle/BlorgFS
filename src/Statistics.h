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
//    the counters no standard type has a field for: read dispatch mix,
//    chunk-fetch latency distribution, connection pool behaviour, TLS
//    record throughput. This is what turns "playback
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
// Eventually consistent is the standard these hold themselves to, and it
// is good enough deliberately. A lost increment moves a rate by a
// millionth; a locked operation on the read path moves the thing being
// measured.
//
// There is one distinction that standard does NOT license, and it is worth
// stating because the difference is easy to lose. Tolerating a stale or
// missing sample is not the same as tolerating an invented one. A reported
// maximum that summed two processors' worst fetches described a latency no
// fetch had, was entirely plausible, and cost a day's wrong conclusions
// before the per-fetch records contradicted it. So: counters may be lossy,
// and nothing here may be fabricated. That is why the outlier ring's
// publish is a store-release and its read is checked for tearing -- neither
// is a locked operation, and both exist to stop a reader seeing a record
// that never happened rather than to make anything exact.
//
// There are no exceptions to that rule, and there used to be one. A gauge
// -- fetches in flight -- was kept in a shared interlocked word on the
// argument that a value going up and down cannot be split across per-CPU
// entries, because the decrement can land on a different processor than
// the increment. That argument is wrong: splitting it into two monotone
// counters makes the difference of their sums exact wherever the halves
// land. The gauge was also redundant, being raised at the same point as
// FetchesIssued and dropped at the same points as FetchesCompleted and
// FetchesFailed, so in-flight is simply
//
//     FetchesIssued - (FetchesCompleted + FetchesFailed)
//
// computed by the reader. What went with it is the running peak, which
// cannot be maintained without knowing the global depth at every issue.
// The per-processor outlier records carry the depth at the moment of each
// slow fetch, which is what the peak was being read for and is strictly
// more useful than one high-water mark.
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
// Per-fetch records for the fetches that ran long, and why summed
// counters were not enough to explain them.
//
// The aggregate split localises the driver's latency tail to the body
// phase, and stops there: a sum and a max cannot say whether one fetch was
// slow in every phase or slow in one, nor what else was in flight beside
// it. That distinction is the whole question, because contention between
// concurrent fetches accounts for the spread up to roughly four times the
// base latency and does not account for the far tail (README, "Playback lag
// is seek latency").
//
// So the outliers are kept individually. They are rare enough to afford it
// -- four fetches over 250 ms in a 49-second playback window.
//
// Filed per processor, like every other counter here, and for the same
// reason: a shared ring needs an interlocked increment to hand out a slot,
// and this block exists so that no processor ever takes a lock or dirties
// another processor's line to record something. Rarity was the argument
// for tolerating an exception, which is not the same as needing one. The
// merge a per-processor arrangement costs is a sort by completion stamp at
// query time, paid once by a reader rather than on every outlier.
//
#define BLORGFS_SLOW_FETCH_THRESHOLD_US 250000

//
// Slots per processor, and the total the query merges them into. Small,
// because a processor filing more than four outliers inside one
// measurement window has a problem the newest four will describe just as
// well as the oldest.
//
#define BLORGFS_SLOW_FETCH_PER_CPU      4
#define BLORGFS_SLOW_FETCH_SAMPLES      16

//
// All fields are 8 bytes so the record carries no padding into the wire
// format. Sequence is 1-based; zero means the slot was never filled.
//
typedef struct _BLORGFS_SLOW_FETCH
{
    ULONG64 Sequence;

    //
    // Completion stamp, in QPC ticks. Records are filed per processor, so
    // this is what puts them back in the order they happened when the
    // query merges the rings -- a per-processor sequence number orders a
    // processor's own records and says nothing about anyone else's.
    //
    ULONG64 CompletedQpc;
    ULONG64 TotalUs;
    ULONG64 AcquireUs;
    ULONG64 SendUs;
    ULONG64 WaitUs;
    ULONG64 TtfbUs;
    ULONG64 BodyUs;
    ULONG64 Bytes;

    //
    // Fetches in flight when this one completed, and whether it ran on a
    // pooled connection. Together these separate "slow because it was
    // sharing the link" from "slow for a reason we have not found yet",
    // which is the only question these records exist to answer.
    //
    LONG64  FetchesActive;
    ULONG64 ConnectionReused;

} BLORGFS_SLOW_FETCH, * PBLORGFS_SLOW_FETCH;

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
    ULONG64 MetaDataReads;               // create-time path resolutions: every lookup, cache hit or miss
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

    //
    // Whether the FSP worker pool is big enough, which has never been
    // measured.
    //
    // FSP_THREAD_COUNT is min(max(4 x cores, 8), 16), and the argument for it
    // is that the pool absorbs blocking rather than CPU. That is the right
    // axis, but the number came from reasoning and nothing here could have
    // contradicted it: the only FSP counter was ReadsPosted, which reads zero
    // in every read workload because the PASSIVE bypass keeps reads off the
    // queue entirely. The pool is used by the metadata path -- Create and
    // DirCtrl -- which no read benchmark exercises.
    //
    // SocketMaxPoolSize was in this state once. A flat 32 justified by an
    // internal notion of pipeline depth cost 35-47 fresh connects a run at
    // 473-660 ms each, and was replaced by a number sized from measured peak
    // concurrency. These are what let the same be done here: how deep the
    // queue actually got, and how long a request waited before a worker took
    // it.
    //
    // Two monotone counters, no gauge. Depth is their difference, which is
    // exact wherever the halves land, and the reader computes it -- the same
    // resolution this block already reached for fetches in flight.
    //
    // A high-water mark was written here first and it needed an interlocked
    // global to maintain, which is precisely what the note above forbids. It
    // is not needed: over a thirty-pass metadata storm the driver posted 30
    // requests in total against a pool of eight, so the totals alone bound
    // the depth without anything shared being written on the path.
    ULONG64 FspPosts;
    ULONG64 FspDispatches;
    ULONG64 ReadsSequential;             // started exactly where a previous read ended
    ULONG64 ReadsEndOfFile;              // rejected at or past EOF

    //
    // Paging reads split by whether anything was waiting on them:
    // speculative is Cc's read-ahead, demand is a fault with an
    // application blocked inside CcCopyRead.
    //
    // The split exists because the measured latency tail is not explained
    // by fetch size. At four concurrent streams a 512 KB granule produced a
    // worst read of 1087 ms against a single fetch's ~29 ms, which is
    // head-of-line blocking rather than transfer time. If that is right,
    // the demand latency is the number that matters and the speculative
    // latency is the queue in front of it -- and these two pairs say so
    // directly instead of by inference from a granularity sweep.
    //
    ULONG64 ReadsSpeculative;            // Cc read-ahead: nobody is waiting
    ULONG64 ReadsDemand;                 // fault: an application is blocked

    //
    // Adaptive read-ahead granularity decisions. Both directions are
    // counted because the failure modes are opposite and neither is
    // visible in throughput alone: a policy that never shrinks is not
    // adapting, and one that oscillates shows up as both counters climbing
    // together over a workload whose pattern never changed.
    //
    ULONG64 ReadAheadShrinks;
    ULONG64 ReadAheadGrows;

    //
    // What the adaptive policy saw, as opposed to what it did.
    //
    // A grow or a shrink is the end of a chain -- a window completing, a
    // vote, two votes agreeing -- and when the chain does not reach the end
    // the counters above are silent about which link broke. These say
    // whether windows are completing at all, how they voted, and the bytes
    // the vote was computed from, which is the difference between a policy
    // that decided not to act and one that never got the chance.
    //
    // Added because a bursty reader inheriting a grown granule over-fetched
    // by 2.81x without a single shrink, and reasoning from the totals could
    // not say why.
    //
    ULONG64 ReadAdaptWindows;
    ULONG64 ReadAdaptVotesShrink;
    ULONG64 ReadAdaptVotesGrow;
    ULONG64 ReadAdaptWindowConsumed;
    ULONG64 ReadAdaptWindowFetched;


    ULONG64 SpeculativeLatencySumUs;
    ULONG64 SpeculativeLatencyMaxUs;
    ULONG64 DemandLatencySumUs;
    ULONG64 DemandLatencyMaxUs;

    //
    // How long an application actually waited for a read, measured from
    // dispatch to completion on non-paging reads only.
    //
    // Every other latency in this block is the driver's own view -- how
    // long a fetch took, how long a round trip took -- and none of it is
    // what a user experiences, because the cache manager sits in front of
    // it and is supposed to hide exactly those numbers. Measured at
    // playback rate the driver was issuing 67 ms fetches while the reader
    // saw 0.11 ms, and reasoning about stutter from the fetch figure was
    // reasoning from the wrong layer.
    //
    // Paging reads are excluded deliberately: they are the cache manager's
    // own traffic, issued on its schedule rather than in answer to anyone
    // waiting, so counting them here would put read-ahead's latency back
    // into the number that exists to exclude it.
    //
    ULONG64 UserReadSamples;
    ULONG64 UserReadLatencySumUs;
    ULONG64 UserReadLatencyMaxUs;
    ULONG64 UserReadLatencyBuckets[BLORGFS_STATISTICS_LATENCY_BUCKETS];

    //
    // The gap between one application-visible read completing on a file and
    // the next arriving on it -- how long the consumer was not asking for
    // anything.
    //
    // A consumer with a deadline idles: a player reading a frame every
    // 41.67 ms spends almost all of that interval asking for nothing, since
    // a read served from cache costs microseconds. A file copy never idles,
    // because its next read is issued the instant the last one returns.
    // The two are otherwise indistinguishable to the read-ahead policy --
    // both sequential, both amplification 1.0, both alone on the transport
    // -- and they want opposite granules, so this is the signal that
    // separates them.
    //
    // Instrumentation only. Nothing reads these to make a decision, and the
    // trap that has to be answered before anything does is visible in the
    // distribution rather than the mean: a player whose bitrate rises until
    // it stops idling looks like a copy exactly when a large granule would
    // hurt it most.
    //
    ULONG64 ReadIdleSamples;
    ULONG64 ReadIdleSumUs;
    ULONG64 ReadIdleMaxUs;
    ULONG64 ReadIdleBuckets[BLORGFS_STATISTICS_LATENCY_BUCKETS];

    // --- HTTP chunk fetches --------------------------------------------
    ULONG64 FetchesIssued;               // every range GET the driver makes
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
    // Issue to socket-in-hand, split out of pre-send, and the same again
    // counting only the fetches that needed a brand-new connection. A
    // clean-boot run showed a 2.0 second pre-send maximum across just
    // eleven fresh connects; these say whether that time is connection
    // establishment or something after it.
    //
    ULONG64 FetchAcquireSumUs;
    ULONG64 FetchAcquireMaxUs;
    ULONG64 FetchFreshAcquireSumUs;
    ULONG64 FetchFreshAcquireMaxUs;
    ULONG64 FetchFreshConnects;

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


    //
    // Everything from here down is per-processor diagnostic state rather
    // than a counter, and must not be summed across processors.
    // StatisticsAccumulate stops at SlowFetchSequence's offset for exactly
    // that reason, so anything added below is excluded automatically and
    // anything added above is summed automatically -- which is the right
    // default in both directions.
    //
    ULONG64 SlowFetchSequence;
    BLORGFS_SLOW_FETCH SlowFetches[BLORGFS_SLOW_FETCH_PER_CPU];

} BLORGFS_STATISTICS, * PBLORGFS_STATISTICS;

//
// Wire format for IOCTL_BLORGFS_QUERY_STATISTICS: the summed per-CPU
// counters, the merged outlier records, and enough timebase to turn counts
// into rates.
// Version is checked by the driver so a stale harness fails loudly
// instead of misreading a struct whose tail moved.
//
//
// Set when the driver was built checked (DBG). A workload measured against
// one of these is measuring the instrumentation: no optimisation, and
// BLORGFS_PRINT compiled in as a live global.LogLevel test on the read
// path rather than as nothing at all.
//
#define BLORGFS_STATS_FLAG_CHECKED_BUILD 0x00000001


#define BLORGFS_STATISTICS_VERSION 13

typedef struct _BLORGFS_STATISTICS_RESPONSE
{
    ULONG Version;                       // BLORGFS_STATISTICS_VERSION
    ULONG SizeOfStruct;                  // sizeof(BLORGFS_STATISTICS_RESPONSE)
    ULONG ProcessorCount;                // entries that were summed

    //
    // BLORGFS_STATS_FLAG_*. Occupies what used to be explicit padding, so
    // the layout is unchanged.
    //
    // BLORGFS_STATS_FLAG_CHECKED_BUILD exists because a performance number
    // taken against a checked driver is not a performance number, and
    // nothing else in this response reveals which build produced it. The
    // harness refuses to present workload results without saying so.
    //
    ULONG Flags;

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

    //
    // Fetches in flight when this response was built, derived rather than
    // tracked: FetchesIssued minus the fetches that have completed or
    // failed. No running peak, because maintaining one would mean knowing
    // the global depth at every issue; the outlier records below carry the
    // depth at each slow fetch instead.
    //
    LONG64 FetchesActive;

    //
    // Every fetch that crossed the threshold, which is not the same as
    // every fetch in the ring: SlowFetchesSeen counts them all, the ring
    // keeps the most recent BLORGFS_SLOW_FETCH_SAMPLES. Reporting both
    // means a reader can tell a full ring from a truncated one instead of
    // assuming it saw everything.
    //
    ULONG64 SlowFetchesSeen;
    BLORGFS_SLOW_FETCH SlowFetches[BLORGFS_SLOW_FETCH_SAMPLES];

} BLORGFS_STATISTICS_RESPONSE, * PBLORGFS_STATISTICS_RESPONSE;

//
//  Vendor IOCTLs on the FSDO. 0x800 is IOCTL_BLORGFS_SET_TLS_PIN
//  (DevIoCtrl.c); these continue that range. Device type is
//  FILE_DEVICE_UNKNOWN, not FILE_DEVICE_FILE_SYSTEM: the I/O manager
//  routes any IOCTL whose CTL_CODE device type is FILE_DEVICE_FILE_SYSTEM
//  as IRP_MJ_FILE_SYSTEM_CONTROL (an FSCTL) instead of
//  IRP_MJ_DEVICE_CONTROL, regardless of driver intent, so these would
//  never reach DevIoCtrlFsdo -- they would fall into FsCtrl.c's
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
// as BLORGFS_STAT_ADD(FetchBytes, n) with no pointer bookkeeping and
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
// Fetches in flight across the whole driver, derived from the monotone
// counters rather than kept in a gauge. Summing every processor is a
// reader's cost, so this is for decisions taken once per evaluation
// window -- never per read.
//
LONG64 BlorgStatisticsFetchesActive(VOID);

//
// Files one outlier. Callable at <= DISPATCH_LEVEL, like everything else
// on the completion chain, and cheap enough to call unconditionally
// because the threshold test happens here rather than at every call site.
//
VOID BlorgStatisticsRecordSlowFetch(
    LONG64 TotalQpc,
    LONG64 AcquireQpc,
    LONG64 SendQpc,
    LONG64 WaitQpc,
    LONG64 TtfbQpc,
    LONG64 BodyQpc,
    ULONG64 Bytes,
    BOOLEAN ConnectionReused);

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
