<#
.SYNOPSIS
    Writes the deterministic file tree server-rs serves to the driver
    during a guest run.

.DESCRIPTION
    The same seed always produces the same bytes, so a failure on one run
    can be replayed byte for byte on the next. The shape is chosen for what
    tends to break a network filesystem rather than for realism:

      boundaries\   sizes either side of the sector, page, 64 KiB and the
                    read-ahead granule (128 KiB) and ceiling (2 MiB)
      large\        one file past Test-BlorgCorrectness's whole-file hash
                    limit, so only its range/tail checks see it
      many\         enough entries that a directory listing spans more than
                    one query buffer
      names\        spaces, brackets, dots, non-ASCII, a long name
      nested\       a deep path
      empty\        an empty directory, and a zero-byte file beside it

    Writes corpus-manifest.json next to (not inside) the corpus: every
    file's relative path, size and SHA-256, and every directory, which the
    listing check compares the mounted volume against.

    Non-ASCII names are built from code points, not literals: Windows
    PowerShell 5.1 reads a BOM-less script as ANSI and would mangle them.

.PARAMETER Path
    Directory to (re)create the corpus in.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Seed = 20261003
)

$ErrorActionPreference = 'Stop'

if (Test-Path $Path) { Remove-Item -Recurse -Force $Path }
New-Item -ItemType Directory -Force -Path $Path | Out-Null
$Path = (Resolve-Path $Path).Path

$rng = [System.Random]::new($Seed)
$files = [System.Collections.Generic.List[object]]::new()

function New-CorpusFile {
    param([string]$Rel, [long]$Size)
    $full = Join-Path $Path $Rel
    [System.IO.Directory]::CreateDirectory((Split-Path $full -Parent)) | Out-Null
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::Create($full)
    try {
        $buf = New-Object byte[] 1048576
        $left = $Size
        while ($left -gt 0) {
            $n = [int][Math]::Min([long]$buf.Length, $left)
            $rng.NextBytes($buf)
            $fs.Write($buf, 0, $n)
            [void]$sha.TransformBlock($buf, 0, $n, $null, 0)
            $left -= $n
        }
        [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
        $hash = -join ($sha.Hash | ForEach-Object { $_.ToString('x2') })
    } finally {
        $fs.Dispose(); $sha.Dispose()
    }
    $files.Add([ordered]@{ path = $Rel; size = $Size; sha256 = $hash })
}

$KiB = 1024; $MiB = 1024 * 1024

foreach ($s in 1, 511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537,
               (128 * $KiB - 1), (128 * $KiB), (128 * $KiB + 1),
               ($MiB - 1), ($MiB + 1), (2 * $MiB - 1), (2 * $MiB + 1), (2 * $MiB + 4097), (8 * $MiB + 3)) {
    New-CorpusFile ("boundaries\size-{0:D9}.bin" -f $s) $s
}

New-CorpusFile 'large\past-hash-limit.bin' (40 * $MiB + 5)

for ($i = 0; $i -lt 300; $i++) {
    New-CorpusFile ("many\entry-{0:D4}.dat" -f $i) (($i * 37) % 3000 + 1)
}

$nonAscii = 'caf' + [char]0x00E9 + ' ' + [char]0x00FC + 'ber ' + [char]0x65E5 + [char]0x672C + '.bin'
$names = @(
    'names\with spaces.bin',
    'names\[bracketed] (parens) {braces}.bin',
    'names\many.dots.in.the.name.bin',
    'names\no-extension',
    "names\$nonAscii",
    ('names\' + ('long' * 50) + '.bin')
)
foreach ($n in $names) { New-CorpusFile $n 4099 }

New-CorpusFile 'nested\a\b\c\d\e\f\g\deep.bin' 70001

New-Item -ItemType Directory -Force -Path (Join-Path $Path 'empty\nothing-here') | Out-Null
New-CorpusFile 'empty\zero-bytes.bin' 0

# .NET enumeration, not Get-ChildItem -Recurse: the provider treats the
# brackets in names\ as wildcards (see Test-BlorgCorrectness.ps1).
$dirs = [System.IO.Directory]::GetDirectories($Path, '*', [System.IO.SearchOption]::AllDirectories) |
    ForEach-Object { $_.Substring($Path.Length + 1) } | Sort-Object

[ordered]@{ seed = $Seed; files = $files; directories = @($dirs) } |
    ConvertTo-Json -Depth 4 |
    Set-Content -Encoding utf8 (Join-Path (Split-Path $Path -Parent) 'corpus-manifest.json')

$total = [long]0
foreach ($f in $files) { $total += $f.size }
Write-Host ("Corpus: {0} files, {1} directories, {2:N1} MiB in {3}" -f $files.Count, $dirs.Count, ($total / $MiB), $Path)
