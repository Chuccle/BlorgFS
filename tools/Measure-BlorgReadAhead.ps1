<#
.SYNOPSIS
    Read-ahead granularity sweep against a mounted BlorgFS volume, from the host.

.DESCRIPTION
    Answers the question the committed 512 KB granularity was never asked:
    what does it cost a latency-shaped workload.

    That value was chosen on an eight-stream throughput benchmark. Nothing
    below 256 KB was ever tried, FastFat uses 64 KB, Cc's own default is a
    page, and NTFS measured on this same guest averages 98 KB per disk read
    against this driver's 675 KB. A granularity that pays for itself when a
    stream is walking a file forwards is a tax when a demuxer is picking at
    it -- every small scattered read drags a full granule across the network.

    So the metric here is deliberately not throughput. It is
    UserReadsOverFrame: reads that took longer than a 24 fps frame interval,
    which is what a viewer can actually see. Throughput is reported beside
    it because a granularity that fixes the tail by destroying sequential
    bandwidth is not a fix, and the amplification column says which is
    happening.

    Each point runs against a freshly rebooted guest. That reboot is the
    measurement, not hygiene: a second run against the same file is served
    from RAM with no paging reads at all, so a sweep without it measures the
    previous point's leftovers. Same reasoning as Measure-BlorgScaling.ps1,
    and the same Reset-Driver shape.

.PARAMETER GranularityKb
    Values to sweep, written to Parameters\ReadAheadGranularityKb before each
    point. 0 means never call CcSetReadAheadGranularity and leave Cc's own
    default -- the case no other value can express.

.PARAMETER Tracks
    Interleaved sequential cursors within the file. Three models the case
    that reproduced the stutter: a video, an audio and a subtitle track
    advancing together through one handle.

.PARAMETER BlockKb
    Read size. Small is the point: the stutter pattern is a demuxer picking
    at a file, not a stream walking it.

.PARAMETER Count
    Reads per point, round-robined across the cursors.

.EXAMPLE
    .\Measure-BlorgReadAhead.ps1 -TargetFile 'B:\shows\...\ep.mkv' -OutputPath readahead.txt
#>
[CmdletBinding()]
param(
    [int[]]$GranularityKb = @(512, 256, 128, 64, 0),
    [Parameter(Mandatory = $true)][string]$TargetFile,
    [int]$Tracks = 3,
    [int]$BlockKb = 4,
    [int]$StrideKb = 512,
    [int]$Burst = 4,
    [int]$Count = 2000,
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

function Invoke-GuestStrict([string[]]$CommandArgs) {
    $out = Invoke-Guest $CommandArgs
    if ($LASTEXITCODE -ne 0) {
        throw "vmrun $($CommandArgs[0]) failed (exit $LASTEXITCODE): $out"
    }
    return $out
}

function Invoke-GuestPowerShell([string]$Command) {
    Invoke-GuestStrict @('runProgramInGuest', $vmx, '-activeWindow',
        'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe',
        '-NoProfile', '-Command', $Command) | Out-Null
}

#
# Every guest call in this sweep goes through here first.
#
# vmrun reports "VMware Tools are not running in the guest" for three
# different situations -- the VM is powered off, it is mid-boot, or Tools
# have not published yet -- and the first sweep died on the second of those
# between two points, discarding four of five measurements after the first
# had already run. A sweep that reboots the guest five times cannot treat
# guest availability as a precondition; it has to be something the script
# re-establishes.
#
# directoryExistsInGuest is deliberately not the probe. It answered
# "exists" while runProgramInGuest was still failing, which is how the
# original readiness loop passed and the very next call did not.
#
function Wait-Guest([int]$TimeoutSeconds = 300) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $failures = 0
    $reset = $false

    while ((Get-Date) -lt $deadline) {
        #
        # Joined to one string on purpose. `vmrun list` returns an array of
        # lines, and `-notmatch` against an array is a filter, not a test:
        # it returns the lines that do not match, which is non-empty (the
        # "Total running VMs" header) even when the VM is listed. As a
        # condition that is always true, so the loop kept restarting a VM
        # that was already up and never reached the probe below.
        #
        $running = (& $vmrun -T ws list 2>&1) -join "`n"

        if ($running -notmatch [regex]::Escape($vmx)) {
            & $vmrun @auth start $vmx nogui 2>&1 | Out-Null
            Start-Sleep -Seconds 15
            continue
        }

        #
        # PowerShell, not cmd.exe. vmrun returns 1 for `cmd.exe /c exit 0`
        # and for `cmd.exe /c ver` on a perfectly healthy guest -- its
        # argument handling for cmd does not survive the trip -- so a
        # cmd-based probe reports every guest as dead. That is worse than no
        # probe: it fired the hard-reset escalation below against a guest
        # that was fine, and the reset is what actually broke it.
        #
        & $vmrun @auth runProgramInGuest $vmx '-activeWindow' `
            'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' `
            '-NoProfile' '-Command' 'exit 0' 2>&1 | Out-Null

        if ($LASTEXITCODE -eq 0) { return }

        #
        # A guest can be listed as running, report Tools as running, and
        # still refuse every program launch -- observed here as a black
        # console with checkToolsState saying "running". Waiting longer does
        # not fix that state; only a hard reset does. Escalating once per
        # Wait-Guest keeps a wedged guest from consuming the whole sweep,
        # and not escalating on the first failure keeps an ordinary
        # mid-boot window from triggering a needless reset.
        #
        $failures++

        if ($failures -eq 6 -and -not $reset) {
            Write-Host '    guest unresponsive with Tools up -- hard reset' -ForegroundColor Yellow
            & $vmrun @auth reset $vmx hard 2>&1 | Out-Null
            $reset = $true
            Start-Sleep -Seconds 30
        }

        Start-Sleep -Seconds 5
    }

    throw "guest did not become usable within $TimeoutSeconds s -- refusing to measure against a VM that may be down"
}

#
# The registry value is written before the reboot rather than after, because
# the driver reads it once in DriverEntry. Writing it to a running driver
# and then measuring would report the previous point's granularity under
# this point's label -- a silent off-by-one across the whole sweep.
#
function Set-Granularity([int]$Kb) {
    Wait-Guest

    Invoke-GuestPowerShell @"
New-Item -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\BlorgFS\Parameters' -Force | Out-Null
Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\BlorgFS\Parameters' -Name 'ReadAheadGranularityKb' -Value $Kb -Type DWord
"@
}

function Reset-Driver {
    Invoke-GuestPowerShell 'shutdown /r /t 0'

    #
    # Long enough that the probe cannot catch the pre-shutdown guest still
    # answering and call it ready, which would run the point against the
    # previous point's driver.
    #
    Start-Sleep -Seconds 25

    Wait-Guest

    Invoke-GuestPowerShell 'sc.exe start BlorgFS *> $null; Start-Sleep -Seconds 4'
}

$rows = @()

foreach ($kb in $GranularityKb) {
    $label = if ($kb -eq 0) { 'Cc default' } else { "$kb KB" }
    Write-Host "==> granularity $label, $Tracks tracks, ${BlockKb}KB x$Burst burst, +${StrideKb}KB, $Count reads" -ForegroundColor Cyan

    Set-Granularity $kb
    Reset-Driver

    $guestReport = "$GuestDeployDir\ra-$kb.kv"
    $guestOut = "$GuestDeployDir\ra-$kb.txt"
    $harness = "$GuestDeployDir\PerfHarness.exe"

    Invoke-GuestPowerShell "& '$harness' demux '$TargetFile' $Tracks $BlockKb $Count $StrideKb $Burst --report '$guestReport' *> '$guestOut'"

    $localReport = Join-Path $env:TEMP "blorg-ra-$kb.kv"
    Remove-Item $localReport -ErrorAction SilentlyContinue
    Invoke-GuestStrict @('copyFileFromGuestToHost', $vmx, $guestReport, $localReport) | Out-Null

    if (-not (Test-Path $localReport)) {
        throw "guest reported success but no report arrived for $label"
    }

    $kv = @{}
    foreach ($line in Get-Content $localReport) {
        if ($line -match '^([A-Za-z0-9_]+)=(.+)$') { $kv[$Matches[1]] = $Matches[2] }
    }

    #
    # A missing key means the harness on the guest predates this report
    # format, which would otherwise surface as a column of zeroes that reads
    # exactly like "granularity made no difference".
    #
    foreach ($required in 'UserReadSamples', 'UserReadsOverFrame', 'ThroughputMBs', 'FetchBytes', 'Bytes') {
        if (-not $kv.ContainsKey($required)) {
            throw "report for $label has no '$required' -- redeploy PerfHarness (saved at $localReport)"
        }
    }

    $requested = [double]$kv['Bytes']
    $fetched = [double]$kv['FetchBytes']

    $rows += [pscustomobject]@{
        GranularityKb = $kb
        ThroughputMBs = [math]::Round([double]$kv['ThroughputMBs'], 2)
        Amplification = if ($requested -gt 0) { [math]::Round($fetched / $requested, 1) } else { [double]::NaN }
        UserReads     = [int64]$kv['UserReadSamples']
        OverFrame     = [int64]$kv['UserReadsOverFrame']
        OverFramePct  = [math]::Round(100.0 * [double]$kv['UserReadsOverFrameShare'], 2)
        UserP50ms     = [math]::Round([double]$kv['UserReadLatencyP50Us'] / 1000.0, 2)
        UserP99ms     = [math]::Round([double]$kv['UserReadLatencyP99Us'] / 1000.0, 2)
        UserMaxMs     = [math]::Round([double]$kv['UserReadLatencyMaxUs'] / 1000.0, 2)
        Fetches       = [int64]$kv['FetchesCompleted']
        FetchP99ms    = [math]::Round([double]$kv['FetchLatencyP99Us'] / 1000.0, 2)
    }
}

if (-not $rows) { throw 'no runs produced results.' }

$rendered = $rows | Format-Table GranularityKb, ThroughputMBs, Amplification, UserReads, OverFrame,
    OverFramePct, UserP50ms, UserP99ms, UserMaxMs, Fetches, FetchP99ms -AutoSize | Out-String

Write-Host $rendered

if ($OutputPath) {
    $rendered | Out-File -Encoding ascii $OutputPath
    Write-Host "written to $OutputPath"
}
