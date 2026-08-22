//
// BlorgFS performance harness.
//
// Runs a defined workload against the mounted volume, then reports what
// the driver's own counters say happened underneath it (Statistics.h).
// The point is to replace "playback stutters" and "that felt faster" with
// numbers that identify *which* stage is the bottleneck -- a workload
// timer alone cannot tell a slow backend from a prefetch pipeline that
// never armed, and those two want opposite fixes.
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

#include "..\..\src\Statistics.h"

//
// Workload read size. Deliberately not PREFETCH_CHUNK: the harness issues
// what an application issues and lets Cc cluster it into whatever paging
// reads the driver actually sees, because pinning the request size to the
// driver's internal chunk would measure a path no real reader takes.
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
// without post-processing. A bimodal distribution (a fast mode from ring
// hits and a slow mode from direct fetches) is the signature of a
// prefetcher that is working but not keeping up, and it is invisible in
// any single summary number.
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

    printf("\n  reads\n");
    printf("    cached                %12llu\n", t.ReadsCached);
    printf("    paging (inline)       %12llu\n", t.ReadsPagingInline);
    printf("    posted to FSP         %12llu\n", t.ReadsPosted);
    printf("    sequential            %12llu  (%.1f%% of paging)\n",
        t.ReadsSequential, SafeRatio(t.ReadsSequential, t.ReadsPagingInline));
    printf("    end-of-file           %12llu\n", t.ReadsEndOfFile);
    printf("    user bytes            %12llu\n", t.UserFileReadBytes);
    printf("    non-cached bytes      %12llu\n", t.NonCachedReadBytes);

    const unsigned long long served = t.PrefetchHits + t.PrefetchParks + t.PrefetchMisses;

    printf("\n  prefetch ring\n");
    printf("    armed / refused       %12llu / %llu\n", t.PrefetchRingsArmed, t.PrefetchRingsRefused);
    printf("    live now              %12lld\n", stats.Gauges.PrefetchRingsLive);
    printf("    hit                   %12llu  (%.1f%%)\n", t.PrefetchHits, SafeRatio(t.PrefetchHits, served));
    printf("    park                  %12llu  (%.1f%%)\n", t.PrefetchParks, SafeRatio(t.PrefetchParks, served));
    printf("    miss                  %12llu  (%.1f%%)\n", t.PrefetchMisses, SafeRatio(t.PrefetchMisses, served));
    printf("      of which near       %12llu  (%.1f%% of miss, slot covered the range)\n",
        t.PrefetchNearMisses, SafeRatio(t.PrefetchNearMisses, t.PrefetchMisses));
    printf("    re-aims               %12llu\n", t.PrefetchReaims);
    printf("      suppressed (in-window) %9llu  (reader had outrun the pipeline; not re-aimed)\n",
        t.PrefetchReaimsSuppressed);
    printf("    depth growths         %12llu\n", t.PrefetchDepthGrowths);
    printf("    fetches issued/failed %12llu / %llu\n", t.PrefetchFetchesIssued, t.PrefetchFetchesFailed);
    printf("    stale discards        %12llu\n", t.PrefetchStaleDiscards);
    printf("    bytes served          %12llu\n", t.PrefetchBytesServed);

    printf("\n  chunk fetches\n");
    printf("    direct issued         %12llu\n", t.FetchesIssued);
    printf("    completed / failed    %12llu / %llu\n", t.FetchesCompleted, t.FetchesFailed);
    printf("    bytes                 %12llu\n", t.FetchBytes);
    printf("    in flight now / peak  %12lld / %lld\n",
        stats.Gauges.FetchesActive, stats.Gauges.FetchesActivePeak);

    if (t.FetchesCompleted > 0)
    {
        printf("    mean latency          %12llu us\n", t.FetchLatencySumUs / t.FetchesCompleted);
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
// Sequential read of one file. Unbuffered mode is what exercises the
// prefetcher and the HTTP path honestly -- a buffered re-read of a file
// already resident in Cc measures memcpy, not this filesystem.
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
// Random reads at a fixed block size. This is the pattern the prefetcher
// deliberately does not arm for, so the interesting output is the
// contrast: near-zero prefetch hits and a direct-fetch latency
// distribution that is the raw backend round trip.
//
static bool RunRandom(
    const wchar_t* path,
    DWORD blockSize,
    unsigned long count,
    unsigned long long* bytesOut,
    double* secondsOut)
{
    HANDLE file = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS,
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
    const unsigned long long served = t.PrefetchHits + t.PrefetchParks + t.PrefetchMisses;
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
    fprintf(f, "UserFileReads=%llu\n", t.UserFileReads);
    fprintf(f, "UserFileReadBytes=%llu\n", t.UserFileReadBytes);
    fprintf(f, "NonCachedReads=%llu\n", t.NonCachedReads);
    fprintf(f, "NonCachedReadBytes=%llu\n", t.NonCachedReadBytes);

    fprintf(f, "PrefetchRingsArmed=%llu\n", t.PrefetchRingsArmed);
    fprintf(f, "PrefetchRingsRefused=%llu\n", t.PrefetchRingsRefused);
    fprintf(f, "PrefetchHits=%llu\n", t.PrefetchHits);
    fprintf(f, "PrefetchParks=%llu\n", t.PrefetchParks);
    fprintf(f, "PrefetchMisses=%llu\n", t.PrefetchMisses);
    fprintf(f, "PrefetchServed=%llu\n", served);
    fprintf(f, "PrefetchHitRate=%.4f\n", SafeRatio(t.PrefetchHits, served));
    fprintf(f, "PrefetchParkRate=%.4f\n", SafeRatio(t.PrefetchParks, served));
    fprintf(f, "PrefetchMissRate=%.4f\n", SafeRatio(t.PrefetchMisses, served));
    fprintf(f, "PrefetchNearMisses=%llu\n", t.PrefetchNearMisses);
    fprintf(f, "PrefetchReaims=%llu\n", t.PrefetchReaims);
    fprintf(f, "PrefetchReaimsSuppressed=%llu\n", t.PrefetchReaimsSuppressed);
    fprintf(f, "PrefetchDepthGrowths=%llu\n", t.PrefetchDepthGrowths);
    fprintf(f, "PrefetchFetchesIssued=%llu\n", t.PrefetchFetchesIssued);
    fprintf(f, "PrefetchFetchesFailed=%llu\n", t.PrefetchFetchesFailed);
    fprintf(f, "PrefetchStaleDiscards=%llu\n", t.PrefetchStaleDiscards);
    fprintf(f, "PrefetchBytesServed=%llu\n", t.PrefetchBytesServed);

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
    fprintf(f, "FetchesActivePeak=%lld\n", stats.Gauges.FetchesActivePeak);

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
    printf("  rand <file> <blockKB> <count>  random reads at a fixed block size\n");
    printf("  meta <dir> <passes>            enumerate + open every entry, repeatedly\n\n");
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

        const DWORD blockKb = static_cast<DWORD>(_wtoi(argv[3]));
        const unsigned long count = static_cast<unsigned long>(_wtoi(argv[4]));

        ok = RunRandom(argv[2], blockKb * 1024, count, &bytes, &seconds);
        sprintf_s(label, "random read (%lu KB x %lu)", blockKb, count);
    }
    else if (0 == wcscmp(argv[1], L"meta"))
    {
        if (argc < 4)
        {
            PrintUsage();
            CloseHandles(&handles);
            return 1;
        }

        const unsigned long passes = static_cast<unsigned long>(_wtoi(argv[3]));

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

    if (QueryDriverStatistics(handles.Control, &stats))
    {
        PrintDriverStatistics(stats);

        if (reportPath)
        {
            WriteReport(reportPath, label, bytes, opens, seconds, stats);
        }
    }

    CloseHandles(&handles);
    return 0;
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

    if (0 == wcscmp(argv[1], L"seq") || 0 == wcscmp(argv[1], L"rand") || 0 == wcscmp(argv[1], L"meta"))
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
