<#
.SYNOPSIS
    Builds the pinned server-rs, the one way the package gets its server.

.DESCRIPTION
    The single recipe for the server half of the package: build.yml's server
    job runs this, and so should anything else that builds a package (a
    local build, an agent, a build VM). A second copy of these steps is how
    two "identical" packages end up built differently.

    Two steps:
      1. flatc, built from server-rs's own buildtools/flatbuffers submodule.
         server-rs's build.rs shells out to flatc and the generated code has
         to match its flatbuffers crate version, so a flatc from anywhere
         else is not good enough. Skipped when FlatcDir already holds one
         (CI caches it there, keyed on the submodule commit).
      2. cargo build --release --locked, default features -- exactly the
         dependency set server-rs's own CI tested.

    Copies the binary (and its .pdb on Windows) into OutDir and returns the
    binary's path.

.PARAMETER ServerRoot
    The server-rs checkout, with submodules. Defaults to third_party/server-rs.

.PARAMETER FlatcDir
    Where flatc lives, or is built into if missing.

.PARAMETER OutDir
    Where to copy the built binary.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) "third_party/server-rs"),
    [string]$FlatcDir = (Join-Path ([System.IO.Path]::GetTempPath()) "blorg-flatc"),
    [Parameter(Mandatory)][string]$OutDir
)

$ErrorActionPreference = "Stop"

$onWindows = $IsWindows -or $env:OS -eq "Windows_NT"
$exe = if ($onWindows) { ".exe" } else { "" }

$flatbuffers = Join-Path $ServerRoot "buildtools/flatbuffers"
if (-not (Test-Path (Join-Path $flatbuffers "CMakeLists.txt"))) {
    throw "server-rs's flatbuffers submodule is missing at '$flatbuffers' -- run 'git submodule update --init --recursive third_party/server-rs'"
}

$flatc = Join-Path $FlatcDir "flatc$exe"
if (Test-Path $flatc) {
    Write-Host "==> Using flatc at $flatc"
} else {
    Write-Host "==> Building flatc from $flatbuffers"
    $build = Join-Path ([System.IO.Path]::GetTempPath()) "blorg-flatc-build"
    # Out-Host on every native call: their stdout would otherwise join this
    # script's return value, which callers take to be the binary's path.
    cmake -S $flatbuffers -B $build -DCMAKE_BUILD_TYPE=Release `
        -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
    cmake --build $build --config Release --target flatc | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "flatc build failed with exit code $LASTEXITCODE" }

    # Multi-config generators (Visual Studio) put it under Release\, single-
    # config ones (Makefiles) at the top of the build tree.
    $built = @((Join-Path $build "Release/flatc$exe"), (Join-Path $build "flatc$exe")) |
             Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $built) { throw "flatc build reported success but no flatc$exe under $build" }
    New-Item -ItemType Directory -Force -Path $FlatcDir | Out-Null
    Copy-Item $built $flatc
}

# build.rs finds flatc on PATH; put ours first so a stray system flatc of a
# different version cannot be the one that runs.
$env:PATH = "$FlatcDir$([System.IO.Path]::PathSeparator)$env:PATH"

Write-Host "==> cargo build --release --locked in $ServerRoot"
Push-Location $ServerRoot
try {
    cargo build --release --locked | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$release = Join-Path $ServerRoot "target/release"
$binary = Join-Path $release "server-rs$exe"
if (-not (Test-Path $binary)) { throw "cargo reported success but $binary is missing" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $binary $OutDir
$pdb = Join-Path $release "server_rs.pdb"
if (Test-Path $pdb) { Copy-Item $pdb $OutDir }

return (Join-Path $OutDir (Split-Path $binary -Leaf))
