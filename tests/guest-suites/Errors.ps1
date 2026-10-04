<#
.SYNOPSIS
    Guest suite: server errors reach the caller as the right Win32 error.

.DESCRIPTION
    The listing and correctness steps already check that what the server
    serves reads back intact through the drive. This checks the other half
    of the driver/server agreement: a status the server answers with turns
    into the error a Windows program expects, not a generic failure. If
    server-rs changes what it answers, this is where it shows.

    Runs against the generated corpus (tools/blorg.d/corpus.py), so it needs
    no fixtures of its own. Exits with the number of failed checks.
    Windows PowerShell 5.1.

.PARAMETER Drive
    Mounted BlorgFS drive letter. Defaults to B.
#>
[CmdletBinding()]
param(
    [string]$Drive = 'B'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$root = "$($Drive.TrimEnd(':')):\"
$failures = 0

function Report([string]$Outcome, [string]$Check, [string]$Detail = '') {
    $line = '{0,-4} {1}' -f $Outcome, $Check
    if ($Detail) { $line += " -- $Detail" }
    Write-Host $line
    if ($Outcome -eq 'FAIL') { $script:failures++ }
}

# Runs $Action and passes if it throws $Expected (or a subclass).
function Expect-Error([string]$Check, [type]$Expected, [scriptblock]$Action) {
    try {
        & $Action
        Report 'FAIL' $Check 'it succeeded'
    } catch {
        $e = $_.Exception
        while ($e -is [System.Management.Automation.MethodInvocationException] -and $e.InnerException) { $e = $e.InnerException }
        if ($Expected.IsInstanceOfType($e)) { Report 'PASS' $Check }
        else { Report 'FAIL' $Check "$($e.GetType().FullName): $($e.Message)" }
    }
}

# 404 -> STATUS_OBJECT_NAME_NOT_FOUND -> FileNotFoundException
Expect-Error 'opening a missing file fails with FileNotFound' ([System.IO.FileNotFoundException]) {
    [System.IO.File]::OpenRead((Join-Path $root 'names\does-not-exist.bin')).Dispose()
}

Expect-Error 'opening under a missing directory fails with DirectoryNotFound' ([System.IO.DirectoryNotFoundException]) {
    [System.IO.File]::OpenRead((Join-Path $root 'no-such-dir\file.bin')).Dispose()
}

Expect-Error 'listing a missing directory fails with DirectoryNotFound' ([System.IO.DirectoryNotFoundException]) {
    [System.IO.Directory]::GetFiles((Join-Path $root 'no-such-dir')) | Out-Null
}

# Reading past the end of a file is end-of-file (0 bytes), not an error.
try {
    $f = [System.IO.File]::OpenRead((Join-Path $root 'names\no-extension'))
    try {
        $f.Seek($f.Length + 4096, [System.IO.SeekOrigin]::Begin) | Out-Null
        $n = $f.Read((New-Object byte[] 512), 0, 512)
        if ($n -eq 0) { Report 'PASS' 'reading past the end returns 0 bytes' }
        else { Report 'FAIL' 'reading past the end returns 0 bytes' "read $n" }
    } finally { $f.Dispose() }
} catch {
    Report 'FAIL' 'reading past the end returns 0 bytes' $_.Exception.Message
}

# Case sensitivity is undecided driver behaviour: reported, never failed.
try {
    $len = (New-Object System.IO.FileInfo (Join-Path $root 'NAMES\NO-EXTENSION')).Length
    Report 'INFO' 'open with different casing' "succeeded, $len bytes"
} catch {
    Report 'INFO' 'open with different casing' $_.Exception.GetType().Name
}

Write-Host ''
Write-Host "$failures failed check(s)"
exit $failures
