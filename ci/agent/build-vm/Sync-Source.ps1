<#
.SYNOPSIS
    Unpacks a source tarball from `blorg win sync` over the build VM's
    persistent checkout.

.DESCRIPTION
    Extracts over what is there rather than into a clean directory: build
    outputs (x64\, packages\, generated\, cargo's target\) live alongside
    the sources and are what make the next build incremental. Unchanged
    files keep their original timestamps, so MSBuild and cargo see them as
    unchanged.

    A file deleted on the host is not deleted here. That is harmless for
    MSBuild (it compiles what the .vcxproj lists) but run with -Clean after
    removing or renaming sources if in doubt.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$Dest,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

if ($Clean -and (Test-Path $Dest)) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# git writes its objects read-only, and Windows will not let tar replace a
# read-only file.
$gitDir = Join-Path $Dest '.git'
if (Test-Path $gitDir) {
    Get-ChildItem $gitDir -Recurse -File -Force |
        Where-Object { $_.IsReadOnly } |
        ForEach-Object { $_.IsReadOnly = $false }
}

# The inbox bsdtar (Windows 10 1803+ / Server 2019+), not a PowerShell
# cmdlet: it reads gzip and keeps the archive's timestamps.
& "$env:SystemRoot\System32\tar.exe" -xzf $Archive -C $Dest
if ($LASTEXITCODE -ne 0) { throw "tar exited $LASTEXITCODE" }
Remove-Item $Archive -Force
Write-Host "synced into $Dest"
