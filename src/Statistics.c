#include "Driver.h"

//
// Implements the statistics block declared in Statistics.h: the
// per-processor counter table, the latency/gauge helpers the
// instrumentation sites call, and the two output formats -- the standard
// FILESYSTEM_STATISTICS/FAT_STATISTICS shape for the documented FSCTLs,
// and the summed BLORGFS_STATISTICS_RESPONSE for the vendor IOCTL.
//
// See Statistics.h for the design: why storage is per-processor and
// non-interlocked, why gauges are the exception, and why the FAT type is
// the honest one to report.
//

#define STATISTICS_TAG 'tSPB'

PBLORGFS_STATISTICS BlorgStatisticsTable = NULL;

//
// The raw allocation behind BlorgStatisticsTable. Kept separately because
// the table pointer is aligned forward off this block, and ExFreePool must
// be handed back exactly what ExAllocatePool returned.
//
static PVOID StatisticsTableAllocation = NULL;
ULONG BlorgStatisticsEntryStride = 0;
ULONG BlorgStatisticsProcessorCount = 0;
BLORGFS_STATISTICS_GLOBAL BlorgStatisticsGauges = { 0 };

//
// QPC frequency, captured once at init. QueryPerformanceCounter's
// frequency is fixed for the life of the boot, so re-reading it per
// sample would buy nothing and cost a call on the fetch completion path.
//
static LONG64 StatisticsQpcFrequency = 0;

//
// QPC stamp of the last reset. Reported to the harness so a rate is
// computed over the window the driver actually observed.
//
static LONG64 StatisticsEpochQpc = 0;

//
// Byte offset of processor Index's entry. The table is one flat
// allocation strided by BlorgStatisticsEntryStride rather than an array
// of BLORGFS_STATISTICS, because the stride is rounded up to a 64-byte
// multiple (the FILESYSTEM_STATISTICS contract, and the property that
// keeps two processors' counters off one cache line) and so is not
// sizeof(BLORGFS_STATISTICS).
//
// A 64-byte stride only delivers that second property if the base is
// itself 64-byte aligned. ExAllocatePool guarantees only
// MEMORY_ALLOCATION_ALIGNMENT (16 on x64), so from a 16-aligned base every
// entry boundary lands mid-cache-line and two adjacent processors share
// one -- exactly the false sharing the stride exists to prevent, on the
// hottest write in the driver. Hence the over-allocate-and-align below:
// StatisticsTableAllocation owns the raw block for the free,
// BlorgStatisticsTable is the aligned view everything else uses.
//
static PBLORGFS_STATISTICS StatisticsEntry(ULONG Index)
{
    return C_CAST(PBLORGFS_STATISTICS,
        C_CAST(PUCHAR, BlorgStatisticsTable) + (C_CAST(SIZE_T, Index) * BlorgStatisticsEntryStride));
}

LONG64 BlorgStatisticsNow(VOID)
{
    return KeQueryPerformanceCounter(NULL).QuadPart;
}

PBLORGFS_STATISTICS BlorgStatisticsForCurrentProcessor(VOID)
{
    if (!BlorgStatisticsTable)
    {
        return NULL;
    }

    ULONG index = KeGetCurrentProcessorIndex();

    if (index >= BlorgStatisticsProcessorCount)
    {
        return NULL;
    }

    return StatisticsEntry(index);
}

//
// Bucket index for a microsecond latency: 0 for anything under 1 us,
// otherwise one past the position of the value's highest set bit, so
// bucket i holds [2^(i-1), 2^i). Saturates at the last bucket rather
// than wrapping, which is what makes the top bucket mean "at least this
// slow" -- the shape the tail question is actually asking about.
//
static ULONG StatisticsLatencyBucket(ULONG64 Microseconds)
{
    ULONG bucket = 0;

    while (Microseconds && bucket < (BLORGFS_STATISTICS_LATENCY_BUCKETS - 1))
    {
        Microseconds >>= 1;
        bucket++;
    }

    return bucket;
}

VOID BlorgStatisticsRecordLatency(
    ULONG64* Sum,
    ULONG64* Max,
    ULONG64* Buckets,
    LONG64 ElapsedQpc)
{
    if (ElapsedQpc < 0 || 0 == StatisticsQpcFrequency)
    {
        return;
    }

    ULONG64 microseconds = (C_CAST(ULONG64, ElapsedQpc) * 1000000ULL) / C_CAST(ULONG64, StatisticsQpcFrequency);

    *Sum += microseconds;

    if (microseconds > *Max)
    {
        *Max = microseconds;
    }

    if (Buckets)
    {
        Buckets[StatisticsLatencyBucket(microseconds)]++;
    }
}

//
// Gauge increment plus running peak. The peak is a compare-exchange loop
// rather than a read-then-store: two CPUs raising the gauge at once would
// otherwise both read the old peak and one update would vanish, which for
// a "how deep did the pipeline ever get" number is the one sample that
// mattered.
//
VOID BlorgStatisticsGaugeIncrement(LONG64 volatile* Gauge, LONG64 volatile* Peak)
{
    LONG64 current = InterlockedIncrement64(Gauge);

    if (!Peak)
    {
        return;
    }

    for (;;)
    {
        LONG64 observedPeak = ReadNoFence64(Peak);

        if (current <= observedPeak)
        {
            return;
        }

        if (observedPeak == InterlockedCompareExchange64(Peak, current, observedPeak))
        {
            return;
        }
    }
}

VOID BlorgStatisticsGaugeDecrement(LONG64 volatile* Gauge)
{
    InterlockedDecrement64(Gauge);
}

//
// Sized for the maximum processor count rather than the active one so a
// processor hot-added after load indexes a block that already exists;
// KeGetCurrentProcessorIndex on such a processor would otherwise run off
// the end of an active-count-sized table. NonPagedPoolNx because the read
// and WSK completion paths touch these at DISPATCH_LEVEL.
//
NTSTATUS BlorgStatisticsInitialize(VOID)
{
    ULONG entrySize = C_CAST(ULONG, sizeof(BLORGFS_STATISTICS));
    ULONG stride = (entrySize + 63) & ~C_CAST(ULONG, 63);
    ULONG processors = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);

    if (0 == processors)
    {
        return STATUS_UNSUCCESSFUL;
    }

    StatisticsTableAllocation = ExAllocatePoolZero(
        NonPagedPoolNx,
        (C_CAST(SIZE_T, stride) * processors) + (BLORGFS_STATISTICS_LINE - 1),
        STATISTICS_TAG);

    if (!StatisticsTableAllocation)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    BlorgStatisticsTable = C_CAST(PBLORGFS_STATISTICS,
        (C_CAST(ULONG_PTR, StatisticsTableAllocation) + (BLORGFS_STATISTICS_LINE - 1)) &
        ~C_CAST(ULONG_PTR, BLORGFS_STATISTICS_LINE - 1));

    BlorgStatisticsEntryStride = stride;
    BlorgStatisticsProcessorCount = processors;

    LARGE_INTEGER frequency;
    StatisticsEpochQpc = KeQueryPerformanceCounter(&frequency).QuadPart;
    StatisticsQpcFrequency = frequency.QuadPart;

    return STATUS_SUCCESS;
}

VOID BlorgStatisticsCleanup(VOID)
{
    if (StatisticsTableAllocation)
    {
        ExFreePool(StatisticsTableAllocation);
        StatisticsTableAllocation = NULL;
    }

    BlorgStatisticsTable = NULL;
    BlorgStatisticsEntryStride = 0;
    BlorgStatisticsProcessorCount = 0;
}

//
// Adds every ULONG64 field of one entry into Out. Done as a field-count
// loop over the struct rather than member by member because
// BLORGFS_STATISTICS is, by construction, nothing but ULONG64 counters --
// so a new counter is summed correctly the moment it is declared, with no
// second place to remember to update. The static assert is what keeps
// that assumption honest if the struct ever gains a field of another
// type.
//
C_ASSERT(0 == (sizeof(BLORGFS_STATISTICS) % sizeof(ULONG64)));

static VOID StatisticsAccumulate(PBLORGFS_STATISTICS Out, const BLORGFS_STATISTICS* Entry)
{
    ULONG64* out = C_CAST(ULONG64*, Out);
    const ULONG64* entry = C_CAST(const ULONG64*, Entry);

    for (SIZE_T i = 0; i < (sizeof(BLORGFS_STATISTICS) / sizeof(ULONG64)); ++i)
    {
        out[i] += entry[i];
    }
}

//
// Counters are read without any lock against the CPUs still updating
// them: a rate taken over a window of thousands of operations does not
// need the fields to be mutually instantaneous, and a lock here would
// cost every counter site more than the precision is worth.
//
VOID BlorgStatisticsQuery(PBLORGFS_STATISTICS_RESPONSE Out)
{
    RtlZeroMemory(Out, sizeof(*Out));

    Out->Version = BLORGFS_STATISTICS_VERSION;

#if DBG
    Out->Flags = BLORGFS_STATS_FLAG_CHECKED_BUILD;
#else
    Out->Flags = 0;
#endif

    Out->SizeOfStruct = C_CAST(ULONG, sizeof(BLORGFS_STATISTICS_RESPONSE));
    Out->ProcessorCount = BlorgStatisticsProcessorCount;
    Out->QpcFrequency = StatisticsQpcFrequency;
    Out->EpochQpc = StatisticsEpochQpc;
    Out->NowQpc = BlorgStatisticsNow();

    for (ULONG i = 0; i < BlorgStatisticsProcessorCount; ++i)
    {
        StatisticsAccumulate(&Out->Totals, StatisticsEntry(i));
    }

    Out->Gauges.FetchesActive = ReadNoFence64(&BlorgStatisticsGauges.FetchesActive);
    Out->Gauges.FetchesActivePeak = ReadNoFence64(&BlorgStatisticsGauges.FetchesActivePeak);
}

//
// FetchesActive is deliberately NOT zeroed: it is a gauge, and a reset
// that cleared it would make the driver report fewer in-flight fetches
// than there are, permanently, once the outstanding ones complete and
// decrement. Only the peak -- a window-scoped high-water mark -- is
// restarted, from the current depth.
//
VOID BlorgStatisticsReset(VOID)
{
    for (ULONG i = 0; i < BlorgStatisticsProcessorCount; ++i)
    {
        RtlZeroMemory(StatisticsEntry(i), sizeof(BLORGFS_STATISTICS));
    }

    InterlockedExchange64(
        &BlorgStatisticsGauges.FetchesActivePeak,
        ReadNoFence64(&BlorgStatisticsGauges.FetchesActive));

    StatisticsEpochQpc = BlorgStatisticsNow();
}

//
// Clamps a ULONG64 counter down to the ULONG field the non-extended
// FILESYSTEM_STATISTICS carries. Saturating rather than truncating: a
// wrapped byte count reads as a plausible small number and silently lies,
// where a pegged MAXULONG is visibly "this counter is too small for this
// workload" and points the caller at the _EX variant.
//
static ULONG StatisticsClampToUlong(ULONG64 Value)
{
    return (Value > MAXULONG) ? MAXULONG : C_CAST(ULONG, Value);
}

static VOID StatisticsFillFat(PFAT_STATISTICS Fat, const BLORGFS_STATISTICS* Entry)
{
    Fat->CreateHits = StatisticsClampToUlong(Entry->CreateHits);
    Fat->SuccessfulCreates = StatisticsClampToUlong(Entry->SuccessfulCreates);
    Fat->FailedCreates = StatisticsClampToUlong(Entry->FailedCreates);
    Fat->NonCachedReads = StatisticsClampToUlong(Entry->NonCachedReads);
    Fat->NonCachedReadBytes = StatisticsClampToUlong(Entry->NonCachedReadBytes);
    Fat->NonCachedWrites = StatisticsClampToUlong(Entry->NonCachedWrites);
    Fat->NonCachedWriteBytes = StatisticsClampToUlong(Entry->NonCachedWriteBytes);
    Fat->NonCachedDiskReads = StatisticsClampToUlong(Entry->NonCachedDiskReads);
    Fat->NonCachedDiskWrites = StatisticsClampToUlong(Entry->NonCachedDiskWrites);
}

//
// One output entry per processor, each the common header followed by
// FAT_STATISTICS and padded out to SizeOfCompleteStructure, which the
// contract requires to be a multiple of 64. The caller's buffer is
// filled entry by entry and stops at whatever it can hold, because these
// FSCTLs are documented to be callable with a short buffer to discover
// the size -- hence STATUS_BUFFER_OVERFLOW plus a byte count rather than
// a hard failure. Only whole entries are written: a consumer strides by
// SizeOfCompleteStructure, so handing it a torn final entry would have it
// read a half-populated one as real.
//
NTSTATUS BlorgStatisticsFillFsctlBuffer(
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN Extended,
    PULONG BytesReturned)
{
    ULONG headerSize = Extended
        ? C_CAST(ULONG, sizeof(FILESYSTEM_STATISTICS_EX))
        : C_CAST(ULONG, sizeof(FILESYSTEM_STATISTICS));

    ULONG entrySize = headerSize + C_CAST(ULONG, sizeof(FAT_STATISTICS));
    ULONG stride = (entrySize + 63) & ~C_CAST(ULONG, 63);

    *BytesReturned = 0;

    if (OutputBufferLength < stride)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    ULONG processors = BlorgStatisticsProcessorCount;

    if (0 == processors)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    ULONG entriesThatFit = OutputBufferLength / stride;
    ULONG entriesToWrite = (entriesThatFit < processors) ? entriesThatFit : processors;

    RtlZeroMemory(OutputBuffer, C_CAST(SIZE_T, stride) * entriesToWrite);

    for (ULONG i = 0; i < entriesToWrite; ++i)
    {
        PUCHAR entryBase = C_CAST(PUCHAR, OutputBuffer) + (C_CAST(SIZE_T, i) * stride);
        const BLORGFS_STATISTICS* source = StatisticsEntry(i);

        if (Extended)
        {
            PFILESYSTEM_STATISTICS_EX header = C_CAST(PFILESYSTEM_STATISTICS_EX, entryBase);

            header->FileSystemType = FILESYSTEM_STATISTICS_TYPE_FAT;
            header->Version = 1;
            header->SizeOfCompleteStructure = stride;

            header->UserFileReads = source->UserFileReads;
            header->UserFileReadBytes = source->UserFileReadBytes;
            header->UserDiskReads = source->UserDiskReads;
            header->UserFileWrites = source->UserFileWrites;
            header->UserFileWriteBytes = source->UserFileWriteBytes;
            header->UserDiskWrites = source->UserDiskWrites;
            header->MetaDataReads = source->MetaDataReads;
            header->MetaDataReadBytes = source->MetaDataReadBytes;
            header->MetaDataDiskReads = source->MetaDataDiskReads;
            header->MetaDataWrites = source->MetaDataWrites;
            header->MetaDataWriteBytes = source->MetaDataWriteBytes;
            header->MetaDataDiskWrites = source->MetaDataDiskWrites;
        }
        else
        {
            PFILESYSTEM_STATISTICS header = C_CAST(PFILESYSTEM_STATISTICS, entryBase);

            header->FileSystemType = FILESYSTEM_STATISTICS_TYPE_FAT;
            header->Version = 1;
            header->SizeOfCompleteStructure = stride;

            header->UserFileReads = StatisticsClampToUlong(source->UserFileReads);
            header->UserFileReadBytes = StatisticsClampToUlong(source->UserFileReadBytes);
            header->UserDiskReads = StatisticsClampToUlong(source->UserDiskReads);
            header->UserFileWrites = StatisticsClampToUlong(source->UserFileWrites);
            header->UserFileWriteBytes = StatisticsClampToUlong(source->UserFileWriteBytes);
            header->UserDiskWrites = StatisticsClampToUlong(source->UserDiskWrites);
            header->MetaDataReads = StatisticsClampToUlong(source->MetaDataReads);
            header->MetaDataReadBytes = StatisticsClampToUlong(source->MetaDataReadBytes);
            header->MetaDataDiskReads = StatisticsClampToUlong(source->MetaDataDiskReads);
            header->MetaDataWrites = StatisticsClampToUlong(source->MetaDataWrites);
            header->MetaDataWriteBytes = StatisticsClampToUlong(source->MetaDataWriteBytes);
            header->MetaDataDiskWrites = StatisticsClampToUlong(source->MetaDataDiskWrites);
        }

        StatisticsFillFat(C_CAST(PFAT_STATISTICS, entryBase + headerSize), source);
    }

    *BytesReturned = stride * entriesToWrite;

    return (entriesToWrite < processors) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
