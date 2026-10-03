<#
.SYNOPSIS
    Builds, checks and packages BlorgFS + server-rs on a Windows machine
    that has the toolchain: the agent build VM, or a hosted Windows runner.

.DESCRIPTION
    The Windows half of `blorg win build` and of agent-remote.yml. It does
    what build.yml does, in the order an iterating agent wants, and writes
    everything to -OutDir so the caller can copy one directory back:

        build-summary.txt   one line per step: ok / FAIL / skip
        logs\<step>.log     full output of each step
        package\            the blorg package, when packaging ran

    Steps:
      1. flatcc (built once from third_party\flatcc) and the generated
         reader headers, as in build.yml.
      2. nuget restore: the WDK and SDK from src\packages.config.
      3. tools\Invoke-BlorgChecks.ps1 -Tier <Tier>: the regression gate,
         unchanged. This is the step that decides pass/fail.
      4. Packaging, when tools\New-BlorgPackage.ps1 and the server-rs
         submodule exist: the driver project built into the solution
         layout the package script reads, server-rs built with cargo
         (MSVC, as shipped) against flatc from server-rs's own pinned
         flatbuffers, then New-BlorgPackage.ps1.

    Nothing here is specific to running in a VM: on a runner it runs
    against the checkout directly (agent-remote.yml's PR check does that).

    This is the iteration build, not the release recipe: the package that
    ships, and that guest-runtime.yml certifies, comes from build.yml only.
    The server-rs step mirrors build.yml's server job; when that job's
    steps move into a shared script, call it here instead.

    Exit code: 0 when every step that ran passed, 1 otherwise.

.PARAMETER Tier
    Passed to Invoke-BlorgChecks.ps1. Build is the quickest compile +
    PREfast gate; Fast (default) adds the usermode suites.

.PARAMETER NoPackage
    Stop after the checks.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    [ValidateSet('Build', 'Fast', 'Proof', 'All')][string]$Tier = 'Fast',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory)][string]$OutDir,
    [switch]$NoPackage
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Source = (Resolve-Path $Source).Path
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
$logs = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'logs')
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'build-summary.txt'
$script:failed = $false
$toolsCache = if ($env:BLORG_TOOLS_CACHE) { $env:BLORG_TOOLS_CACHE } else { 'C:\blorg-tools' }
New-Item -ItemType Directory -Force -Path $toolsCache | Out-Null

function Add-Summary([string]$Line) {
    Add-Content -Path $summary -Value $Line
    Write-Host $Line
}

# Runs one step, logging its output; a failure is recorded, not thrown, so
# later independent steps still report.
function Step([string]$Name, [scriptblock]$Body) {
    $log = Join-Path $logs "$Name.log"
    Write-Host "==> $Name"
    $start = Get-Date
    try {
        & $Body *>&1 | Tee-Object -FilePath $log | Out-Host
        Add-Summary ("ok    {0} ({1:N0}s)" -f $Name, ((Get-Date) - $start).TotalSeconds)
        return $true
    } catch {
        "$_" | Add-Content -Path $log
        Add-Summary ("FAIL  {0} -- {1} (logs\{0}.log)" -f $Name, $_.Exception.Message)
        $script:failed = $true
        return $false
    }
}

# Windows PowerShell 5.1 turns each stderr line of a native command into an
# error record, and under 'Stop' the first one -- cargo's progress output,
# cmake's notices -- aborts the step. Only the exit code decides here.
function Invoke-Native([string]$File, [string[]]$Arguments) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $File @Arguments 2>&1 | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
    if ($rc -ne 0) { throw "$(Split-Path $File -Leaf) exited $rc" }
}

# Where the source came from, for whoever reads the results.
$ErrorActionPreference = 'Continue'
$commit = (git -C $Source rev-parse HEAD 2>$null)
$dirty = [bool](git -C $Source status --porcelain --untracked-files=no 2>$null)
$ErrorActionPreference = 'Stop'
Add-Summary "source $Source commit $commit$(if ($dirty) { ' (with uncommitted changes)' })"
Add-Summary "tier $Tier, $Configuration"

Push-Location $Source
try {
    $ok = Step 'codegen' {
        $flatcc = 'third_party\flatcc\bin\Release\flatcc.exe'
        if (-not (Test-Path $flatcc)) {
            $build = 'third_party\flatcc\build\MSVC'
            New-Item -ItemType Directory -Force -Path $build | Out-Null
            Invoke-Native cmake @('-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-S', 'third_party\flatcc', '-B', $build)
            Invoke-Native cmake @('--build', $build, '--config', 'Release')
        }
        New-Item -ItemType Directory -Force -Path generated | Out-Null
        Invoke-Native $flatcc @('-rv', '--common_reader', '-o', 'generated', 'third_party\schemas\metadata_flatbuffer.fbs')
    }

    if ($ok) {
        $ok = Step 'nuget-restore' {
            $nuget = (Get-Command nuget -ErrorAction SilentlyContinue).Source
            if (-not $nuget) { $nuget = Join-Path $toolsCache 'nuget.exe' }
            Invoke-Native $nuget @('restore', 'BlorgFS.sln', '-PackagesDirectory', '.\packages', '-NonInteractive')
        }
    }

    if ($ok) {
        $ok = Step "checks-$Tier" {
            Invoke-Native powershell @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools\Invoke-BlorgChecks.ps1',
                '-Tier', $Tier, '-Configuration', $Configuration)
        }
    }

    $canPackage = (Test-Path 'tools\New-BlorgPackage.ps1') -and (Test-Path 'third_party\server-rs\Cargo.toml')
    if ($NoPackage) {
        Add-Summary 'skip  package (-NoPackage)'
    } elseif (-not $canPackage) {
        Add-Summary 'skip  package (this tree has no tools\New-BlorgPackage.ps1 or third_party\server-rs yet)'
    } elseif (-not $ok) {
        Add-Summary 'skip  package (an earlier step failed)'
    } else {
        $ok = Step 'driver-for-package' {
            # Into the solution layout ($(Platform)\$(Configuration)\BlorgFS\,
            # stamped INF + catalog), which is what New-BlorgPackage.ps1 reads.
            $msbuild = & (Join-Path $Source 'tools\Get-BlorgMSBuild.ps1')
            Invoke-Native $msbuild @('src\BlorgFS.vcxproj', "/p:Configuration=$Configuration", '/p:Platform=x64',
                "/p:SolutionDir=$Source\", '/v:minimal', '/nologo')
        }
        if ($ok) {
            $ok = Step 'server-rs' {
                $srv = Join-Path $Source 'third_party\server-rs'
                # flatc from server-rs's own pinned flatbuffers, cached by
                # commit: the generated Rust must match the crate version.
                $rev = (git -C $srv rev-parse HEAD:buildtools/flatbuffers).Trim()
                $flatcDir = Join-Path $toolsCache "flatc-$($rev.Substring(0, 12))"
                if (-not (Test-Path "$flatcDir\flatc.exe")) {
                    $build = Join-Path $env:TEMP 'blorg-flatc-build'
                    Invoke-Native cmake @('-S', "$srv\buildtools\flatbuffers", '-B', $build,
                        '-DFLATBUFFERS_BUILD_TESTS=OFF', '-DFLATBUFFERS_BUILD_FLATLIB=OFF', '-DFLATBUFFERS_BUILD_FLATHASH=OFF')
                    Invoke-Native cmake @('--build', $build, '--config', 'Release', '--target', 'flatc')
                    New-Item -ItemType Directory -Force -Path $flatcDir | Out-Null
                    Copy-Item "$build\Release\flatc.exe" $flatcDir
                }
                $env:Path = "$flatcDir;$env:Path"
                Push-Location $srv
                try { Invoke-Native cargo @('build', '--release', '--locked') } finally { Pop-Location }
            }
        }
        if ($ok) {
            $ok = Step 'package' {
                & (Join-Path $Source 'tools\New-BlorgPackage.ps1') `
                    -ServerExe (Join-Path $Source 'third_party\server-rs\target\release\server-rs.exe') `
                    -OutDir (Join-Path $OutDir 'package') -Configuration $Configuration
            }
        }
    }
} finally {
    Pop-Location
}

if ($script:failed) { Add-Summary 'verdict=fail'; exit 1 }
Add-Summary 'verdict=pass'
exit 0
