<#
.SYNOPSIS
    Fails if the driver and the pinned server-rs disagree on the wire contract.

.DESCRIPTION
    BlorgFS and server-rs ship as one package (see "Packaging" in AGENTS.md),
    and the thing that ties them together is the FlatBuffers schema: the
    driver compiles third_party/schemas with flatcc, the server compiles its
    own schemas submodule with flatc, and both are the same Chuccle/schemas
    repository. If the two submodules point at different commits, each side
    builds cleanly against its own copy and the mismatch only shows up as
    garbage directory listings on a mounted volume.

    This compares the two gitlinks -- BlorgFS's third_party/schemas and
    third_party/server-rs's own schemas -- and fails if they differ. It reads
    the gitlinks from the index rather than the working tree, so a local
    `git submodule update` drift does not mask a committed mismatch, and it
    needs only the server-rs submodule checked out (not recursively), which
    keeps the CI job that runs it cheap.

    Dependabot groups the two submodules into one PR for exactly this reason
    (.github/dependabot.yml); a schema change has to land as a matching pair.

    Writes the resolved pins to the pipeline as an object, which
    tools\New-BlorgPackage.ps1 records in the package manifest.

.PARAMETER RepoRoot
    The BlorgFS checkout. Defaults to the parent of this script's directory.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = "Stop"

# `git ls-files -s <path>` prints "<mode> <sha> <stage>\t<path>"; mode 160000
# is a gitlink. Anything else means the path is not a submodule at all.
function Get-Gitlink {
    param([string]$Repo, [string]$Path)
    $line = git -C $Repo ls-files -s -- $Path
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed in '$Repo'" }
    if (-not $line) { throw "'$Path' is not tracked in '$Repo'" }
    $fields = ($line -split '\s+')
    if ($fields[0] -ne '160000') { throw "'$Path' in '$Repo' is not a submodule (mode $($fields[0]))" }
    return $fields[1]
}

$serverRoot = Join-Path $RepoRoot "third_party/server-rs"
if (-not (Test-Path (Join-Path $serverRoot ".git"))) {
    throw "third_party/server-rs is not checked out -- run 'git submodule update --init third_party/server-rs'"
}

$serverPin = Get-Gitlink $RepoRoot "third_party/server-rs"
$driverSchemas = Get-Gitlink $RepoRoot "third_party/schemas"

# Read server-rs's schemas pin at the commit BlorgFS pins, not at whatever the
# submodule happens to have checked out, so the answer is about what ships.
$serverSchemasLine = git -C $serverRoot ls-tree $serverPin -- schemas
if ($LASTEXITCODE -ne 0 -or -not $serverSchemasLine) {
    throw "Could not read 'schemas' at server-rs $serverPin -- is that commit fetched in third_party/server-rs?"
}
$serverSchemas = ($serverSchemasLine -split '\s+')[2]

$serverHead = git -C $serverRoot rev-parse HEAD
if ($serverHead -ne $serverPin) {
    Write-Warning "third_party/server-rs is checked out at $serverHead but BlorgFS pins $serverPin; checking the pin."
}

$pins = [pscustomobject]@{
    ServerRs      = $serverPin
    DriverSchemas = $driverSchemas
    ServerSchemas = $serverSchemas
}

Write-Host "server-rs pin:            $serverPin"
Write-Host "BlorgFS schemas pin:      $driverSchemas"
Write-Host "server-rs schemas pin:    $serverSchemas"

if ($driverSchemas -ne $serverSchemas) {
    $msg = "Schema mismatch: BlorgFS builds against schemas $driverSchemas but the pinned server-rs ($serverPin) builds against $serverSchemas. " +
           "Bump third_party/schemas and third_party/server-rs together so both sides compile the same schema."
    if ($env:GITHUB_ACTIONS) { Write-Host "::error title=Package pins::$msg" }
    throw $msg
}

Write-Host "OK: driver and server compile the same schema."
return $pins
