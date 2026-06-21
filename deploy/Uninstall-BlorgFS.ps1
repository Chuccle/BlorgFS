#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Stops and removes the BlorgFS kernel driver service (run inside the VM guest).

.DESCRIPTION
    Reverses Install-BlorgFS.ps1's service registration: sc stop/delete plus
    explicit removal of the copied System32\drivers\BlorgFS.sys. BlorgFS.inf
    deliberately has no [DefaultUninstall] section (see its header comment --
    the platform only derives uninstall for driver-store installs via
    DiUninstallDriver/pnputil, and InstallHinfSection with a section name the
    INF doesn't contain is a silent no-op), so the INF plays no part here.
    Leaves test-signing mode and the trusted cert alone by default (harmless to
    leave enabled between iterations, and other test-signed drivers on the same
    dev VM may depend on it) -- pass -DisableTestSigning / -RemoveCert to also
    undo those.

.PARAMETER RemoveCert
    Also removes the BlorgFS cert from TrustedPublisher/Root.

.PARAMETER DisableTestSigning
    Also disables test-signing mode (bcdedit /set testsigning off). Requires a reboot
    to take effect, same as enabling it did.
#>
[CmdletBinding()]
param(
    [string]$ServiceName = "BlorgFS",
    [string]$CertPath = (Join-Path $PSScriptRoot "BlorgFS.cer"),
    [switch]$RemoveCert,
    [switch]$DisableTestSigning
)

$ErrorActionPreference = "Continue"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

Write-Step "Stopping service '$ServiceName'"
sc.exe stop $ServiceName | Out-Null
Start-Sleep -Seconds 2

Write-Step "Deleting service '$ServiceName'"
sc.exe delete $ServiceName | Out-Null

$driverFile = Join-Path $env:windir "System32\drivers\$ServiceName.sys"
if (Test-Path $driverFile) {
    Write-Step "Removing $driverFile"
    # sc delete only marks the service for deletion if a stop is still pending;
    # the file itself can lag a moment behind the stop, hence the retry rather
    # than a single attempt.
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        try {
            Remove-Item -Path $driverFile -Force -ErrorAction Stop
            break
        } catch {
            Start-Sleep -Seconds 1
        }
    }
    if (Test-Path $driverFile) {
        Write-Host "WARNING: could not remove $driverFile (driver may still be loaded -- reboot and re-run, or delete it manually)." -ForegroundColor Yellow
    }
}

if ($RemoveCert -and (Test-Path $CertPath)) {
    Write-Step "Removing driver certificate from trust stores"
    $cert = Get-PfxCertificate -FilePath $CertPath
    Remove-Item -Path "Cert:\LocalMachine\TrustedPublisher\$($cert.Thumbprint)" -Force -ErrorAction SilentlyContinue
    Remove-Item -Path "Cert:\LocalMachine\Root\$($cert.Thumbprint)" -Force -ErrorAction SilentlyContinue
}

if ($DisableTestSigning) {
    Write-Step "Disabling test-signing mode (reboot required to take effect)"
    bcdedit /set testsigning off | Out-Null
}

Write-Host "BlorgFS uninstalled." -ForegroundColor Green
