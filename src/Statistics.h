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

} BLORGFS_STATISTICS_GLOBAL, * PBLORGFS_STATISTICS_GLOBAL;

//
// Wire format for IOCTL_BLORGFS_QUERY_STATISTICS: the summed per-CPU
// counters, the gauges, and enough timebase to turn counts into rates.
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

#define BLORGFS_STATISTICS_VERSION 5

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
