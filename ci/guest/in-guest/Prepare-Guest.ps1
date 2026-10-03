<#
.SYNOPSIS
    Puts the guest into the state a test run needs, and says whether that
    takes a reboot.

.DESCRIPTION
    Runs inside the guest before Invoke-GuestTests.ps1. Idempotent, and
    deliberately independent of how the guest was built or which hypervisor
    hosts it: the golden image already has test signing on, but a guest
    from anywhere else (the VMware dev VM, a cloud VM) gets the same
    treatment.

      - Test signing on (a reboot to apply, if it was off).
      - Driver Verifier's standard checks on BlorgFS.sys, or off with
        -NoVerifier. Correctness runs want it on; benchmark runs must not
        have it (see "Measuring performance" in AGENTS.md). Either change
        needs a reboot.
      - Old crash dumps cleared, and the time recorded, so the diagnostics
        afterwards only ever report crashes from this run.

    Exit codes: 0 ready now; 3010 ready after a reboot; 1 failed.

.PARAMETER ResultsDir
    Where this run's results go. Recreated empty.

.PARAMETER NoVerifier
    Leave Driver Verifier off for BlorgFS.sys (and turn it off if set).
#>
[CmdletBinding()]
param(
    [string]$ResultsDir = 'C:\blorgfs-ci\results',
    [switch]$NoVerifier
)

$ErrorActionPreference = 'Stop'
$reboot = $false

try {
    if (Test-Path $ResultsDir) { Remove-Item -Recurse -Force $ResultsDir }
    New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

    $bcd = bcdedit /enum '{current}' | Out-String
    if ($bcd -notmatch 'testsigning\s+Yes') {
        Write-Host '==> Enabling test signing'
        bcdedit /set '{current}' testsigning on | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bcdedit testsigning failed ($LASTEXITCODE) -- is Secure Boot on? It must be off for a test-signed driver." }
        $reboot = $true
    }

    $verifierOn = (verifier /querysettings | Out-String) -match '(?im)^\s*BlorgFS\.sys\s*$'
    if (-not $NoVerifier -and -not $verifierOn) {
        Write-Host '==> Enabling Driver Verifier (standard) for BlorgFS.sys'
        verifier /standard /driver BlorgFS.sys | Out-Null
        $reboot = $true
    } elseif ($NoVerifier -and $verifierOn) {
        Write-Host '==> Clearing Driver Verifier'
        verifier /reset | Out-Null
        $reboot = $true
    }

    Write-Host '==> Clearing old crash dumps'
    Remove-Item -Force 'C:\Windows\MEMORY.DMP' -ErrorAction SilentlyContinue
    Remove-Item -Force 'C:\Windows\Minidump\*' -ErrorAction SilentlyContinue

    (Get-Date).ToUniversalTime().ToString('o') | Set-Content -Encoding ascii (Join-Path $ResultsDir 'prepared-at.txt')
    [ordered]@{ verifier = (-not $NoVerifier); rebootRequired = $reboot } |
        ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $ResultsDir 'prepare.json')
} catch {
    Write-Host "PREPARE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

if ($reboot) {
    Write-Host 'REBOOT REQUIRED'
    exit 3010
}
Write-Host 'Guest ready.'
exit 0
