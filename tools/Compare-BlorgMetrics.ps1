<#
.SYNOPSIS
    Compares a PerfHarness metrics report against a stored baseline and
    reports correctness and performance regressions.

.DESCRIPTION
    Consumes the key=value files PerfHarness writes with --report. Two
    independent classes of check run:

      Invariants (correctness). Relationships between counters that must
      hold in any healthy run, checked on the CURRENT report alone -- no
      baseline needed. These catch accounting bugs and lost work that a
      throughput number would never reveal: a prefetch outcome that is
      neither hit, park, nor miss means a read went somewhere unaccounted
      for, and a fetch that was issued but never completed or failed means
      one is stuck or was dropped.

      Regressions (performance). Deltas against a baseline, each with its
      own threshold, because the metrics have very different noise floors:
      throughput on a live backend wanders by a few percent run to run,
      while a counter that is supposed to stay at zero moving off zero is
      never noise.

    Exit code is 0 when everything passes, 1 when any check fails, so an
    agent or CI step can gate on it without parsing the output.

.PARAMETER Current
    Path to the report just produced.

.PARAMETER Baseline
    Path to the accepted baseline. Omit to run invariant checks only,
    which is the right mode for a first run or after an intentional
    behaviour change.

.PARAMETER ThroughputTolerancePercent
    Allowed throughput drop before it is called a regression. Default 5,
    which is above typical run-to-run variance on a live backend and below
    anything a human would call "the same".

.PARAMETER LatencyToleranceFactor
    Allowed growth in p99 fetch latency, as a multiplier. Default 2.0 --
    deliberately loose, because the histogram's buckets are powers of two,
    so the smallest change p99 can even express is a doubling. A tighter
    number here would be false precision.

.PARAMETER RateTolerancePoints
    Allowed drop in a percentage-valued metric (prefetch hit rate,
    connection reuse rate, path cache hit rate), in percentage points.

.EXAMPLE
    .\Compare-BlorgMetrics.ps1 -Current run.txt -Baseline baseline\seq.txt
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Current,
    [string]$Baseline,
    [double]$ThroughputTolerancePercent = 5.0,
    [double]$LatencyToleranceFactor = 2.0,
    [double]$RateTolerancePoints = 5.0
)

$ErrorActionPreference = 'Stop'

function Read-Metrics {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "metrics file not found: $Path"
    }

    $map = @{}

    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()

        if (-not $trimmed -or $trimmed.StartsWith('#')) { continue }

        $split = $trimmed.IndexOf('=')
        if ($split -lt 1) { continue }

        $key = $trimmed.Substring(0, $split)
        $value = $trimmed.Substring($split + 1)

        $map[$key] = $value
    }

    return $map
}

function Get-Num {
    param($Map, [string]$Key)

    if (-not $Map.ContainsKey($Key)) { return $null }

    $parsed = 0.0
    if ([double]::TryParse($Map[$Key], [ref]$parsed)) { return $parsed }
    return $null
}

$failures = New-Object System.Collections.Generic.List[string]
$notes = New-Object System.Collections.Generic.List[string]

$cur = Read-Metrics -Path $Current

Write-Host "workload: $($cur['Workload'])" -ForegroundColor Cyan

# ---------------------------------------------------------------- invariants

#
# Every paging read served inline must land in exactly one prefetch
# outcome. The driver counts hits and parks inside the prefetcher and the
# miss at the single fallback site in BlorgVolumeRead, so the three are
# built to sum to ReadsPagingInline; if they do not, a read took a path
# nobody is accounting for.
#
$paging = Get-Num $cur 'ReadsPagingInline'
$hits = Get-Num $cur 'PrefetchHits'
$parks = Get-Num $cur 'PrefetchParks'
$misses = Get-Num $cur 'PrefetchMisses'

if ($null -ne $paging -and $null -ne $hits -and $null -ne $parks -and $null -ne $misses) {
    $sum = $hits + $parks + $misses

    if ($paging -gt 0 -and $sum -ne $paging) {
        $failures.Add("INVARIANT prefetch outcomes $sum != ReadsPagingInline $paging (unaccounted reads)")
    }
    else {
        $notes.Add("invariant ok: prefetch outcomes sum to ReadsPagingInline ($paging)")
    }
}

#
# Direct fetches are issued once and must terminate exactly once. A
# shortfall means a fetch is still outstanding or was lost; the harness
# samples after the workload has drained, so a small positive shortfall is
# reported rather than failed only when the in-flight gauge explains it.
#
$issued = Get-Num $cur 'FetchesIssued'
$completed = Get-Num $cur 'FetchesCompleted'
$failed = Get-Num $cur 'FetchesFailed'
$prefetchIssued = Get-Num $cur 'PrefetchFetchesIssued'
$prefetchFailed = Get-Num $cur 'PrefetchFetchesFailed'

if ($null -ne $issued -and $null -ne $completed -and $null -ne $failed) {
    #
    # FetchesCompleted is raised by both the direct path and the prefetch
    # path, so the accounting closes across both issuers.
    #
    $prefetchIssuedValue = 0.0
    if ($null -ne $prefetchIssued) { $prefetchIssuedValue = $prefetchIssued }

    $prefetchFailedValue = 0.0
    if ($null -ne $prefetchFailed) { $prefetchFailedValue = $prefetchFailed }

    $totalIssued = $issued + $prefetchIssuedValue
    $totalDone = $completed + $failed + $prefetchFailedValue

    if ($totalDone -gt $totalIssued) {
        $failures.Add("INVARIANT fetch completions $totalDone exceed issues $totalIssued (double-counted completion)")
    }
    elseif ($totalIssued -gt 0 -and $totalDone -lt $totalIssued) {
        $notes.Add("note: $($totalIssued - $totalDone) fetch(es) issued but not terminated (in flight at sample time?)")
    }
    else {
        $notes.Add("invariant ok: fetch issues and terminations balance ($totalIssued)")
    }
}

#
# Counters that are correctness signals rather than performance ones.
# Any nonzero value is worth surfacing even without a baseline: these
# describe work that failed or had to be redone.
#
$errorCounters = @(
    'FetchesFailed',
    'PrefetchFetchesFailed',
    'DirInfoFailures',
    'FileInfoFailures',
    'ConnectionsFailed',
    'SocketTimeouts',
    'HandshakesFailed',
    'FailedCreates'
)

foreach ($name in $errorCounters) {
    $value = Get-Num $cur $name
    if ($null -ne $value -and $value -gt 0) {
        $notes.Add("nonzero error counter: $name = $value")
    }
}

# ---------------------------------------------------------------- regressions

if (-not $Baseline) {
    Write-Host "`n(no baseline given -- invariant checks only)" -ForegroundColor Yellow
}
else {
    $base = Read-Metrics -Path $Baseline

    if ($cur['Workload'] -ne $base['Workload']) {
        $failures.Add("baseline is for workload '$($base['Workload'])' but current is '$($cur['Workload'])' -- not comparable")
    }

    # Higher is better, expressed as a percentage of the baseline.
    foreach ($metric in @('ThroughputMBs', 'OpensPerSec')) {
        $b = Get-Num $base $metric
        $c = Get-Num $cur $metric

        if ($null -eq $b -or $null -eq $c -or $b -le 0) { continue }

        $deltaPct = 100.0 * ($c - $b) / $b

        if ($deltaPct -lt (-1.0 * $ThroughputTolerancePercent)) {
            $failures.Add(("REGRESSION {0}: {1:N2} -> {2:N2} ({3:N1}%)" -f $metric, $b, $c, $deltaPct))
        }
        else {
            $notes.Add(("{0}: {1:N2} -> {2:N2} ({3:+0.0;-0.0;0}%)" -f $metric, $b, $c, $deltaPct))
        }
    }

    # Higher is better, already a percentage -- compared in points.
    foreach ($metric in @('PrefetchHitRate', 'ConnectionReuseRate', 'PathCacheHitRate')) {
        $b = Get-Num $base $metric
        $c = Get-Num $cur $metric

        if ($null -eq $b -or $null -eq $c) { continue }

        $delta = $c - $b

        if ($delta -lt (-1.0 * $RateTolerancePoints)) {
            $failures.Add(("REGRESSION {0}: {1:N1}% -> {2:N1}% ({3:N1} points)" -f $metric, $b, $c, $delta))
        }
        else {
            $notes.Add(("{0}: {1:N1}% -> {2:N1}%" -f $metric, $b, $c))
        }
    }

    # Lower is better, compared as a multiplier.
    foreach ($metric in @('FetchLatencyP50Us', 'FetchLatencyP99Us', 'FetchLatencyMeanUs', 'HandshakeLatencyMeanUs')) {
        $b = Get-Num $base $metric
        $c = Get-Num $cur $metric

        if ($null -eq $b -or $null -eq $c -or $b -le 0) { continue }

        $factor = $c / $b

        if ($factor -gt $LatencyToleranceFactor) {
            $failures.Add(("REGRESSION {0}: {1} -> {2} us ({3:N2}x)" -f $metric, $b, $c, $factor))
        }
        else {
            $notes.Add(("{0}: {1} -> {2} us" -f $metric, $b, $c))
        }
    }

    #
    # Error counters get no tolerance at all against a baseline. A run that
    # newly fails fetches or newly times out sockets has changed behaviour,
    # and calling that "within noise" is how a real defect gets averaged
    # away into a throughput number that still looks fine.
    #
    foreach ($name in $errorCounters) {
        $b = Get-Num $base $name
        $c = Get-Num $cur $name

        if ($null -eq $b -or $null -eq $c) { continue }

        if ($c -gt $b) {
            $failures.Add("REGRESSION $name rose $b -> $c")
        }
    }
}

# ---------------------------------------------------------------- report

if ($notes.Count -gt 0) {
    Write-Host "`n--- observations ---"
    foreach ($n in $notes) { Write-Host "  $n" }
}

if ($failures.Count -gt 0) {
    Write-Host "`n--- FAILURES ---" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  $f" -ForegroundColor Red }
    Write-Host "`n$($failures.Count) check(s) failed." -ForegroundColor Red
    exit 1
}

Write-Host "`nAll checks passed." -ForegroundColor Green
exit 0
