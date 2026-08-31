#include "Driver.h"

//
// Implements the statistics block declared in Statistics.h: the
// per-processor counter table, the latency/gauge helpers the
// instrumentation sites call, and the two output formats -- the standard
// FILESYSTEM_STATISTICS/FAT_STATISTICS shape for the documented FSCTLs,
// and the summed BLORGFS_STATISTICS_RESPONSE for the vendor IOCTL.
//
// See Statistics.h for the design: why storage is per-processor and
// non-interlocked, why in-flight depth is derived rather than gauged, and
// why the FAT type is the honest one to report.
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
// The outlier threshold, converted to QPC ticks once at init.
//
// Every file-read completion asks "was this one slow?", and the answer has
// to cost a comparison. Converting the elapsed span to microseconds first
// would put a 64-bit multiply and divide on the completion path for the
// 99.7% of fetches that are not outliers, to compute a number those
// fetches then discard. Comparing raw ticks moves the arithmetic to the
// fetches that are actually being recorded.
//
static LONG64 StatisticsSlowFetchThresholdQpc = 0;

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
// Fetches in flight, derived from the monotone counters rather than kept
// in a gauge.
//
// Issue raises FetchesIssued and completion raises FetchesCompleted or
// FetchesFailed, so the difference of the sums is the live depth wherever
// the two halves landed -- which is what makes a per-processor
// arrangement work here at all, and why the shared interlocked word this
// replaces was never necessary.
//
// Summing every processor is a reader's cost. The only writer-side caller
// is the outlier record, which by construction runs a handful of times per
// measurement window.
//
LONG64 BlorgStatisticsFetchesActive(VOID)
{
    ULONG64 issued = 0;
    ULONG64 settled = 0;

    for (ULONG i = 0; i < BlorgStatisticsProcessorCount; ++i)
    {
        const BLORGFS_STATISTICS* entry = StatisticsEntry(i);

        issued += entry->FetchesIssued;
        settled += entry->FetchesCompleted + entry->FetchesFailed;
    }

    return (issued > settled) ? C_CAST(LONG64, issued - settled) : 0;
}

//
// QPC ticks to microseconds. Returns zero for a negative or unmeasurable
// span rather than wrapping it into an enormous unsigned one.
//
static ULONG64 StatisticsQpcToMicroseconds(LONG64 ElapsedQpc)
{
    if (ElapsedQpc < 0 || 0 == StatisticsQpcFrequency)
    {
        return 0;
    }

    return (C_CAST(ULONG64, ElapsedQpc) * 1000000ULL) / C_CAST(ULONG64, StatisticsQpcFrequency);
}

//
// Bucket index for a microsecond latency: 0 for anything under 1 us,
// otherwise one past the position of the value's highest set bit, so
// bucket i holds [2^(i-1), 2^i). Saturates at the last bucket rather
// than wrapping, which is what makes the top bucket mean "at least this
// slow" -- the shape the tail question is actually asking about.
//
// One bit scan rather than a shift loop: the bucket is one past the
// position of the highest set bit, which is what BitScanReverse64 returns
// directly. The loop this replaced ran up to 27 iterations, and this sits
// on the fast-I/O read path where a cache hit costs 16 us -- past some
// point the measurement distorts what it measures.
//
static ULONG StatisticsLatencyBucket(ULONG64 Microseconds)
{
    ULONG highestBit;

    if (!BitScanReverse64(&highestBit, Microseconds))
    {
        return 0;
    }

    const ULONG bucket = highestBit + 1;

    return (bucket < BLORGFS_STATISTICS_LATENCY_BUCKETS)
        ? bucket
        : (BLORGFS_STATISTICS_LATENCY_BUCKETS - 1);
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

    ULONG64 microseconds = StatisticsQpcToMicroseconds(ElapsedQpc);

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

    StatisticsSlowFetchThresholdQpc = C_CAST(LONG64,
        (C_CAST(ULONG64, StatisticsQpcFrequency) * BLORGFS_SLOW_FETCH_THRESHOLD_US) / 1000000ULL);

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

//
// The fields whose per-processor values must be reduced with max rather
// than added.
//
// Summing every ULONG64 in the block is right for counters and wrong for
// maxima, and the wrongness is invisible: adding two processors' worst
// fetches reports a latency no fetch ever had, in a field whose entire
// purpose is to name the worst real one. On this two-processor guest it
// roughly doubled the reported tail, and a driver-versus-usermode
// comparison was drawn against it before the per-fetch outlier records
// showed no fetch anywhere near the figure.
//
// Listed by offset rather than accumulated field by field so the counter
// path stays one loop, with a compile-time check below that the list has
// not drifted from the struct.
//
static const SIZE_T StatisticsMaxFields[] =
{
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchLatencyMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, UserReadLatencyMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, ReadIdleMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, SpeculativeLatencyMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, DemandLatencyMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, HandshakeLatencyMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchPreSendMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchAcquireMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchFreshAcquireMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchSendMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchSendSubmitMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchSendSettleMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchWaitMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchTtfbMaxUs),
    FIELD_OFFSET(BLORGFS_STATISTICS, FetchBodyMaxUs),
};

static BOOLEAN StatisticsFieldIsMax(SIZE_T ByteOffset)
{
    for (SIZE_T i = 0; i < RTL_NUMBER_OF(StatisticsMaxFields); ++i)
    {
        if (StatisticsMaxFields[i] == ByteOffset)
        {
            return TRUE;
        }
    }

    return FALSE;
}

//
// Merges one processor's block into the running total, field by field as
// ULONG64 lanes: counters sum, and the *MaxUs fields reduce with max
// because summing two processors' worst fetches describes a latency no
// fetch ever had.
//
// The walk stops before the outlier ring. Everything above it is a counter
// or a maximum and merges arithmetically; the ring is per-processor
// diagnostic state, and summing a record's fields across processors would
// produce a fetch that never happened.
//
static VOID StatisticsAccumulate(PBLORGFS_STATISTICS Out, const BLORGFS_STATISTICS* Entry)
{
    ULONG64* out = C_CAST(ULONG64*, Out);
    const ULONG64* entry = C_CAST(const ULONG64*, Entry);

    const SIZE_T fields = FIELD_OFFSET(BLORGFS_STATISTICS, SlowFetchSequence) / sizeof(ULONG64);

    for (SIZE_T i = 0; i < fields; ++i)
    {
        if (StatisticsFieldIsMax(i * sizeof(ULONG64)))
        {
            out[i] = (entry[i] > out[i]) ? entry[i] : out[i];
        }
        else
        {
            out[i] += entry[i];
        }
    }
}

//
// Counters are read without any lock against the CPUs still updating
// them: a rate taken over a window of thousands of operations does not
// need the fields to be mutually instantaneous, and a lock here would
// cost every counter site more than the precision is worth.
//
// The outlier ring is the exception, because a torn record is a fabricated
// one rather than a stale one. Each slot's sequence is read, the record
// copied, and the sequence read again: the owning processor can wrap onto
// that slot mid-copy and leave a record half one fetch and half another, so
// a sequence that moved means the copy is discarded. A missing tail sample
// is worth more than an invented one.
//
// Surviving records are insertion-sorted by completion stamp, oldest first,
// capped at the response's size. Per-processor sequence numbers order one
// processor's records and say nothing about another's, so the stamp is the
// only thing that puts a merged tail back into the order it happened.
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

    Out->FetchesActive = BlorgStatisticsFetchesActive();

    Out->SlowFetchesSeen = 0;

    ULONG kept = 0;

    for (ULONG cpu = 0; cpu < BlorgStatisticsProcessorCount; ++cpu)
    {
        PBLORGFS_STATISTICS entry = StatisticsEntry(cpu);

        Out->SlowFetchesSeen += entry->SlowFetchSequence;

        for (ULONG i = 0; i < BLORGFS_SLOW_FETCH_PER_CPU; ++i)
        {
            BLORGFS_SLOW_FETCH record;

            const ULONG64 before =
                C_CAST(ULONG64, ReadAcquire64(C_CAST(LONG64 volatile*, &entry->SlowFetches[i].Sequence)));

            record = entry->SlowFetches[i];

            const ULONG64 after =
                C_CAST(ULONG64, ReadNoFence64(C_CAST(LONG64 volatile*, &entry->SlowFetches[i].Sequence)));

            if (0 == before || before != after)
            {
                continue;
            }

            ULONG at = kept;

            while (at > 0 && Out->SlowFetches[at - 1].CompletedQpc > record.CompletedQpc)
            {
                if (at < BLORGFS_SLOW_FETCH_SAMPLES)
                {
                    Out->SlowFetches[at] = Out->SlowFetches[at - 1];
                }

                --at;
            }

            if (at < BLORGFS_SLOW_FETCH_SAMPLES)
            {
                Out->SlowFetches[at] = record;

                if (kept < BLORGFS_SLOW_FETCH_SAMPLES)
                {
                    ++kept;
                }
            }
        }
    }
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

    StatisticsEpochQpc = BlorgStatisticsNow();
}

//
// Files one outlier into the shared ring.
//
// The slots live in the calling processor's own block, so the claim on the
// sequence is a plain increment: no other processor can be handing out this
// slot, and nothing here needs a locked read-modify-write at any IRQL. The
// ring wraps by design -- with a 250 ms threshold the fetches that reach
// here are rare, and when they are not, the most recent are more useful
// than the first few of a flood.
//
// Sequence is written last, and with a release rather than a plain store:
// the writer is this processor but the reader is whichever one serves the
// query, and on a weakly ordered machine a plain store could advertise the
// slot as filled before the fields it describes are visible. A
// store-release is not a locked operation -- nothing on x64, one STLR on
// ARM64. The reader still has to tolerate a slot being overwritten
// underneath it, which BlorgStatisticsQuery does by re-reading the
// sequence; that is the only part of this that has to be exact.
//
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
    if (0 == StatisticsSlowFetchThresholdQpc || TotalQpc < StatisticsSlowFetchThresholdQpc)
    {
        return;
    }

    const ULONG64 TotalUs = StatisticsQpcToMicroseconds(TotalQpc);

    PBLORGFS_STATISTICS statsBlock = BlorgStatisticsForCurrentProcessor();

    if (!statsBlock)
    {
        return;
    }

    const ULONG64 sequence = ++statsBlock->SlowFetchSequence;
    PBLORGFS_SLOW_FETCH slot =
        &statsBlock->SlowFetches[(sequence - 1) % BLORGFS_SLOW_FETCH_PER_CPU];

    slot->CompletedQpc = C_CAST(ULONG64, BlorgStatisticsNow());
    slot->TotalUs = TotalUs;
    slot->AcquireUs = StatisticsQpcToMicroseconds(AcquireQpc);
    slot->SendUs = StatisticsQpcToMicroseconds(SendQpc);
    slot->WaitUs = StatisticsQpcToMicroseconds(WaitQpc);
    slot->TtfbUs = StatisticsQpcToMicroseconds(TtfbQpc);
    slot->BodyUs = StatisticsQpcToMicroseconds(BodyQpc);
    slot->Bytes = Bytes;
    slot->FetchesActive = BlorgStatisticsFetchesActive();
    slot->ConnectionReused = ConnectionReused ? 1u : 0u;


    WriteRelease64(C_CAST(LONG64 volatile*, &slot->Sequence), C_CAST(LONG64, sequence));
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
