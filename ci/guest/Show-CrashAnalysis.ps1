<#
.SYNOPSIS
    Analyses the minidumps a guest run brought back and prints what the
    driver did: bugcheck, faulting module, stack.

.DESCRIPTION
    Runs tools\Get-CrashVerdict.ps1 (cdb, !analyze -v) over every dump in
    <ResultsDir>\results\diag\dumps, with the driver's PDB from -SymbolDir
    and Microsoft's symbol server for the OS. Needs a Windows machine with
    the Debugging Tools for Windows (a GitHub windows-latest runner has
    them); guest-runtime.yml runs it whenever a guest run bugchecked.

    For each dump it prints the verdict, the bugcheck arguments, the
    faulting source line when symbols give one, and the stack. Under
    GitHub Actions the same text is also a "blorg guest-crash" check
    annotation, readable where logs and artifacts are not.

.EXAMPLE
    ci\guest\Show-CrashAnalysis.ps1 -ResultsDir guest-results -SymbolDir BlorgFS-Release-x64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ResultsDir,
    [string]$SymbolDir
)

$ErrorActionPreference = 'Stop'
$verdictTool = Join-Path $PSScriptRoot '..\..\tools\Get-CrashVerdict.ps1'
$dumps = @(Get-ChildItem (Join-Path $ResultsDir 'results\diag\dumps') -Filter '*.dmp' -ErrorAction SilentlyContinue)
if (-not $dumps) { Write-Host 'No dumps to analyse.'; exit 0 }

# The parts of !analyze -v worth reading first; the whole of it stays in
# <dump>.analyze.txt beside the dump.
$sections = 'BUGCHECK_CODE', 'BUGCHECK_P1', 'BUGCHECK_P2', 'BUGCHECK_P3', 'BUGCHECK_P4',
            'READ_ADDRESS', 'WRITE_ADDRESS', 'CURRENT_IRQL', 'FAULTING_IP', 'IMAGE_NAME',
            'SYMBOL_NAME', 'FAULTING_SOURCE_FILE', 'FAULTING_SOURCE_LINE_NUMBER',
            'FAILURE_BUCKET_ID'

# The build artifact keeps its x64\Release\ layout; use wherever the PDB is.
if ($SymbolDir) {
    $pdb = Get-ChildItem $SymbolDir -Recurse -Filter 'BlorgFS.pdb' | Select-Object -First 1
    if ($pdb) { $SymbolDir = $pdb.DirectoryName } else { Write-Warning "no BlorgFS.pdb under $SymbolDir" }
}

$triageFailed = $false
foreach ($dump in $dumps) {
    $params = @{ DumpPath = $dump.FullName; SymbolServer = $true }
    if ($SymbolDir) { $params.SymbolDir = $SymbolDir }
    & $verdictTool @params | Out-Null
    if ($LASTEXITCODE -eq 3) { $triageFailed = $true }

    $verdict = Get-Content ([IO.Path]::ChangeExtension($dump.FullName, '.verdict.txt'))
    $analysis = Get-Content ([IO.Path]::ChangeExtension($dump.FullName, '.analyze.txt'))

    $picked = foreach ($line in $analysis) {
        foreach ($s in $sections) { if ($line -match "^$s\s*:") { $line.Trim(); break } }
    }
    $stack = @()
    $inStack = $false
    foreach ($line in $analysis) {
        if ($line -match '^STACK_TEXT:') { $inStack = $true; continue }
        if ($inStack) { if ($line -match '^\s*$') { break }; $stack += $line.TrimEnd() }
    }

    $text = @("dump=$($dump.Name)") + $verdict + '' + $picked + '' + 'STACK_TEXT:' + ($stack | Select-Object -First 40)
    $text | ForEach-Object { Write-Host $_ }

    if ($env:GITHUB_ACTIONS) {
        $body = ($text -join "`n").Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "::error title=blorg guest-crash::$body"
    }
}

# A bugcheck is the guest run's verdict, already reported; this fails only
# when the analysis itself did not work.
if ($triageFailed) { exit 3 }
exit 0
