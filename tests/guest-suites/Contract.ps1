<#
.SYNOPSIS
    Guest suite: the server-rs <-> BlorgFS wire contract, checked live.

.DESCRIPTION
    Two halves, both against the real server and the real driver the guest
    run just installed:

      1. The wire probe (third_party/schemas/conformance/Test-BlorgContract.ps1)
         against -BackendUrl, seeded into -CorpusDir so it can also change a
         file underneath the server. Checks the server keeps the behaviours
         the driver relies on (contract.json B01-B05, B08).

      2. The same probe tree read back through the mounted drive, so the
         driver's side of those behaviours is exercised end to end: bytes
         across the resident/streamed boundary, errors surfacing as the right
         Win32 error, listings matching the host. The two known gaps (B09
         case sensitivity, B11 reads across a shrunk EOF) are run and
         reported as INFO, never failed.

    Exits with the number of failed checks. Windows PowerShell 5.1: no
    PowerShell 7 syntax, and non-ASCII strings are built from code points.

    The probe is looked for next to this script first (contract\, where a
    guest bundle carries it) and then in this repo's third_party/schemas
    checkout (running from a clone, e.g. on the dev VM).
#>
[CmdletBinding()]
param(
    [string]$Drive = 'B:',
    [string]$BackendUrl = 'http://127.0.0.1:8080',
    [Parameter(Mandatory)][string]$CorpusDir,
    [string]$ResultsDir = $env:TEMP
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$candidates = @(
    (Join-Path $PSScriptRoot 'contract\Test-BlorgContract.ps1'),
    (Join-Path $PSScriptRoot '..\..\third_party\schemas\conformance\Test-BlorgContract.ps1')
)
$probe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $probe) {
    Write-Host "FAIL contract probe not found -- looked in: $($candidates -join ', ')"
    exit 1
}

$server = ([Uri]$BackendUrl).Authority
$failures = 0

function Report([string]$Behaviour, [string]$Outcome, [string]$Check, [string]$Detail = '') {
    $line = '{0,-4} {1} {2}' -f $Outcome, $Behaviour, $Check
    if ($Detail) { $line += " -- $Detail" }
    Write-Host $line
    if ($Outcome -eq 'FAIL') { $script:failures++ }
}

function Check([string]$Behaviour, [string]$Check, [bool]$Condition, [string]$Detail = '') {
    if ($Condition) { Report $Behaviour 'PASS' $Check } else { Report $Behaviour 'FAIL' $Check $Detail }
}

# --------------------------------------------------------------------------
# 1. The server, over the wire
# --------------------------------------------------------------------------

Write-Host "--- wire probe ($probe) against $server ---"
& $probe -Server $server -SeedRoot $CorpusDir -ResultPath (Join-Path $ResultsDir 'contract-wire.json')
$wireFailures = $LASTEXITCODE
if ($wireFailures -ne 0) { $failures += $wireFailures }

# --------------------------------------------------------------------------
# 2. The same tree, through the driver
# --------------------------------------------------------------------------

Write-Host "--- through $Drive ---"
$hostDir = Join-Path $CorpusDir 'contract-probe'
$mountDir = "$Drive\contract-probe"

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return [BitConverter]::ToString($sha.ComputeHash($stream))
    } finally {
        $stream.Dispose()
    }
}

# contract: B06 -- the listing on the drive is the listing on the host
$want = @(Get-ChildItem -LiteralPath $hostDir | ForEach-Object { $_.Name } | Sort-Object)
$got = @(Get-ChildItem -LiteralPath $mountDir | ForEach-Object { $_.Name } | Sort-Object)
Check 'B06' 'drive listing matches the host' ((@(Compare-Object $want $got -CaseSensitive)).Count -eq 0) "host: $($want -join ', ') / drive: $($got -join ', ')"

# contract: B01 B07 -- whole files read back byte-identical, either side of the resident limit
foreach ($name in @('small.bin', 'large.bin', ([string][char]0x00e9 + 't' + [char]0x00e9 + '.txt'))) {
    $hostFile = Join-Path $hostDir $name
    $mountFile = Join-Path $mountDir $name
    try {
        $sizeOk = (Get-Item -LiteralPath $mountFile).Length -eq (Get-Item -LiteralPath $hostFile).Length
        Check 'B07' "$name size on the drive matches the host" $sizeOk
        Check 'B01' "$name reads back byte-identical" ((Get-Sha256 $mountFile) -eq (Get-Sha256 $hostFile))
    } catch {
        Report 'B01' 'FAIL' "$name reads back" $_.Exception.Message
    }
}

# contract: B04 -- a missing file is "not found" to the caller, not a generic failure
try {
    [System.IO.File]::OpenRead((Join-Path $mountDir 'missing.bin')).Dispose()
    Report 'B04' 'FAIL' 'opening a missing file fails' 'it opened'
} catch [System.IO.FileNotFoundException] {
    Report 'B04' 'PASS' 'opening a missing file fails with FileNotFound'
} catch {
    Report 'B04' 'FAIL' 'opening a missing file fails with FileNotFound' $_.Exception.GetType().FullName
}

# contract: B09 (gap) -- different casing than on disk
try {
    $text = [System.IO.File]::ReadAllText((Join-Path $mountDir 'mixedcase.txt'))
    Report 'B09' 'INFO' 'open with different casing' "read back '$text'"
} catch {
    Report 'B09' 'INFO' 'open with different casing' $_.Exception.Message
}

# contract: B11 (gap) -- the driver holds a size; the host file then shrinks
$shrinkHost = Join-Path $hostDir 'shrink-through-drive.bin'
$shrinkMount = Join-Path $mountDir 'shrink-through-drive.bin'
[System.IO.File]::WriteAllBytes($shrinkHost, (New-Object byte[] 4096))
Start-Sleep -Seconds 3
try {
    $stream = [System.IO.File]::OpenRead($shrinkMount)
    try {
        $before = $stream.Length
        [System.IO.File]::WriteAllBytes($shrinkHost, (New-Object byte[] 100))
        Start-Sleep -Seconds 3
        $buffer = New-Object byte[] 4096
        $read = $stream.Read($buffer, 0, $buffer.Length)
        Report 'B11' 'INFO' 'read across a shrunk EOF' "size $before when opened; read returned $read bytes"
    } finally {
        $stream.Dispose()
    }
} catch {
    Report 'B11' 'INFO' 'read across a shrunk EOF' $_.Exception.Message
}

Write-Host ''
Write-Host "$failures failed contract check(s)"
exit $failures
