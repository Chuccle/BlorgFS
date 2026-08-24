<#
.SYNOPSIS
    Runs INSIDE the guest. Blocks until the guest CPU has been quiet long
    enough that a benchmark run means something.

.DESCRIPTION
    A freshly booted Windows guest runs Defender, SearchIndexer, Windows
    Update and TiWorker for minutes. On a 2-vCPU guest that is both vCPUs
    saturated, which does two things to a measurement: it steals the CPU the
    driver needs, and it makes vmrun guest operations slow enough to look
    wedged.

    This is not hypothetical. A benchmark taken during that window measured
    its usermode control at 25.63 MB/s before the driver run and 14.04 MB/s
    after -- a 45% collapse inside one cycle, which is what exposed it.

    Exits 0 either way. A timeout is reported in the log rather than thrown,
    because a caller that has waited this long is better served by a warning
    and a number it can judge than by a failed deploy.
#>
param([int]$MaxWaitSeconds = 420, [int]$IdlePercent = 25, [int]$ConsecutiveSamples = 3)
#
# Wait for the guest to finish its post-boot maintenance before measuring.
# A freshly booted Windows guest runs Defender, SearchIndexer, Windows Update
# and TiWorker, which saturate a 2-vCPU guest for minutes -- long enough to
# swallow a whole benchmark and make vmrun calls look wedged.
#
$log = 'C:\BlorgFS-Deploy\settle.log'
"waiting for guest idle (<= $IdlePercent% for $ConsecutiveSamples samples)" | Set-Content $log
$deadline = (Get-Date).AddSeconds($MaxWaitSeconds)
$hits = 0
while ((Get-Date) -lt $deadline) {
    $c = Get-Counter '\Processor(_Total)\% Processor Time' -ErrorAction SilentlyContinue
    $v = if ($c) { [math]::Round($c.CounterSamples[0].CookedValue,1) } else { 100 }
    Add-Content $log ("cpu " + $v + "%")
    if ($v -le $IdlePercent) { $hits++ } else { $hits = 0 }
    if ($hits -ge $ConsecutiveSamples) {
        Add-Content $log "settled"
        Get-Content $log | Select-Object -Last 6
        exit 0
    }
    Start-Sleep -Seconds 5
}
Add-Content $log "TIMED OUT still busy"
Get-Content $log | Select-Object -Last 6
exit 0
