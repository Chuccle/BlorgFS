<#
.SYNOPSIS
    Loads deploy/debug settings from blorgfs.env so a session can start
    working without being handed credentials by hand.

.DESCRIPTION
    Everything the deploy and debug pipeline needs about *this* machine's VM
    lives in one gitignored file: where the .vmx is, the guest account, the
    .vmx config-encryption password, and the KDNET key. All of those are
    credentials for the debug VM, which is why the file is gitignored and why
    blorgfs.env.example -- the committed template -- carries no real values.

    Callers use it to default their parameters, so an explicit argument still
    wins and nothing becomes impossible to override:

        $env = & "$PSScriptRoot\Get-BlorgEnv.ps1"
        if (-not $GuestUser) { $GuestUser = $env.GuestUser }

    Returns a hashtable. Missing file returns an empty one rather than
    throwing: the settings are a convenience, and a caller given explicit
    arguments should not need the file to exist at all.

    Format is KEY=VALUE, one per line. '#' starts a comment, blank lines are
    ignored, and surrounding quotes on a value are stripped so a password
    containing '#' or spaces can be quoted safely.

.PARAMETER Path
    Explicit env file. Defaults to blorgfs.env beside this script, then
    blorgfs.env at the repo root, then .env at the repo root.
#>
[CmdletBinding()]
param([string]$Path)

$ErrorActionPreference = "Stop"

if (-not $Path) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    foreach ($candidate in @(
        (Join-Path $PSScriptRoot "blorgfs.env"),
        (Join-Path $repoRoot "blorgfs.env"),
        (Join-Path $repoRoot ".env"))) {
        if (Test-Path $candidate) { $Path = $candidate; break }
    }
}

$settings = @{}

if (-not $Path -or -not (Test-Path $Path)) {
    return $settings
}

foreach ($line in Get-Content -LiteralPath $Path) {
    $trimmed = $line.Trim()

    if ($trimmed.Length -eq 0 -or $trimmed.StartsWith("#")) { continue }

    $split = $trimmed.IndexOf("=")
    if ($split -lt 1) { continue }

    $key = $trimmed.Substring(0, $split).Trim()
    $value = $trimmed.Substring($split + 1).Trim()

    if ($value.Length -ge 2 -and
        (($value.StartsWith('"') -and $value.EndsWith('"')) -or
         ($value.StartsWith("'") -and $value.EndsWith("'")))) {
        $value = $value.Substring(1, $value.Length - 2)
    }

    $settings[$key] = $value
}

return $settings
