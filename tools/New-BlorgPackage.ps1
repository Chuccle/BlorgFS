<#
.SYNOPSIS
    Assembles the combined BlorgFS + server-rs package from build outputs.

.DESCRIPTION
    The package is what a test guest (or a person) installs: the test-signed
    driver as an installable unit, the server binary built from the
    server-rs commit this repo pins, and a manifest.json that says exactly
    which commits went in. Layout:

        manifest.json
        driver\BlorgFS.sys, BlorgFS.inf, BlorgFS.cat, BlorgFS.cer,
               Install-BlorgFS.ps1, Uninstall-BlorgFS.ps1
        server\server-rs.exe

    driver\ is self-contained: Install-BlorgFS.ps1 defaults to the INF and
    cert next to itself, so `driver\Install-BlorgFS.ps1` works as is.

    The INF is the *staged* copy under $(Platform)\$(Configuration)\BlorgFS,
    not src\BlorgFS.inf -- stampinf fills in DriverVer before Inf2Cat hashes
    it, so only the staged copy matches the catalog (see the same note in
    deploy\Deploy-ToVM.ps1).

    Runs tools\Test-BlorgPackagePins.ps1 first, so a package is never built
    from a driver and server that disagree on the schema. On Windows it also
    checks that BlorgFS.sys and BlorgFS.cat are signed by BlorgFS.cer, since
    an install against a mismatched cert fails in the guest with a far less
    useful error.

.PARAMETER ServerExe
    The built server-rs binary to include.

.PARAMETER OutDir
    Directory to assemble into. Created; must not already contain a package.

.PARAMETER Configuration
    Driver build configuration to package. Release by default.

.PARAMETER Release
    Use the bare VERSION as the package version (a tagged release). Without
    it the version carries both commits as build metadata, e.g.
    0.1.0+g417fc40.s2fa2955, so two CI packages are never confused.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ServerExe,
    [Parameter(Mandatory)][string]$OutDir,
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [ValidateSet("x64")][string]$Platform = "x64",
    [string]$ServerTarget = "x86_64-pc-windows-msvc",
    [switch]$Release,
    [string]$RepoRoot = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = "Stop"

$pins = & (Join-Path $PSScriptRoot "Test-BlorgPackagePins.ps1") -RepoRoot $RepoRoot

$baseVersion = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
if ($baseVersion -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must be MAJOR.MINOR.PATCH, got '$baseVersion'" }

$blorgCommit = (git -C $RepoRoot rev-parse HEAD).Trim()
$version = if ($Release) { $baseVersion } else {
    "$baseVersion+g$($blorgCommit.Substring(0, 7)).s$($pins.ServerRs.Substring(0, 7))"
}

# The server's own crate version, from the pinned checkout's Cargo.toml.
$cargoToml = Get-Content (Join-Path $RepoRoot "third_party/server-rs/Cargo.toml") -Raw
if ($cargoToml -notmatch '(?ms)^\[package\].*?^version\s*=\s*"([^"]+)"') { throw "No [package] version in server-rs Cargo.toml" }
$serverCrateVersion = $Matches[1]

$buildDir = Join-Path $RepoRoot "$Platform/$Configuration"
$stagedDir = Join-Path $buildDir "BlorgFS"

# The staged package directory is the installable unit; the .cer only ever
# lands in the output directory itself.
$driverFiles = [ordered]@{
    "BlorgFS.sys" = @((Join-Path $stagedDir "BlorgFS.sys"), (Join-Path $buildDir "BlorgFS.sys"))
    "BlorgFS.inf" = @((Join-Path $stagedDir "BlorgFS.inf"))
    "BlorgFS.cat" = @((Join-Path $stagedDir "BlorgFS.cat"))
    "BlorgFS.cer" = @((Join-Path $buildDir "BlorgFS.cer"))
}

if (Test-Path $OutDir) {
    if (Get-ChildItem $OutDir -Force | Select-Object -First 1) { throw "OutDir '$OutDir' is not empty" }
} else {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
$driverOut = New-Item -ItemType Directory -Path (Join-Path $OutDir "driver")
$serverOut = New-Item -ItemType Directory -Path (Join-Path $OutDir "server")

foreach ($name in $driverFiles.Keys) {
    $src = $driverFiles[$name] | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $src) { throw "$name not found -- looked in: $($driverFiles[$name] -join ', '). Was the driver built ($Configuration|$Platform)?" }
    Copy-Item $src (Join-Path $driverOut $name)
}
foreach ($script in @("Install-BlorgFS.ps1", "Uninstall-BlorgFS.ps1")) {
    Copy-Item (Join-Path $RepoRoot "deploy/$script") (Join-Path $driverOut $script)
}

if (-not (Test-Path $ServerExe)) { throw "Server binary not found at '$ServerExe'" }
Copy-Item $ServerExe (Join-Path $serverOut (Split-Path $ServerExe -Leaf))

$signer = $null
if ($IsWindows -or $env:OS -eq "Windows_NT") {
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $driverOut "BlorgFS.cer"))
    foreach ($signed in @("BlorgFS.sys", "BlorgFS.cat")) {
        $sig = Get-AuthenticodeSignature (Join-Path $driverOut $signed)
        # Status is expected to be UnknownError/NotTrusted on a machine that
        # does not trust the test cert; what matters is who signed it.
        if (-not $sig.SignerCertificate) { throw "$signed is not signed (status: $($sig.Status))" }
        if ($sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
            throw "$signed is signed by $($sig.SignerCertificate.Thumbprint), not by the packaged BlorgFS.cer ($($cert.Thumbprint))"
        }
    }
    $signer = [ordered]@{ subject = $cert.Subject; thumbprint = $cert.Thumbprint }
} else {
    Write-Warning "Not on Windows: skipping the driver signature check."
}

$files = [ordered]@{}
Get-ChildItem $OutDir -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = [System.IO.Path]::GetRelativePath($OutDir, $_.FullName).Replace('\', '/')
    $files[$rel] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}

$manifest = [ordered]@{
    schema     = 1
    name       = "blorg"
    version    = $version
    components = [ordered]@{
        blorgfs   = [ordered]@{ commit = $blorgCommit; configuration = $Configuration; platform = $Platform; signing = "test"; signer = $signer }
        server_rs = [ordered]@{ commit = $pins.ServerRs; version = $serverCrateVersion; target = $ServerTarget }
        schemas   = [ordered]@{ commit = $pins.DriverSchemas }
    }
    files      = $files
}
if ($env:GITHUB_ACTIONS) {
    $manifest.ci = [ordered]@{
        run = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
        ref = $env:GITHUB_REF
    }
}

$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutDir "manifest.json") -Encoding utf8NoBOM
Write-Host "Packaged blorg $version into $OutDir"
return $version
