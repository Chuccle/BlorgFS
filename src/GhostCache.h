#pragma once

//
// Ghost cache: a model of an on-disk block cache that stores no data.
//
// The on-disk hot cache in AGENTS.md ("Future work") only pays off for
// bytes read more than once after the Windows cache has dropped them, and
// nothing in this driver could say how often that happens. This answers
// it before any of that is built. Every range GET the driver issues is
// replayed against a table of the 64 KB blocks recent fetches covered,
// which records when each block was last fetched and how many times, and
// the counters it raises (Ghost* in Statistics.h) say how many fetches a
// real cache of a given size would have served from local disk instead.
//
// Off by default. GhostCacheMb in the service's Parameters key sizes the
// table in megabytes (rounded down to a power of two, capped at
// GHOST_CACHE_MAX_TABLE_MB); each MB of table remembers 4 GB of fetched
// blocks, so the default suggestion of 64 covers the largest capacity the
// counters report. Zero, absent, or an allocation failure leaves
// BlorgGhostCacheObserve a no-op.
//

//
// Allocates the table, or does nothing when TableMb is zero. Called once
// from DriverEntry after the registry is read. Failure is non-fatal and
// leaves the model off.
//
NTSTATUS BlorgGhostCacheInitialize(ULONG TableMb);

// Frees the table. Called from DriverUnload.
VOID BlorgGhostCacheCleanup(VOID);

//
// Replays one fetch against the model. PASSIVE_LEVEL only: the table is
// paged, and every fetch is issued at PASSIVE, inline or from the FSP.
//
VOID BlorgGhostCacheObserve(
    const UNICODE_STRING* Path,
    ULONG64 FileSize,
    ULONG64 LastModifiedTime,
    ULONG64 Offset,
    ULONG Length,
    BOOLEAN Demand);
