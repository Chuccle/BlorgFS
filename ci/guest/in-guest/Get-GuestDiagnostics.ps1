<#
.SYNOPSIS
    Collects what explains a failed or crashed run, and says whether the
    guest bugchecked during it.

.DESCRIPTION
    Runs inside the guest after Invoke-GuestTests.ps1, or after the guest
    came back from a crash mid-run, and writes into <ResultsDir>\diag:

      crash.json        bugchecked yes/no since Prepare-Guest.ps1 ran, the
                        bugcheck events, and the dumps found
      dumps\            minidumps, and the kernel MEMORY.DMP (zipped) when
                        -IncludeKernelDump is given
      setupapi.*.log    the driver install's own log
      system.evtx       the System event log, plus system-errors.txt
      state.txt         sc query, bcdedit, verifier settings, driver store

    A dump is evidence for tools\Get-CrashVerdict.ps1, which needs cdb and
    symbols and so runs on a Windows host, not here.

.PARAMETER ResultsDir
    The run's results directory (holds prepared-at.txt from Prepare-Guest.ps1).

.PARAMETER IncludeKernelDump
    Also zip C:\Windows\MEMORY.DMP into the results. Large; off by default.
#>
[CmdletBinding()]
param(
    [string]$ResultsDir = 'C:\blorgfs-ci\results',
    [switch]$IncludeKernelDump
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$diag = Join-Path $ResultsDir 'diag'
New-Item -ItemType Directory -Force -Path (Join-Path $diag 'dumps') | Out-Null

$since = (Get-Date).AddHours(-6)
$preparedAt = Join-Path $ResultsDir 'prepared-at.txt'
if (Test-Path $preparedAt) { $since = [datetime]::Parse((Get-Content $preparedAt -Raw).Trim()).ToLocalTime() }

foreach ($log in 'setupapi.dev.log', 'setupapi.app.log') {
    Copy-Item (Join-Path 'C:\Windows\INF' $log) $diag -ErrorAction SilentlyContinue
}

wevtutil.exe epl System (Join-Path $diag 'system.evtx') /ow:true
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1, 2; StartTime = $since } -ErrorAction SilentlyContinue |
    Select-Object -First 200 TimeCreated, Id, ProviderName, Message |
    Format-List | Out-File -Encoding ascii -Width 200 (Join-Path $diag 'system-errors.txt')

# 1001 from WER-SystemErrorReporting is "the computer has rebooted from a
# bugcheck", with the code and parameters; 41 from Kernel-Power is the
# unclean-restart marker that accompanies it.
$bugEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object {
        ($_.ProviderName -eq 'Microsoft-Windows-WER-SystemErrorReporting' -and $_.Id -eq 1001) -or
        ($_.ProviderName -eq 'Microsoft-Windows-Kernel-Power' -and $_.Id -eq 41)
    } |
    ForEach-Object { [ordered]@{ time = $_.TimeCreated.ToUniversalTime().ToString('o'); provider = $_.ProviderName; id = $_.Id; message = $_.Message } })

$dumps = @()
foreach ($d in Get-ChildItem 'C:\Windows\Minidump' -Filter '*.dmp' -ErrorAction SilentlyContinue) {
    Copy-Item $d.FullName (Join-Path $diag 'dumps')
    $dumps += [ordered]@{ name = $d.Name; size = $d.Length; kind = 'mini' }
}
$kernel = Get-Item 'C:\Windows\MEMORY.DMP' -ErrorAction SilentlyContinue
if ($kernel) {
    $entry = [ordered]@{ name = 'MEMORY.DMP'; size = $kernel.Length; kind = 'kernel'; collected = $false }
    if ($IncludeKernelDump) {
        Compress-Archive -Path $kernel.FullName -DestinationPath (Join-Path $diag 'dumps\MEMORY.zip') -Force
        $entry.collected = $true
    }
    $dumps += $entry
}

& {
    '### sc query BlorgFS'; sc.exe query BlorgFS
    '### sc qc BlorgFS'; sc.exe qc BlorgFS
    '### Parameters'; Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\BlorgFS\Parameters' -ErrorAction SilentlyContinue | Format-List
    '### bcdedit'; bcdedit /enum '{current}'
    '### verifier /querysettings'; verifier /querysettings
    '### pnputil /enum-drivers (BlorgFS)'; pnputil /enum-drivers | Select-String -Context 0, 7 'blorgfs'
    '### volumes'; Get-PSDrive -PSProvider FileSystem | Format-Table -AutoSize
    '### server-rs processes'; Get-Process server-rs -ErrorAction SilentlyContinue | Format-Table -AutoSize
} 2>&1 | Out-File -Encoding ascii -Width 200 (Join-Path $diag 'state.txt')

$crash = [ordered]@{
    since      = $since.ToUniversalTime().ToString('o')
    bugchecked = ($dumps.Count -gt 0) -or [bool]($bugEvents | Where-Object { $_.id -eq 1001 })
    events     = $bugEvents
    dumps      = $dumps
}
$json = $crash | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText((Join-Path $diag 'crash.json'), $json, (New-Object System.Text.UTF8Encoding $false))
Write-Host $json
if ($crash.bugchecked) { Write-Host 'BUGCHECK DETECTED during this run' -ForegroundColor Red }
exit 0
