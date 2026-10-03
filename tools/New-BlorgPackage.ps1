<#
.SYNOPSIS
    Assembles the combined BlorgFS + server-rs package from build outputs.

.DESCRIPTION
    The package is what a test guest (or a person) installs: the test-signed
    driver as an installable unit, the server built from the server-rs
    commit this repo pins, and a manifest.json that says exactly which
    commits went in. Layout:

        manifest.json
        driver\BlorgFS.sys, BlorgFS.inf, BlorgFS.cat, BlorgFS.cer,
               Install-BlorgFS.ps1, Uninstall-BlorgFS.ps1
        server\linux-x64\server-rs        static (musl); the real deployment
        server\windows-x64\server-rs.exe  for running beside the driver

    The server is shipped for both platforms because the backend normally
    runs on Linux while the driver runs on Windows; the Windows build is the
    fallback for a single-machine setup. Artifact zips drop the Unix
    executable bit, so `chmod +x` the Linux binary after unpacking.

    driver\ is self-contained: Install-BlorgFS.ps1 defaults to the INF and
    cert next to itself, so `driver\Install-BlorgFS.ps1` works as is.

    The INF is the *staged* copy under $(Platform)\$(Configuration)\BlorgFS,
    not src\BlorgFS.inf -- stampinf fills in DriverVer before Inf2Cat hashes
    it, so only the staged copy matches the catalog (see the same note in
    deploy\Deploy-ToVM.ps1).

    Runs tools\Test-BlorgPackagePins.ps1 first, so a package is never built
    from a driver and server that disagree on the schema. Runs anywhere
    PowerShell does; CI runs it on Linux.

.PARAMETER ServerDir
    Built servers, one subdirectory per platform (linux-x64, windows-x64),
    each as tools\Build-BlorgServer.ps1 left it. Both are required.

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
    [Parameter(Mandatory)][string]$ServerDir,
    [Parameter(Mandatory)][string]$OutDir,
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [ValidateSet("x64")][string]$Platform = "x64",
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
    "BlorgFS.sys" = @($stagedDir, $buildDir)
    "BlorgFS.inf" = @($stagedDir)
    "BlorgFS.cat" = @($stagedDir)
    "BlorgFS.cer" = @($buildDir)
}

# Case-insensitive lookup: Inf2Cat writes the catalog as blorgfs.cat, which
# Windows never noticed, but CI assembles the package on Linux. The packaged
# copy is always written under the canonical name.
function Find-BuildFile {
    param([string]$Name, [string[]]$Dirs)
    foreach ($dir in $Dirs) {
        if (-not (Test-Path $dir)) { continue }
        $hit = Get-ChildItem $dir -File | Where-Object { $_.Name -ieq $Name } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
}

if (Test-Path $OutDir) {
    if (Get-ChildItem $OutDir -Force | Select-Object -First 1) { throw "OutDir '$OutDir' is not empty" }
} else {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
$driverOut = New-Item -ItemType Directory -Path (Join-Path $OutDir "driver")
$serverOut = New-Item -ItemType Directory -Path (Join-Path $OutDir "server")

foreach ($name in $driverFiles.Keys) {
    $src = Find-BuildFile $name $driverFiles[$name]
    if (-not $src) { throw "$name not found (any case) in: $($driverFiles[$name] -join ', '). Was the driver built ($Configuration|$Platform)?" }
    Copy-Item $src (Join-Path $driverOut $name)
}
foreach ($script in @("Install-BlorgFS.ps1", "Uninstall-BlorgFS.ps1")) {
    Copy-Item (Join-Path $RepoRoot "deploy/$script") (Join-Path $driverOut $script)
}

# Platform directory -> the Rust target and binary name build.yml builds.
$serverPlatforms = [ordered]@{
    "linux-x64"   = @{ target = "x86_64-unknown-linux-musl"; binary = "server-rs" }
    "windows-x64" = @{ target = "x86_64-pc-windows-gnu"; binary = "server-rs.exe" }
}
$serverTargets = [ordered]@{}
foreach ($serverPlatform in $serverPlatforms.Keys) {
    $binary = Join-Path $ServerDir "$serverPlatform/$($serverPlatforms[$serverPlatform].binary)"
    if (-not (Test-Path $binary)) { throw "Server binary for $serverPlatform not found at '$binary'" }
    $dest = New-Item -ItemType Directory -Path (Join-Path $serverOut $serverPlatform)
    Copy-Item $binary $dest
    $serverTargets[$serverPlatform] = $serverPlatforms[$serverPlatform].target
}

# Who signed it is checked on the Windows build machine, where Authenticode
# can be read (tools\Test-BlorgDriverSignature.ps1); here it is only
# recorded, which works anywhere.
$cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $driverOut "BlorgFS.cer"))
$signer = [ordered]@{ subject = $cert.Subject; thumbprint = $cert.Thumbprint }

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
        server_rs = [ordered]@{ commit = $pins.ServerRs; version = $serverCrateVersion; targets = $serverTargets }
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
