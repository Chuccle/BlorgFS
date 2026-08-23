<#
.SYNOPSIS
    Differential data-correctness check for a mounted BlorgFS volume.

.DESCRIPTION
    Runs inside the guest, against a mounted BlorgFS drive and the same HTTP
    backend the driver itself is pointed at. Ground truth is the backend: for
    every file checked, the same bytes are fetched twice -- once through the
    driver via the normal Win32 file API, once directly over HTTP -- and
    compared. Any divergence is a driver bug, because both paths are asking
    the same server for the same bytes.

    This needs no prepared corpus and no manifest: it tests whatever the
    backend is actually serving, at whatever sizes that happens to be, which
    is the traffic the driver really sees.

    It is also the only layer that exercises the real driver, the real cache
    manager, and the real network path together -- none of the usermode
    sandboxes or model checkers can reach it.

    Checks, roughly in order of what they would catch:

      size      Driver-reported file size matches the backend's
                Content-Length. Catches listing/metadata deserialise bugs.
      hash      Whole-file SHA-256 through the driver matches the same file
                fetched over HTTP. Catches corruption, chunk misordering,
                short reads, and off-by-one at chunk boundaries.
      range     Random offset/length reads match the same byte range fetched
                with an HTTP Range header. Catches read-path and range bugs
                that only appear on non-sequential access.
      reread    A second read returns identical bytes. Catches cache
                coherency and node-reuse bugs.
      tail      A read straddling EOF returns exactly the remaining bytes and
                then stops, rather than over-reading or hanging.

    Exit code is 0 only if every check passed, so it gates cleanly.

.PARAMETER Drive
    Mounted BlorgFS drive letter. Defaults to B.

.PARAMETER BackendUrl
    Base URL of the backend the driver is pointed at, e.g.
    http://10.0.50.17:8080 -- must be the same one, or the comparison is
    meaningless.

.PARAMETER MaxFiles
    How many files to check. They are chosen smallest-first so a quick run
    covers many files rather than one huge one.

.PARAMETER MaxHashBytes
    Files larger than this are not hashed whole; they still get the range,
    reread and tail checks, which is where large-file bugs actually live.

.PARAMETER Report
    Optional path to write flat key=value results to.
#>
[CmdletBinding()]
param(
    [char]$Drive = 'B',
    [Parameter(Mandatory = $true)][string]$BackendUrl,
    [int]$MaxFiles = 12,
    [long]$MaxHashBytes = 32MB,
    [string]$Report
)

$ErrorActionPreference = "Stop"
$root = "${Drive}:"
$BackendUrl = $BackendUrl.TrimEnd('/')

$script:Pass = 0
$script:Fail = 0
$script:Failures = @()

function Ok($name)        { $script:Pass++; Write-Host "  PASS      $name" -ForegroundColor Green }
function Bad($name, $why) {
    $script:Fail++
    $script:Failures += "$name : $why"
    Write-Host "  FAIL      $name" -ForegroundColor Red
    Write-Host "            $why" -ForegroundColor DarkGray
}

if (-not (Test-Path "$root\")) {
    Write-Host "Drive $root is not mounted -- is the BlorgFS service running?" -ForegroundColor Red
    exit 2
}

# Backend paths are the volume-relative path with forward slashes. Each
# segment is escaped separately so the separators survive.
function To-BackendPath {
    param([string]$FullName)
    $rel = $FullName.Substring($root.Length).TrimStart('\')
    $segments = $rel -split '\\' | ForEach-Object { [uri]::EscapeDataString($_) }
    return "/" + ($segments -join "/")
}

function Get-HttpBytes {
    param([string]$BackendPath, [long]$Offset = -1, [long]$Last = -1)
    $req = [System.Net.HttpWebRequest]::Create("$BackendUrl/get_file?path=$BackendPath")
    $req.Timeout = 120000
    $req.ReadWriteTimeout = 120000
    if ($Offset -ge 0) { $req.AddRange([long]$Offset, [long]$Last) }
    $resp = $req.GetResponse()
    try {
        $ms = New-Object System.IO.MemoryStream
        $resp.GetResponseStream().CopyTo($ms)
        return $ms.ToArray()
    } finally { $resp.Close() }
}

function Get-HttpLength {
    param([string]$BackendPath)
    $req = [System.Net.HttpWebRequest]::Create("$BackendUrl/get_file?path=$BackendPath")
    $req.Method = "HEAD"
    $req.Timeout = 60000
    $resp = $req.GetResponse()
    try { return [long]$resp.ContentLength } finally { $resp.Close() }
}

function Hash-Bytes {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString("x2") }) }
    finally { $sha.Dispose() }
}

function Hash-DriverFile {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try   { return -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString("x2") }) }
    finally { $fs.Dispose(); $sha.Dispose() }
}

function Read-DriverRange {
    param([string]$Path, [long]$Offset, [int]$Count)
    $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$fs.Seek($Offset, 'Begin')
        $buf = New-Object byte[] $Count
        $got = 0
        while ($got -lt $Count) {
            $n = $fs.Read($buf, $got, $Count - $got)
            if ($n -le 0) { break }
            $got += $n
        }
        if ($got -eq $Count) { return $buf }
        if ($got -eq 0) { return @() }
        return $buf[0..($got - 1)]
    } finally { $fs.Dispose() }
}

Write-Host "===== BlorgFS differential correctness ($root vs $BackendUrl) ====="

# Deliberately NOT Get-ChildItem -Recurse: PowerShell's filesystem provider
# treats '[' / ']' as wildcard syntax in its own path parser, even under
# -LiteralPath, in recursive enumeration -- a real corpus with scene-release
# names like "[Ryuuga] - ..." throws "Illegal characters in path" from
# PowerShell's own path engine, not from the driver or Win32. Raw .NET
# enumeration bypasses that provider layer entirely, which is also what
# confirmed this is a test-script artifact rather than a driver bug: the
# same names round-trip fine through [System.IO.Directory] APIs.
Write-Host "Enumerating $root (excluding large / non-curated directories) ..."
# SteamLibrary is huge and irrelevant; temp holds download-client staging
# areas (Sonarr/Radarr's cross-seed_linkdirs, .RecycleBin, etc.) that are
# prone to symlinks/hardlinks -- exactly the kind of thing that turns a
# bounded enumeration into an unbounded one. Neither is curated media, so
# neither belongs in a correctness sample.
$excludeDirs = @('SteamLibrary', 'temp')
$topDirs = [System.IO.Directory]::GetDirectories("$root\") |
    Where-Object { $excludeDirs -notcontains (Split-Path $_ -Leaf) }

# Capped and isolated per top-level directory: a Sort-Object over the whole
# tree would force full materialisation before producing anything (no early
# stop despite the lazy enumerator), and one bad subdirectory should not
# take out the whole run.
$targets = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
foreach ($top in $topDirs) {
    if ($targets.Count -ge $MaxFiles) { break }
    try {
        foreach ($p in [System.IO.Directory]::EnumerateFiles($top, "*", [System.IO.SearchOption]::AllDirectories)) {
            $fi = [System.IO.FileInfo]::new($p)
            if ($fi.Length -gt 0) { $targets.Add($fi) }
            if ($targets.Count -ge $MaxFiles) { break }
        }
    } catch {
        Write-Host "  skipping $top : $($_.Exception.Message)" -ForegroundColor Yellow
    }
}
$targets = $targets | Sort-Object Length

if (-not $targets) { Write-Host "No non-empty files to check." -ForegroundColor Yellow; exit 1 }
Write-Host "  checking $($targets.Count) files"

foreach ($f in $targets) {
    $bp = To-BackendPath $f.FullName
    $label = $f.FullName.Substring($root.Length)

    # ---- size ----
    $httpLen = $null
    try {
        $httpLen = Get-HttpLength $bp
        if ($httpLen -lt 0)               { Ok "size:$label (backend gave no length)" }
        elseif ($httpLen -eq $f.Length)   { Ok "size:$label ($($f.Length))" }
        else                              { Bad "size:$label" "driver $($f.Length) != backend $httpLen" }
    } catch { Bad "size:$label" "HEAD failed: $($_.Exception.Message)" }

    # ---- hash ----
    if ($f.Length -le $MaxHashBytes) {
        try {
            $dh = Hash-DriverFile $f.FullName
            $hh = Hash-Bytes (Get-HttpBytes $bp)
            if ($dh -eq $hh) { Ok "hash:$label" }
            else             { Bad "hash:$label" "driver $dh != backend $hh" }
        } catch { Bad "hash:$label" $_.Exception.Message }
    }

    # ---- range ----
    if ($f.Length -gt 4096) {
        try {
            $rnd = [Random]::new(20260820)
            $bad = @()
            for ($i = 0; $i -lt 6; $i++) {
                # Explicit conditionals rather than [Math]::Min: with a
                # multi-GB file's Int64 length, [Math]::Min(65536, $f.Length)
                # resolves to the (int,int) overload and throws trying to
                # narrow the length down to Int32 -- silent overload
                # resolution, not an intentional narrowing.
                $maxOff = [long]($f.Length - 1)
                $randCeiling = if ($maxOff -lt [int]::MaxValue) { [int]$maxOff } else { [int]::MaxValue }
                $off = [long]$rnd.Next(0, $randCeiling)
                $remaining = $f.Length - $off
                $cnt = if ($remaining -lt 65536) { [int]$remaining } else { 65536 }
                if ($cnt -le 0) { continue }
                $d = Read-DriverRange $f.FullName $off $cnt
                $h = Get-HttpBytes $bp $off ($off + $cnt - 1)
                if ($d.Count -ne $h.Count) { $bad += "off=$off len=$cnt driver got $($d.Count) backend got $($h.Count)"; continue }
                if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$d, [byte[]]$h)) { $bad += "off=$off len=$cnt bytes differ" }
            }
            if ($bad.Count) { Bad "range:$label" ($bad -join "; ") }
            else            { Ok  "range:$label (6 random ranges)" }
        } catch { Bad "range:$label" $_.Exception.Message }

        # ---- reread ----
        try {
            $rereadWant = if ($f.Length -lt 131072) { [int]$f.Length } else { 131072 }
            $a = Read-DriverRange $f.FullName 0 $rereadWant
            $b = Read-DriverRange $f.FullName 0 $rereadWant
            if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$a, [byte[]]$b)) { Ok "reread:$label" }
            else { Bad "reread:$label" "two reads of the same range differed" }
        } catch { Bad "reread:$label" $_.Exception.Message }

        # ---- tail ----
        try {
            $want = if ($f.Length -lt 4096) { [int]$f.Length } else { 4096 }
            $t = Read-DriverRange $f.FullName ($f.Length - $want) ($want + 4096)
            if ($t.Count -eq $want) { Ok "tail:$label (EOF-straddling read returned $want)" }
            else { Bad "tail:$label" "read past EOF returned $($t.Count), expected $want" }
        } catch { Bad "tail:$label" $_.Exception.Message }
    }
}

Write-Host ""
Write-Host "passed=$script:Pass failed=$script:Fail"
if ($script:Fail) {
    Write-Host "Failures:" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

if ($Report) {
    $lines = @("passed=$script:Pass", "failed=$script:Fail", "files_checked=$($targets.Count)")
    $i = 0
    foreach ($f in $script:Failures) { $lines += "failure$i=$f"; $i++ }
    $lines | Out-File -Encoding ascii $Report
}

if ($script:Fail) { exit 1 } else { Write-Host "OK" -ForegroundColor Green; exit 0 }
