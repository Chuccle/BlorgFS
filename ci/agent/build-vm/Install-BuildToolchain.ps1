<#
.SYNOPSIS
    Adds the BlorgFS build toolchain to a test-guest image, making it the
    build VM image. Runs once, inside the guest, from build-layer.sh.

.DESCRIPTION
    The golden test image (ci/guest/image) deliberately has no compiler:
    what is tested must be exactly what CI shipped. This layer sits on top
    of it as a separate qcow2 overlay, so the test image stays untouched and
    the build VM still boots the same Windows with the same SSH channel.

    Installs, all unattended:
      - Visual Studio Build Tools: the C++ x64 toolset, Spectre-mitigated
        libraries (BlorgFS.vcxproj sets Driver_SpectreMitigation) and
        MSBuild. The WDK itself is not installed here -- BlorgFS takes it
        from NuGet (src/packages.config), exactly as build.yml does.
      - CMake (flatcc and flatc are built from their submodules),
        nuget.exe, MinGit (the package script reads commits from git),
        and rustup with the stable MSVC toolchain for server-rs.

    Idempotent: a second run skips what is already there, so it can be
    re-run to repair a layer.

.PARAMETER VsBootstrapperUrl
    The Build Tools bootstrapper. Defaults to the current VS 2022 release;
    tools\Get-BlorgMSBuild.ps1 also finds VS 18 (2026) Build Tools.
#>
[CmdletBinding()]
param(
    [string]$VsBootstrapperUrl = 'https://aka.ms/vs/17/release/vs_buildtools.exe',
    [string]$CMakeVersion = '3.31.6',
    [string]$MinGitUrl = 'https://github.com/git-for-windows/git/releases/download/v2.51.0.windows.1/MinGit-2.51.0-64-bit.zip'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$tools = 'C:\blorg-tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
$downloads = Join-Path $env:TEMP 'blorg-toolchain'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

function Get-File([string]$Url, [string]$Name) {
    $dest = Join-Path $downloads $Name
    if (-not (Test-Path $dest)) {
        Write-Host "  downloading $Url"
        Invoke-WebRequest -Uri $Url -OutFile $dest -UseBasicParsing
    }
    return $dest
}

function Add-MachinePath([string]$Dir) {
    $path = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if (($path -split ';') -notcontains $Dir) {
        [Environment]::SetEnvironmentVariable('Path', "$path;$Dir", 'Machine')
    }
    if (($env:Path -split ';') -notcontains $Dir) { $env:Path = "$env:Path;$Dir" }
}

Write-Host '==> Visual Studio Build Tools'
$vsRoots = @('C:\Program Files\Microsoft Visual Studio\18\BuildTools', 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools', 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools')
if (-not ($vsRoots | Where-Object { Test-Path (Join-Path $_ 'MSBuild\Current\Bin\amd64\MSBuild.exe') })) {
    $vs = Get-File $VsBootstrapperUrl 'vs_buildtools.exe'
    # 3010 = success, reboot required. The layer build powers off afterwards
    # anyway, which is that reboot.
    $p = Start-Process -FilePath $vs -Wait -PassThru -ArgumentList @(
        '--quiet', '--wait', '--norestart', '--nocache',
        '--installPath', 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools',
        '--add', 'Microsoft.VisualStudio.Workload.VCTools',
        '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '--add', 'Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre',
        '--includeRecommended'
    )
    if ($p.ExitCode -notin 0, 3010) { throw "Build Tools installer exited $($p.ExitCode)" }
} else {
    Write-Host '  already installed'
}

Write-Host '==> CMake'
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and -not (Test-Path "$tools\cmake\bin\cmake.exe")) {
    $zip = Get-File "https://github.com/Kitware/CMake/releases/download/v$CMakeVersion/cmake-$CMakeVersion-windows-x86_64.zip" 'cmake.zip'
    Expand-Archive $zip -DestinationPath $downloads -Force
    Move-Item (Join-Path $downloads "cmake-$CMakeVersion-windows-x86_64") "$tools\cmake"
}
Add-MachinePath "$tools\cmake\bin"

Write-Host '==> nuget.exe'
if (-not (Test-Path "$tools\nuget.exe")) {
    Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile "$tools\nuget.exe" -UseBasicParsing
}
Add-MachinePath $tools

Write-Host '==> MinGit'
if (-not (Test-Path "$tools\git\cmd\git.exe")) {
    $zip = Get-File $MinGitUrl 'mingit.zip'
    Expand-Archive $zip -DestinationPath "$tools\git" -Force
}
Add-MachinePath "$tools\git\cmd"
# The source tree arrives as a tarball extracted by Administrator over SSH;
# git's ownership check would otherwise refuse to read it.
& "$tools\git\cmd\git.exe" config --system --add safe.directory '*'
& "$tools\git\cmd\git.exe" config --system core.autocrlf false

Write-Host '==> rustup (stable, MSVC)'
$cargo = Join-Path $env:USERPROFILE '.cargo\bin'
if (-not (Test-Path (Join-Path $cargo 'cargo.exe'))) {
    $rustup = Get-File 'https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe' 'rustup-init.exe'
    & $rustup -y --profile minimal --default-toolchain stable --default-host x86_64-pc-windows-msvc
    if ($LASTEXITCODE -ne 0) { throw "rustup-init exited $LASTEXITCODE" }
}
Add-MachinePath $cargo

Remove-Item -Recurse -Force $downloads -ErrorAction SilentlyContinue
Write-Host 'build toolchain ready'
exit 0
