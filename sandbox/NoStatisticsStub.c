//
// The statistics counter storage, for targets that do not compile the real
// Statistics.c.
//
// Statistics.h declares these and every driver file that increments a
// counter calls them, so they must resolve everywhere; but a target that
// links the real Statistics.c must not also link this, or the counters it
// is testing are not the counters being written. Same split as
// NoPrefetchStub.c and NoClientStub.c.
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

#include "..\Driver.h"
LONG64 BlorgStatisticsNow(VOID)
{
    return KmNow();
}

//
BLORGFS_STATISTICS ShimStatistics;
BLORGFS_STATISTICS_GLOBAL BlorgStatisticsGauges;

PBLORGFS_STATISTICS BlorgStatisticsForCurrentProcessor(VOID)
{
    return &ShimStatistics;
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

VOID BlorgStatisticsGaugeIncrement(LONG64 volatile* Gauge, LONG64 volatile* Peak)
{
    LONG64 current = InterlockedIncrement64(Gauge);

    if (!Peak)
    {
        return;
    }

    for (;;)
    {
        LONG64 observed = *Peak;

        if (current <= observed)
        {
            return;
        }

        if (observed == InterlockedCompareExchange64(Peak, current, observed))
        {
            return;
        }
    }
}

VOID BlorgStatisticsGaugeDecrement(LONG64 volatile* Gauge)
{
    InterlockedDecrement64(Gauge);
}
