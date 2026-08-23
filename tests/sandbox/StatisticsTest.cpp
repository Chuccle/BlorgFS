//
// Coverage for the real Statistics.c: the per-processor counter block, the
// FSCTL_FILESYSTEM_GET_STATISTICS(_EX) / FAT_STATISTICS translation, and
// the vendor IOCTL_BLORGFS_QUERY_STATISTICS response. DispatchSandbox is
// the only sandbox target that links the real file -- every other target
// links NoStatisticsStub.c instead, specifically so it does not collide
// with this one -- so this is the only place any of it can be tested.
//
// The kernel model fixes KeGetCurrentProcessorIndex() at 0 and
// KeQueryMaximumProcessorCountEx() at 1 (DispatchModel.c), so every test
// here runs against a single-processor table: BlorgStatisticsForCurrentProcessor()
// always returns the same block, which is what makes direct field
// inspection meaningful without summing across processors by hand.
//

#include <gtest/gtest.h>
#include <vector>

extern "C" {
#include "..\..\src\Driver.h"
}

namespace
{

//
// BlorgStatisticsForCurrentProcessor() with no NULL check -- Statistics.h's
// "every accessor tolerates that" promise covers the counter macros, not
// call sites written that way. Bracketing the whole process with one
// Initialize/Cleanup pair, the same way DriverEntry/DriverUnload bracket
// the real driver's lifetime, is what makes the table live for every test
// in this binary rather than only the ones that happen to run after this
// file's.
//
class StatisticsEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsInitialize());

        //
        // This table lives for the whole binary, not one test. Without
        // this, the first test whose SetUp() calls ShimReset() would wipe
        // it from the model's object ledger, and the first later free of
        // it (StatisticsLifecycleTest.CleanupThenInitializeRoundTrips)
        // would trip "destroyed more times than created" for an
        // allocation the model no longer remembers creating.
        //
        KmAbsorbBaseline();
    }

    void TearDown() override
    {
        BlorgStatisticsCleanup();
    }
};

::testing::Environment* const g_statisticsEnvironment =
    ::testing::AddGlobalTestEnvironment(new StatisticsEnvironment());

class StatisticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        //
        // A clean window for every test, through the real Reset() path
        // rather than a test-only shortcut, without touching the table
        // allocation or ProcessorCount the environment already set up.
        // Reset() deliberately leaves the gauges alone (see Statistics.c),
        // so they are zeroed here instead -- otherwise a gauge left
        // nonzero by one test would leak into the next.
        //
        BlorgStatisticsReset();
        InterlockedExchange64(&BlorgStatisticsGauges.FetchesActive, 0);
        InterlockedExchange64(&BlorgStatisticsGauges.FetchesActivePeak, 0);

        Block = BlorgStatisticsForCurrentProcessor();
        ASSERT_NE(nullptr, Block);
    }

    PBLORGFS_STATISTICS Block = nullptr;
};

///////////////////////////////////////////////////////////////////////////
// Initialize / Cleanup lifecycle
///////////////////////////////////////////////////////////////////////////

TEST(StatisticsLifecycleTest, CleanupThenInitializeRoundTrips)
{
    BlorgStatisticsCleanup();
    EXPECT_EQ(nullptr, BlorgStatisticsForCurrentProcessor())
        << "Cleanup must drop the table so every accessor degrades to a no-op";

    ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsInitialize());
    PBLORGFS_STATISTICS block = BlorgStatisticsForCurrentProcessor();
    ASSERT_NE(nullptr, block);

    // DriverUnload calls Cleanup once, but nothing in the contract limits
    // callers to exactly one call -- a second call must not double-free.
    BlorgStatisticsCleanup();
    BlorgStatisticsCleanup();
    EXPECT_EQ(nullptr, BlorgStatisticsForCurrentProcessor());

    // Leave the table live: every other suite in this binary calls
    // BlorgStatisticsForCurrentProcessor() assuming StatisticsEnvironment's
    // table is still there.
    ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsInitialize());
}

//
// Every processor's counter block must start on its own cache line. The
// counters are written per-CPU without interlocks precisely so an update is
// an uncontended add; if two processors share a line, that add turns into
// cache-line ping-pong on the hottest write in the driver and the whole
// per-CPU split buys nothing.
//
// A 64-byte stride alone does not give this. ExAllocatePool guarantees only
// MEMORY_ALLOCATION_ALIGNMENT (16 on x64), and striding by 64 from a
// 16-aligned base puts every entry boundary mid-line -- which is what this
// code did until the base was explicitly aligned. Asserting on the stride
// would have passed throughout; only the absolute address catches it.
//
TEST(StatisticsLifecycleTest, EveryProcessorEntryStartsOnItsOwnCacheLine)
{
    ASSERT_NE(nullptr, BlorgStatisticsTable);
    ASSERT_GT(BlorgStatisticsProcessorCount, 0u);

    EXPECT_EQ(0u, BlorgStatisticsEntryStride % BLORGFS_STATISTICS_LINE)
        << "stride must be a whole number of cache lines";

    for (ULONG i = 0; i < BlorgStatisticsProcessorCount; ++i)
    {
        const ULONG_PTR entry =
            reinterpret_cast<ULONG_PTR>(BlorgStatisticsTable) +
            (static_cast<ULONG_PTR>(i) * BlorgStatisticsEntryStride);

        EXPECT_EQ(0u, entry % BLORGFS_STATISTICS_LINE)
            << "processor " << i << "'s block shares a cache line with its neighbour";
    }
}

TEST(StatisticsLifecycleTest, InitializeFailsCleanlyWhenPoolAllocationFails)
{
    BlorgStatisticsCleanup();

    ShimPoolFailAt(0); // fail the very next pool allocation: Initialize's table
    NTSTATUS status = BlorgStatisticsInitialize();
    ShimPoolFailAt(-1);

    EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, status);
    EXPECT_EQ(nullptr, BlorgStatisticsForCurrentProcessor())
        << "a failed allocation must leave the table NULL, not a partial pointer";

    // Leave the table live for sibling suites, same as above.
    ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsInitialize());
}

TEST_F(StatisticsTest, ForCurrentProcessorReturnsNullWhenIndexIsOutOfRange)
{
    //
    // The model fixes KeGetCurrentProcessorIndex() at 0, so a claimed
    // ProcessorCount of 0 -- restored before the table is touched again --
    // is what puts that fixed index out of range without needing a second
    // processor to exist.
    //
    ULONG savedProcessorCount = BlorgStatisticsProcessorCount;
    BlorgStatisticsProcessorCount = 0;

    EXPECT_EQ(nullptr, BlorgStatisticsForCurrentProcessor());

    BlorgStatisticsProcessorCount = savedProcessorCount;
}

///////////////////////////////////////////////////////////////////////////
// Counter increments -> Query
///////////////////////////////////////////////////////////////////////////

TEST_F(StatisticsTest, QueryReflectsDirectCounterIncrements)
{
    BLORGFS_STAT_ADD(FetchBytes, 4096);
    BLORGFS_STAT_INC(CreateHits);

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);

    EXPECT_EQ(C_CAST(ULONG, BLORGFS_STATISTICS_VERSION), response.Version);
    EXPECT_EQ(sizeof(BLORGFS_STATISTICS_RESPONSE), response.SizeOfStruct);
    EXPECT_EQ(1u, response.ProcessorCount);
    EXPECT_GT(response.QpcFrequency, 0);
    EXPECT_GE(response.NowQpc, response.EpochQpc);

    EXPECT_EQ(4096u, response.Totals.FetchBytes);
    EXPECT_EQ(1u, response.Totals.CreateHits);
}

TEST_F(StatisticsTest, QueryReportsGaugesDirectlyRatherThanSummingThem)
{
    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.FetchesActive, &BlorgStatisticsGauges.FetchesActivePeak);
    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.FetchesActive, &BlorgStatisticsGauges.FetchesActivePeak);

    BLORGFS_STATISTICS_RESPONSE response;
    BlorgStatisticsQuery(&response);

    EXPECT_EQ(2, response.Gauges.FetchesActive);
    EXPECT_EQ(2, response.Gauges.FetchesActivePeak);
}

///////////////////////////////////////////////////////////////////////////
// Reset
///////////////////////////////////////////////////////////////////////////

TEST_F(StatisticsTest, ResetZeroesCountersButPreservesLiveGauges)
{
    BLORGFS_STAT_ADD(FetchBytes, 999);

    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.FetchesActive, &BlorgStatisticsGauges.FetchesActivePeak);
    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.FetchesActive, &BlorgStatisticsGauges.FetchesActivePeak);
    BlorgStatisticsGaugeIncrement(&BlorgStatisticsGauges.FetchesActive, &BlorgStatisticsGauges.FetchesActivePeak);
    BlorgStatisticsGaugeDecrement(&BlorgStatisticsGauges.FetchesActive); // depth 3 -> 2, peak stays 3

    BLORGFS_STATISTICS_RESPONSE before;
    BlorgStatisticsQuery(&before);
    ASSERT_EQ(999u, before.Totals.FetchBytes);
    ASSERT_EQ(2, before.Gauges.FetchesActive);
    ASSERT_EQ(3, before.Gauges.FetchesActivePeak);

    BlorgStatisticsReset();

    BLORGFS_STATISTICS_RESPONSE after;
    BlorgStatisticsQuery(&after);

    EXPECT_EQ(0u, after.Totals.FetchBytes);
    EXPECT_EQ(2, after.Gauges.FetchesActive)
        << "FetchesActive is a live in-flight gauge -- Reset must not clear it";
    EXPECT_EQ(2, after.Gauges.FetchesActivePeak)
        << "Reset restarts the peak from the CURRENT depth, not zero";
    EXPECT_GT(after.EpochQpc, before.EpochQpc) << "Reset must restamp the epoch to start a fresh window";
}

///////////////////////////////////////////////////////////////////////////
// StatisticsFillFat, via BlorgStatisticsFillFsctlBuffer
///////////////////////////////////////////////////////////////////////////

ULONG NonExtendedStride()
{
    ULONG entrySize = C_CAST(ULONG, sizeof(FILESYSTEM_STATISTICS)) + C_CAST(ULONG, sizeof(FAT_STATISTICS));
    return (entrySize + 63) & ~C_CAST(ULONG, 63);
}

ULONG ExtendedStride()
{
    ULONG entrySize = C_CAST(ULONG, sizeof(FILESYSTEM_STATISTICS_EX)) + C_CAST(ULONG, sizeof(FAT_STATISTICS));
    return (entrySize + 63) & ~C_CAST(ULONG, 63);
}

TEST_F(StatisticsTest, FillFsctlBufferNonExtendedMapsFieldsAndClampsToUlong)
{
    Block->UserFileReads = 10;
    Block->UserFileReadBytes = 20;
    Block->UserDiskReads = 30;
    Block->UserFileWrites = 40;
    Block->UserFileWriteBytes = 50;
    Block->UserDiskWrites = 60;
    Block->MetaDataReads = 70;
    Block->MetaDataReadBytes = 80;
    Block->MetaDataDiskReads = 90;
    Block->MetaDataWrites = 100;
    Block->MetaDataWriteBytes = 110;
    Block->MetaDataDiskWrites = 120;

    Block->CreateHits = 1;
    Block->SuccessfulCreates = 2;
    Block->FailedCreates = 3;
    Block->NonCachedReads = 4;
    Block->NonCachedReadBytes = 5;
    Block->NonCachedWrites = 6;
    Block->NonCachedWriteBytes = C_CAST(ULONG64, MAXULONG) + 1000; // must saturate, not wrap
    Block->NonCachedDiskReads = 8;
    Block->NonCachedDiskWrites = 9;

    ULONG stride = NonExtendedStride();
    std::vector<unsigned char> buffer(stride, 0xCD);
    ULONG bytesReturned = 0;

    ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsFillFsctlBuffer(buffer.data(), stride, FALSE, &bytesReturned));
    EXPECT_EQ(stride, bytesReturned);

    auto* header = reinterpret_cast<PFILESYSTEM_STATISTICS>(buffer.data());
    EXPECT_EQ(FILESYSTEM_STATISTICS_TYPE_FAT, header->FileSystemType);
    EXPECT_EQ(1, header->Version);
    EXPECT_EQ(stride, header->SizeOfCompleteStructure);
    EXPECT_EQ(10u, header->UserFileReads);
    EXPECT_EQ(20u, header->UserFileReadBytes);
    EXPECT_EQ(30u, header->UserDiskReads);
    EXPECT_EQ(40u, header->UserFileWrites);
    EXPECT_EQ(50u, header->UserFileWriteBytes);
    EXPECT_EQ(60u, header->UserDiskWrites);
    EXPECT_EQ(70u, header->MetaDataReads);
    EXPECT_EQ(80u, header->MetaDataReadBytes);
    EXPECT_EQ(90u, header->MetaDataDiskReads);
    EXPECT_EQ(100u, header->MetaDataWrites);
    EXPECT_EQ(110u, header->MetaDataWriteBytes);
    EXPECT_EQ(120u, header->MetaDataDiskWrites);

    auto* fat = reinterpret_cast<PFAT_STATISTICS>(buffer.data() + sizeof(FILESYSTEM_STATISTICS));
    EXPECT_EQ(1u, fat->CreateHits);
    EXPECT_EQ(2u, fat->SuccessfulCreates);
    EXPECT_EQ(3u, fat->FailedCreates);
    EXPECT_EQ(4u, fat->NonCachedReads);
    EXPECT_EQ(5u, fat->NonCachedReadBytes);
    EXPECT_EQ(6u, fat->NonCachedWrites);
    EXPECT_EQ(MAXULONG, fat->NonCachedWriteBytes) << "a counter above MAXULONG must saturate, not wrap";
    EXPECT_EQ(8u, fat->NonCachedDiskReads);
    EXPECT_EQ(9u, fat->NonCachedDiskWrites);
}

TEST_F(StatisticsTest, FillFsctlBufferExtendedPassesFullWidthCountersUnclamped)
{
    Block->UserFileReadBytes = C_CAST(ULONG64, MAXULONG) + 12345; // would clamp in the non-EX shape

    ULONG stride = ExtendedStride();
    std::vector<unsigned char> buffer(stride, 0);
    ULONG bytesReturned = 0;

    ASSERT_EQ(STATUS_SUCCESS, BlorgStatisticsFillFsctlBuffer(buffer.data(), stride, TRUE, &bytesReturned));
    EXPECT_EQ(stride, bytesReturned);

    auto* header = reinterpret_cast<PFILESYSTEM_STATISTICS_EX>(buffer.data());
    EXPECT_EQ(FILESYSTEM_STATISTICS_TYPE_FAT, header->FileSystemType);
    EXPECT_EQ(1, header->Version);
    EXPECT_EQ(stride, header->SizeOfCompleteStructure);
    EXPECT_EQ(C_CAST(ULONG64, MAXULONG) + 12345, header->UserFileReadBytes)
        << "the _EX shape is ULONGLONG-wide and must not clamp";
}

TEST_F(StatisticsTest, FillFsctlBufferTooSmallForEvenOneEntry)
{
    ULONG stride = NonExtendedStride();
    std::vector<unsigned char> buffer(stride - 1, 0);
    ULONG bytesReturned = 123;

    EXPECT_EQ(STATUS_BUFFER_TOO_SMALL, BlorgStatisticsFillFsctlBuffer(buffer.data(), stride - 1, FALSE, &bytesReturned));
    EXPECT_EQ(0u, bytesReturned);
}

TEST_F(StatisticsTest, FillFsctlBufferOverflowWhenNotEveryProcessorEntryFits)
{
    ULONG stride = NonExtendedStride();
    std::vector<unsigned char> buffer(stride, 0);
    ULONG bytesReturned = 0;

    //
    // The model fixes the real processor count at 1, so the table this
    // fixture's table was allocated for holds exactly one entry. Claiming
    // 2 processors here -- restored before the buffer is touched again --
    // exercises the overflow branch without indexing past that allocation:
    // a one-entry buffer only ever admits entriesToWrite == 1, so
    // StatisticsEntry(1) is never read.
    //
    ULONG savedProcessorCount = BlorgStatisticsProcessorCount;
    BlorgStatisticsProcessorCount = 2;

    NTSTATUS status = BlorgStatisticsFillFsctlBuffer(buffer.data(), stride, FALSE, &bytesReturned);

    BlorgStatisticsProcessorCount = savedProcessorCount;

    EXPECT_EQ(STATUS_BUFFER_OVERFLOW, status);
    EXPECT_EQ(stride, bytesReturned) << "the one entry that did fit must still be written";
}

TEST_F(StatisticsTest, FillFsctlBufferDeviceNotReadyWithNoProcessors)
{
    ULONG stride = NonExtendedStride();
    std::vector<unsigned char> buffer(stride, 0);
    ULONG bytesReturned = 0;

    ULONG savedProcessorCount = BlorgStatisticsProcessorCount;
    BlorgStatisticsProcessorCount = 0;

    NTSTATUS status = BlorgStatisticsFillFsctlBuffer(buffer.data(), stride, FALSE, &bytesReturned);

    BlorgStatisticsProcessorCount = savedProcessorCount;

    EXPECT_EQ(STATUS_DEVICE_NOT_READY, status);
    EXPECT_EQ(0u, bytesReturned);
}

///////////////////////////////////////////////////////////////////////////
// Latency bucket boundaries
///////////////////////////////////////////////////////////////////////////

TEST_F(StatisticsTest, RecordLatencyBucketsMicrosecondsAtPowerOfTwoBoundaries)
{
    ULONG64 sum = 0, max = 0;
    ULONG64 buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS] = {};

    //
    // The shim fixes QPC's frequency at 10,000,000 (NtShim.c), so
    // microseconds = ElapsedQpc / 10 by integer division. Ticks below are
    // chosen to land exactly on and just past each bucket boundary: bucket
    // i covers microseconds in [2^(i-1), 2^i).
    //
    struct { LONG64 ticks; ULONG expectedBucket; } cases[] = {
        { 0, 0 },   // 0 us  -> bucket 0, [0, 1)
        { 10, 1 },  // 1 us  -> bucket 1, [1, 2)
        { 19, 1 },  // 1 us  (1.9 truncates down)
        { 20, 2 },  // 2 us  -> bucket 2, [2, 4)
        { 30, 2 },  // 3 us  -> bucket 2
        { 40, 3 },  // 4 us  -> bucket 3, [4, 8)
    };

    for (const auto& c : cases)
    {
        BlorgStatisticsRecordLatency(&sum, &max, buckets, c.ticks);
        EXPECT_EQ(1u, buckets[c.expectedBucket])
            << "ticks=" << c.ticks << " did not land in bucket " << c.expectedBucket;
        buckets[c.expectedBucket] = 0; // isolate the next case
    }
}

//
// The top bucket's floor is derived from the bucket count rather than
// written as a literal. A previous version hardcoded 2^14 as "the top
// bucket's floor", which silently stopped being true the moment the
// histogram was resized.
//
static constexpr ULONG64 kTopBucketFloorMicros =
    1ULL << (BLORGFS_STATISTICS_LATENCY_BUCKETS - 2);

// The shim fixes QPC at 10,000,000 Hz (NtShim.c), so ticks = us * 10.
static constexpr LONG64 MicrosToTicks(ULONG64 micros)
{
    return static_cast<LONG64>(micros * 10ULL);
}

TEST_F(StatisticsTest, RecordLatencySaturatesAtTheTopBucketInsteadOfWrapping)
{
    ULONG64 sum = 0, max = 0;
    ULONG64 buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS] = {};

    const ULONG64 firstMicros = kTopBucketFloorMicros;
    const ULONG64 secondMicros = kTopBucketFloorMicros * 64ULL;

    BlorgStatisticsRecordLatency(&sum, &max, buckets, MicrosToTicks(firstMicros));
    BlorgStatisticsRecordLatency(&sum, &max, buckets, MicrosToTicks(secondMicros));

    for (ULONG i = 0; i < BLORGFS_STATISTICS_LATENCY_BUCKETS - 1; ++i)
    {
        EXPECT_EQ(0u, buckets[i]) << "bucket " << i << " must be untouched by a saturating sample";
    }

    EXPECT_EQ(2u, buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS - 1]);
    EXPECT_EQ(firstMicros + secondMicros, sum);
    EXPECT_EQ(secondMicros, max);
}

//
// The histogram has to resolve the tail it exists to show. Sized at 16
// buckets it topped out at 2^14 us -- 16 ms, below the median real fetch --
// so 99.2% of samples piled into the last bucket and p50, p90 and p99 all
// reported the same saturated bound.
//
// These two samples are the ones that matter in practice: the worst fetch
// actually observed against the live backend (2.5 s), and a request dying
// on SOCKET_RECEIVE_TIMEOUT_MS (30 s), which is the upper end the histogram
// is explicitly sized for. Both must land strictly below the saturating
// bucket, or the tail is unmeasurable again.
//
TEST_F(StatisticsTest, RecordLatencyResolvesSlowFetchesInsteadOfSaturating)
{
    ULONG64 sum = 0, max = 0;
    ULONG64 buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS] = {};

    const ULONG64 observedWorstFetchMicros = 2528253ULL;
    const ULONG64 receiveWatchdogMicros = 30000000ULL;

    BlorgStatisticsRecordLatency(&sum, &max, buckets, MicrosToTicks(observedWorstFetchMicros));
    BlorgStatisticsRecordLatency(&sum, &max, buckets, MicrosToTicks(receiveWatchdogMicros));

    EXPECT_EQ(0u, buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS - 1])
        << "a 2.5 s fetch and a 30 s watchdog timeout must both be resolved, not saturated";

    EXPECT_LT(observedWorstFetchMicros, kTopBucketFloorMicros);
    EXPECT_LT(receiveWatchdogMicros, kTopBucketFloorMicros);
}

TEST_F(StatisticsTest, RecordLatencyIgnoresANegativeElapsedSample)
{
    ULONG64 sum = 111;
    ULONG64 max = 222;
    ULONG64 buckets[BLORGFS_STATISTICS_LATENCY_BUCKETS] = {};
    buckets[3] = 5;

    BlorgStatisticsRecordLatency(&sum, &max, buckets, -1);

    EXPECT_EQ(111u, sum) << "a negative elapsed sample (clock went backward) must be a no-op";
    EXPECT_EQ(222u, max);
    EXPECT_EQ(5u, buckets[3]);
}

///////////////////////////////////////////////////////////////////////////
// Gauge peak tracking
///////////////////////////////////////////////////////////////////////////

TEST(StatisticsGaugeTest, IncrementTracksPeakOnlyWhenCurrentExceedsIt)
{
    LONG64 gauge = 0;
    LONG64 peak = 0;

    BlorgStatisticsGaugeIncrement(&gauge, &peak);
    EXPECT_EQ(1, gauge);
    EXPECT_EQ(1, peak);

    BlorgStatisticsGaugeIncrement(&gauge, &peak);
    EXPECT_EQ(2, gauge);
    EXPECT_EQ(2, peak);

    BlorgStatisticsGaugeDecrement(&gauge);
    EXPECT_EQ(1, gauge);
    EXPECT_EQ(2, peak) << "a decrement must never lower the high-water mark";

    BlorgStatisticsGaugeIncrement(&gauge, &peak); // back to 2, a depth already seen
    EXPECT_EQ(2, gauge);
    EXPECT_EQ(2, peak) << "returning to a depth already seen must not disturb the peak";

    BlorgStatisticsGaugeIncrement(&gauge, &peak); // 3, a new high
    EXPECT_EQ(3, gauge);
    EXPECT_EQ(3, peak);
}

TEST(StatisticsGaugeTest, IncrementToleratesNullPeak)
{
    LONG64 gauge = 0;

    BlorgStatisticsGaugeIncrement(&gauge, nullptr);
    BlorgStatisticsGaugeIncrement(&gauge, nullptr);
    BlorgStatisticsGaugeDecrement(&gauge);

    EXPECT_EQ(1, gauge);
}

} // namespace
