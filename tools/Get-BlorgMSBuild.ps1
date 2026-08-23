<#
.SYNOPSIS
    Returns the path to a 64-bit MSBuild.exe.

.DESCRIPTION
    The 64-bit one specifically. The WDK NuGet picks its PREfast and
    ApiValidator tool directory from the MSBuild process's own architecture,
    and the 32-bit MSBuild selects an x86 directory where those tools are
    missing or broken -- so building with it silently loses the static
    analysis, without failing. Whatever `where msbuild` resolves to is not
    good enough, and outside a Developer PowerShell it resolves to nothing
    at all.

    Kept as its own script because two callers need the same answer:
    Invoke-BlorgChecks.ps1 and deploy/Deploy-ToVM.ps1. They had different
    answers, and the deploy script's -- trust PATH -- was both weaker and a
    hard failure on a plain shell.

    Also honours a MSBUILD_PATH environment variable, so a machine with an
    edition not listed here can point at its own without editing this file.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

if ($env:MSBUILD_PATH -and (Test-Path $env:MSBUILD_PATH)) {
    return $env:MSBUILD_PATH
}

$editions = @('Enterprise', 'Professional', 'Community', 'BuildTools')
$versions = @('18', '2022')

foreach ($version in $versions) {
    foreach ($edition in $editions) {
        $candidate = "C:\Program Files\Microsoft Visual Studio\$version\$edition\MSBuild\Current\Bin\amd64\MSBuild.exe"
        if (Test-Path $candidate) { return $candidate }
    }
}

throw "Could not find a 64-bit MSBuild.exe. Set MSBUILD_PATH to one, or see tools\Get-BlorgMSBuild.ps1 for why the 32-bit one will not do."
