//
// BlorgFS performance harness.
//
// Runs a defined workload against the mounted volume, then reports what
// the driver's own counters say happened underneath it (Statistics.h).
// The point is to replace "playback stutters" and "that felt faster" with
// numbers that identify *which* stage is the bottleneck -- a workload
// timer alone cannot tell a slow backend from a read path that never
// reached it, and those two want opposite fixes.
//
// Two surfaces are read:
//  - The standard FSCTL_FILESYSTEM_GET_STATISTICS_EX on the volume
//    handle, which is also what `fsutil fsinfo statistics B:` reads.
//  - IOCTL_BLORGFS_QUERY_STATISTICS on \\.\BlorgFS, for the counters no
//    standard type has a field for.
//
// Elevation: the query IOCTL is FILE_READ_ACCESS and works unelevated,
// but the reset is FILE_WRITE_ACCESS, so every workload subcommand (which
// resets to open a measurement window) needs an admin prompt. `stats` and
// `fsstats` alone do not.
//
// Machine-readable output: --report <path> writes every metric as flat
// key=value lines alongside the human-readable dump. Deliberately not
// JSON -- the consumer is Compare-BlorgMetrics.ps1, PowerShell parses
// this format with a single ConvertFrom-StringData, and hand-rolling a
// JSON reader here would add a few hundred lines of parser to a harness
// whose whole job is to be trustworthy. It is greppable and diffable as a
// side benefit. Comparison thresholds live in the script, not here, so
// tuning them does not mean rebuilding.
//

#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

#include "..\..\src\Statistics.h"

//
// Workload read size. Deliberately what an application issues, letting Cc
// cluster it into whatever paging reads the driver actually sees -- pinning
// it to any internal size would measure a path no real reader takes.
//
static const DWORD kWorkloadReadSize = 64 * 1024;

//
// Unbuffered reads must be sector-aligned in size, offset, and buffer
// address. 4096 covers every sector size this volume reports.
//
static const DWORD kSectorAlignment = 4096;

struct HandlePair
{
    HANDLE Control;   // \\.\BlorgFS, for the vendor IOCTLs
    HANDLE Volume;    // \\.\<drive>:, for the standard statistics FSCTLs
};

//
// Percentile readout derived from the base-2 microsecond histogram. The
// bucket boundaries are all the resolution there is, so a percentile is
// reported as the upper bound of the bucket it falls in -- stating a
// precise-looking interpolated microsecond figure would imply precision
// the histogram does not carry.
//
struct LatencyPercentiles
{
    unsigned long long P50UpperUs;
    unsigned long long P90UpperUs;
    unsigned long long P99UpperUs;
    unsigned long long Samples;
};

static void PrintLastError(const char* what)
{
    DWORD error = GetLastError();
    printf("  [FAIL] %s: error %lu\n", what, error);
}

//
// Opens whichever handles the command actually needs, and only those. The
// two are independent: the driver's own counters live on the control
// device and are readable whenever the driver is loaded, mounted volume
// or not, while the standard statistics FSCTL is a property of the
// volume. Requiring both for every command would make `stats` fail on an
// unmounted volume for no reason, which is exactly when the counters are
// worth looking at.
//
static bool OpenHandles(const wchar_t* driveLetter, bool needControl, bool needWrite, bool needVolume, HandlePair* out)
{
    out->Control = nullptr;
    out->Volume = nullptr;

    if (needVolume)
    {
        wchar_t volumePath[16];
        swprintf_s(volumePath, L"\\\\.\\%c:", driveLetter[0]);

        out->Volume = CreateFileW(
            volumePath,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (INVALID_HANDLE_VALUE == out->Volume)
        {
            PrintLastError("open volume (is it mounted?)");
            out->Volume = nullptr;
            return false;
        }
    }

    if (needControl)
    {
        out->Control = CreateFileW(
            BLORGFS_FSDO_USERMODE_PATH,
            needWrite ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (INVALID_HANDLE_VALUE == out->Control)
        {
            DWORD error = GetLastError();

            printf("  [FAIL] open \\\\.\\BlorgFS: error %lu\n", error);

            if (ERROR_ACCESS_DENIED == error && needWrite)
            {
                printf("         Write access needs an elevated prompt.\n");
            }
            else if (ERROR_FILE_NOT_FOUND == error)
            {
                printf("         Driver not loaded, or too old to publish the symbolic link.\n");
            }

            out->Control = nullptr;
            return false;
        }
    }

    return true;
}

static void CloseHandles(HandlePair* handles)
{
    if (handles->Volume)
    {
        CloseHandle(handles->Volume);
        handles->Volume = nullptr;
    }

    if (handles->Control)
    {
        CloseHandle(handles->Control);
        handles->Control = nullptr;
    }
}

//
// The driver validates Version and SizeOfStruct on the way in, so they
// are stamped by the caller rather than returned -- a harness built
// against a different revision of the counter block is rejected instead
// of silently reading the wrong fields out of the tail.
//
static bool QueryDriverStatistics(HANDLE control, BLORGFS_STATISTICS_RESPONSE* out)
{
    if (!control)
    {
        return false;
    }

    ZeroMemory(out, sizeof(*out));
    out->Version = BLORGFS_STATISTICS_VERSION;
    out->SizeOfStruct = sizeof(BLORGFS_STATISTICS_RESPONSE);

    DWORD returned = 0;

    BOOL ok = DeviceIoControl(
        control,
        IOCTL_BLORGFS_QUERY_STATISTICS,
        out,
        sizeof(*out),
        out,
        sizeof(*out),
        &returned,
        nullptr);

    if (!ok)
    {
        PrintLastError("IOCTL_BLORGFS_QUERY_STATISTICS");
        return false;
    }

    return true;
}

static bool ResetDriverStatistics(HANDLE control)
{
    if (!control)
    {
        printf("  [FAIL] reset needs \\\\.\\BlorgFS opened for write (run elevated).\n");
        return false;
    }

    DWORD returned = 0;

    BOOL ok = DeviceIoControl(
        control,
        IOCTL_BLORGFS_RESET_STATISTICS,
        nullptr,
        0,
        nullptr,
        0,
        &returned,
        nullptr);

    if (!ok)
    {
        PrintLastError("IOCTL_BLORGFS_RESET_STATISTICS");
        return false;
    }

    return true;
}

//
// Walks the histogram to the first bucket whose cumulative count crosses
// each target fraction. Bucket i's upper bound is 2^i microseconds, and
// bucket 0 means "under 1 us".
//
static LatencyPercentiles ComputePercentiles(const ULONG64* buckets)
{
    LatencyPercentiles result = {};

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        result.Samples += buckets[i];
    }

    if (0 == result.Samples)
    {
        return result;
    }

    const unsigned long long p50Target = (result.Samples * 50) / 100;
    const unsigned long long p90Target = (result.Samples * 90) / 100;
    const unsigned long long p99Target = (result.Samples * 99) / 100;

    unsigned long long cumulative = 0;

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        cumulative += buckets[i];

        unsigned long long upper = (0 == i) ? 1ULL : (1ULL << i);

        if (0 == result.P50UpperUs && cumulative > p50Target)
        {
            result.P50UpperUs = upper;
        }

        if (0 == result.P90UpperUs && cumulative > p90Target)
        {
            result.P90UpperUs = upper;
        }

        if (0 == result.P99UpperUs && cumulative > p99Target)
        {
            result.P99UpperUs = upper;
        }
    }

    return result;
}

static double SafeRatio(unsigned long long numerator, unsigned long long denominator)
{
    return (0 == denominator) ? 0.0 : (100.0 * static_cast<double>(numerator) / static_cast<double>(denominator));
}

//
// Renders the histogram as one line per non-empty bucket, so the shape --
// which is the whole reason for keeping a distribution -- is visible
// without post-processing. A bimodal distribution -- a fast mode from
// requests the backend served warm and a slow mode from cold ones -- is
// invisible in any single summary number.
//
// The individual fetches that ran long, newest last.
//
// The histogram above says how many fetches were slow; it cannot say why
// any one of them was, because a sum and a max cannot distinguish a fetch
// that was slow in every phase from one that stalled in a single phase.
// That is the open question about this driver's tail: contention between
// concurrent fetches explains the spread up to roughly four times the base
// latency and does not explain the far tail, where usermode driving the
// same server over matched runs caps at ~100 ms and the driver reaches
// ~900 ms.
//
// So read these by comparing "active" against "body". Several fetches in
// flight with a long body is the link being shared and is expected. One
// fetch in flight with a long body is the thing we are looking for.
//
static void PrintSlowFetches(const BLORGFS_STATISTICS_RESPONSE& stats)
{
    if (0 == stats.SlowFetchesSeen)
    {
        return;
    }

    printf("\n  slow fetches (over %u ms)\n", BLORGFS_SLOW_FETCH_THRESHOLD_US / 1000);

    const unsigned long long kept =
        (stats.SlowFetchesSeen < BLORGFS_SLOW_FETCH_SAMPLES)
            ? stats.SlowFetchesSeen
            : BLORGFS_SLOW_FETCH_SAMPLES;

    printf("    %llu seen", stats.SlowFetchesSeen);

    if (stats.SlowFetchesSeen > BLORGFS_SLOW_FETCH_SAMPLES)
    {
        printf(", showing the most recent %llu", kept);
    }

    printf("\n");
    printf("      %6s %9s %8s %8s %8s %8s %9s %6s %5s\n",
        "seq", "total us", "acquire", "send", "wait", "ttfb", "body us", "KB", "activ");

    //
    // Ascending by sequence, so the ring reads in completion order however
    // it happens to have wrapped. A zeroed slot is one the driver dropped
    // as torn, or one never filled; either way it has nothing to say.
    //
    for (unsigned long long want = (stats.SlowFetchesSeen > kept)
             ? (stats.SlowFetchesSeen - kept + 1) : 1;
         want <= stats.SlowFetchesSeen;
         ++want)
    {
        for (ULONG i = 0; i < BLORGFS_SLOW_FETCH_SAMPLES; ++i)
        {
            const BLORGFS_SLOW_FETCH& f = stats.SlowFetches[i];

            if (f.Sequence != want)
            {
                continue;
            }

            printf("      %6llu %9llu %8llu %8llu %8llu %8llu %9llu %6llu %5lld%s\n",
                f.Sequence, f.TotalUs, f.AcquireUs, f.SendUs, f.WaitUs,
                f.TtfbUs, f.BodyUs, f.Bytes / 1024, f.FetchesActive,
                f.ConnectionReused ? "" : "  (fresh connection)");
            break;
        }
    }
}

//
static void PrintLatencyHistogram(const ULONG64* buckets)
{
    unsigned long long peak = 0;

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        if (buckets[i] > peak)
        {
            peak = buckets[i];
        }
    }

    if (0 == peak)
    {
        printf("    (no fetches recorded)\n");
        return;
    }

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        if (0 == buckets[i])
        {
            continue;
        }

        unsigned long long lower = (0 == i) ? 0ULL : (1ULL << (i - 1));
        unsigned long long upper = (0 == i) ? 1ULL : (1ULL << i);

        int bar = static_cast<int>((buckets[i] * 40) / peak);

        char label[64];

        if (i == BLORGFS_STATISTICS_LATENCY_BUCKETS - 1)
        {
            sprintf_s(label, ">=%llu us", lower);
        }
        else
        {
            sprintf_s(label, "%llu-%llu us", lower, upper);
        }

        printf("    %-18s %10llu  ", label, buckets[i]);

        for (int b = 0; b < bar; ++b)
        {
            printf("#");
        }

        printf("\n");
    }
}

//
// The interpretation layer, and the reason this harness exists rather
// than a raw counter dump: each block pairs a raw count with the ratio
// that makes it actionable, so the output says which stage to look at.
//
static void PrintDriverStatistics(const BLORGFS_STATISTICS_RESPONSE& stats)
{
    const BLORGFS_STATISTICS& t = stats.Totals;

    double windowSeconds = 0.0;

    if (stats.QpcFrequency > 0)
    {
        windowSeconds = static_cast<double>(stats.NowQpc - stats.EpochQpc) / static_cast<double>(stats.QpcFrequency);
    }

    printf("\n=== driver counters (window %.3f s, %lu processors) ===\n", windowSeconds, stats.ProcessorCount);

    //
    // Said before any number, not after, because a reader who has already
    // read the throughput figure has already been misled.
    //
    if (stats.Flags & BLORGFS_STATS_FLAG_CHECKED_BUILD)
    {
        printf("\n  *** CHECKED (Debug) DRIVER -- these are not performance numbers ***\n");
        printf("      Deploy with: Deploy-ToVM.ps1 -Configuration Release\n");
    }

    printf("\n  reads\n");
    printf("    cached                %12llu\n", t.ReadsCached);
    printf("    paging (inline)       %12llu\n", t.ReadsPagingInline);
    printf("    posted to FSP         %12llu\n", t.ReadsPosted);
    //
    // Clamped, for the reason BlorgStatisticsFetchesActive clamps: the merge
    // walks processors in order, so a post counted on one and its dispatch
    // counted on another read later makes dispatches exceed posts globally.
    // Unsigned subtraction would print that as 2^64.
    //
    const unsigned long long queued =
        (t.FspPosts > t.FspDispatches) ? (t.FspPosts - t.FspDispatches) : 0;

    printf("    FSP posts / dispatch  %12llu / %llu  (%llu still queued, pool %s)\n",
        t.FspPosts, t.FspDispatches, queued,
        (t.FspPosts > 0) ? "exercised" : "never queued");
    printf("    sequential            %12llu  (%.1f%% of paging)\n",
        t.ReadsSequential, SafeRatio(t.ReadsSequential, t.ReadsPagingInline));
    printf("    end-of-file           %12llu\n", t.ReadsEndOfFile);
    printf("    speculative (Cc RA)   %12llu  mean %llu us / max %llu us\n",
        t.ReadsSpeculative,
        (t.ReadsSpeculative > 0) ? (t.SpeculativeLatencySumUs / t.ReadsSpeculative) : 0ull,
        t.SpeculativeLatencyMaxUs);
    printf("    demand (someone waits)%12llu  mean %llu us / max %llu us\n",
        t.ReadsDemand,
        (t.ReadsDemand > 0) ? (t.DemandLatencySumUs / t.ReadsDemand) : 0ull,
        t.DemandLatencyMaxUs);
    printf("    granularity shrink/grow%11llu / %llu\n",
        t.ReadAheadShrinks, t.ReadAheadGrows);
    printf("    adapt windows         %12llu  (%llu shrink votes, %llu grow votes)\n",
        t.ReadAdaptWindows, t.ReadAdaptVotesShrink, t.ReadAdaptVotesGrow);
    printf("    adapt window bytes    %12llu consumed / %llu fetched  (%.2fx)\n",
        t.ReadAdaptWindowConsumed, t.ReadAdaptWindowFetched,
        (t.ReadAdaptWindowConsumed > 0)
            ? (static_cast<double>(t.ReadAdaptWindowFetched) / static_cast<double>(t.ReadAdaptWindowConsumed))
            : 0.0);
    printf("    user bytes            %12llu\n", t.UserFileReadBytes);
    printf("    non-cached bytes      %12llu\n", t.NonCachedReadBytes);

    //
    // What the application waited, as opposed to what the driver did.
    //
    // This is the only latency here a user can feel. Every figure in the
    // chunk-fetch block below is behind the cache manager, which exists to
    // hide exactly those numbers and demonstrably does: at playback rate
    // the driver issued 67 ms fetches while the reader saw 0.11 ms. Read
    // this section first, and treat the fetch block as the explanation for
    // whatever this one shows rather than as a symptom in its own right.
    //
    if (t.UserReadSamples > 0)
    {
        printf("\n  application-visible read latency (non-paging reads only)\n");
        printf("    samples               %12llu\n", t.UserReadSamples);
        printf("    mean                  %12llu us\n",
            t.UserReadLatencySumUs / t.UserReadSamples);
        printf("    max                   %12llu us\n", t.UserReadLatencyMaxUs);

        LatencyPercentiles u = ComputePercentiles(t.UserReadLatencyBuckets);

        if (u.Samples > 0)
        {
            printf("    p50 / p90 / p99       <=%llu / <=%llu / <=%llu us\n",
                u.P50UpperUs, u.P90UpperUs, u.P99UpperUs);
        }

        //
        // A read that took longer than a frame interval is a read a viewer
        // could have seen. At 24 fps a frame is 41667 us.
        //
        unsigned long long overFrame = 0;

        for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
        {
            if ((1ull << i) > 41667ull)
            {
                overFrame += t.UserReadLatencyBuckets[i];
            }
        }

        printf("    over one frame (41 ms)%12llu  (%.2f%%)\n",
            overFrame, SafeRatio(overFrame, t.UserReadSamples));

        printf("\n  application-visible read latency distribution\n");
        PrintLatencyHistogram(t.UserReadLatencyBuckets);
    }

    if (t.ReadIdleSamples > 0)
    {
        printf("\n  consumer idle between reads (gap from one read ending to the next arriving)\n");
        printf("    samples               %12llu\n", t.ReadIdleSamples);
        printf("    mean                  %12llu us\n",
            t.ReadIdleSumUs / t.ReadIdleSamples);
        printf("    max                   %12llu us\n", t.ReadIdleMaxUs);

        const unsigned long long busy = t.UserReadLatencySumUs;
        const unsigned long long idle = t.ReadIdleSumUs;

        printf("    idle share of wall    %12.2f%%  (idle %llu us, serving %llu us)\n",
            SafeRatio(idle, idle + busy), idle, busy);

        LatencyPercentiles g = ComputePercentiles(t.ReadIdleBuckets);

        if (g.Samples > 0)
        {
            printf("    p50 / p90 / p99       <=%llu / <=%llu / <=%llu us\n",
                g.P50UpperUs, g.P90UpperUs, g.P99UpperUs);
        }

        printf("\n  consumer idle distribution\n");
        PrintLatencyHistogram(t.ReadIdleBuckets);
    }

    printf("\n  chunk fetches\n");
    printf("    direct issued         %12llu\n", t.FetchesIssued);
    printf("    completed / failed    %12llu / %llu\n", t.FetchesCompleted, t.FetchesFailed);
    printf("    bytes                 %12llu\n", t.FetchBytes);
    printf("    in flight now         %12lld\n", stats.FetchesActive);

    if (t.FetchesCompleted > 0)
    {
        printf("    mean latency          %12llu us\n", t.FetchLatencySumUs / t.FetchesCompleted);

        if (t.FetchSplitSamples)
        {
            printf("      acquire mean / max  %12llu / %llu us\n",
                t.FetchAcquireSumUs / t.FetchSplitSamples, t.FetchAcquireMaxUs);
            if (t.FetchFreshConnects)
            {
                printf("        fresh: n=%llu mean / max %llu / %llu us\n",
                    t.FetchFreshConnects,
                    t.FetchFreshAcquireSumUs / t.FetchFreshConnects,
                    t.FetchFreshAcquireMaxUs);
            }
            printf("      pre-send mean / max %12llu / %llu us\n",
                t.FetchPreSendSumUs / t.FetchSplitSamples, t.FetchPreSendMaxUs);
            printf("      send mean / max     %12llu / %llu us\n",
                t.FetchSendSumUs / t.FetchSplitSamples, t.FetchSendMaxUs);
            printf("      wait mean / max     %12llu / %llu us\n",
                t.FetchWaitSumUs / t.FetchSplitSamples, t.FetchWaitMaxUs);
            printf("      ttfb mean / max     %12llu / %llu us\n",
                t.FetchTtfbSumUs / t.FetchSplitSamples, t.FetchTtfbMaxUs);
            printf("      body mean / max     %12llu / %llu us\n",
                t.FetchBodySumUs / t.FetchSplitSamples, t.FetchBodyMaxUs);
            printf("      split samples       %12llu\n", t.FetchSplitSamples);
        }
    }

    printf("    max latency           %12llu us\n", t.FetchLatencyMaxUs);

    LatencyPercentiles p = ComputePercentiles(t.FetchLatencyBuckets);

    if (p.Samples > 0)
    {
        printf("    p50 / p90 / p99       <=%llu / <=%llu / <=%llu us\n",
            p.P50UpperUs, p.P90UpperUs, p.P99UpperUs);
    }

    printf("\n  chunk fetch latency distribution\n");
    PrintLatencyHistogram(t.FetchLatencyBuckets);

    PrintSlowFetches(stats);

    printf("\n  metadata\n");
    printf("    dir listings          %12llu  (%llu failed, mean %llu us)\n",
        t.DirInfoRequests, t.DirInfoFailures,
        (t.DirInfoRequests > 0) ? (t.DirInfoLatencySumUs / t.DirInfoRequests) : 0ULL);
    printf("    file info             %12llu  (%llu failed, mean %llu us)\n",
        t.FileInfoRequests, t.FileInfoFailures,
        (t.FileInfoRequests > 0) ? (t.FileInfoLatencySumUs / t.FileInfoRequests) : 0ULL);
    printf("    creates hit/ok/fail   %12llu / %llu / %llu\n",
        t.CreateHits, t.SuccessfulCreates, t.FailedCreates);
    printf("    path cache hit rate   %12.1f%%  (%llu hit, %llu miss)\n",
        SafeRatio(t.PathCacheHits, t.PathCacheHits + t.PathCacheMisses),
        t.PathCacheHits, t.PathCacheMisses);

    const unsigned long long acquires = t.ConnectionsPooled + t.ConnectionsFresh;

    printf("\n  connections\n");
    printf("    reuse rate            %12.1f%%  (%llu pooled, %llu fresh)\n",
        SafeRatio(t.ConnectionsPooled, acquires), t.ConnectionsPooled, t.ConnectionsFresh);
    printf("    failed connects       %12llu\n", t.ConnectionsFailed);
    printf("    released / pool-full  %12llu / %llu\n",
        t.ConnectionsReleasedToPool, t.ConnectionsClosedPoolFull);
    printf("    keep-alive retries    %12llu\n", t.KeepAliveRetries);
    printf("    socket timeouts       %12llu\n", t.SocketTimeouts);

    if (t.HandshakesStarted > 0)
    {
        printf("\n  tls\n");
        printf("    handshakes ok/fail    %12llu / %llu\n", t.HandshakesCompleted, t.HandshakesFailed);
        printf("    mean handshake        %12llu us\n",
            (t.HandshakesCompleted > 0) ? (t.HandshakeLatencySumUs / t.HandshakesCompleted) : 0ULL);
        printf("    max handshake         %12llu us\n", t.HandshakeLatencyMaxUs);
        printf("    records decrypted     %12llu  (%llu bytes)\n",
            t.TlsRecordsDecrypted, t.TlsBytesDecrypted);
        printf("    bulk receives         %12llu  (%.1f records each)\n",
            t.TlsBulkReceives,
            (t.TlsBulkReceives > 0)
                ? (static_cast<double>(t.TlsRecordsDecrypted) / static_cast<double>(t.TlsBulkReceives))
                : 0.0);
    }
}

//
// Reads the standard statistics FSCTL. Sized by probing: these FSCTLs
// return one entry per processor, so the buffer is grown until the call
// stops reporting overflow rather than assuming a processor count.
//
static bool PrintFilesystemStatistics(HANDLE volume)
{
    std::vector<unsigned char> buffer(64 * 1024);
    DWORD returned = 0;

    BOOL ok = DeviceIoControl(
        volume,
        FSCTL_FILESYSTEM_GET_STATISTICS_EX,
        nullptr,
        0,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &returned,
        nullptr);

    if (!ok && ERROR_MORE_DATA != GetLastError())
    {
        PrintLastError("FSCTL_FILESYSTEM_GET_STATISTICS_EX");
        return false;
    }

    if (returned < sizeof(FILESYSTEM_STATISTICS_EX))
    {
        printf("  [FAIL] statistics FSCTL returned %lu bytes\n", returned);
        return false;
    }

    const FILESYSTEM_STATISTICS_EX* first =
        reinterpret_cast<const FILESYSTEM_STATISTICS_EX*>(buffer.data());

    const DWORD stride = first->SizeOfCompleteStructure;

    if (0 == stride || (stride % 64) != 0)
    {
        printf("  [FAIL] bad SizeOfCompleteStructure %lu (must be a non-zero multiple of 64)\n", stride);
        return false;
    }

    const DWORD entries = returned / stride;

    printf("\n=== FSCTL_FILESYSTEM_GET_STATISTICS_EX (%lu entries, type %u, version %u) ===\n",
        entries, first->FileSystemType, first->Version);

    unsigned long long userReads = 0;
    unsigned long long userReadBytes = 0;
    unsigned long long metaReads = 0;
    unsigned long long metaReadBytes = 0;
    unsigned long long metaDiskReads = 0;
    unsigned long long nonCachedReads = 0;
    unsigned long long nonCachedReadBytes = 0;
    unsigned long long createHits = 0;
    unsigned long long successfulCreates = 0;
    unsigned long long failedCreates = 0;

    for (DWORD i = 0; i < entries; ++i)
    {
        const unsigned char* entryBase = buffer.data() + (static_cast<size_t>(i) * stride);

        const FILESYSTEM_STATISTICS_EX* header =
            reinterpret_cast<const FILESYSTEM_STATISTICS_EX*>(entryBase);

        const FAT_STATISTICS* fat =
            reinterpret_cast<const FAT_STATISTICS*>(entryBase + sizeof(FILESYSTEM_STATISTICS_EX));

        userReads += header->UserFileReads;
        userReadBytes += header->UserFileReadBytes;
        metaReads += header->MetaDataReads;
        metaReadBytes += header->MetaDataReadBytes;
        metaDiskReads += header->MetaDataDiskReads;

        nonCachedReads += fat->NonCachedReads;
        nonCachedReadBytes += fat->NonCachedReadBytes;
        createHits += fat->CreateHits;
        successfulCreates += fat->SuccessfulCreates;
        failedCreates += fat->FailedCreates;
    }

    printf("    UserFileReads         %12llu  (%llu bytes)\n", userReads, userReadBytes);
    printf("    MetaDataReads         %12llu  (%llu bytes, %llu to backend)\n",
        metaReads, metaReadBytes, metaDiskReads);
    printf("    NonCachedReads        %12llu  (%llu bytes)\n", nonCachedReads, nonCachedReadBytes);
    printf("    Creates hit/ok/fail   %12llu / %llu / %llu\n",
        createHits, successfulCreates, failedCreates);

    return true;
}

//
// One file the stream workload could run against. The size rides along
// because the find walk already paid for it and the largest-first ordering
// in CollectStreamFiles needs it; querying each path again afterwards
// would spend a metadata round trip relearning what was just in hand.
//
struct StreamFileCandidate
{
    std::wstring Path;
    unsigned long long Bytes;
};

//
// Distinct files for the stream workload. Recursive, because the media
// layout that motivates this puts one file per directory.
//
static void CollectStreamFilesInto(
    const std::wstring& directory,
    unsigned long long minimumBytes,
    std::vector<StreamFileCandidate>* files)
{
    std::wstring pattern = directory + L"\\*";

    WIN32_FIND_DATAW find = {};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &find);

    if (INVALID_HANDLE_VALUE == handle)
    {
        return;
    }

    do
    {
        if (0 == wcscmp(find.cFileName, L".") || 0 == wcscmp(find.cFileName, L".."))
        {
            continue;
        }

        std::wstring full = directory + L"\\" + find.cFileName;

        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            CollectStreamFilesInto(full, minimumBytes, files);
            continue;
        }

        const unsigned long long size =
            (static_cast<unsigned long long>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;

        if (size >= minimumBytes)
        {
            StreamFileCandidate candidate;
            candidate.Path = full;
            candidate.Bytes = size;
            files->push_back(candidate);
        }
    }
    while (FindNextFileW(handle, &find));

    FindClose(handle);
}

static bool StreamFileIsLarger(const StreamFileCandidate& left, const StreamFileCandidate& right)
{
    return left.Bytes > right.Bytes;
}

static bool CollectStreamFiles(
    const wchar_t* directory,
    unsigned long streamCount,
    std::vector<std::wstring>* files)
{
    //
    // Big enough that a stream spends the run reading rather than opening,
    // and comfortably past Cc's read-ahead window so the read reaches steady
    // state instead of measuring its spin-up.
    //
    const unsigned long long minimumBytes = 64ull * 1024 * 1024;

    std::vector<StreamFileCandidate> candidates;

    CollectStreamFilesInto(directory, minimumBytes, &candidates);

    if (candidates.size() < streamCount)
    {
        candidates.clear();
        CollectStreamFilesInto(directory, 8ull * 1024 * 1024, &candidates);
    }

    //
    // Largest-first, so the first streamCount files never include one that
    // hits EOF partway through the window: a stream that ends early shrinks
    // its own share of the aggregate and drags the fairness ratio around as
    // a side effect of directory enumeration order rather than of the
    // driver.
    //
    std::sort(candidates.begin(), candidates.end(), StreamFileIsLarger);

    files->clear();

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        files->push_back(candidates[i].Path);
    }

    return !files->empty();
}
//
// Concurrent-stream workload: the shape this driver actually has to survive.
//
// Every other workload here drives one file. That measures the pipeline but
// says nothing about what happens when several readers want it at once,
// which is the normal case -- a video and its subtitle track, a game
// streaming assets while its own data file is open, a library browse
// overlapping playback. Aggregate throughput alone is not the answer
// either: a stream that stalls for a second has failed even if the total
// looks healthy, so this records per-read latency and reports the tail.
//
// Each stream gets its own file and its own thread, reads buffered and
// sequentially (the playback shape, and the only one Cc reads ahead for --
// see BlorgVolumeRead), and runs until the deadline or EOF.
// Latency is per ReadFile of kWorkloadReadSize, which is the stall a player
// actually feels.
//
// Fairness is reported as the spread between the slowest and fastest
// stream. Equal aggregate throughput split unequally is a different system
// from one that shares, and only the second one plays video without
// stuttering.
//
//
// A consumer's schedule, and what it missed.
//
// Reading flat out and reading to a deadline are different workloads, and
// the difference is not a detail of the reader: a reader going flat out sits
// permanently at the read-ahead frontier, taking each granule the instant Cc
// produces it, while one on a schedule spends nearly all of its time asking
// for nothing. Only the second can miss a deadline, and only the second is
// what a player does.
//
// Block n is due at `start + (n+1) * interval` and the schedule is absolute,
// so a consumer that falls behind accumulates lateness rather than quietly
// dropping to whatever the link will give it. That is the point: a player
// that cannot keep up stutters, it does not renegotiate its bitrate.
//
// Shared by play, demux and streams so that "paced" means one thing across
// all three, and so a workload gains a deadline by holding one of these
// rather than by growing its own copy of the arithmetic.
//
struct Pacer
{
    HANDLE Timer;
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start;

    //
    // Seconds between block due times. Zero means unpaced, in which case
    // every call below is a no-op and the workload reads flat out.
    //
    double Interval;

    unsigned long long Blocks;
    unsigned long long Missed;
    double WorstLateness;
    double LatenessSum;
};

//
// A high-resolution timer where the platform has one, because the default
// timer granularity is ~15.6 ms against frame intervals of 15-42 ms. Falling
// back to an ordinary timer keeps the workload runnable rather than exact;
// the reported lateness shows when that has happened.
//
// Phase shifts this schedule within its own interval, so that N paced
// readers started together do not all come due at the same instant.
//
// Eight streams in lockstep are not eight players: they are one burst of
// eight demands every interval, with the transport idle between. Measured
// in phase, eight streams at 1536 KB/s each missed 7.33% of their deadlines
// with a 500 ms worst case while asking for 12 MB/s of a link that had just
// delivered 21.7 -- a queueing artifact of the harness, not of the driver.
//
static bool PacerInit(Pacer* pacer, DWORD blockSize, unsigned long kbPerSecond, double phase)
{
    ZeroMemory(pacer, sizeof(*pacer));

    QueryPerformanceFrequency(&pacer->Frequency);
    QueryPerformanceCounter(&pacer->Start);

    if (0 == kbPerSecond)
    {
        return true;
    }

    pacer->Interval =
        static_cast<double>(blockSize) / (static_cast<double>(kbPerSecond) * 1024.0);

    pacer->Start.QuadPart += static_cast<long long>(
        phase * pacer->Interval * static_cast<double>(pacer->Frequency.QuadPart));

    pacer->Timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (nullptr == pacer->Timer)
    {
        pacer->Timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    if (nullptr == pacer->Timer)
    {
        PrintLastError("CreateWaitableTimerEx");
        return false;
    }

    return true;
}

static void PacerDestroy(Pacer* pacer)
{
    if (pacer->Timer)
    {
        CloseHandle(pacer->Timer);
        pacer->Timer = nullptr;
    }
}

static double PacerElapsed(const Pacer* pacer)
{
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);

    return static_cast<double>(now.QuadPart - pacer->Start.QuadPart) /
           static_cast<double>(pacer->Frequency.QuadPart);
}

//
// When the block about to be read is due. Meaningless for an unpaced run,
// which is why PacerComplete ignores it there.
//
static double PacerDue(const Pacer* pacer)
{
    return static_cast<double>(pacer->Blocks + 1) * pacer->Interval;
}

static void PacerComplete(Pacer* pacer, double due)
{
    if (pacer->Interval > 0.0)
    {
        const double completed = PacerElapsed(pacer);

        if (completed > due)
        {
            const double lateness = completed - due;

            ++pacer->Missed;
            pacer->LatenessSum += lateness;

            if (lateness > pacer->WorstLateness)
            {
                pacer->WorstLateness = lateness;
            }
        }
    }

    ++pacer->Blocks;
}

//
// Sleeps until the next block's slot. Returns immediately when unpaced, and
// when the reader is already late -- a consumer behind schedule does not get
// to skip ahead, it just stops idling.
//
static void PacerWait(Pacer* pacer)
{
    if (0.0 == pacer->Interval || nullptr == pacer->Timer)
    {
        return;
    }

    const double next = static_cast<double>(pacer->Blocks) * pacer->Interval;
    const double now = PacerElapsed(pacer);

    if (next <= now)
    {
        return;
    }

    LARGE_INTEGER dueTime;

    dueTime.QuadPart = -static_cast<long long>((next - now) * 10000000.0);

    if (SetWaitableTimer(pacer->Timer, &dueTime, 0, nullptr, nullptr, FALSE))
    {
        WaitForSingleObject(pacer->Timer, INFINITE);
    }
}

static void PacerReport(const Pacer* pacer, unsigned long kbPerSecond, DWORD blockSize)
{
    if (0.0 == pacer->Interval)
    {
        return;
    }

    printf("\n  playback schedule (%lu KB/s, %lu KB blocks, %.2f ms apart)\n",
        kbPerSecond, blockSize / 1024, pacer->Interval * 1000.0);
    printf("    blocks                %12llu\n", pacer->Blocks);
    printf("    missed deadline       %12llu  (%.2f%%)\n",
        pacer->Missed,
        (pacer->Blocks > 0)
            ? ((static_cast<double>(pacer->Missed) * 100.0) / static_cast<double>(pacer->Blocks))
            : 0.0);
    printf("    worst lateness        %12.2f ms\n", pacer->WorstLateness * 1000.0);
    printf("    mean lateness of miss %12.2f ms\n",
        (pacer->Missed > 0)
            ? ((pacer->LatenessSum / static_cast<double>(pacer->Missed)) * 1000.0)
            : 0.0);
}

//
// Reads a `pace=<KBps>` token from anywhere in the trailing arguments,
// yielding 0 when there is none.
//
// Scanned by name rather than taken as another positional, because both
// demux and streams already end in name-matched flags at fixed indices and
// Measure-BlorgReadAhead.ps1 passes --report into one of those slots.
// Inserting a positional would silently re-map what existing callers mean.
//
static unsigned long ParsePaceKbPerSecond(int argc, wchar_t** argv, int from)
{
    for (int i = from; i < argc; ++i)
    {
        if (0 == wcsncmp(argv[i], L"pace=", 5))
        {
            const int parsed = _wtoi(argv[i] + 5);

            if (parsed > 0)
            {
                return static_cast<unsigned long>(parsed);
            }
        }
    }

    return 0;
}

struct StreamResult
{
    unsigned long long Bytes;
    std::vector<double> LatenciesMs;
    bool Ok;

    //
    // The stream's own schedule, so each reader is a player in its own
    // right rather than N readers sharing one clock. Zeroed when unpaced.
    //
    unsigned long long Blocks;
    unsigned long long Missed;
    double WorstLateness;
};

struct StreamContext
{
    const wchar_t* Path;
    double DeadlineSeconds;
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start;
    StreamResult* Result;

    //
    // Opens the stream with FILE_FLAG_NO_BUFFERING, so every read goes to
    // the driver instead of being served by Cc. Buffered numbers measure
    // the filesystem; unbuffered ones measure the transport, which is what
    // a comparison against a usermode HTTP client is actually asking about.
    //
    bool Unbuffered;

    //
    // Bytes per second this stream reads at, or zero to read flat out.
    //
    unsigned long PaceKbPerSecond;

    //
    // Where in the interval this stream's schedule sits, so concurrent
    // readers are independent players rather than one synchronised burst.
    //
    double PacePhase;
};

static DWORD WINAPI StreamWorker(LPVOID parameter)
{
    StreamContext* ctx = static_cast<StreamContext*>(parameter);
    ctx->Result->Ok = false;
    ctx->Result->Bytes = 0;

    DWORD streamFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

    if (ctx->Unbuffered)
    {
        streamFlags |= FILE_FLAG_NO_BUFFERING;
    }

    HANDLE file = CreateFileW(ctx->Path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, streamFlags, nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open stream file");
        return 0;
    }

    //
    // Unbuffered streams read 512 KB blocks rather than the 64 KB an
    // application would issue. With Cc bypassed there is no read-ahead to
    // cluster small reads, so 64 KB would leave only 64 KB per stream in
    // flight and measure request overhead instead of transport -- and the
    // usermode client this is compared against issues 512 KB.
    //
    const DWORD streamReadSize = ctx->Unbuffered
        ? ((512u * 1024u + kSectorAlignment - 1) & ~(kSectorAlignment - 1))
        : kWorkloadReadSize;

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, streamReadSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (!buffer)
    {
        PrintLastError("allocate stream buffer");
        CloseHandle(file);
        return 0;
    }

    ctx->Result->LatenciesMs.reserve(65536);

    Pacer pacer;

    if (!PacerInit(&pacer, streamReadSize, ctx->PaceKbPerSecond, ctx->PacePhase))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        return 0;
    }

    for (;;)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        const double elapsed = static_cast<double>(now.QuadPart - ctx->Start.QuadPart) /
                               static_cast<double>(ctx->Frequency.QuadPart);

        if (elapsed >= ctx->DeadlineSeconds)
        {
            break;
        }

        const double due = PacerDue(&pacer);

        LARGE_INTEGER readStart;
        QueryPerformanceCounter(&readStart);

        DWORD read = 0;

        if (!ReadFile(file, buffer, streamReadSize, &read, nullptr))
        {
            PrintLastError("stream read");
            break;
        }

        if (0 == read)
        {
            break;
        }

        LARGE_INTEGER readEnd;
        QueryPerformanceCounter(&readEnd);

        ctx->Result->LatenciesMs.push_back(
            1000.0 * static_cast<double>(readEnd.QuadPart - readStart.QuadPart) /
            static_cast<double>(ctx->Frequency.QuadPart));

        ctx->Result->Bytes += read;

        PacerComplete(&pacer, due);
        PacerWait(&pacer);
    }

    ctx->Result->Blocks = pacer.Blocks;
    ctx->Result->Missed = pacer.Missed;
    ctx->Result->WorstLateness = pacer.WorstLateness;

    PacerDestroy(&pacer);
    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    ctx->Result->Ok = true;
    return 0;
}

static double Percentile(std::vector<double>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }

    size_t index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));

    return sorted[index];
}

//
// Exact percentiles for one workload's reads, and for its throughput over
// time, which a single mean cannot show.
//
// The driver's own histogram answers latency in power-of-two buckets, so a
// p99 reported as "<=32768 us" means somewhere between 16 and 32 ms. That is
// wide enough to hide the difference between two arms that differ by a fifth,
// which is the size of most differences measured here. These are exact.
//
// Throughput had no distribution at all: every figure was bytes over the
// whole run. A workload that stalls for two seconds and then races reads
// identically to one that runs evenly, and the two are not the same thing to
// anyone waiting. Bytes are stamped into fixed intervals and each interval's
// rate is a sample, so a p1 says what the worst moments looked like rather
// than what the average hid.
//
// p1 is reported alongside p50 and p99 because for throughput the interesting
// tail is the LOW one, and for latency it is the high one; carrying both ends
// on both metrics costs nothing and stops the wrong end being quoted.
//
struct Sampler
{
    std::vector<double> LatenciesMs;

    // Bytes accumulated in the interval currently open, and the closed ones.
    std::vector<double> IntervalMbPerSec;
    double IntervalSeconds;
    unsigned long long IntervalBytes;
    LARGE_INTEGER IntervalStart;
    LARGE_INTEGER Frequency;
};

static void SamplerInit(Sampler* s, double intervalSeconds)
{
    s->LatenciesMs.clear();
    s->IntervalMbPerSec.clear();
    s->IntervalSeconds = intervalSeconds;
    s->IntervalBytes = 0;
    QueryPerformanceFrequency(&s->Frequency);
    QueryPerformanceCounter(&s->IntervalStart);
    s->LatenciesMs.reserve(65536);
}

//
// Files one read and closes any intervals it completed. A read longer than
// an interval closes several, and the empty ones are real: no bytes moved in
// them, and that is exactly what a throughput p1 should see.
//
static void SamplerRecord(Sampler* s, double latencyMs, DWORD bytes, LARGE_INTEGER now)
{
    s->LatenciesMs.push_back(latencyMs);
    s->IntervalBytes += bytes;

    for (;;)
    {
        const double open = static_cast<double>(now.QuadPart - s->IntervalStart.QuadPart) /
                            static_cast<double>(s->Frequency.QuadPart);

        if (open < s->IntervalSeconds)
        {
            break;
        }

        s->IntervalMbPerSec.push_back(
            (s->IntervalBytes / (1024.0 * 1024.0)) / s->IntervalSeconds);

        s->IntervalBytes = 0;
        s->IntervalStart.QuadPart +=
            static_cast<long long>(s->IntervalSeconds * static_cast<double>(s->Frequency.QuadPart));
    }
}

static void SamplerReport(Sampler* s, const char* what)
{
    std::sort(s->LatenciesMs.begin(), s->LatenciesMs.end());
    std::sort(s->IntervalMbPerSec.begin(), s->IntervalMbPerSec.end());

    printf("\n  %s: exact percentiles\n", what);

    if (!s->LatenciesMs.empty())
    {
        printf("    latency p1/p50/p99    %10.3f / %.3f / %.3f ms   max %.3f\n",
            Percentile(s->LatenciesMs, 0.01), Percentile(s->LatenciesMs, 0.50),
            Percentile(s->LatenciesMs, 0.99), s->LatenciesMs.back());
    }

    if (!s->IntervalMbPerSec.empty())
    {
        printf("    throughput p1/p50/p99 %10.2f / %.2f / %.2f MB/s  (%zu x %.0f ms)\n",
            Percentile(s->IntervalMbPerSec, 0.01), Percentile(s->IntervalMbPerSec, 0.50),
            Percentile(s->IntervalMbPerSec, 0.99), s->IntervalMbPerSec.size(),
            s->IntervalSeconds * 1000.0);
    }
}


static bool RunStreams(
    const wchar_t* directory,
    unsigned long streamCount,
    double seconds,
    bool unbuffered,
    unsigned long paceKbPerSecond,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    std::vector<std::wstring> files;

    if (!CollectStreamFiles(directory, streamCount, &files))
    {
        return false;
    }

    if (files.size() < streamCount)
    {
        printf("only %zu suitable files under %ws, need %lu\n", files.size(), directory, streamCount);
        return false;
    }

    std::vector<StreamResult> results(streamCount);
    std::vector<StreamContext> contexts(streamCount);
    std::vector<HANDLE> threads(streamCount);

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    for (unsigned long i = 0; i < streamCount; ++i)
    {
        contexts[i].Path = files[i].c_str();
        contexts[i].DeadlineSeconds = seconds;
        contexts[i].Frequency = frequency;
        contexts[i].Start = start;
        contexts[i].Result = &results[i];
        contexts[i].Unbuffered = unbuffered;
        contexts[i].PaceKbPerSecond = paceKbPerSecond;
        contexts[i].PacePhase = static_cast<double>(i) / static_cast<double>(streamCount);

        threads[i] = CreateThread(nullptr, 0, StreamWorker, &contexts[i], 0, nullptr);

        if (!threads[i])
        {
            PrintLastError("create stream thread");
            return false;
        }
    }

    WaitForMultipleObjects(streamCount, threads.data(), TRUE, INFINITE);

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    for (unsigned long i = 0; i < streamCount; ++i)
    {
        CloseHandle(threads[i]);
    }

    const double elapsed = static_cast<double>(end.QuadPart - start.QuadPart) /
                           static_cast<double>(frequency.QuadPart);

    unsigned long long total = 0;
    double slowest = 0.0;
    double fastest = 0.0;
    std::vector<double> all;

    for (unsigned long i = 0; i < streamCount; ++i)
    {
        if (!results[i].Ok)
        {
            return false;
        }

        total += results[i].Bytes;

        const double mbps = (static_cast<double>(results[i].Bytes) / (1024.0 * 1024.0)) / elapsed;

        if (0 == i || mbps < slowest) { slowest = mbps; }
        if (0 == i || mbps > fastest) { fastest = mbps; }

        all.insert(all.end(), results[i].LatenciesMs.begin(), results[i].LatenciesMs.end());
    }

    std::sort(all.begin(), all.end());

    printf("\n  per-stream (%lu streams)\n", streamCount);
    printf("    aggregate             %12.2f MB/s\n",
        (static_cast<double>(total) / (1024.0 * 1024.0)) / elapsed);
    printf("    per stream mean       %12.2f MB/s\n",
        ((static_cast<double>(total) / (1024.0 * 1024.0)) / elapsed) / static_cast<double>(streamCount));
    printf("    slowest / fastest     %12.2f / %.2f MB/s", slowest, fastest);
    printf("   (fairness %.2f)\n", (fastest > 0.0) ? (slowest / fastest) : 0.0);

    if (paceKbPerSecond > 0)
    {
        unsigned long long blocks = 0;
        unsigned long long missed = 0;
        double worst = 0.0;
        unsigned long starved = 0;

        for (unsigned long i = 0; i < streamCount; ++i)
        {
            blocks += results[i].Blocks;
            missed += results[i].Missed;

            if (results[i].WorstLateness > worst)
            {
                worst = results[i].WorstLateness;
            }

            if (results[i].Missed > 0)
            {
                ++starved;
            }
        }

        printf("\n  playback schedule (%lu KB/s per stream)\n", paceKbPerSecond);
        printf("    blocks                %12llu\n", blocks);
        printf("    missed deadline       %12llu  (%.2f%%)\n", missed, SafeRatio(missed, blocks));
        printf("    worst lateness        %12.2f ms\n", worst * 1000.0);
        printf("    streams that missed   %12lu of %lu\n", starved, streamCount);
    }
    printf("    reads                 %12zu\n", all.size());
    printf("    latency p50           %12.3f ms\n", Percentile(all, 0.50));
    printf("    latency p95           %12.3f ms\n", Percentile(all, 0.95));
    printf("    latency p99           %12.3f ms\n", Percentile(all, 0.99));
    printf("    latency max           %12.3f ms\n", all.empty() ? 0.0 : all.back());

    *bytesOut = total;
    *secondsOut = elapsed;

    return true;
}

//
// Sequential read of one file. Unbuffered mode is what exercises the HTTP
// path honestly -- a buffered re-read of a file already resident in Cc
// measures memcpy, not this filesystem.
//
static bool RunSequential(const wchar_t* path, bool unbuffered, unsigned long long* bytesOut, double* secondsOut)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

    if (unbuffered)
    {
        flags |= FILE_FLAG_NO_BUFFERING;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open workload file");
        return false;
    }

    const DWORD readSize = unbuffered
        ? ((kWorkloadReadSize + kSectorAlignment - 1) & ~(kSectorAlignment - 1))
        : kWorkloadReadSize;

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, readSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (!buffer)
    {
        PrintLastError("allocate read buffer");
        CloseHandle(file);
        return false;
    }

    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    unsigned long long total = 0;

    for (;;)
    {
        DWORD read = 0;

        if (!ReadFile(file, buffer, readSize, &read, nullptr))
        {
            PrintLastError("ReadFile");
            break;
        }

        if (0 == read)
        {
            break;
        }

        total += read;
    }

    QueryPerformanceCounter(&end);

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    *bytesOut = total;
    *secondsOut = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    return true;
}

//
// Random reads at a fixed block size.
//
// Unbuffered is the default and answers "what does one backend round trip
// cost": Cc is out of the path entirely, every read becomes exactly one
// fetch of the requested size, and the fetch-latency distribution is the
// raw round trip with no lookahead hiding any of it.
//
// Buffered is a different question and the reason the mode exists. A
// demuxer picking at a file -- the subtitle case -- reads small and
// scattered through an ordinary buffered handle, so Cc sits in front of it
// and every small read drags a full read-ahead granule across the network.
// That amplification is invisible to the unbuffered mode, which by
// construction fetches exactly what was asked for.
//
// FILE_FLAG_RANDOM_ACCESS is deliberately not passed in buffered mode. It
// is a hint to Cc to suppress read-ahead, which would switch off the very
// behaviour being measured and report a granularity sweep as a flat line.
// Media Foundation does not pass it either.
//
static bool RunRandom(
    const wchar_t* path,
    DWORD blockSize,
    unsigned long count,
    bool buffered,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    DWORD flags = FILE_ATTRIBUTE_NORMAL;

    if (!buffered)
    {
        flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS;
    }

    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open workload file");
        return false;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(file, &fileSize))
    {
        PrintLastError("GetFileSizeEx");
        CloseHandle(file);
        return false;
    }

    const DWORD alignedBlock = (blockSize + kSectorAlignment - 1) & ~(kSectorAlignment - 1);

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, alignedBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (!buffer)
    {
        PrintLastError("allocate read buffer");
        CloseHandle(file);
        return false;
    }

    const unsigned long long blocks =
        (fileSize.QuadPart > alignedBlock) ? (fileSize.QuadPart / alignedBlock) : 1ULL;

    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    unsigned long long total = 0;

    for (unsigned long i = 0; i < count; ++i)
    {
        unsigned long long block = (static_cast<unsigned long long>(rand()) * RAND_MAX + rand()) % blocks;

        LARGE_INTEGER offset;
        offset.QuadPart = static_cast<long long>(block * alignedBlock);

        OVERLAPPED overlapped = {};
        overlapped.Offset = offset.LowPart;
        overlapped.OffsetHigh = static_cast<DWORD>(offset.HighPart);

        DWORD read = 0;

        if (!ReadFile(file, buffer, alignedBlock, &read, &overlapped))
        {
            if (ERROR_HANDLE_EOF != GetLastError())
            {
                PrintLastError("ReadFile (random)");
                break;
            }
        }

        total += read;
    }

    QueryPerformanceCounter(&end);

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    *bytesOut = total;
    *secondsOut = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    return true;
}

//
// Several sequential cursors interleaved through one buffered handle --
// a demuxer reading a container.
//
// This exists because neither of the other read workloads can see what
// read-ahead granularity costs. `seq` is one cursor walking forwards, which
// is the case a large granule is chosen for and always wins. `rand` is
// uniformly random, and Cc's read-ahead is pattern-triggered: with no two
// consecutive reads adjacent, no lookahead is ever armed, so granularity
// is not merely unhelpful there, it is inert -- measured amplification of
// exactly 1.0 at every setting.
//
// The playback case is neither. A container holds a video track, an audio
// track and a subtitle track, and the demuxer advances all of them through
// one file object, so each cursor is locally sequential while the merged
// request stream jumps between regions far apart. That is the pattern that
// arms read-ahead on every cursor and then abandons most of what it fetched
// when the next read lands in a different track -- which is where a 512 KB
// granule turns a 4 KB subtitle read into half a megabyte over the network.
//
// Tracks are spread evenly across the file rather than packed, because a
// real container interleaves at a fine grain but its tracks span the whole
// duration; cursors a few kilobytes apart would be one sequential stream
// wearing a disguise and would measure `seq` again.
//
// Stride is the parameter that decides whether this is a measurement or a
// restatement of `seq`, and it is worth being explicit about why. A track's
// consecutive packets are not adjacent on disk: the other tracks' packets
// sit between them, so a cursor advances by much more than it reads. With
// stride equal to block -- the first version of this workload -- each cursor
// walks a contiguous run, read-ahead consumes everything it fetches, and the
// measured amplification was 1.002 at 99.8% sequential. That is read-ahead
// working perfectly, which is the answer for a dense track and not the
// question. With stride well above block, each granule serves only
// block/stride of its own fetch before the cursor jumps past the rest, which
// is where a large granule becomes a tax and what the real trace's 70%
// sequential share and 22x amplification look like.
//
// Burst is the third knob and the one that makes the other two describe a
// real trace rather than a corner of one. A demuxer does not alternate
// tracks every read: it pulls a whole frame -- several adjacent blocks --
// then switches. Without that, every read lands somewhere new, the driver's
// sequential test (which is exact adjacency) reports ~1%, and Cc never arms
// read-ahead at all. With it, each burst is adjacent enough to arm
// read-ahead and short enough to abandon most of the granule at the jump.
//
// Calibration target is the captured playback trace: 70.4% sequential and
// 22x amplification. Bursts of B blocks give (B-1)/B sequential by
// construction, so B=4 is 75% -- and the granule wasted is what the sweep
// is there to measure.
//
// SuppressReadAhead passes FILE_FLAG_RANDOM_ACCESS, which asks Cc not to
// read ahead on this handle. Every other mode here deliberately withholds
// that flag, because suppressing read-ahead switches off the behaviour a
// granularity sweep exists to measure. It is offered as an explicit mode
// because it is the controlled way to ask whether read-ahead is what an
// application is actually waiting on: if the tail collapses to the demand
// latency when read-ahead is suppressed, the tail was read-ahead's, and if
// it does not, it was never read-ahead's to begin with.
//
static bool RunDemux(
    const wchar_t* path,
    unsigned long tracks,
    DWORD blockSize,
    DWORD stride,
    unsigned long burst,
    unsigned long count,
    bool suppressReadAhead,
    unsigned long paceKbPerSecond,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        suppressReadAhead ? (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS)
                          : FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open workload file");
        return false;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(file, &fileSize))
    {
        PrintLastError("GetFileSizeEx");
        CloseHandle(file);
        return false;
    }

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (!buffer)
    {
        PrintLastError("allocate read buffer");
        CloseHandle(file);
        return false;
    }

    std::vector<unsigned long long> cursor(tracks);
    const unsigned long long span = static_cast<unsigned long long>(fileSize.QuadPart) / tracks;

    for (unsigned long t = 0; t < tracks; ++t)
    {
        cursor[t] = span * t;
    }

    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    unsigned long long total = 0;

    Pacer pacer;

    if (!PacerInit(&pacer, blockSize, paceKbPerSecond, 0.0))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        return false;
    }

    for (unsigned long i = 0; i < count; ++i)
    {
        const double due = PacerDue(&pacer);

        //
        // Track switches every `burst` reads, not every read: the reads
        // within a burst are adjacent, and the jump happens between bursts.
        //
        const unsigned long track = (i / burst) % tracks;
        const bool lastOfBurst = ((i % burst) == (burst - 1));

        //
        // A cursor that runs off the end of its own span wraps within that
        // span rather than walking into the next track's region, which
        // would quietly merge two cursors into one and reduce the track
        // count the run is labelled with.
        //
        if (cursor[track] + stride > span * (track + 1))
        {
            cursor[track] = span * track;
        }

        LARGE_INTEGER offset;
        offset.QuadPart = static_cast<long long>(cursor[track]);

        OVERLAPPED overlapped = {};
        overlapped.Offset = offset.LowPart;
        overlapped.OffsetHigh = static_cast<DWORD>(offset.HighPart);

        DWORD read = 0;

        if (!ReadFile(file, buffer, blockSize, &read, &overlapped))
        {
            if (ERROR_HANDLE_EOF != GetLastError())
            {
                PrintLastError("ReadFile (demux)");
                break;
            }
        }

        //
        // Inside a burst the cursor advances by exactly the block, so the
        // next read is adjacent and the driver counts it sequential. Only
        // the last read of a burst pays the stride, which is what skips
        // over the other tracks' packets and abandons the rest of whatever
        // read-ahead fetched.
        //
        cursor[track] += lastOfBurst ? stride : blockSize;
        total += read;

        PacerComplete(&pacer, due);
        PacerWait(&pacer);
    }

    QueryPerformanceCounter(&end);

    PacerReport(&pacer, paceKbPerSecond, blockSize);
    PacerDestroy(&pacer);
    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    *bytesOut = total;
    *secondsOut = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    return true;
}

//
// A consumer with a deadline, which is the workload this harness did not
// have. Every other mode here reads flat out, and a reader going flat out
// is permanently at the read-ahead frontier: it consumes each granule the
// instant Cc produces it, so it is maximally exposed to fetch latency and
// the "over one frame" figure reported for it is hypothetical. A player is
// normally behind the frontier, which is the condition under which the
// driver was seen issuing 67 ms fetches while the reader saw 0.11 ms.
//
// Blocks are read on a fixed schedule rather than as fast as they arrive.
// Block n is due at start + (n+1) * blockSize / rate, and the metric is how
// many blocks missed their due time and by how much -- a viewer's hitch,
// measured against a deadline that exists, rather than a read that happened
// to take longer than a frame interval on a reader with no deadline.
//
// Choose blockSize and rate together so the interval is a frame: 64 KB at
// 1536 KB/s is 41.67 ms, which is 24 fps at roughly a Blu-ray bitrate.
//
// A player that falls behind does not skip forward to catch up, and neither
// does this: the schedule is absolute from the start, so sustained lateness
// accumulates and shows up as a growing miss count instead of being hidden
// by a reader that quietly drops its rate to whatever the link can deliver.
//
static bool RunPlayback(
    const wchar_t* path,
    unsigned long kbPerSecond,
    DWORD blockSize,
    double seconds,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open workload file");
        return false;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(file, &fileSize))
    {
        PrintLastError("GetFileSizeEx");
        CloseHandle(file);
        return false;
    }

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (nullptr == buffer)
    {
        PrintLastError("VirtualAlloc");
        CloseHandle(file);
        return false;
    }

    Pacer pacer;

    if (!PacerInit(&pacer, blockSize, kbPerSecond, 0.0))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        return false;
    }

    unsigned long long total = 0;
    long long offset = 0;

    while (PacerElapsed(&pacer) < seconds)
    {
        if (offset + static_cast<long long>(blockSize) > fileSize.QuadPart)
        {
            offset = 0;
        }

        const double due = PacerDue(&pacer);

        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);

        DWORD read = 0;

        if (!ReadFile(file, buffer, blockSize, &read, &overlapped))
        {
            PrintLastError("ReadFile");
            break;
        }

        if (0 == read)
        {
            break;
        }

        offset += read;
        total += read;

        PacerComplete(&pacer, due);
        PacerWait(&pacer);
    }

    const double ran = PacerElapsed(&pacer);

    PacerReport(&pacer, kbPerSecond, blockSize);
    PacerDestroy(&pacer);
    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    *bytesOut = total;
    *secondsOut = ran;

    return true;
}

//
// Two handles on ONE file, with patterns that want opposite granules.
//
// The read-ahead policy keeps its state on the FCB, which every handle to a
// file shares, while Cc keeps the granularity itself in each handle's
// PrivateCacheMap. The reasoning for that split is in Structs.h; what it
// predicts is a hazard this workload exists to look for. One handle's
// verdict is applied to whichever file object closed the window, so a copy
// that grows the shared verdict and a seeking reader that shrinks it are
// writing to one number and reading it back for different consumers.
//
// The greedy half is always a sequential reader going flat out -- the copy.
// The other half is the argument: another copy (same workload, so the two
// should agree), a bursty demuxer (opposite, and the pattern a large
// granule punishes), or a paced player (opposite, and the one with a
// deadline to miss).
//
// Each half reports its own throughput, and the driver's grow and shrink
// counts say whether the shared verdict moved at all.
//
enum class CompeteMode
{
    Sequential,
    Bursty,
    Paced
};

struct CompeteContext
{
    const wchar_t* Path;
    double DeadlineSeconds;
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start;
    CompeteMode Mode;

    // Which half of the file this handle owns, so the two do not share data.
    long long Half;

    // Bytes this half actually consumed, and what it missed if it was paced.
    unsigned long long Bytes;
    unsigned long long Blocks;
    unsigned long long Missed;
    double WorstLateness;
};

static DWORD WINAPI CompeteWorker(LPVOID parameter)
{
    CompeteContext* ctx = static_cast<CompeteContext*>(parameter);

    HANDLE file = CreateFileW(
        ctx->Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open competing handle");
        return 0;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(file, &fileSize))
    {
        PrintLastError("GetFileSizeEx");
        CloseHandle(file);
        return 0;
    }

    const DWORD block = kWorkloadReadSize;

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, block, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (nullptr == buffer)
    {
        PrintLastError("VirtualAlloc");
        CloseHandle(file);
        return 0;
    }

    Pacer pacer;

    if (!PacerInit(&pacer, block, (CompeteMode::Paced == ctx->Mode) ? 3072u : 0u, 0.0))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        return 0;
    }

    //
    // Each handle owns half the file, so neither is reading what the other
    // just pulled into the cache. Sharing a range made both halves run at
    // thousands of MB/s and never touched the fetch path, which is where the
    // shared-FCB hazard this workload exists for actually lives.
    //
    const long long half = (fileSize.QuadPart - static_cast<long long>(block) - 1) / 2;
    const long long base = ctx->Half * half;
    const long long span = half;
    long long cursor = base;

    Sampler sampler;
    SamplerInit(&sampler, 0.25);
    unsigned long long index = 0;

    for (;;)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        const double elapsed = static_cast<double>(now.QuadPart - ctx->Start.QuadPart) /
                               static_cast<double>(ctx->Frequency.QuadPart);

        if (elapsed >= ctx->DeadlineSeconds)
        {
            break;
        }

        const double due = PacerDue(&pacer);

        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(cursor & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>((cursor >> 32) & 0xFFFFFFFF);

        DWORD read = 0;

        LARGE_INTEGER readStart;
        QueryPerformanceCounter(&readStart);

        if (!ReadFile(file, buffer, block, &read, &overlapped) || 0 == read)
        {
            break;
        }

        LARGE_INTEGER readEnd;
        QueryPerformanceCounter(&readEnd);

        SamplerRecord(&sampler, 1000.0 * static_cast<double>(readEnd.QuadPart - readStart.QuadPart) /
            static_cast<double>(ctx->Frequency.QuadPart), read, readEnd);

        ctx->Bytes += read;
        ++index;

        if (CompeteMode::Bursty == ctx->Mode)
        {
            cursor += (0 == (index % 4)) ? (1024 * 1024) : block;
        }
        else
        {
            cursor += block;
        }

        if (span > 0 && (cursor - base) > span)
        {
            cursor = base;
        }

        PacerComplete(&pacer, due);
        PacerWait(&pacer);
    }

        SamplerReport(&sampler, (0 == ctx->Half) ? "handle 0" : "handle 1");

    ctx->Blocks = pacer.Blocks;
    ctx->Missed = pacer.Missed;
    ctx->WorstLateness = pacer.WorstLateness;

    PacerDestroy(&pacer);
    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);

    return 0;
}

static bool RunCompete(
    const wchar_t* path,
    CompeteMode mode,
    double seconds,
    bool swapHalves,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    HANDLE control = CreateFileW(
        BLORGFS_FSDO_USERMODE_PATH, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (INVALID_HANDLE_VALUE == control)
    {
        PrintLastError("open the driver control device");
        return false;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    CompeteContext contexts[2] = {};
    HANDLE threads[2] = {};

    for (int i = 0; i < 2; ++i)
    {
        contexts[i].Path = path;
        contexts[i].DeadlineSeconds = seconds;
        contexts[i].Frequency = frequency;
        contexts[i].Start = start;
        contexts[i].Mode = (0 == i) ? CompeteMode::Sequential : mode;
        contexts[i].Half = swapHalves ? (1 - i) : i;

        threads[i] = CreateThread(nullptr, 0, CompeteWorker, &contexts[i], 0, nullptr);

        if (!threads[i])
        {
            PrintLastError("CreateThread");
            CloseHandle(control);
            return false;
        }
    }

    WaitForMultipleObjects(2, threads, TRUE, INFINITE);

    for (int i = 0; i < 2; ++i)
    {
        CloseHandle(threads[i]);
    }

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    BLORGFS_STATISTICS_RESPONSE after = {};
    const bool haveAfter = QueryDriverStatistics(control, &after);

    CloseHandle(control);

    const double ran =
        static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    const char* names[2] = { "sequential (copy)", nullptr };
    names[1] = (CompeteMode::Sequential == mode) ? "sequential (copy)"
             : (CompeteMode::Bursty == mode) ? "bursty (demuxer)"
             : "paced 3072 (player)";

    printf("\n  competing handles on one file\n");

    for (int i = 0; i < 2; ++i)
    {
        printf("    handle %d  %-20s %8.2f MB  %7.2f MB/s  (half %lld)\n",
            i, names[i], contexts[i].Bytes / (1024.0 * 1024.0),
            (ran > 0.0) ? ((contexts[i].Bytes / (1024.0 * 1024.0)) / ran) : 0.0,
            contexts[i].Half);
    }

    if (CompeteMode::Paced == mode)
    {
        printf("    player missed         %12llu of %llu  (worst %.2f ms)\n",
            contexts[1].Missed, contexts[1].Blocks, contexts[1].WorstLateness * 1000.0);
    }

    if (haveAfter)
    {
        printf("    granule grow/shrink   %12llu / %llu\n",
            after.Totals.ReadAheadGrows, after.Totals.ReadAheadShrinks);
    }

    *bytesOut = contexts[0].Bytes + contexts[1].Bytes;
    *secondsOut = ran;

    return true;
}

//
// A reader that changes its mind: sequential, then seeking, on one handle.
//
// Every other workload here holds one pattern for its whole run, so nothing
// measured what the adaptive policy costs while it is catching up. That is
// the interesting moment: a granule grown for a file copy is exactly wrong
// for the seek that follows it, and the policy only unwinds on evidence --
// two agreeing windows per halving, and a window is two granules wide, so
// the bytes it takes to come back down scale with how far it went up.
//
// The phases are counted separately against the driver's own counters,
// snapshotted between them, because the question is not the wall clock of
// either half but what the seeking half fetched against what it consumed
// while the granule was still large.
//
// Offsets come from a fixed linear congruential sequence rather than rand(),
// so the same run is comparable across arms.
//
static bool RunMixed(
    const wchar_t* path,
    unsigned long sequentialMb,
    DWORD blockSize,
    unsigned long randomCount,
    DWORD stride,
    unsigned long burst,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    HANDLE control = CreateFileW(
        BLORGFS_FSDO_USERMODE_PATH, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

    if (INVALID_HANDLE_VALUE == control)
    {
        PrintLastError("open the driver control device");
        return false;
    }

    HANDLE file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (INVALID_HANDLE_VALUE == file)
    {
        PrintLastError("open workload file");
        CloseHandle(control);
        return false;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(file, &fileSize))
    {
        PrintLastError("GetFileSizeEx");
        CloseHandle(file);
        CloseHandle(control);
        return false;
    }

    unsigned char* buffer = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (nullptr == buffer)
    {
        PrintLastError("VirtualAlloc");
        CloseHandle(file);
        CloseHandle(control);
        return false;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER afterSequential;
    LARGE_INTEGER end;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    const unsigned long long sequentialTarget =
        static_cast<unsigned long long>(sequentialMb) * 1024ull * 1024ull;

    unsigned long long sequentialBytes = 0;

    while (sequentialBytes < sequentialTarget)
    {
        DWORD read = 0;

        if (!ReadFile(file, buffer, blockSize, &read, nullptr) || 0 == read)
        {
            break;
        }

        sequentialBytes += read;
    }

    QueryPerformanceCounter(&afterSequential);

    BLORGFS_STATISTICS_RESPONSE mid = {};

    if (!QueryDriverStatistics(control, &mid))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(file);
        CloseHandle(control);
        return false;
    }

    const long long span = fileSize.QuadPart - static_cast<long long>(blockSize) - 1;
    unsigned long long randomBytes = 0;
    unsigned long long cursor = 2246822519ull;

    long long burstCursor = static_cast<long long>(sequentialBytes);

    for (unsigned long i = 0; i < randomCount; ++i)
    {
        cursor = (cursor * 6364136223846793005ull) + 1442695040888963407ull;

        long long offset;

        if (burst > 0)
        {
            const bool lastOfBurst = (0 == ((i + 1) % burst));

            offset = burstCursor;
            burstCursor += lastOfBurst ? static_cast<long long>(stride)
                                       : static_cast<long long>(blockSize);

            if (span > 0 && burstCursor > span)
            {
                burstCursor = 0;
            }
        }
        else
        {
            offset = (span > 0)
                ? static_cast<long long>((cursor >> 16) % static_cast<unsigned long long>(span))
                : 0;
        }

        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);

        DWORD read = 0;

        if (!ReadFile(file, buffer, blockSize, &read, &overlapped) || 0 == read)
        {
            break;
        }

        randomBytes += read;
    }

    QueryPerformanceCounter(&end);

    BLORGFS_STATISTICS_RESPONSE after = {};
    const bool haveAfter = QueryDriverStatistics(control, &after);

    VirtualFree(buffer, 0, MEM_RELEASE);
    CloseHandle(file);
    CloseHandle(control);

    const double sequentialSeconds =
        static_cast<double>(afterSequential.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
    const double randomSeconds =
        static_cast<double>(end.QuadPart - afterSequential.QuadPart) / static_cast<double>(frequency.QuadPart);

    printf("\n  phase split (one handle, sequential then seeking)\n");
    printf("    sequential            %12.1f MB in %.2f s  (%.2f MB/s)\n",
        sequentialBytes / (1024.0 * 1024.0), sequentialSeconds,
        (sequentialSeconds > 0.0) ? ((sequentialBytes / (1024.0 * 1024.0)) / sequentialSeconds) : 0.0);
    printf("    random                %12.1f MB in %.2f s  (%.2f MB/s)\n",
        randomBytes / (1024.0 * 1024.0), randomSeconds,
        (randomSeconds > 0.0) ? ((randomBytes / (1024.0 * 1024.0)) / randomSeconds) : 0.0);

    if (haveAfter)
    {
        const unsigned long long fetched = after.Totals.FetchBytes - mid.Totals.FetchBytes;
        const unsigned long long grows = after.Totals.ReadAheadGrows - mid.Totals.ReadAheadGrows;
        const unsigned long long shrinks = after.Totals.ReadAheadShrinks - mid.Totals.ReadAheadShrinks;

        printf("    random phase fetched  %12.1f MB for %.1f MB consumed  (%.2fx)\n",
            fetched / (1024.0 * 1024.0), randomBytes / (1024.0 * 1024.0),
            (randomBytes > 0) ? (static_cast<double>(fetched) / static_cast<double>(randomBytes)) : 0.0);
        printf("    grow/shrink           %12llu / %llu in random phase  (%llu / %llu by end of sequential)\n",
            grows, shrinks, mid.Totals.ReadAheadGrows, mid.Totals.ReadAheadShrinks);
    }

    *bytesOut = sequentialBytes + randomBytes;
    *secondsOut = sequentialSeconds + randomSeconds;

    return true;
}

//
// Metadata storm: enumerate a directory and open/close every entry,
// repeated. This is the workload the path cache and the listing cache
// exist for, so the number that matters in the report is the path cache
// hit rate and the dir-listing request count, not the wall clock.
//
static bool RunMetadata(
    const wchar_t* directory,
    unsigned long passes,
    unsigned long long* opensOut,
    double* secondsOut)
{
    std::wstring pattern(directory);

    if (!pattern.empty() && pattern.back() != L'\\')
    {
        pattern += L'\\';
    }

    const std::wstring base = pattern;
    pattern += L'*';

    LARGE_INTEGER start;
    LARGE_INTEGER end;
    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    unsigned long long opens = 0;

    for (unsigned long pass = 0; pass < passes; ++pass)
    {
        WIN32_FIND_DATAW findData;
        HANDLE find = FindFirstFileW(pattern.c_str(), &findData);

        if (INVALID_HANDLE_VALUE == find)
        {
            PrintLastError("FindFirstFile");
            return false;
        }

        do
        {
            if (0 == wcscmp(findData.cFileName, L".") || 0 == wcscmp(findData.cFileName, L".."))
            {
                continue;
            }

            std::wstring full = base + findData.cFileName;

            HANDLE entry = CreateFileW(
                full.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr);

            if (INVALID_HANDLE_VALUE != entry)
            {
                CloseHandle(entry);
                opens++;
            }
        }
        while (FindNextFileW(find, &findData));

        FindClose(find);
    }

    QueryPerformanceCounter(&end);

    *opensOut = opens;
    *secondsOut = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);

    return true;
}

static void PrintThroughput(const char* label, unsigned long long bytes, double seconds)
{
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);

    printf("\n=== %s ===\n", label);
    printf("    bytes                 %12llu (%.1f MB)\n", bytes, megabytes);
    printf("    elapsed               %12.3f s\n", seconds);
    printf("    throughput            %12.2f MB/s\n", (seconds > 0.0) ? (megabytes / seconds) : 0.0);
}

//
// Writes the flat key=value report. Every counter goes out raw, plus the
// derived ratios, because a baseline is only useful if the thing that
// regressed is actually in it -- recomputing a ratio from two raw
// counters is easy, but a counter that was never recorded is gone.
//
static bool WriteReport(
    const wchar_t* path,
    const char* workload,
    unsigned long long bytes,
    unsigned long long opens,
    double seconds,
    const BLORGFS_STATISTICS_RESPONSE& stats)
{
    FILE* f = nullptr;

    if (0 != _wfopen_s(&f, path, L"w") || !f)
    {
        printf("  [FAIL] cannot write report to %ws\n", path);
        return false;
    }

    const BLORGFS_STATISTICS& t = stats.Totals;

    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const unsigned long long acquires = t.ConnectionsPooled + t.ConnectionsFresh;

    LatencyPercentiles p = ComputePercentiles(t.FetchLatencyBuckets);

    fprintf(f, "Workload=%s\n", workload);
    fprintf(f, "ElapsedSeconds=%.6f\n", seconds);
    fprintf(f, "Bytes=%llu\n", bytes);
    fprintf(f, "Opens=%llu\n", opens);
    fprintf(f, "ThroughputMBs=%.4f\n", (seconds > 0.0) ? (megabytes / seconds) : 0.0);
    fprintf(f, "OpensPerSec=%.4f\n", (seconds > 0.0) ? (static_cast<double>(opens) / seconds) : 0.0);

    fprintf(f, "ReadsCached=%llu\n", t.ReadsCached);
    fprintf(f, "ReadsPagingInline=%llu\n", t.ReadsPagingInline);
    fprintf(f, "ReadsPosted=%llu\n", t.ReadsPosted);
    fprintf(f, "ReadsSequential=%llu\n", t.ReadsSequential);
    fprintf(f, "ReadsEndOfFile=%llu\n", t.ReadsEndOfFile);
    fprintf(f, "ReadsSpeculative=%llu\n", t.ReadsSpeculative);
    fprintf(f, "ReadsDemand=%llu\n", t.ReadsDemand);
    fprintf(f, "ReadAheadShrinks=%llu\n", t.ReadAheadShrinks);
    fprintf(f, "ReadAheadGrows=%llu\n", t.ReadAheadGrows);
    fprintf(f, "SpeculativeLatencyMeanUs=%llu\n",
        (t.ReadsSpeculative > 0) ? (t.SpeculativeLatencySumUs / t.ReadsSpeculative) : 0ull);
    fprintf(f, "SpeculativeLatencyMaxUs=%llu\n", t.SpeculativeLatencyMaxUs);
    fprintf(f, "DemandLatencyMeanUs=%llu\n",
        (t.ReadsDemand > 0) ? (t.DemandLatencySumUs / t.ReadsDemand) : 0ull);
    fprintf(f, "DemandLatencyMaxUs=%llu\n", t.DemandLatencyMaxUs);
    fprintf(f, "UserFileReads=%llu\n", t.UserFileReads);
    fprintf(f, "UserFileReadBytes=%llu\n", t.UserFileReadBytes);

    //
    // What the application waited, in the machine-readable form. The human
    // table has carried these since they were added and the report did not,
    // so any sweep driven off this file compared throughput and inferred
    // the viewer's experience from it -- the exact substitution the
    // application-visible measurement exists to stop.
    //
    // UserReadsOverFrame counts reads longer than a 24 fps frame interval
    // (41667 us), by the same bucket walk the table prints, because the
    // number that decides whether playback stutters is not the mean but
    // how many reads a viewer could have seen.
    //
    const LatencyPercentiles u = ComputePercentiles(t.UserReadLatencyBuckets);

    unsigned long long overFrame = 0;

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        if ((1ull << i) > 41667ull)
        {
            overFrame += t.UserReadLatencyBuckets[i];
        }
    }

    fprintf(f, "UserReadSamples=%llu\n", t.UserReadSamples);
    fprintf(f, "UserReadLatencyMeanUs=%llu\n",
        (t.UserReadSamples > 0) ? (t.UserReadLatencySumUs / t.UserReadSamples) : 0ull);
    fprintf(f, "UserReadLatencyMaxUs=%llu\n", t.UserReadLatencyMaxUs);
    fprintf(f, "UserReadLatencyP50Us=%llu\n", u.P50UpperUs);
    fprintf(f, "UserReadLatencyP90Us=%llu\n", u.P90UpperUs);
    fprintf(f, "UserReadLatencyP99Us=%llu\n", u.P99UpperUs);
    fprintf(f, "UserReadsOverFrame=%llu\n", overFrame);
    fprintf(f, "UserReadsOverFrameShare=%.6f\n", SafeRatio(overFrame, t.UserReadSamples));

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        fprintf(f, "UserReadLatencyBucket%02d=%llu\n", i, t.UserReadLatencyBuckets[i]);
    }

    fprintf(f, "NonCachedReads=%llu\n", t.NonCachedReads);
    fprintf(f, "NonCachedReadBytes=%llu\n", t.NonCachedReadBytes);


    fprintf(f, "FetchesIssued=%llu\n", t.FetchesIssued);
    fprintf(f, "FetchesCompleted=%llu\n", t.FetchesCompleted);
    fprintf(f, "FetchesFailed=%llu\n", t.FetchesFailed);
    fprintf(f, "FetchBytes=%llu\n", t.FetchBytes);
    fprintf(f, "FetchLatencyMeanUs=%llu\n",
        (t.FetchesCompleted > 0) ? (t.FetchLatencySumUs / t.FetchesCompleted) : 0ULL);
    fprintf(f, "FetchLatencyMaxUs=%llu\n", t.FetchLatencyMaxUs);
    fprintf(f, "FetchLatencyP50Us=%llu\n", p.P50UpperUs);
    fprintf(f, "FetchLatencyP90Us=%llu\n", p.P90UpperUs);
    fprintf(f, "FetchLatencyP99Us=%llu\n", p.P99UpperUs);
    fprintf(f, "FetchLatencySamples=%llu\n", p.Samples);
    fprintf(f, "FetchesActive=%lld\n", stats.FetchesActive);

    //
    // The per-phase split, in the machine-readable form. The human table has
    // printed these since they were added and the report did not, so every
    // comparison driven off this file could see that a fetch was slow and
    // not where the time went -- which is the difference between tuning the
    // granule and finding out that most of a small fetch is fixed overhead.
    //
    fprintf(f, "FetchPhaseSamples=%llu\n", t.FetchSplitSamples);
    fprintf(f, "FetchAcquireMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchAcquireSumUs / t.FetchSplitSamples) : 0ull);
    fprintf(f, "FetchPreSendMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchPreSendSumUs / t.FetchSplitSamples) : 0ull);
    fprintf(f, "FetchSendMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchSendSumUs / t.FetchSplitSamples) : 0ull);
    fprintf(f, "FetchWaitMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchWaitSumUs / t.FetchSplitSamples) : 0ull);
    fprintf(f, "FetchTtfbMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchTtfbSumUs / t.FetchSplitSamples) : 0ull);
    fprintf(f, "FetchBodyMeanUs=%llu\n",
        (t.FetchSplitSamples > 0) ? (t.FetchBodySumUs / t.FetchSplitSamples) : 0ull);

    for (int i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS; ++i)
    {
        fprintf(f, "FetchLatencyBucket%02d=%llu\n", i, t.FetchLatencyBuckets[i]);
    }

    fprintf(f, "DirInfoRequests=%llu\n", t.DirInfoRequests);
    fprintf(f, "DirInfoFailures=%llu\n", t.DirInfoFailures);
    fprintf(f, "DirInfoLatencyMeanUs=%llu\n",
        (t.DirInfoRequests > 0) ? (t.DirInfoLatencySumUs / t.DirInfoRequests) : 0ULL);
    fprintf(f, "FileInfoRequests=%llu\n", t.FileInfoRequests);
    fprintf(f, "FileInfoFailures=%llu\n", t.FileInfoFailures);
    fprintf(f, "FileInfoLatencyMeanUs=%llu\n",
        (t.FileInfoRequests > 0) ? (t.FileInfoLatencySumUs / t.FileInfoRequests) : 0ULL);
    fprintf(f, "CreateHits=%llu\n", t.CreateHits);
    fprintf(f, "SuccessfulCreates=%llu\n", t.SuccessfulCreates);
    fprintf(f, "FailedCreates=%llu\n", t.FailedCreates);
    fprintf(f, "PathCacheHits=%llu\n", t.PathCacheHits);
    fprintf(f, "PathCacheMisses=%llu\n", t.PathCacheMisses);
    fprintf(f, "PathCacheHitRate=%.4f\n", SafeRatio(t.PathCacheHits, t.PathCacheHits + t.PathCacheMisses));

    fprintf(f, "ConnectionsPooled=%llu\n", t.ConnectionsPooled);
    fprintf(f, "ConnectionsFresh=%llu\n", t.ConnectionsFresh);
    fprintf(f, "ConnectionsFailed=%llu\n", t.ConnectionsFailed);
    fprintf(f, "ConnectionReuseRate=%.4f\n", SafeRatio(t.ConnectionsPooled, acquires));
    fprintf(f, "ConnectionsReleasedToPool=%llu\n", t.ConnectionsReleasedToPool);
    fprintf(f, "ConnectionsClosedPoolFull=%llu\n", t.ConnectionsClosedPoolFull);
    fprintf(f, "KeepAliveRetries=%llu\n", t.KeepAliveRetries);
    fprintf(f, "SocketTimeouts=%llu\n", t.SocketTimeouts);

    fprintf(f, "HandshakesStarted=%llu\n", t.HandshakesStarted);
    fprintf(f, "HandshakesCompleted=%llu\n", t.HandshakesCompleted);
    fprintf(f, "HandshakesFailed=%llu\n", t.HandshakesFailed);
    fprintf(f, "HandshakeLatencyMeanUs=%llu\n",
        (t.HandshakesCompleted > 0) ? (t.HandshakeLatencySumUs / t.HandshakesCompleted) : 0ULL);
    fprintf(f, "HandshakeLatencyMaxUs=%llu\n", t.HandshakeLatencyMaxUs);
    fprintf(f, "TlsRecordsDecrypted=%llu\n", t.TlsRecordsDecrypted);
    fprintf(f, "TlsBytesDecrypted=%llu\n", t.TlsBytesDecrypted);
    fprintf(f, "TlsBulkReceives=%llu\n", t.TlsBulkReceives);

    fclose(f);

    printf("\n  [OK] report written to %ws\n", path);
    return true;
}

static void PrintUsage(void)
{
    printf("BlorgFS performance harness\n\n");
    printf("Usage: PerfHarness <command> [args]\n\n");
    printf("  stats [drive]                  print driver counters (no reset, unelevated)\n");
    printf("  fsstats [drive]                print the standard statistics FSCTL\n");
    printf("  reset [drive]                  start a fresh measurement window (elevated)\n");
    printf("  seq <file> [buffered]          sequential read; unbuffered unless 'buffered'\n");
    printf("  rand <file> <blockKB> <count> [buffered]\n");
    printf("                                 random reads at a fixed block size;\n");
    printf("                                 unbuffered unless 'buffered' -- buffered puts Cc in\n");
    printf("                                 front, which is what makes read-ahead granularity\n");
    printf("                                 observable at all\n");
    printf("  demux <file> <tracks> <blockKB> <count> [strideKB] [burst] [suppressra]\n");
    printf("                                 N cursors interleaved through one buffered\n");
    printf("                                 handle -- a demuxer reading a container. Each\n");
    printf("                                 reads blockKB then skips to strideKB later, the\n");
    printf("                                 way a track's packets are separated by the other\n");
    printf("                                 tracks'. strideKB defaults to blockKB, which is a\n");
    printf("                                 dense track and measures read-ahead working;\n");
    printf("                                 raise it to measure what a granule costs when\n");
    printf("                                 most of it is skipped\n");
    printf("  play <file> <KBps> <blockKB> <secs>\n");
    printf("                                 a consumer with a deadline: blocks read on a\n");
    printf("                                 fixed schedule rather than flat out, reporting\n");
    printf("                                 deadline misses. Every other mode reads as fast\n");
    printf("                                 as it can, which sits permanently at the\n");
    printf("                                 read-ahead frontier and makes its 'over one\n");
    printf("                                 frame' figure hypothetical. 64 KB at 1536 KB/s\n");
    printf("                                 is a 41.67 ms frame\n");
    printf("  mixed <file> <seqMB> <blockKB> <count> [strideKB] [burst]\n");
    printf("                                 read seqMB sequentially then count random blocks\n");
    printf("                                 on the SAME handle, reporting each phase and what\n");
    printf("                                 the random half fetched while the granule was\n");
    printf("                                 still sized for the sequential half\n");
    printf("                                 with strideKB and burst the second phase is bursty\n");
    printf("                                 rather than random -- the pattern that DOES arm Cc\n");
    printf("                                 read-ahead, so the granule it inherits can matter\n");
    printf("  compete <file> seq|bursty|paced <secs>\n");
    printf("                                 two handles on ONE file: a sequential copy against\n");
    printf("                                 another copy, a demuxer, or a paced player. They\n");
    printf("                                 share one FCB and so one policy verdict, which is\n");
    printf("                                 the hazard this measures\n");
    printf("  meta <dir> <passes>            enumerate + open every entry, repeatedly\n");
    printf("  streams <dir> <count> [secs] [unbuffered]\n");
    printf("                                 N concurrent sequential readers, one file each;\n");
    printf("                                 'unbuffered' bypasses Cc so every read reaches the driver\n");
    printf("                                 reports aggregate, fairness and latency tail\n\n");
    printf("  pace=<KBps>                    (demux, streams) read to a schedule instead of flat\n");
    printf("                                 out, reporting missed deadlines. Without it those\n");
    printf("                                 modes sit at the read-ahead frontier and cannot miss\n");
    printf("                                 one, which is not what the player they stand for does\n");
    printf("  --report <path>                (any workload) also write key=value metrics\n");
    printf("                                 for Compare-BlorgMetrics.ps1; position-independent\n\n");
    printf("Workload commands reset the counters first, run, then report both the\n");
    printf("wall-clock result and what the driver counted underneath it.\n");
    printf("Default drive is B:.\n");
}

//
// A workload command is always reset -> run -> report, because a counter
// window that spans unrelated activity cannot be attributed to the
// workload, and attribution is the entire point.
//
//
// Lifts --report <path> out of the argument vector and returns the
// remaining arguments, so the positional parsing below can stay index
// based. Removing the flag rather than requiring it last matters: the
// workload commands read argv[3] and argv[4] by position, and a stray
// flag shifting those by one would yield a plausible-looking measurement
// of the wrong thing rather than an error.
//
static const wchar_t* ExtractReportPath(std::vector<wchar_t*>* args)
{
    const wchar_t* reportPath = nullptr;

    for (size_t i = 1; i < args->size(); ++i)
    {
        if (0 != wcscmp((*args)[i], L"--report"))
        {
            continue;
        }

        if (i + 1 < args->size())
        {
            reportPath = (*args)[i + 1];
            args->erase(args->begin() + i, args->begin() + i + 2);
        }
        else
        {
            args->erase(args->begin() + i);
        }

        break;
    }

    return reportPath;
}

static int RunWorkloadCommand(int argc, wchar_t** argv, const wchar_t* drive, const wchar_t* reportPath)
{
    HandlePair handles = {};

    if (!OpenHandles(drive, true, true, false, &handles))
    {
        return 1;
    }

    if (!ResetDriverStatistics(handles.Control))
    {
        CloseHandles(&handles);
        return 1;
    }

    bool ok = false;
    unsigned long long bytes = 0;
    unsigned long long opens = 0;
    double seconds = 0.0;
    char label[256] = {};

    if (0 == wcscmp(argv[1], L"seq"))
    {
        const bool buffered = (argc > 3) && (0 == wcscmp(argv[3], L"buffered"));

        ok = RunSequential(argv[2], !buffered, &bytes, &seconds);
        sprintf_s(label, "sequential read (%s)", buffered ? "buffered" : "unbuffered");
    }
    else if (0 == wcscmp(argv[1], L"rand"))
    {
        if (argc < 5)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        //
        // _wtoi yields 0 for anything unparsable, and a zero block size or
        // count would otherwise run a "workload" that reads nothing and
        // reports a confident 0.00 MB/s -- a plausible-looking measurement
        // of a typo.
        //
        const int parsedBlockKb = _wtoi(argv[3]);
        const int parsedCount = _wtoi(argv[4]);

        if (parsedBlockKb <= 0 || parsedCount <= 0)
        {
            fprintf(stderr, "  [FAIL] blockKB and count must be positive integers (got '%ws', '%ws')\n", argv[3], argv[4]);
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const DWORD blockKb = static_cast<DWORD>(parsedBlockKb);
        const unsigned long count = static_cast<unsigned long>(parsedCount);

        const bool buffered = (argc > 5) && (0 == wcscmp(argv[5], L"buffered"));

        ok = RunRandom(argv[2], blockKb * 1024, count, buffered, &bytes, &seconds);
        sprintf_s(label, "random read (%lu KB x %lu, %s)", blockKb, count,
            buffered ? "buffered" : "unbuffered");
    }
    else if (0 == wcscmp(argv[1], L"play"))
    {
        if (argc < 6)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const int parsedRate = _wtoi(argv[3]);
        const int parsedBlockKb = _wtoi(argv[4]);
        const double parsedSeconds = _wtof(argv[5]);

        if (parsedRate <= 0 || parsedBlockKb <= 0 || parsedSeconds <= 0.0)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        ok = RunPlayback(
            argv[2],
            static_cast<unsigned long>(parsedRate),
            static_cast<DWORD>(parsedBlockKb) * 1024,
            parsedSeconds,
            &bytes,
            &seconds);

        sprintf_s(label, "playback (%d KB/s, %d KB blocks)", parsedRate, parsedBlockKb);
    }
    else if (0 == wcscmp(argv[1], L"compete"))
    {
        if (argc < 5)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        CompeteMode mode = CompeteMode::Sequential;

        if (0 == wcscmp(argv[3], L"bursty"))
        {
            mode = CompeteMode::Bursty;
        }
        else if (0 == wcscmp(argv[3], L"paced"))
        {
            mode = CompeteMode::Paced;
        }
        else if (0 != wcscmp(argv[3], L"seq"))
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const double parsedSeconds = _wtof(argv[4]);

        if (!(parsedSeconds > 0.0))
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const bool swapHalves = (argc > 5) && (0 == wcscmp(argv[5], L"swap"));

        ok = RunCompete(argv[2], mode, parsedSeconds, swapHalves, &bytes, &seconds);

        sprintf_s(label, "compete (sequential against %ws, %.0f s)", argv[3], parsedSeconds);
    }
    else if (0 == wcscmp(argv[1], L"mixed"))
    {
        if (argc < 6)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const int parsedSeqMb = _wtoi(argv[3]);
        const int parsedBlockKb = _wtoi(argv[4]);
        const int parsedCount = _wtoi(argv[5]);

        if (parsedSeqMb <= 0 || parsedBlockKb <= 0 || parsedCount <= 0)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const int parsedStrideKb = (argc > 6) ? _wtoi(argv[6]) : 0;
        const int parsedBurst = (argc > 7) ? _wtoi(argv[7]) : 0;

        ok = RunMixed(
            argv[2],
            static_cast<unsigned long>(parsedSeqMb),
            static_cast<DWORD>(parsedBlockKb) * 1024,
            static_cast<unsigned long>(parsedCount),
            static_cast<DWORD>((parsedStrideKb > 0) ? parsedStrideKb : parsedBlockKb) * 1024,
            static_cast<unsigned long>((parsedBurst > 0) ? parsedBurst : 0),
            &bytes,
            &seconds);

        sprintf_s(label, "mixed (%d MB sequential, then %d KB x %d %s)",
            parsedSeqMb, parsedBlockKb, parsedCount,
            (parsedBurst > 0) ? "bursty" : "random");
    }
    else if (0 == wcscmp(argv[1], L"demux"))
    {
        if (argc < 6)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const int parsedTracks = _wtoi(argv[3]);
        const int parsedBlockKb = _wtoi(argv[4]);
        const int parsedCount = _wtoi(argv[5]);
        const int parsedStrideKb = (argc > 6) ? _wtoi(argv[6]) : parsedBlockKb;
        const int parsedBurst = (argc > 7) ? _wtoi(argv[7]) : 1;

        if (parsedTracks <= 0 || parsedBlockKb <= 0 || parsedCount <= 0 ||
            parsedStrideKb <= 0 || parsedBurst <= 0)
        {
            fprintf(stderr, "  [FAIL] tracks, blockKB, count, strideKB and burst must be positive integers\n");
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        if (parsedStrideKb < parsedBlockKb)
        {
            fprintf(stderr, "  [FAIL] strideKB (%d) below blockKB (%d) would re-read the same bytes\n",
                parsedStrideKb, parsedBlockKb);
            CloseHandles(&handles);
            return 1;
        }

        const bool suppressReadAhead = (argc > 8) && (0 == wcscmp(argv[8], L"suppressra"));
        const unsigned long paceKbPerSecond = ParsePaceKbPerSecond(argc, argv, 6);

        ok = RunDemux(argv[2],
            static_cast<unsigned long>(parsedTracks),
            static_cast<DWORD>(parsedBlockKb) * 1024,
            static_cast<DWORD>(parsedStrideKb) * 1024,
            static_cast<unsigned long>(parsedBurst),
            static_cast<unsigned long>(parsedCount),
            suppressReadAhead,
            paceKbPerSecond,
            &bytes, &seconds);

        sprintf_s(label, "demux (%d tracks, %d KB x%d burst, +%d KB x %d%s)",
            parsedTracks, parsedBlockKb, parsedBurst, parsedStrideKb, parsedCount,
            suppressReadAhead ? ", read-ahead suppressed" : "");
    }
    else if (0 == wcscmp(argv[1], L"streams"))
    {
        if (argc < 4)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const unsigned long streamCount = static_cast<unsigned long>(_wtoi(argv[3]));
        const double runSeconds = (argc > 4) ? _wtof(argv[4]) : 30.0;

        if (0 == streamCount)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        //
        // _wtof yields 0.0 for unparsable text, and a zero deadline ends
        // the loop before the first read: a valid-looking report carrying
        // ThroughputMBs=0 would be written and exit 0.
        //
        if (!(runSeconds > 0.0))
        {
            fprintf(stderr, "  [FAIL] duration must be a positive number of seconds (got '%ws')\n", argc > 4 ? argv[4] : L"");
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const bool unbuffered = (argc > 5) && (0 == wcscmp(argv[5], L"unbuffered"));
        const unsigned long paceKbPerSecond = ParsePaceKbPerSecond(argc, argv, 4);

        ok = RunStreams(argv[2], streamCount, runSeconds, unbuffered, paceKbPerSecond, &bytes, &seconds);
        sprintf_s(label, "concurrent streams (%lu x %.0f s%s)", streamCount, runSeconds,
            unbuffered ? ", unbuffered" : "");
    }
    else if (0 == wcscmp(argv[1], L"meta"))
    {
        if (argc < 4)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const int parsedPasses = _wtoi(argv[3]);

        if (parsedPasses <= 0)
        {
            fprintf(stderr, "  [FAIL] passes must be a positive integer (got '%ws')\n", argv[3]);
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const unsigned long passes = static_cast<unsigned long>(parsedPasses);

        ok = RunMetadata(argv[2], passes, &opens, &seconds);
        sprintf_s(label, "metadata storm (%lu passes)", passes);
    }

    if (!ok)
    {
        CloseHandles(&handles);
        return 1;
    }

    if (opens > 0)
    {
        printf("\n=== %s ===\n", label);
        printf("    opens                 %12llu\n", opens);
        printf("    elapsed               %12.3f s\n", seconds);
        printf("    opens/sec             %12.1f\n", (seconds > 0.0) ? (opens / seconds) : 0.0);
    }
    else
    {
        PrintThroughput(label, bytes, seconds);
    }

    BLORGFS_STATISTICS_RESPONSE stats;

    bool reported = false;

    if (QueryDriverStatistics(handles.Control, &stats))
    {
        PrintDriverStatistics(stats);

        if (reportPath)
        {
            reported = WriteReport(reportPath, label, bytes, opens, seconds, stats);
        }
        else
        {
            reported = true;
        }
    }

    CloseHandles(&handles);

    //
    // Exit nonzero when the counters could not be read or the report could
    // not be written, even though the workload itself ran. The gate checks
    // this exit code AND the presence of a fresh report; a silent success
    // here is precisely what lets the previous session's report pose as
    // this run's.
    //
    return reported ? 0 : 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    std::vector<wchar_t*> args(argv, argv + argc);

    const wchar_t* reportPath = ExtractReportPath(&args);

    argv = args.data();
    argc = static_cast<int>(args.size());

    const wchar_t* drive = L"B";

    if (0 == wcscmp(argv[1], L"stats") || 0 == wcscmp(argv[1], L"fsstats") || 0 == wcscmp(argv[1], L"reset"))
    {
        if (argc > 2)
        {
            drive = argv[2];
        }

        const bool needWrite = (0 == wcscmp(argv[1], L"reset"));
        const bool needVolume = (0 == wcscmp(argv[1], L"fsstats"));
        const bool needControl = !needVolume;

        HandlePair handles = {};

        if (!OpenHandles(drive, needControl, needWrite, needVolume, &handles))
        {
            return 1;
        }

        int result = 0;

        if (0 == wcscmp(argv[1], L"reset"))
        {
            result = ResetDriverStatistics(handles.Control) ? 0 : 1;

            if (0 == result)
            {
                printf("  [OK] counters reset; measurement window restarted.\n");
            }
        }
        else if (0 == wcscmp(argv[1], L"fsstats"))
        {
            result = PrintFilesystemStatistics(handles.Volume) ? 0 : 1;
        }
        else
        {
            BLORGFS_STATISTICS_RESPONSE stats;

            if (QueryDriverStatistics(handles.Control, &stats))
            {
                PrintDriverStatistics(stats);
            }
            else
            {
                result = 1;
            }
        }

        CloseHandles(&handles);
        return result;
    }

    if (0 == wcscmp(argv[1], L"seq") || 0 == wcscmp(argv[1], L"rand") ||
        0 == wcscmp(argv[1], L"meta") || 0 == wcscmp(argv[1], L"streams") ||
        0 == wcscmp(argv[1], L"demux") || 0 == wcscmp(argv[1], L"play") ||
        0 == wcscmp(argv[1], L"mixed") || 0 == wcscmp(argv[1], L"compete"))
    {
        if (argc < 3)
        {
            PrintUsage();
            return 1;
        }

        return RunWorkloadCommand(argc, argv, drive, reportPath);
    }

    PrintUsage();
    return 1;
}
