#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installs and starts the BlorgFS kernel driver on this machine (run inside the VM guest).

.DESCRIPTION
    BlorgFS is a legacy WDM filesystem driver: it creates its own device objects in
    DriverEntry rather than through a bus-enumerated FDO, so BlorgFS.inf installs it via
    the primitive (non-PnP) DefaultInstall path -- there's no hardware ID to match, so
    this never goes through Device Manager. Steps:
      1. Enables Windows test-signing mode (bcdedit) if it isn't already on.
      2. Imports the driver's self-signed cert into TrustedPublisher and Root so the
         test-signed .sys is trusted.
      3. Runs BlorgFS.inf's DefaultInstall section (rundll32 setupapi.dll,InstallHinfSection),
         which copies BlorgFS.sys to System32\drivers and registers the service, including
         the INF's seeded Parameters\RemoteHost default (see BlorgFS.inf).
      4. Optionally overrides TlsEnabled / TlsPin / RemotePort / RemoteHost under the
         service's Parameters key (see Driver.c's ReadBlorgfsRegistryConfig) on top of
         whatever the INF seeded.
      5. Starts the service and polls for the B: drive to come up (the driver
         self-mounts at load; there is no separate "mount" step).

    Safe to re-run: an existing BlorgFS service is stopped and deleted first.

.PARAMETER InfPath
    Path to BlorgFS.inf. Defaults to .\BlorgFS.inf next to this script. BlorgFS.sys (and
    BlorgFS.cat, if present) must sit alongside it -- InstallHinfSection looks for the
    files named in BlorgFS.inf's [SourceDisksFiles] in the INF's own directory.

.PARAMETER CertPath
    Path to the signing cert (BlorgFS.cer) produced alongside BlorgFS.sys by the build.
    Defaults to .\BlorgFS.cer next to this script.

.PARAMETER RemoteHost
    If passed, overrides Parameters\RemoteHost (REG_SZ) -- the backend hostname/IP the
    driver's HTTP client connects to and sends as the Host header. BlorgFS.inf already
    seeds this with its compiled-in default (blorgfs.blorg.lan); only pass this to point
    at something else (e.g. a different dev backend).

.PARAMETER TlsEnabled
    If passed, writes TlsEnabled (REG_DWORD) to the service Parameters key. Leave this
    off until there's a cert/pin to configure -- see TlsPinHex.

.PARAMETER RemotePort
    If passed, writes RemotePort (REG_SZ) to the service Parameters key, overriding the
    driver's TlsEnabled-based default (8080 plaintext / 443 TLS).

.PARAMETER TlsPinHex
    If passed, a 64-hex-char (32-byte) SHA-256 SPKI pin, written as TlsPin (REG_BINARY).
    Required for -TlsEnabled to actually complete a handshake -- without it every TLS
    connection fails closed at the Certificate message (see Driver.h's TlsEnabled comment).

.EXAMPLE
    .\Install-BlorgFS.ps1
    Installs against the plaintext default (port 8080, no TLS, RemoteHost from the INF).

.EXAMPLE
    .\Install-BlorgFS.ps1 -RemoteHost "192.168.1.50"
    Installs against a different backend without touching TLS at all.
#>
[CmdletBinding()]
param(
    [string]$InfPath = (Join-Path $PSScriptRoot "BlorgFS.inf"),
    [string]$CertPath = (Join-Path $PSScriptRoot "BlorgFS.cer"),
    [string]$ServiceName = "BlorgFS",
    [string]$RemoteHost,
    [switch]$TlsEnabled,
    [string]$RemotePort,
    [string]$TlsPinHex,
    [char]$DriveLetter = 'B',
    [int]$MountTimeoutSeconds = 20
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Warn2($msg) { Write-Host "WARNING: $msg" -ForegroundColor Yellow }

if ($RemoteHost -and $RemoteHost.Length -gt 127) {
    # Driver-side limit: BLORGFS_REG_HOST_MAX_CHARS (Driver.h) is 128 WCHARs
    # including the NUL. An oversized value wouldn't error in the driver -- it
    # silently falls back to the compiled-in default host, which is far more
    # confusing than failing here.
    throw "RemoteHost must be at most 127 characters, got $($RemoteHost.Length)."
}

if (-not (Test-Path $InfPath)) {
    throw "BlorgFS.inf not found at '$InfPath'. Build the driver first or pass -InfPath."
}
$InfPath = (Resolve-Path $InfPath).Path

Write-Step "Checking test-signing mode"
$bcdOutput = bcdedit /enum '{current}' | Out-String
$testSigningOn = $bcdOutput -match 'testsigning\s+Yes'
if (-not $testSigningOn) {
    Write-Step "Enabling test-signing (bcdedit /set testsigning on)"
    bcdedit /set testsigning on | Out-Null
    Write-Warn2 "Test-signing was just enabled. Windows requires a REBOOT before an unsigned/test-signed driver will load."
    Write-Warn2 "Reboot the VM, then re-run this script to finish the install."
    exit 2
}
Write-Host "Test-signing already enabled."

if (Test-Path $CertPath) {
    $CertPath = (Resolve-Path $CertPath).Path
    Write-Step "Trusting driver certificate ($CertPath)"
    certutil -addstore -f "TrustedPublisher" $CertPath | Out-Null
    certutil -addstore -f "Root" $CertPath | Out-Null
} else {
    Write-Warn2 "No cert found at '$CertPath' -- skipping cert trust step. If BlorgFS.sys isn't already trusted, the service will fail to start with STATUS_IMAGE_CERT_REVOKED / access denied."
}

Write-Step "(Re)installing '$ServiceName' from $InfPath"
$existing = sc.exe query $ServiceName 2>&1 | Out-String
if ($existing -notmatch "1060") {
    # 1060 = service does not exist; anything else means it's there and needs clearing first
    sc.exe stop $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    sc.exe delete $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 1
}

# rundll32's own exit code says nothing about whether InstallHinfSection actually
# succeeded (it returns 0 even on failure) -- confirm via sc query afterward instead,
# and point at setupapi's own log for the real error if that comes back empty.
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 $InfPath
Start-Sleep -Seconds 2

$installed = sc.exe query $ServiceName 2>&1 | Out-String
if ($installed -match "1060") {
    throw "INF install did not register the '$ServiceName' service. Check %windir%\inf\setupapi.dev.log for the setupapi-side error."
}

if ($RemoteHost -or $TlsEnabled -or $RemotePort -or $TlsPinHex) {
    Write-Step "Applying registry Parameters overrides"
    $paramsKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName\Parameters"
    New-Item -Path $paramsKey -Force | Out-Null

    if ($RemoteHost) {
        New-ItemProperty -Path $paramsKey -Name "RemoteHost" -PropertyType String -Value $RemoteHost -Force | Out-Null
    }
    if ($TlsEnabled) {
        New-ItemProperty -Path $paramsKey -Name "TlsEnabled" -PropertyType DWord -Value 1 -Force | Out-Null
    }
    if ($RemotePort) {
        New-ItemProperty -Path $paramsKey -Name "RemotePort" -PropertyType String -Value $RemotePort -Force | Out-Null
    }
    if ($TlsPinHex) {
        if ($TlsPinHex.Length -ne 64) {
            throw "TlsPinHex must be 64 hex characters (32-byte SHA-256 SPKI pin), got $($TlsPinHex.Length)."
        }
        $bytes = [byte[]]($TlsPinHex -split '(?<=\G.{2})(?!$)' | ForEach-Object { [Convert]::ToByte($_, 16) })
        New-ItemProperty -Path $paramsKey -Name "TlsPin" -PropertyType Binary -Value $bytes -Force | Out-Null
    }
}

Write-Step "Starting service '$ServiceName'"
$startResult = sc.exe start $ServiceName
if ($LASTEXITCODE -ne 0) {
    throw "sc start failed:`n$startResult`nCheck DbgView / the kernel debugger for the DriverEntry failure reason."
}

Write-Step "Waiting for $($DriveLetter): to mount"
$deadline = (Get-Date).AddSeconds($MountTimeoutSeconds)
$mounted = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path "${DriveLetter}:\") {
        $mounted = $true
        break
    }
    Start-Sleep -Milliseconds 500
}

if ($mounted) {
    Write-Host "BlorgFS is up: $($DriveLetter): is mounted." -ForegroundColor Green
    # Explicit so callers reading $LASTEXITCODE (Deploy-ToVM.ps1's status-file
    # wrapper) get a deliberate 0 rather than whatever the last native command
    # happened to leave behind.
    exit 0
} else {
    Write-Warn2 "$($DriveLetter): did not appear within $MountTimeoutSeconds s. The service reports running, but the volume-arrival/mount step may have failed -- check the debugger."
    exit 1
}
