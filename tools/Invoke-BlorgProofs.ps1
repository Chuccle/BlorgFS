<#
.SYNOPSIS
    Runs the CBMC proof harnesses against the real driver sources.

.DESCRIPTION
    Bounded model checking, not testing. Each harness in sandbox\verification
    includes a driver .c directly, so what is proved is a property of the
    shipping translation unit rather than of a transcription of it. Within the
    stated bound CBMC explores every input -- all byte values, all lengths --
    so a SUCCESS here is a proof over that whole space, not a sample of it.

    The bound is the honest limit. `-unwind` and the harness's buffer size
    cap what is covered; `--unwinding-assertions` is always on, so if a loop
    needed more iterations than the bound allows the run FAILS rather than
    silently proving a truncated program.

    Each proof must be falsifiable to be worth anything. The negative control
    for TlsDerReadTlv: delete `C_CAST(ULONG, End - p) < lenBytes` from its
    length-byte check in Tls.c and re-run -- "cursor never moves past the end"
    must go to FAILURE. Restore it afterwards and confirm SUCCESS returns.

.PARAMETER Harness
    Run only the harness whose name matches. Default: all of them.

.PARAMETER CbmcPath
    Path to cbmc.exe. Default: the cached copy under LOCALAPPDATA, then PATH.

.NOTES
    CBMC preprocesses with MSVC's cl, so this shells through vcvars64.bat.
    Without that it fails with "CL Preprocessing failed / PARSING ERROR".
#>
[CmdletBinding()]
param(
    [string]$Harness,
    [string]$CbmcPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Get-Cbmc {
    if ($CbmcPath) { return $CbmcPath }

    $cached = Join-Path $env:LOCALAPPDATA 'BlorgFS\cbmc\bin\cbmc.exe'
    if (Test-Path $cached) { return $cached }

    $onPath = Get-Command cbmc.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "cbmc.exe not found. Install from https://github.com/diffblue/cbmc/releases and extract to $env:LOCALAPPDATA\BlorgFS\cbmc, or pass -CbmcPath."
}

function Get-VcVars {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    )

    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }

    throw "vcvars64.bat not found; CBMC needs MSVC's cl to preprocess."
}

#
# One entry per proof. Bound is deliberately explicit and visible in the
# output: a reader should never have to guess what "SUCCESS" covered.
#
$proofs = @(
    @{
        Name     = 'TlsDerReadTlv'
        Source   = 'sandbox\verification\TlsDerHarness.c'
        Function = 'TlsDerReadTlvHarness'
        Unwind   = 12
        Covers   = 'DER tag/length reads over every input up to an 8-byte buffer'
    }
)

$cbmc = Get-Cbmc
$vcvars = Get-VcVars

$runner = Join-Path $env:TEMP 'blorgfs-cbmc.bat'
@"
@echo off
call "$vcvars" >nul 2>&1
"$cbmc" %*
"@ | Set-Content -Encoding ascii $runner

Write-Host "==> CBMC proofs ($([IO.Path]::GetFileName($cbmc)))" -ForegroundColor Cyan

$failed = 0
$ran = 0

foreach ($proof in $proofs) {
    if ($Harness -and $proof.Name -notlike "*$Harness*") { continue }

    $ran++

    $args = @(
        (Join-Path $repoRoot $proof.Source),
        '--function', $proof.Function,
        '--bounds-check',
        '--pointer-check',
        '--drop-unused-functions',
        '--unwind', $proof.Unwind,
        '--unwinding-assertions',
        '-I', $repoRoot
    )

    #
    # No 2>&1 here: in Windows PowerShell 5.1 redirecting a native
    # executable's stderr wraps every line in an ErrorRecord and trips
    # $ErrorActionPreference='Stop' even when CBMC exits 0. CBMC writes its
    # verdict to stdout, which is all this needs.
    #
    $output = & $runner @args
    $verified = ($output -match 'VERIFICATION SUCCESSFUL')

    #
    # Second pass: the same harness with an assert(0) compiled in. It MUST
    # fail. If it passes, the harness body is unreachable and the first
    # pass proved properties of code that never runs -- which is not a
    # weaker result, it is a meaningless one. A loop truncated by too small
    # an -unwind does exactly this and reports VERIFICATION SUCCESSFUL
    # while checking nothing.
    #
    $probeArgs = $args + @('-DBLORGFS_PROOF_REACHABILITY_PROBE')
    $probeOutput = & $runner @probeArgs
    $reachable = ($probeOutput -match 'VERIFICATION FAILED')

    if ($verified -and -not $reachable) {
        $failed++
        Write-Host ("  VACUOUS   {0}  --  harness body is unreachable; the proof checked nothing" -f $proof.Name) -ForegroundColor Red
        continue
    }

    if ($verified) {
        Write-Host ("  PROVED    {0}  --  {1} (unwind {2})" -f $proof.Name, $proof.Covers, $proof.Unwind) -ForegroundColor Green
    }
    else {
        $failed++
        Write-Host ("  FAILED    {0}" -f $proof.Name) -ForegroundColor Red
        $output | Where-Object { $_ -match 'FAILURE' } | Select-Object -First 10 | ForEach-Object { Write-Host "            $_" }
    }
}

Remove-Item $runner -ErrorAction SilentlyContinue

if ($ran -eq 0) { throw "No harness matched '$Harness'." }

if ($failed -gt 0) {
    Write-Host "`nFAILED ($failed of $ran)" -ForegroundColor Red
    exit 1
}

Write-Host "`nOK ($ran proved)" -ForegroundColor Green
exit 0
