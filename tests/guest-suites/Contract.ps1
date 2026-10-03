<#
.SYNOPSIS
    Guest suite: the server-rs <-> BlorgFS wire contract, checked live.

.DESCRIPTION
    Two halves, both against the real server and the real driver the guest
    run just installed:

      1. The contract corpus read through the mounted drive, so the driver's
         side of the behaviours is exercised end to end: bytes across the
         resident/streamed boundary, errors surfacing as the right Win32
         error, the listing matching what the host serves.

      2. The wire probe (third_party/schemas/conformance/Test-BlorgContract.ps1)
         against -BackendUrl, checking the server keeps the behaviours the
         driver relies on (contract.json B01-B05, B08).

    The corpus is Contract.corpus\ beside this script: exactly the tree the
    probe seeds, committed so a host that serves it needs no write access
    from the guest. The host serves it as Contract\ and lists it in
    -CorpusManifest. The guest can't change a file on the host, so the
    checks that need one (B08 after a change, B11) are not run here: the
    probe runs them with -SeedRoot in server-rs CI, and B11 is a known gap.
    B09 (case sensitivity) is reported as INFO, never failed.

    Exits with the number of failed checks. Windows PowerShell 5.1: no
    PowerShell 7 syntax, and non-ASCII strings are built from code points.

    The probe is looked for next to this script first (contract\, where a
    guest bundle carries it) and then in this repo's third_party/schemas
    checkout (running from a clone, e.g. on the dev VM).

.PARAMETER CorpusManifest
    The host's corpus manifest: files (path, size, sha256) and directories,
    relative to the served root. The Contract\ entries are what B: must
    show. Without it, Contract.corpus\ beside this script is the reference.
#>
[CmdletBinding()]
param(
    [string]$Drive = 'B:',
    [string]$BackendUrl = 'http://127.0.0.1:8080',
    [string]$CorpusManifest,
    [string]$ResultsDir = $env:TEMP
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$ServedName = 'Contract'

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

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return (-join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }))
    } finally {
        $stream.Dispose()
    }
}

# What B:\Contract has to hold: relative path -> size and hash.
$expected = @{}
$prefix = $ServedName + '\'
if ($CorpusManifest) {
    $manifest = Get-Content -LiteralPath $CorpusManifest -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($f in @($manifest.files)) {
        $rel = ([string]$f.path).Replace('/', '\')
        if ($rel.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $expected[$rel.Substring($prefix.Length)] = @{ Size = [long]$f.size; Sha = ([string]$f.sha256).ToLowerInvariant() }
        }
    }
}
if ($expected.Count -eq 0) {
    $reference = Join-Path $PSScriptRoot 'Contract.corpus'
    if ($CorpusManifest) { Write-Host "No $prefix entries in $CorpusManifest; using $reference" }
    $root = (Resolve-Path -LiteralPath $reference).Path
    foreach ($f in [System.IO.Directory]::GetFiles($root, '*', [System.IO.SearchOption]::AllDirectories)) {
        $expected[$f.Substring($root.Length + 1).Replace('/', '\')] = @{ Size = (New-Object System.IO.FileInfo $f).Length; Sha = (Get-Sha256 $f) }
    }
}

$mountDir = Join-Path $Drive $ServedName

# --------------------------------------------------------------------------
# 1. The corpus, through the driver
# --------------------------------------------------------------------------

Write-Host "--- through $mountDir ---"

# contract: B06 -- the listing on the drive is the listing the host serves
$mountRoot = $mountDir.TrimEnd('\')
$got = @([System.IO.Directory]::GetFiles($mountRoot, '*', [System.IO.SearchOption]::AllDirectories) |
    ForEach-Object { $_.Substring($mountRoot.Length + 1).Replace('/', '\') } | Sort-Object)
$want = @($expected.Keys | Sort-Object)
Check 'B06' 'drive listing matches the served corpus' ((@(Compare-Object $want $got -CaseSensitive)).Count -eq 0) "served: $($want -join ', ') / drive: $($got -join ', ')"

# contract: B01 B07 -- whole files read back byte-identical, either side of the resident limit
foreach ($name in @('small.bin', 'large.bin', ([string][char]0x00e9 + 't' + [char]0x00e9 + '.txt'))) {
    $mountFile = Join-Path $mountDir $name
    if (-not $expected.ContainsKey($name)) {
        Report 'B01' 'FAIL' "$name reads back" 'not in the served corpus'
        continue
    }
    try {
        $size = (Get-Item -LiteralPath $mountFile).Length
        Check 'B07' "$name size on the drive matches the host" ($size -eq $expected[$name].Size) "drive $size, host $($expected[$name].Size)"
        Check 'B01' "$name reads back byte-identical" ((Get-Sha256 $mountFile) -eq $expected[$name].Sha)
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

# --------------------------------------------------------------------------
# 2. The server, over the wire
# --------------------------------------------------------------------------

Write-Host "--- wire probe ($probe) against $server ---"
$probeArgs = @{ Server = $server; ProbeDir = $ServedName; ResultPath = (Join-Path $ResultsDir 'contract-wire.json') }
& $probe @probeArgs
$failures += $LASTEXITCODE

Write-Host ''
Write-Host "$failures failed contract check(s)"
exit $failures
