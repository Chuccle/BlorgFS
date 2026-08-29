//
// The statistics counter storage, for targets that do not compile the real
// Statistics.c.
//
// Statistics.h declares these and every driver file that increments a
// counter calls them, so they must resolve everywhere; but a target that
// links the real Statistics.c must not also link this, or the counters it
// is testing are not the counters being written. Same split as
// NoClientStub.c and NoTlsHandshakeStub.c.
//
// One block stands in for the driver's per-processor table. These targets
// are not measuring contention on the counters -- they are measuring the
// code that reads and writes them -- so a single block is the honest
// simplification.
//

//
// This is scaffolding, not driver code: its atomics must not become
// scheduling points (see NtShim.h).
//
#define BLORGFS_SHIM_INTERNAL

#include "..\..\src\Driver.h"
LONG64 BlorgStatisticsNow(VOID)
{
    return KmNow();
}

BLORGFS_STATISTICS ShimStatistics;

PBLORGFS_STATISTICS BlorgStatisticsForCurrentProcessor(VOID)
{
    return &ShimStatistics;
}

//
// The outlier ring is a reporting surface, not behaviour: nothing in
// Client.c reads it back, so the scenarios and the fuzzer only need the
// symbol to exist. Counting the calls anyway makes it cheap for a test to
// assert that a slow fetch was filed at all, without pulling the real
// ring's wrap and tear-detection into the sandbox.
//
ULONG64 ShimSlowFetchesRecorded;

VOID BlorgStatisticsRecordSlowFetch(
    LONG64 TotalQpc,
    LONG64 AcquireQpc,
    LONG64 SendQpc,
    LONG64 WaitQpc,
    LONG64 TtfbQpc,
    LONG64 BodyQpc,
    ULONG64 Bytes,
    BOOLEAN ConnectionReused)
{
    UNREFERENCED_PARAMETER(AcquireQpc);
    UNREFERENCED_PARAMETER(SendQpc);
    UNREFERENCED_PARAMETER(WaitQpc);
    UNREFERENCED_PARAMETER(TtfbQpc);
    UNREFERENCED_PARAMETER(BodyQpc);
    UNREFERENCED_PARAMETER(Bytes);
    UNREFERENCED_PARAMETER(ConnectionReused);

    if (TotalQpc >= 0)
    {
        ShimSlowFetchesRecorded++;
    }
}

VOID BlorgStatisticsRecordLatency(ULONG64* Sum, ULONG64* Max, ULONG64* Buckets, LONG64 ElapsedQpc)
{
    if (ElapsedQpc < 0)
    {
        return;
    }

    ULONG64 microseconds = (ULONG64)ElapsedQpc / 10ULL;

    *Sum += microseconds;

    if (microseconds > *Max)
    {
        *Max = microseconds;
    }

    if (Buckets)
    {
        ULONG bucket = 0;

        while (microseconds && bucket < (BLORGFS_STATISTICS_LATENCY_BUCKETS - 1))
        {
            microseconds >>= 1;
            bucket++;
        }

        Buckets[bucket]++;
    }
}

