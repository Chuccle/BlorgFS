<#
.SYNOPSIS
    Multi-stream scaling sweep against a mounted BlorgFS volume, from the host.

.DESCRIPTION
    Answers the question a single-stream number cannot: what happens to
    throughput, fairness and the latency tail as concurrent streams increase.
    That is the workload this driver exists for -- a video and its subtitle
    track, a game streaming assets while its own data file is open, a library
    browse overlapping playback.

    Runs PerfHarness's `streams` workload at each stream count in turn, each
    against a freshly rebooted guest so the Windows cache starts empty. That
    reboot is not hygiene, it is the measurement: a second run against the
    same files is served from RAM at thousands of MB/s with no paging reads at
    all, so a sweep without it measures the previous run's leftovers rather
    than the configuration under test.

    Emits one row per stream count and a scaling-efficiency column, plus the
    driver's own ring counters so a throughput change can be attributed rather
    than guessed at.

.PARAMETER StreamCounts
    Concurrency levels to sweep. Default 1,2,4,8,12 -- spanning below, at, and
    above the default ring budget, which is where the interesting behaviour is.

.PARAMETER Seconds
    Duration of each run. Short runs are dominated by pipeline spin-up.

.PARAMETER MediaDirectory
    Directory on the volume to draw stream files from.

.PARAMETER OutputPath
    Where to write the results table for later comparison.

.EXAMPLE
    .\Measure-BlorgScaling.ps1 -OutputPath baseline.txt
    .\Measure-BlorgScaling.ps1 -OutputPath after.txt
    Compare-Object (Get-Content baseline.txt) (Get-Content after.txt)
#>
[CmdletBinding()]
param(
    [int[]]$StreamCounts = @(1, 2, 4, 8, 12),
    [int]$Seconds = 30,
    [string]$MediaDirectory = 'B:\',
    [string]$OutputPath,
    [string]$GuestDeployDir = 'C:\BlorgFS-Deploy'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$blorgEnv = & (Join-Path $repoRoot 'deploy\Get-BlorgEnv.ps1')

foreach ($required in 'VmxPath', 'GuestUser', 'GuestPassword') {
    if (-not $blorgEnv.ContainsKey($required)) {
        throw "$required missing from blorgfs.env -- see deploy\blorgfs.env.example."
    }
}

$vmrun = @(
    'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe',
    'C:\Program Files\VMware\VMware Workstation\vmrun.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $vmrun) { throw 'vmrun.exe not found.' }

$vmx = $blorgEnv['VmxPath']
$auth = @('-T', 'ws', '-gu', $blorgEnv['GuestUser'], '-gp', $blorgEnv['GuestPassword'])
if ($blorgEnv['VmPassword']) { $auth += @('-vp', $blorgEnv['VmPassword']) }

function Invoke-Guest([string[]]$CommandArgs) {
    & $vmrun @auth @CommandArgs 2>&1
}

function Invoke-GuestPowerShell([string]$Command) {
    Invoke-Guest @('runProgramInGuest', $vmx, '-activeWindow',
        'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe',
        '-NoProfile', '-Command', $Command) | Out-Null
}

#
# A guest reboot, not a service restart, and not just a counter reset.
#
# PerfHarness resets the statistics at the start of every workload, but the
# chunk pool and the rings still attached to cached FCBs are live driver state
# that a statistics reset does not touch -- so without a real reset each run
# inherits the previous run's warmed pool and its still-open files, and every
# number after the first measures that inheritance rather than the
# configuration under test.
#
# `sc stop` cannot provide that reset: it wedges in STOP_PENDING because
# nothing dismounts a self-mounted volume, and the service then cannot be
# started again without a reboot (see deploy/DEBUGGING.md). Rebooting is
# slower but it is the only reset that actually works, and a sweep that
# silently measured a wedged driver would be worse than a slow one.
#
function Reset-Driver {
    Invoke-GuestPowerShell 'shutdown /r /t 0'

    Start-Sleep -Seconds 20

    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 5
        $probe = Invoke-Guest @('directoryExistsInGuest', $vmx, $GuestDeployDir)
        if ($probe -match 'exists') { break }
    }

    # Demand-start: the driver does not come up by itself after a reboot.
    Invoke-GuestPowerShell 'sc.exe start BlorgFS *> $null; Start-Sleep -Seconds 4'
}

$rows = @()

foreach ($count in $StreamCounts) {
    Write-Host "==> $count stream(s), $Seconds s" -ForegroundColor Cyan

    Reset-Driver

    $guestOut = "$GuestDeployDir\scaling-$count.txt"
    $harness = "$GuestDeployDir\PerfHarness.exe"

    #
    # Trailing backslash stripped before the argument crosses into a native
    # command line: Windows treats a backslash before a closing quote as an
    # escape, so a bare drive root would arrive mangled and shift every
    # argument after it.
    #
    $mediaArg = $MediaDirectory.TrimEnd('\')
    if ($mediaArg -match '^[A-Za-z]:$') { $mediaArg = "$mediaArg\." }

    Invoke-GuestPowerShell "& '$harness' streams '$mediaArg' $count $Seconds *> '$guestOut'"

    $localOut = Join-Path $env:TEMP "blorg-scaling-$count.txt"
    Remove-Item $localOut -ErrorAction SilentlyContinue
    Invoke-Guest @('copyFileFromGuestToHost', $vmx, $guestOut, $localOut) | Out-Null

    if (-not (Test-Path $localOut)) {
        Write-Warning "no result for $count stream(s)"
        continue
    }

    $text = Get-Content $localOut -Raw

    function Field([string]$pattern) {
        $m = [regex]::Match($text, $pattern)
        if ($m.Success) { return [double]$m.Groups[1].Value }
        return [double]::NaN
    }

    $rows += [pscustomobject]@{
        Streams   = $count
        Aggregate = Field 'aggregate\s+([\d.]+) MB/s'
        Fairness  = Field 'fairness ([\d.]+)'
        P50ms     = Field 'latency p50\s+([\d.]+) ms'
        P95ms     = Field 'latency p95\s+([\d.]+) ms'
        P99ms     = Field 'latency p99\s+([\d.]+) ms'
        MaxMs     = Field 'latency max\s+([\d.]+) ms'
        Armed     = Field 'armed / refused\s+(\d+)'
        Refused   = Field 'armed / refused\s+\d+ / (\d+)'
        Hits      = Field 'hit\s+(\d+)'
        Misses    = Field 'miss\s+(\d+)'
    }
}

if (-not $rows) { throw 'no runs produced results.' }

#
# Scaling efficiency against the single-stream result: 1.0 means aggregate
# throughput grew linearly with stream count, which is the ideal this workload
# is measured against. Anything well below it is contention, starvation or
# serialisation, and the ring counters beside it say which.
#
$single = ($rows | Where-Object { $_.Streams -eq $rows[0].Streams } | Select-Object -First 1).Aggregate

$table = $rows | ForEach-Object {
    $ideal = $single * ($_.Streams / $rows[0].Streams)
    $_ | Add-Member -NotePropertyName Efficiency -NotePropertyValue ([math]::Round($_.Aggregate / $ideal, 3)) -PassThru
}

$rendered = $table | Format-Table Streams, Aggregate, Efficiency, Fairness, P50ms, P95ms, P99ms, MaxMs, Armed, Refused -AutoSize | Out-String

Write-Host $rendered

if ($OutputPath) {
    $rendered | Out-File -Encoding ascii $OutputPath
    Write-Host "written to $OutputPath"
}
