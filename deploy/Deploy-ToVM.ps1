<#
.SYNOPSIS
    Builds BlorgFS and deploys it into a VMware Workstation Windows 11 guest.

.DESCRIPTION
    Runs on the HOST. Drives the guest entirely through `vmrun` (VMware Workstation's
    guest-automation CLI) + VMware Tools, so no shared folder or manual copy step is
    needed -- just a VM with VMware Tools installed and a guest account to log in as.

    Pipeline:
      1. (optional) Revert the VM to a known-good snapshot -- recommended, since a
         buggy driver load can bugcheck the guest.
      2. Power the VM on (if not already) and wait for VMware Tools to come up.
      3. (optional) Build BlorgFS.sln locally with msbuild.
      4. Copy BlorgFS.sys / BlorgFS.inf / BlorgFS.cat / BlorgFS.cer / Install-BlorgFS.ps1
         into the guest, plus PerfHarness.exe when it has been built.
      5. Run Install-BlorgFS.ps1 inside the guest as admin.
      6. If the guest reports test-signing was just turned on (exit code 2 -- see
         Install-BlorgFS.ps1), reset the VM, wait for it to come back up, and retry
         the install once automatically.

.PARAMETER VmxPath
    Path to the guest's .vmx file.

.PARAMETER GuestUser / GuestPassword
    Credentials for a guest account with admin rights. VMware Tools must be running
    in the guest for any of this to work.

.PARAMETER VmPassword
    The .vmx's own config-encryption password (separate from GuestPassword), needed
    when the VM itself is encrypted at the VMware Workstation level -- every guest
    operation against such a VM fails with "A password is required for this
    operation" without it, even though "vmrun list" alone does not need it. Leave
    unset for an unencrypted VM.

.PARAMETER SnapshotName
    If given, revert to this snapshot before deploying -- the recommended way to get
    a clean, un-bugchecked VM for each iteration.

.PARAMETER SkipBuild
    Skip the local msbuild step and deploy whatever is already in the output directory.

.PARAMETER Configuration / Platform
    Build configuration to deploy. Platform must be x64 (only platform BlorgFS builds
    test binaries for; ARM64 output, if built, isn't covered by this script).

.PARAMETER RemoteHost
    Overrides Parameters\RemoteHost -- the backend hostname/IP the driver's HTTP client
    connects to. Passed straight through to Install-BlorgFS.ps1; leave unset to keep the
    INF-seeded default (blorgfs.blorg.lan).

.EXAMPLE
    .\Deploy-ToVM.ps1 -VmxPath "D:\VMs\Win11\Win11.vmx" -GuestUser dev -GuestPassword "hunter2" -SnapshotName clean
    vmrun itself only accepts the guest password as plaintext on the command line -- there's
    no way around that with vmrun's guest-automation API -- so treat this VM's guest account
    as a disposable dev credential, not a real secret.

.EXAMPLE
    .\Deploy-ToVM.ps1 -VmxPath "D:\VMs\Win11\Win11.vmx" -GuestUser dev -GuestPassword $pw -RemoteHost "192.168.1.50"
    Deploys against a different backend than the driver's compiled-in default
    (blorgfs.blorg.lan), without touching TLS.
#>
[CmdletBinding()]
param(
    [string]$VmxPath,
    [string]$GuestUser,
    [string]$GuestPassword,
    [string]$VmPassword,
    [string]$SnapshotName,
    [switch]$SkipBuild,
    [ValidateSet("Debug", "Release")][string]$Configuration = "Debug",
    [ValidateSet("x64")][string]$Platform = "x64",
    [string]$VmrunPath,
    [string]$GuestDeployDir = "C:\BlorgFS-Deploy",
    [string]$RemoteHost,
    [switch]$TlsEnabled,
    [string]$RemotePort,
    [string]$TlsPinHex,
    [int]$ToolsWaitTimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

#
# Anything not passed explicitly falls back to blorgfs.env (gitignored;
# see blorgfs.env.example). That is what lets a fresh session start
# deploying without being handed the VM path, the guest account and the
# .vmx password first. An explicit argument always wins.
#
$BlorgEnv = & (Join-Path $PSScriptRoot "Get-BlorgEnv.ps1")

function Get-Setting($Current, $Key) {
    if ($Current) { return $Current }
    if ($BlorgEnv.ContainsKey($Key)) { return $BlorgEnv[$Key] }
    return $Current
}

$VmxPath       = Get-Setting $VmxPath       "VmxPath"
$GuestUser     = Get-Setting $GuestUser     "GuestUser"
$GuestPassword = Get-Setting $GuestPassword "GuestPassword"
$VmPassword    = Get-Setting $VmPassword    "VmPassword"
$SnapshotName  = Get-Setting $SnapshotName  "SnapshotName"
$RemoteHost    = Get-Setting $RemoteHost    "RemoteHost"
$RemotePort    = Get-Setting $RemotePort    "RemotePort"

foreach ($required in @("VmxPath", "GuestUser", "GuestPassword")) {
    if (-not (Get-Variable -Name $required -ValueOnly)) {
        throw "$required not supplied and not found in blorgfs.env. Copy deploylorgfs.env.example to deploylorgfs.env and fill it in, or pass -$required."
    }
}

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

if (-not $VmrunPath) {
    $candidates = @(
        "C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe",
        "C:\Program Files\VMware\VMware Workstation\vmrun.exe"
    )
    $VmrunPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $VmrunPath) {
        $cmd = Get-Command vmrun.exe -ErrorAction SilentlyContinue
        if ($cmd) { $VmrunPath = $cmd.Source }
    }
    if (-not $VmrunPath) {
        throw "Couldn't find vmrun.exe. Pass -VmrunPath explicitly."
    }
}

# vmrun's own CLI order is fixed: [-T ws] [-gu user -gp pass] COMMAND <vmx> [command args...].
# Both helpers take a fully-formed COMMAND-and-onward array so callers control that order
# explicitly, rather than relying on PowerShell's positional/remaining-argument binding
# (which mis-parses vmrun flags like "-activeWindow" as PowerShell parameter names).
function Invoke-Vmrun {
    param([Parameter(Mandatory = $true)][string[]]$CommandArgs)
    $vpArgs = if ($VmPassword) { @("-vp", $VmPassword) } else { @() }
    $output = & $VmrunPath -T ws @vpArgs @CommandArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "vmrun $($CommandArgs -join ' ') failed:`n$output"
    }
    return $output
}

function Invoke-VmrunGuest {
    param([Parameter(Mandatory = $true)][string[]]$CommandArgs)
    Invoke-Vmrun -CommandArgs (@("-gu", $GuestUser, "-gp", $GuestPassword) + $CommandArgs)
}

if ($SnapshotName) {
    Write-Step "Reverting to snapshot '$SnapshotName'"
    Invoke-Vmrun -CommandArgs @("revertToSnapshot", $VmxPath, $SnapshotName)
}

$runningVms = & $VmrunPath -T ws list
if ($runningVms -notmatch [Regex]::Escape($VmxPath)) {
    Write-Step "Starting VM"
    Invoke-Vmrun -CommandArgs @("start", $VmxPath, "nogui")
}

function Wait-ForTools {
    Write-Step "Waiting for VMware Tools in guest"
    $deadline = (Get-Date).AddSeconds($ToolsWaitTimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            Invoke-VmrunGuest -CommandArgs @("listProcessesInGuest", $VmxPath) | Out-Null
            return
        } catch {
            Start-Sleep -Seconds 3
        }
    }
    throw "VMware Tools didn't come up within $ToolsWaitTimeoutSeconds s."
}
Wait-ForTools

if (-not $SkipBuild) {
    Write-Step "Building BlorgFS.sln ($Configuration|$Platform)"
    #
    # Not Get-Command: PATH may hold the 32-bit MSBuild, or none at all
    # outside a Developer PowerShell. The WDK NuGet picks its PREfast and
    # ApiValidator tool directory from the MSBuild process architecture, so
    # the 32-bit one silently produces a different build than the check
    # runner does. Invoke-BlorgChecks.ps1 already knows how to find the right
    # one; use that rather than keeping a second, weaker answer here.
    #
    $msbuild = & (Join-Path $RepoRoot "tools\Get-BlorgMSBuild.ps1")
    & $msbuild (Join-Path $RepoRoot "BlorgFS.sln") /p:Configuration=$Configuration /p:Platform=$Platform /m
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
}

$outDir = Join-Path $RepoRoot "$Platform\$Configuration"
$sysPath = Join-Path $outDir "BlorgFS.sys"
$certPath = Join-Path $outDir "BlorgFS.cer"
# Inf2Cat writes the catalog next to the staged INF it validated ($stagedInfPath
# below), not into $outDir directly -- same directory, not the same path shape
# as $sysPath/$certPath.
$catPath = Join-Path $outDir "BlorgFS\BlorgFS.cat"
if (-not (Test-Path $sysPath)) { throw "BlorgFS.sys not found at '$sysPath'." }

function Copy-ToGuest {
    param([string]$LocalPath, [string]$GuestPath)
    Invoke-VmrunGuest -CommandArgs @("copyFileFromHostToGuest", $VmxPath, $LocalPath, $GuestPath)
}

Write-Step "Creating deploy directory in guest ($GuestDeployDir)"
try {
    Invoke-VmrunGuest -CommandArgs @("createDirectoryInGuest", $VmxPath, $GuestDeployDir) | Out-Null
} catch {
    # Already exists -- vmrun has no "mkdir -p" idempotence, so tolerate the one failure mode.
}

Write-Step "Copying driver files into guest"
# BlorgFS.inf's [SourceDisksFiles] expects BlorgFS.sys (and BlorgFS.cat, if present, for
# BlorgFS.inf's CatalogFile= to resolve) next to the INF, so all three land in the same
# guest directory as Install-BlorgFS.ps1.
#
# The INF must be the *staged* copy under $outDir\BlorgFS, not the repo-root
# BlorgFS.inf: MSBuild's stampinf step fills in DriverVer on the staged copy
# before Inf2Cat hashes it, so the root INF (DriverVer left blank) has
# different bytes -- and therefore a different hash -- than what's actually
# in the catalog. Deploying the root INF makes pnputil/InstallHinfSection
# reject the package with "hash for the file is not present in the
# specified catalog file", even though the .sys and .cat themselves are
# perfectly consistent with each other.
$stagedInfPath = Join-Path $outDir "BlorgFS\BlorgFS.inf"
if (-not (Test-Path $stagedInfPath)) { throw "Staged INF not found at '$stagedInfPath' -- did the build run stampinf/Inf2Cat?" }
Copy-ToGuest $sysPath "$GuestDeployDir\BlorgFS.sys"
Copy-ToGuest $stagedInfPath "$GuestDeployDir\BlorgFS.inf"
if (Test-Path $certPath) {
    Copy-ToGuest $certPath "$GuestDeployDir\BlorgFS.cer"
}
if (Test-Path $catPath) {
    Copy-ToGuest $catPath "$GuestDeployDir\BlorgFS.cat"
}
Copy-ToGuest (Join-Path $PSScriptRoot "Install-BlorgFS.ps1") "$GuestDeployDir\Install-BlorgFS.ps1"

# PerfHarness speaks a versioned statistics contract with the driver, and the
# driver checks the version so a stale harness fails loudly rather than
# misreading a struct whose tail has moved. Staging it with the driver is what
# keeps that check from being the thing that finds out: deploying the two
# separately has twice meant measuring with a harness older than the .sys
# beside it. Skipped rather than fatal when it has not been built -- a deploy
# for debugging has no reason to need it.
#
# Release specifically, and from the project's own output directory: a
# solution build and a per-project build write to different places, so the
# copy at the repo root can be arbitrarily older than this one.
$harnessPath = Join-Path (Split-Path -Parent $PSScriptRoot) "tests\PerfHarness\x64\Release\PerfHarness.exe"
if (Test-Path $harnessPath) {
    Copy-ToGuest $harnessPath "$GuestDeployDir\PerfHarness.exe"
} else {
    Write-Host "  (PerfHarness.exe not built; skipping)" -ForegroundColor DarkGray
}

function Invoke-InstallInGuest {
    $extraArgs = @()
    if ($RemoteHost) { $extraArgs += "-RemoteHost", $RemoteHost }
    if ($TlsEnabled) { $extraArgs += "-TlsEnabled" }
    if ($RemotePort) { $extraArgs += "-RemotePort", $RemotePort }
    if ($TlsPinHex) { $extraArgs += "-TlsPinHex", $TlsPinHex }

    # runProgramInGuest doesn't surface the guest process's exit code, so have the
    # guest script write it to a status file we copy back and inspect ourselves.
    # Two failure modes need explicit handling or a stale status file from a
    # previous deploy gets read back as this run's result: (a) the install script
    # throwing (which would abort -Command before the Out-File ever ran), handled
    # by the try/catch below always writing *something*; (b) the status file from
    # the last run surviving in the guest, handled by deleting it up front.
    try {
        Invoke-VmrunGuest -CommandArgs @("deleteFileInGuest", $VmxPath, "$GuestDeployDir\install-status.txt") | Out-Null
    } catch {
        # No stale status file to remove
    }

    $psCommand = "`$code = 1; try { & '$GuestDeployDir\Install-BlorgFS.ps1' $($extraArgs -join ' '); `$code = `$LASTEXITCODE } catch { Write-Host `$_ }; `$code | Out-File -Encoding ascii '$GuestDeployDir\install-status.txt'"

    Write-Step "Running Install-BlorgFS.ps1 in guest"
    Invoke-VmrunGuest -CommandArgs @(
        "runProgramInGuest", $VmxPath, "-activeWindow",
        "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
        "-ExecutionPolicy", "Bypass", "-NoProfile", "-Command", $psCommand
    ) | Out-Null

    $localStatusFile = Join-Path $env:TEMP "blorgfs-install-status.txt"
    Invoke-VmrunGuest -CommandArgs @("copyFileFromGuestToHost", $VmxPath, "$GuestDeployDir\install-status.txt", $localStatusFile) | Out-Null
    return [int](Get-Content $localStatusFile | Select-Object -First 1)
}

$installStatus = Invoke-InstallInGuest

if ($installStatus -eq 2) {
    Write-Step "Test-signing was just enabled in the guest -- resetting VM to apply it"
    Invoke-Vmrun -CommandArgs @("reset", $VmxPath)
    Wait-ForTools
    Write-Step "Retrying install after reboot"
    $installStatus = Invoke-InstallInGuest
}

if ($installStatus -eq 0) {
    Write-Host "Deployment succeeded." -ForegroundColor Green
} else {
    throw "Install-BlorgFS.ps1 in the guest exited with status $installStatus. Check the guest for details."
}
