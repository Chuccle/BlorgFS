<#
.SYNOPSIS
    Triages a crash dump and emits a machine-readable verdict.

.DESCRIPTION
    The piece that closes the agentic loop for runtime failures. Until now
    a bugcheck in the test VM was a dead end: the machine reverted and
    nothing said why. This runs the debugger over the dump and reduces it
    to a handful of key=value lines plus an exit code, so a script, a CI
    step, or an agent can act on the result without reading WinDbg output.

    Deliberately driven through cdb.exe rather than an interactive
    debugger or an MCP bridge: cdb is scriptable, its output is stable
    enough to parse, and it needs nothing running to be present. The same
    invocation works whether it is a kernel dump from the VM or a usermode
    dump from a harness.

    Symbols resolve from a local cache by default. That is a deliberate
    choice: pointing at the Microsoft symbol server makes first-run triage
    take minutes and fail entirely on a machine without egress, which is
    exactly when you most want a verdict. Pass -SymbolServer to opt in.

    Exit codes: 0 no bugcheck found; 1 a bugcheck was found (driver on
    stack or not -- see DriverOnStack in the verdict); 3 triage itself
    failed and no verdict should be trusted.

.PARAMETER DumpPath
    The .dmp to analyse.

.PARAMETER SymbolServer
    Also consult the Microsoft public symbol server. Slower, but resolves
    OS frames the local cache has never seen.

.PARAMETER DriverName
    Module name to look for on the faulting stack, without extension.
    A bugcheck whose stack names this is ours; one that does not may still
    be ours by corruption, but is reported differently because the two
    want very different next steps.

.PARAMETER ReportPath
    Where to write the key=value verdict. Defaults to alongside the dump.

.EXAMPLE
    .\Get-CrashVerdict.ps1 -DumpPath C:\dumps\MEMORY.DMP
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DumpPath,
    [switch]$SymbolServer,
    [string]$DriverName = 'BlorgFS',
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'

#
# cdb lives in one of three places depending on how the machine got it:
# the local cache this script's companions populate, a full SDK install,
# or the Store WinDbg package. The package's own copy cannot be executed
# in place (WindowsApps denies it), which is why the cache exists.
#
function Get-Cdb {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'BlorgFS\dbg\cdb.exe'),
        'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe',
        'C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe'
    )

    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }

    #
    # Last resort: copy it out of the Store package. Executing from
    # WindowsApps is denied, but reading is not, so a one-time copy into
    # the cache turns an unusable install into a usable one.
    #
    $pkg = Get-AppxPackage -Name 'Microsoft.WinDbg' -ErrorAction SilentlyContinue

    if ($pkg) {
        $source = Join-Path $pkg.InstallLocation 'amd64'
        $cache = Join-Path $env:LOCALAPPDATA 'BlorgFS\dbg'

        if (Test-Path $source) {
            Write-Host "Populating debugger cache from the WinDbg package..." -ForegroundColor Cyan
            New-Item -ItemType Directory -Force $cache | Out-Null
            Copy-Item (Join-Path $source '*') $cache -Recurse -Force
            $cached = Join-Path $cache 'cdb.exe'
            if (Test-Path $cached) { return $cached }
        }
    }

    throw "cdb.exe not found. Install WinDbg (Store or SDK 'Debugging Tools for Windows')."
}

if (-not (Test-Path $DumpPath)) {
    throw "dump not found: $DumpPath"
}

if (-not $ReportPath) {
    $ReportPath = [System.IO.Path]::ChangeExtension($DumpPath, '.verdict.txt')
}

$cdb = Get-Cdb

$symbolCache = Join-Path $env:LOCALAPPDATA 'BlorgFS\symbols'
New-Item -ItemType Directory -Force $symbolCache | Out-Null

$env:_NT_SYMBOL_PATH = if ($SymbolServer) {
    "srv*$symbolCache*https://msdl.microsoft.com/download/symbols"
} else {
    "cache*$symbolCache"
}

#
# !analyze -v is the workhorse; .lastevent and k give the fault and the
# stack even when analyze cannot classify it. !verifier only means
# anything on a kernel dump with Verifier enabled, and prints a harmless
# error otherwise -- cheaper than detecting the dump type first.
#
$commands = '.lastevent; !analyze -v; k; !verifier 3; q'

Write-Host "Triaging $DumpPath" -ForegroundColor Cyan

#
# 2>&1 under $ErrorActionPreference='Stop' turns cdb's routine stderr
# chatter -- symbol-load lines above all -- into a terminating
# NativeCommandError mid-pipeline, so whether triage survives depends on
# whether the debugger happened to mutter. Invoke-BlorgProofs.ps1 dodges
# the same trap by not redirecting; here stderr carries part of the
# analysis, so the preference is relaxed for the duration of the call and
# restored afterwards.
#
$previousEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'

try {
    $output = & $cdb -z $DumpPath -c $commands 2>&1 | Out-String
    $cdbExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousEap
}

$lines = $output -split "`r?`n"

function Find-First {
    param([string]$Pattern, [int]$Group = 1)

    foreach ($line in $lines) {
        $m = [regex]::Match($line, $Pattern)
        if ($m.Success) { return $m.Groups[$Group].Value.Trim() }
    }

    return ''
}

$bugcheck = Find-First 'BugCheck\s+([0-9A-Fa-f]+)'
$analysisName = Find-First 'MODULE_NAME:\s*(\S+)'
$faultModule = Find-First 'FAULTING_MODULE:\s*\S+\s+(\S+)'
$failureBucket = Find-First 'FAILURE_BUCKET_ID:\s*(\S+)'
$exceptionCode = Find-First 'ExceptionCode:\s*([0-9A-Fa-fx]+)'
$lastEvent = Find-First 'Last event:\s*(.+)$'

#
# Whether our driver appears on the captured stack. The single most
# actionable bit: a bugcheck naming BlorgFS is ours to fix now; one that
# does not is either unrelated or a corruption whose culprit is
# elsewhere, and those want completely different next moves.
#
# Matched module-qualified (BlorgFS! / BlorgFS.sys / BlorgFS+) against the
# stack region only -- NOT as a bare substring of the whole output. A
# plain match reports a hit on any dump stored under a path containing
# the driver's name, which is every dump this project produces. A gate
# that says "your driver crashed" because of a directory name is worse
# than no gate.
#
$stackStart = -1
$stackEnd = $lines.Length

for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($stackStart -lt 0 -and $lines[$i] -match 'Call Site') { $stackStart = $i; continue }
    if ($stackStart -ge 0 -and $lines[$i] -match '^\s*$') { $stackEnd = $i; break }
}

$stackText = if ($stackStart -ge 0) {
    ($lines[$stackStart..([Math]::Min($stackEnd, $lines.Length - 1))]) -join "`n"
} else {
    ''
}

$modulePattern = '(?i)\b' + [regex]::Escape($DriverName) + '\s*(!|\.sys|\+0x)'

$driverOnStack = ($stackText -match $modulePattern)

#
# The analyze block names the module independently of the stack, so a
# frame the stack walk missed still counts.
#
if (-not $driverOnStack) {
    $driverOnStack = ($analysisName -match '(?i)^' + [regex]::Escape($DriverName)) -or
                     ($faultModule -match '(?i)^' + [regex]::Escape($DriverName))
}

#
# Only a positive statement from the debugger counts. Matching the string
# "!verifier" would match this script's own command echo, which is how a
# check ends up reporting Verifier active on a machine that never enabled
# it.
#
$verifierFlagged = ($output -match 'Verifier is enabled' -or
                    $output -match 'Driver Verifier is enabled' -or
                    $output -match 'VERIFIER_ENABLED')

$report = [ordered]@{
    Dump             = $DumpPath
    Bugcheck         = $bugcheck
    ExceptionCode    = $exceptionCode
    LastEvent        = $lastEvent
    ModuleName       = $analysisName
    FaultingModule   = $faultModule
    FailureBucket    = $failureBucket
    DriverOnStack    = [int]$driverOnStack
    VerifierPresent  = [int]$verifierFlagged
    SymbolServerUsed = [int]$SymbolServer.IsPresent
    CdbExit          = $cdbExit
}

$body = ($report.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join "`n"
Set-Content -Encoding utf8 -Path $ReportPath -Value $body

#
# The raw debugger output is kept next to the verdict. The parsed fields
# are for automation; when they are not enough, the thing a human or an
# agent actually needs is the full !analyze, and regenerating it means
# finding the dump again.
#
$logPath = [System.IO.Path]::ChangeExtension($DumpPath, '.analyze.txt')
Set-Content -Encoding utf8 -Path $logPath -Value $output

Write-Host ""
foreach ($kv in $report.GetEnumerator()) {
    Write-Host ("  {0,-17} {1}" -f $kv.Key, $kv.Value)
}

Write-Host ""
Write-Host "verdict: $ReportPath"
Write-Host "full analysis: $logPath"

if ($driverOnStack) {
    Write-Host "`n$DriverName IS on the faulting stack." -ForegroundColor Red
    exit 1
}

if ($bugcheck) {
    Write-Host "`nBugcheck $bugcheck, $DriverName not on the captured stack." -ForegroundColor Yellow
    exit 1
}

#
# Fail closed on broken triage, which is reached only when nothing above
# matched: an unreadable dump, a wrong dump type or a cdb that died yields
# empty fields, and calling that "no bugcheck" would let whoever consumes
# this verdict close the loop on a crashed machine believing none
# happened. Real debugger output always carries its banner and a prompt,
# so their absence means the analysis never actually ran.
#
$looksLikeDebuggerOutput = ($output -match '(?i)Microsoft \(R\) Windows Debugger') -or ($output -match '\b\d+:\d+>')

if (-not $looksLikeDebuggerOutput -or
    (((-not $bugcheck) -and (-not $exceptionCode) -and (-not $lastEvent)) -and $cdbExit -ne 0))
{
    Write-Host "`nTRIAGE FAILED: the debugger produced no parsable verdict (cdb exit $cdbExit) -- read the full log before concluding anything." -ForegroundColor Red
    exit 3
}

Write-Host "`nNo bugcheck signature found (usermode dump, or a clean break)." -ForegroundColor Green
exit 0
