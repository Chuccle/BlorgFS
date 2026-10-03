<#
.SYNOPSIS
    The runtime test run: real driver, real server, real kernel. Runs inside
    the guest and leaves a machine-readable verdict.

.DESCRIPTION
    Expects the bundle run-guest-tests.sh pushes:

        <BundleDir>\package\    the blorg-package-windows-x64 artifact
                                (manifest.json, driver\, server\)
        <BundleDir>\in-guest\   these scripts
        <BundleDir>\tools\      Test-BlorgCorrectness.ps1
        <BundleDir>\suites\     optional extra suites (see below)

    Steps, each recorded in results.json as it finishes so a bugcheck
    mid-run still leaves everything up to that point:

      package      every file matches the SHA-256 in manifest.json
      corpus       the corpus manifest (host/make-corpus.py) was pushed in
      server       the Linux server-rs on the KVM host answers /healthcheck
                   through the guest's NIC at 10.0.2.2 -- the topology the
                   product actually runs in
      install      driver\Install-BlorgFS.ps1 against that server; B: mounts
      service      BlorgFS is RUNNING
      volume       VolumeTester.exe, when the package carries it
      listing      the tree on B: matches the corpus: every path, every size
      correctness  tools\Test-BlorgCorrectness.ps1 (size, hash, range,
                   reread, tail) against the same server
      suite:<name> each suites\*.ps1
      survived     BlorgFS still RUNNING afterwards

    Suite contract, for contract and behavioural tests from elsewhere: a
    suites\<name>.ps1 that exits 0 on pass. It is offered -Drive (the
    mounted letter), -BackendUrl, -CorpusManifest and
    -ResultsDir (a directory of its own for logs), and given whichever of
    those its param() block declares. The served tree lives on the host,
    so a suite that needs fixtures on the volume ships them as
    suites\<name>.corpus\, which the host serves as <name>\
    (run-guest-tests.sh).

    The driver is not stopped at the end: there is no dismount handler, so
    `sc stop` wedges in STOP_PENDING (AGENTS.md). Runs end by discarding the
    guest's disk instead.

    Exit code 0 only when the verdict is "pass".

.PARAMETER BundleDir
    Where the bundle was pushed.

.PARAMETER ResultsDir
    Where results.json and the logs go. Prepare-Guest.ps1 creates it.

.PARAMETER BackendHost
    Where the driver and the checks reach server-rs. 10.0.2.2 is the KVM
    host as seen through QEMU user networking.

.PARAMETER Port
    server-rs's port.
#>
[CmdletBinding()]
param(
    [string]$BundleDir = 'C:\blorgfs-ci\bundle',
    [string]$ResultsDir = 'C:\blorgfs-ci\results',
    [string]$BackendHost = '10.0.2.2',
    [int]$Port = 18080,
    [char]$Drive = 'B'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$package = Join-Path $BundleDir 'package'
$corpusManifest = 'C:\blorgfs-ci\corpus-manifest.json'
$backendUrl = "http://${BackendHost}:$Port"
$logs = Join-Path $ResultsDir 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$script:Steps = [System.Collections.Generic.List[object]]::new()
$script:Started = (Get-Date).ToUniversalTime()
$script:Mounted = $false

# Logs and results as BOM-less UTF-8: Windows PowerShell 5.1's own
# Tee-Object and Set-Content -Encoding utf8 write UTF-16 or a BOM, which
# the host side (and anything reading results.json) then trips over.
$script:Utf8 = New-Object System.Text.UTF8Encoding $false
filter Write-StepLog {
    param([string]$Log)
    $line = "$_"
    [System.IO.File]::AppendAllText($Log, $line + "`r`n", $script:Utf8)
    Write-Host $line
}

function Save-Results {
    param([string]$Verdict = 'running')
    $manifest = $null
    $mf = Join-Path $package 'manifest.json'
    if (Test-Path $mf) { $manifest = Get-Content $mf -Raw | ConvertFrom-Json }
    [ordered]@{
        verdict  = $Verdict
        started  = $script:Started.ToString('o')
        updated  = (Get-Date).ToUniversalTime().ToString('o')
        package  = if ($manifest) { $manifest.version } else { $null }
        commits  = if ($manifest) { $manifest.components } else { $null }
        os       = (Get-CimInstance Win32_OperatingSystem).Version
        steps    = $script:Steps
    } | ConvertTo-Json -Depth 6 | ForEach-Object {
        [System.IO.File]::WriteAllText((Join-Path $ResultsDir 'results.json'), $_, $script:Utf8)
    }
}

# Runs one step. The body returns $true/$false (or throws); 'skip' and a
# reason string are for steps whose precondition is missing.
function Invoke-Step {
    param([string]$Name, [scriptblock]$Body, [switch]$NeedsMount)
    $log = Join-Path $logs (($Name -replace '[^\w.-]', '_') + '.log')
    Write-Host ""
    Write-Host "===== $Name =====" -ForegroundColor Cyan
    $t0 = Get-Date
    $status = 'fail'; $detail = $null
    if ($NeedsMount -and -not $script:Mounted) {
        $status = 'skip'; $detail = "${Drive}: is not mounted"
    } else {
        try {
            $r = & $Body $log
            if ($r -is [array]) { $r = $r[-1] }
            if ($r -eq 'skip') { $status = 'skip'; $detail = 'not applicable' }
            elseif ($r -is [string]) { $status = 'fail'; $detail = $r }
            elseif ($r) { $status = 'pass' }
        } catch {
            $detail = $_.Exception.Message
        }
    }
    $secs = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $color = @{ pass = 'Green'; fail = 'Red'; skip = 'Yellow' }[$status]
    Write-Host ("----- {0}: {1} ({2}s){3}" -f $Name, $status.ToUpper(), $secs, $(if ($detail) { " -- $detail" } else { '' })) -ForegroundColor $color
    $script:Steps.Add([ordered]@{
        name = $Name; status = $status; seconds = $secs; detail = $detail
        log = if (Test-Path $log) { "logs/" + (Split-Path $log -Leaf) } else { $null }
    })
    Save-Results
    return ($status -ne 'fail')
}

# Runs a PowerShell script in a child process (so its `exit N` is just an
# exit code), streaming output to the console and the step log.
function Invoke-ChildScript {
    param([string]$Log, [string]$File, [hashtable]$Arguments = @{})
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $File)
    foreach ($k in $Arguments.Keys) {
        $v = $Arguments[$k]
        if ($v -is [switch] -or $v -is [bool]) { if ($v) { $argList += "-$k" } }
        else { $argList += "-$k"; $argList += "$v" }
    }
    & powershell.exe @argList 2>&1 | Write-StepLog -Log $Log
    return $LASTEXITCODE
}

try {
    Save-Results

    Invoke-Step 'package' {
        param($log)
        $m = Get-Content (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
        "package $($m.version)" | Write-StepLog -Log $log
        $bad = @()
        foreach ($p in $m.files.PSObject.Properties) {
            $f = Join-Path $package ($p.Name -replace '/', '\')
            if (-not (Test-Path -LiteralPath $f)) { $bad += "missing $($p.Name)"; continue }
            $h = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($h -ne $p.Value) { $bad += "hash mismatch $($p.Name)" }
        }
        if ($bad) { $bad | Add-Content $log; return ($bad -join '; ') }
        $true
    } | Out-Null

    Invoke-Step 'corpus' {
        param($log)
        if (-not (Test-Path $corpusManifest)) { return 'corpus-manifest.json was not pushed into the guest' }
        $m = Get-Content $corpusManifest -Raw | ConvertFrom-Json
        "corpus: $($m.files.Count) files, $($m.directories.Count) directories" | Write-StepLog -Log $log
        $true
    } | Out-Null

    # Across the NIC, before the driver tries the same path: a failure
    # here is the network or the host, not the filesystem.
    Invoke-Step 'server' {
        param($log)
        try {
            $r = Invoke-WebRequest -UseBasicParsing -Uri "$backendUrl/healthcheck" -TimeoutSec 15
            "server at $backendUrl answered $($r.StatusCode)" | Write-StepLog -Log $log
            $r.StatusCode -eq 200
        } catch { "server at $backendUrl unreachable from the guest: $($_.Exception.Message)" }
    } | Out-Null

    Invoke-Step 'install' {
        param($log)
        $code = Invoke-ChildScript $log (Join-Path $package 'driver\Install-BlorgFS.ps1') @{
            RemoteHost = $BackendHost; RemotePort = "$Port"; DriveLetter = $Drive
        }
        if ($code -eq 2) { return 'test signing is off (Install-BlorgFS.ps1 exit 2) -- Prepare-Guest.ps1 and a reboot should have handled this' }
        if ($code -ne 0) { return "Install-BlorgFS.ps1 exited $code" }
        $script:Mounted = Test-Path "${Drive}:\"
        $script:Mounted
    } | Out-Null

    $serviceRunning = {
        param($log)
        $q = sc.exe query BlorgFS | Out-String
        $q | Set-Content $log
        if ($q -match 'STATE\s+:\s+4\s+RUNNING') { return $true }
        'BlorgFS is not RUNNING: ' + (($q -split "`n" | Where-Object { $_ -match 'STATE' }) -join ' ').Trim()
    }
    Invoke-Step 'service' $serviceRunning | Out-Null

    Invoke-Step 'volume' -NeedsMount {
        param($log)
        $exe = @((Join-Path $package 'tests\VolumeTester.exe'), (Join-Path $BundleDir 'tests\VolumeTester.exe')) |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $exe) { 'VolumeTester.exe not in the package' | Set-Content $log; return 'skip' }
        & $exe 2>&1 | Write-StepLog -Log $log
        if ($LASTEXITCODE -ne 0) { return "VolumeTester exited $LASTEXITCODE" }
        $true
    } | Out-Null

    Invoke-Step 'listing' -NeedsMount {
        param($log)
        $want = Get-Content $corpusManifest -Raw | ConvertFrom-Json
        $root = "${Drive}:\"
        $haveFiles = @{}
        foreach ($p in [System.IO.Directory]::EnumerateFiles($root, '*', [System.IO.SearchOption]::AllDirectories)) {
            $haveFiles[$p.Substring($root.Length)] = ([System.IO.FileInfo]::new($p)).Length
        }
        $haveDirs = @{}
        foreach ($p in [System.IO.Directory]::EnumerateDirectories($root, '*', [System.IO.SearchOption]::AllDirectories)) {
            $haveDirs[$p.Substring($root.Length)] = $true
        }
        $bad = @()
        foreach ($f in $want.files) {
            if (-not $haveFiles.ContainsKey($f.path)) { $bad += "missing file $($f.path)" }
            elseif ($haveFiles[$f.path] -ne [long]$f.size) { $bad += "size $($f.path): volume $($haveFiles[$f.path]) != $($f.size)" }
            $haveFiles.Remove($f.path)
        }
        foreach ($extra in $haveFiles.Keys) { $bad += "unexpected file $extra" }
        foreach ($d in $want.directories) {
            if (-not $haveDirs.ContainsKey($d)) { $bad += "missing directory $d" }
        }
        "checked $($want.files.Count) files, $($want.directories.Count) directories" | Set-Content $log
        if ($bad) { $bad | Add-Content $log; $bad | Select-Object -First 20 | Write-Host; return "$($bad.Count) listing mismatches (first: $($bad[0]))" }
        $true
    } | Out-Null

    Invoke-Step 'correctness' -NeedsMount {
        param($log)
        $code = Invoke-ChildScript $log (Join-Path $BundleDir 'tools\Test-BlorgCorrectness.ps1') @{
            Drive = $Drive; BackendUrl = $backendUrl; MaxFiles = 1000
            Report = (Join-Path $ResultsDir 'correctness.txt')
        }
        if ($code -ne 0) { return "Test-BlorgCorrectness.ps1 exited $code (see correctness.txt)" }
        $true
    } | Out-Null

    $suiteDir = Join-Path $BundleDir 'suites'
    if (Test-Path $suiteDir) {
        foreach ($suite in Get-ChildItem $suiteDir -Filter '*.ps1' | Sort-Object Name) {
            $name = $suite.BaseName
            Invoke-Step "suite:$name" -NeedsMount {
                param($log)
                $out = New-Item -ItemType Directory -Force -Path (Join-Path $ResultsDir "suites\$name")
                $offered = @{
                    Drive = $Drive; BackendUrl = $backendUrl
                    CorpusManifest = $corpusManifest; ResultsDir = $out.FullName
                }
                $declared = (Get-Command $suite.FullName).Parameters.Keys
                $pass = @{}
                foreach ($k in $offered.Keys) { if ($declared -contains $k) { $pass[$k] = $offered[$k] } }
                $code = Invoke-ChildScript $log $suite.FullName $pass
                if ($code -ne 0) { return "exited $code" }
                $true
            } | Out-Null
        }
    }

    Invoke-Step 'survived' $serviceRunning | Out-Null
} catch {
    Write-Host "RUNNER ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $script:Steps.Add([ordered]@{ name = 'runner'; status = 'fail'; seconds = 0; detail = $_.Exception.Message; log = $null })
}

$failed = @($script:Steps | Where-Object { $_.status -eq 'fail' })
$verdict = if ($failed.Count) { 'fail' } else { 'pass' }
Save-Results $verdict

Write-Host ""
foreach ($s in $script:Steps) { Write-Host ("  {0,-5} {1}" -f $s.status.ToUpper(), $s.name) }
Write-Host "VERDICT: $verdict"
if ($verdict -eq 'pass') { exit 0 } else { exit 1 }
