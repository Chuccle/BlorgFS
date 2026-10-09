<#
.SYNOPSIS
    One command that answers "did I break anything?" for BlorgFS.

.DESCRIPTION
    The regression gate for an iterative (agentic or human) edit loop.
    Everything is tiered by what it costs and what it needs, because a
    check that is too slow or needs a mounted volume will not get run
    after every edit, and a check that does not get run is not a gate.

      Build  Compile and link everything, PREfast on. Catches compile
             breaks, PREfast findings, and the CHECK_PADDING static
             asserts. Cheap enough to run unattended after every batch of
             edits, which is what the Stop hook in .claude\settings.json
             does.

      Proof  Build, plus exactly what Fast leaves out: the exhaustive
             interleaving explorations and the full fuzz corpus. Minutes.
             Run before landing lifetime, dispatch or parser changes.

      Fast   Build, plus every usermode test: the crypto vector suite,
             the TLS fuzz corpus, the HTTP client logic sandbox (scenarios
             and fuzzer), and the kernel-behaviour suite -- the real
             Socket.c run against a rule-enforcing IRQL/lock/DPC/timer
             model, covering watchdog timeouts, the DPC/completion race,
             reentrancy, and quiescence. No driver load, no volume, no
             backend. The gate to run by hand before considering a change
             done.

      Perf   Fast, plus PerfHarness workloads against a mounted BlorgFS
             volume, compared to a stored baseline by
             Compare-BlorgMetrics.ps1. Needs the driver loaded and a
             backend reachable -- so it runs where the driver runs, which
             in this project usually means inside the VM (deploy it with
             deploy\Deploy-ToVM.ps1 first).

      All    Every tier, in that order.

    Exit code is 0 only if every check in the selected tier passed, so it
    can gate a hook, a CI step, or an agent's next action without parsing
    output.

.PARAMETER Tier
    Build, Fast (default), Proof, Perf, or All.

.PARAMETER CacheMutantsOnly
    Rebuild DispatchSandbox with deliberate cache safeguards removed and
    require each removed safeguard to fail its regression; also record an
    expected surviving reordering. Restore exact source bytes and
    rebuild the fixed sandbox before returning. Does not measure coverage.

.PARAMETER CoverageOnly
    Measure existing Debug x64 binaries with OpenCppCoverage after running
    the Fast gate. Does not build. Requires an empty CoverageDirectory.

.PARAMETER CoverageDirectory
    Output directory for per-suite coverage, merged Cobertura/HTML and a
    manifest recording the exact head, pins and test profile.

.PARAMETER Configuration
    Debug (default) or Release.

.PARAMETER PerfDrive
    Drive letter of the mounted BlorgFS volume for the Perf tier.

.PARAMETER PerfFile
    A file on that volume large enough for a sequential read to be
    meaningful -- comfortably larger than Cc's read-ahead window so the
    read reaches steady state rather than measuring only its spin-up.

.PARAMETER BaselineDirectory
    Where accepted baselines live. Defaults to tools\baselines.

.PARAMETER UpdateBaseline
    Write this run's metrics as the new baseline instead of comparing.
    Use after an intentional change, once the numbers have been eyeballed.

.EXAMPLE
    .\Invoke-BlorgChecks.ps1
    Fast tier. What to run after editing driver source.

.EXAMPLE
    .\Invoke-BlorgChecks.ps1 -Tier Perf -PerfFile B:\media\big.mkv
    Measures against the stored baseline and fails on regression.

.EXAMPLE
    .\Invoke-BlorgChecks.ps1 -Tier Perf -PerfFile B:\media\big.mkv -UpdateBaseline
    Accepts the current numbers as the new baseline.
#>
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Fast', 'Proof', 'Perf', 'All')][string]$Tier = 'Fast',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$PerfDrive = 'B',
    [string]$PerfFile,
    [string]$BaselineDirectory,
    [switch]$UpdateBaseline,
    [switch]$CacheMutantsOnly,
    [switch]$CoverageOnly,
    [string]$CoverageDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BaselineDirectory) { $BaselineDirectory = Join-Path $PSScriptRoot 'baselines' }

if ($CoverageOnly) {
    if ($Tier -ne 'Fast' -or $Configuration -ne 'Debug' -or -not $CoverageDirectory) {
        throw '-CoverageOnly requires -Tier Fast -Configuration Debug -CoverageDirectory <empty directory>'
    }
    $coverageTool = (Get-Command OpenCppCoverage.exe -ErrorAction Stop).Source
    $CoverageDirectory = [IO.Path]::GetFullPath($CoverageDirectory)
    if ((Test-Path $CoverageDirectory) -and (Get-ChildItem $CoverageDirectory -Force)) {
        throw 'CoverageDirectory must be empty; never merge coverage from an earlier head'
    }
    New-Item -ItemType Directory -Force $CoverageDirectory | Out-Null
}

$results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param([string]$Name, [string]$Status, [string]$Detail = '')
    $results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
}

#
# The 64-bit MSBuild specifically, and deploy/Deploy-ToVM.ps1 needs the same
# answer -- see tools\Get-BlorgMSBuild.ps1 for why the 32-bit one silently
# loses the static analysis this tier exists to run. One locator rather than
# two, because the two had drifted: this one had a list, the deploy script
# trusted PATH and simply failed outside a Developer PowerShell.
#
$msbuild = & (Join-Path $PSScriptRoot "Get-BlorgMSBuild.ps1")


#
# Mutate the real source and run the observing-layer regression, independently
# in each process. Compilation failures and unrelated crashes are not kills.
# Restore byte-for-byte originals even when a build or assertion fails.
#
if ($CacheMutantsOnly) {
    if ($CoverageOnly) { throw 'Mutation and coverage modes must run separately' }
    $project = Join-Path $repoRoot 'tests\sandbox\DispatchSandbox.vcxproj'
    $outputDirectory = Join-Path $repoRoot "x64\$Configuration"
    $exe = Join-Path $outputDirectory 'DispatchSandbox.exe'
    $diskPath = Join-Path $repoRoot 'src\DiskCache.c'
    $readPath = Join-Path $repoRoot 'src\Read.c'
    $dirPath = Join-Path $repoRoot 'src\DirCtrl.c'
    $originals = @{}
    foreach ($path in @($diskPath, $readPath, $dirPath)) { $originals[$path] = [IO.File]::ReadAllBytes($path) }
    $encoding = [Text.UTF8Encoding]::new($false)
    $disk = [IO.File]::ReadAllText($diskPath)
    $read = [IO.File]::ReadAllText($readPath)
    $dir = [IO.File]::ReadAllText($dirPath)
    $subtreeTest = 'DirCtrlTest.NetworkSubtreePublishesDescendantListings'
    $fillTest = 'DiskCacheTest.FillAllocationFailuresCountTheRemainingBlocksAndRollBack'
    $eofTest = 'DiskCacheTest.ValidBytesCannotExtendBeyondTheImmutableKey'
    $snapshotTest = 'DiskCacheTest.PagingResourceKeepsTrimAndCacheKeyInOneVersion'
    $revisionTest = 'DiskCacheTest.AFetchedRunIsCheckedAgainstTheVersionTheReadStartedWith'
    $count = '(?s)BLORGFS_STAT_ADD\(DiskCacheDropped,\s*\(end >> DISK_CACHE_BLOCK_SHIFT\) - key.Block \+\s*\(\(end == key.Size && \(end & \(DISK_CACHE_BLOCK_SIZE - 1\)\)\) \? 1 : 0\)\);'
    $snapshot = [regex]::Match($read, '(?s)static READ_FILE_VERSION ReadSnapshotFile\(.*?\n\}')
    if (-not $snapshot.Success) { throw 'snapshot helper not found' }
    $unlocked = $snapshot.Value.Replace('ExAcquireResourceSharedLite(Fcb->Header.PagingIoResource, TRUE);', '')
    $unlocked = $unlocked.Replace('ExReleaseResourceLite(Fcb->Header.PagingIoResource);', '')
    $rootPublish = 'if (BlorgPathCachePublishListing(Dir, DirInfo, Ticket))'
    $lateSeed = $dir.Replace($rootPublish, "const BOOLEAN rootPublished = BlorgPathCachePublishListing(Dir, DirInfo, Ticket);`n    if (rootPublished)")
    $lateSeed = $lateSeed.Replace('BlorgPathCacheSeedListing(Dir, DirInfo, Ticket);', '')
    $publishEnd = '(?m)        BlorgReleaseDescendants\(descendants, count\);\r?\n    \}'
    if ([regex]::Matches($lateSeed, $publishEnd).Count -ne 1) { throw 'descendant publication end not found' }
    $lateSeed = [regex]::Replace($lateSeed, $publishEnd, '$0' + "`n    if (rootPublished)`n    {`n        BlorgPathCacheSeedListing(Dir, DirInfo, Ticket);`n    }")
    $variants = @(
        @{ Name = 'skip-subtree-publication'; Path = $dirPath; Text = $dir.Replace('const SIZE_T published = BlorgPathCachePublishDescendants(Dir, DirInfo, descendants, count, Ticket);', 'const SIZE_T published = 0;'); Test = $subtreeTest },
        @{ Name = 'seed-root-after-descendants'; ExpectedKilled = $false; Path = $dirPath; Text = $lateSeed; Test = $subtreeTest },
        @{ Name = 'remove-cache-EOF-offset-guard'; Path = $diskPath; Text = $disk.Replace('Offset >= Key->Size || ', ''); Test = $eofTest },
        @{ Name = 'weaken-cache-EOF'; Path = $diskPath; Text = $disk.Replace('Offset >= Key->Size || Valid > Key->Size - Offset', 'FALSE'); Test = $eofTest },
        @{ Name = 'remove-snapshot-resource'; Path = $readPath; Text = $read.Replace($snapshot.Value, $unlocked); Test = $snapshotTest },
        @{ Name = 'skip-fill-rollback'; Path = $diskPath; Text = $disk.Replace('BlorgDiskCacheIndexCommit(&DiskCache.Index, slot, FALSE);', ''); Test = $fillTest },
        @{ Name = 'count-one-fill-drop'; Path = $diskPath; Text = [regex]::Replace($disk, $count, 'BLORGFS_STAT_INC(DiskCacheDropped);'); Test = $fillTest },
        @{ Name = 'omit-partial-EOF-drop'; Path = $diskPath; Text = $disk.Replace('((end == key.Size && (end & (DISK_CACHE_BLOCK_SIZE - 1))) ? 1 : 0)', '0'); Test = $fillTest },
        @{ Name = 'remove-response-version-check'; Path = $diskPath; Text = $disk.Replace('return FileBuffer->HasVersion && FileBuffer->VersionSize == Key->Size && FileBuffer->VersionTime == Key->ModifiedTime;', 'return TRUE;'); Test = $revisionTest }
    )
    try {
        & $msbuild $project "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:OutDir=$outputDirectory\" '/v:minimal' '/nologo'
        if ($LASTEXITCODE -ne 0) { throw 'fixed sandbox build failed' }
        foreach ($test in @($fillTest, $eofTest, $snapshotTest, $revisionTest, $subtreeTest)) {
            & $exe "--gtest_filter=$test"
            if ($LASTEXITCODE -ne 0) { throw "fixed regression failed: $test" }
        }
        foreach ($variant in $variants) {
            $original = [IO.File]::ReadAllText($variant.Path)
            if ($variant.Text -eq $original) { throw "mutant did not change source: $($variant.Name)" }
            [IO.File]::WriteAllText($variant.Path, $variant.Text, $encoding)
            & $msbuild $project "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:OutDir=$outputDirectory\" '/v:minimal' '/nologo'
            if ($LASTEXITCODE -ne 0) { throw "mutant build failed: $($variant.Name)" }
            $output = & $exe "--gtest_filter=$($variant.Test)" 2>&1
            $exitCode = $LASTEXITCODE
            $output | ForEach-Object { Write-Host $_ }
            $killed = $exitCode -ne 0 -and ($output -match '\): error:') -and ($output -match [regex]::Escape($variant.Test))
            $expectedKilled = if ($variant.ContainsKey('ExpectedKilled')) { $variant.ExpectedKilled } else { $true }
            $result = [ordered]@{ expected_killed = $expectedKilled; mutant = $variant.Name; test = $variant.Test; configuration = $Configuration; killed = [bool]$killed; exit_code = $exitCode }
            Write-Host ('CACHE_MUTANT_RESULT ' + ($result | ConvertTo-Json -Compress))
            [IO.File]::WriteAllBytes($variant.Path, $originals[$variant.Path])
            if ($expectedKilled -and -not $killed) { throw "mutant survived or failed without a named assertion: $($variant.Name)" }
            if (-not $expectedKilled -and $exitCode -ne 0) { throw "equivalent ordering changed the observed contract: $($variant.Name)" }
        }
    } finally {
        foreach ($path in $originals.Keys) { [IO.File]::WriteAllBytes($path, $originals[$path]) }
        & $msbuild $project "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:OutDir=$outputDirectory\" '/v:minimal' '/nologo'
        if ($LASTEXITCODE -ne 0) { throw 'restored sandbox build failed' }
    }
    foreach ($test in @($fillTest, $eofTest, $snapshotTest, $revisionTest, $subtreeTest)) {
        & $exe "--gtest_filter=$test"
        if ($LASTEXITCODE -ne 0) { throw "restored regression failed: $test" }
    }
    exit 0
}

function Invoke-Build {
    param([string]$Project, [string]$Label)

    $path = Join-Path $repoRoot $Project
    $output = & $msbuild $path "/p:Configuration=$Configuration" '/p:Platform=x64' '/v:minimal' '/nologo' 2>&1
    $exit = $LASTEXITCODE

    $errors = $output | Select-String -Pattern ': error' | ForEach-Object { $_.Line.Trim() }
    $warnings = $output | Select-String -Pattern ': warning' | ForEach-Object { $_.Line.Trim() }

    #
    # BlorgFS.inf leaves DriverVer empty, so stampinf fills it from the
    # local clock while inf2cat validates against UTC. Between local
    # midnight and the UTC offset they disagree and catalog generation
    # fails on a tree that compiled and linked perfectly. Calling that a
    # build failure would train everyone to ignore a red result, and
    # silently swallowing it would hide a genuine packaging break -- so it
    # gets its own status.
    #
    $postdated = $output | Select-String -Pattern 'postdated DriverVer'

    if ($postdated) {
        $realErrors = $errors | Where-Object { $_ -notmatch 'inf2cat' }

        if (-not $realErrors) {
            Add-Result $Label 'CLOCK' 'compiled and linked; catalog step blocked by stampinf/inf2cat UTC date skew'
            return $true
        }
    }

    if ($exit -ne 0 -or $errors) {
        $detail = if ($errors) { ($errors | Select-Object -First 3) -join '; ' } else { "exit $exit" }
        Add-Result $Label 'FAIL' $detail
        $script:FailedBuilds[$Label] = $true
        return $false
    }

    if ($warnings) {
        Add-Result $Label 'WARN' (($warnings | Select-Object -First 3) -join '; ')
        return $true
    }

    Add-Result $Label 'PASS'
    return $true
}

#
# Tracks which builds failed, so a test is never run against the binary a
# previous build left behind. A PASS line next to a failed build claims
# the code works when what actually ran was last build's exe -- the most
# dangerous thing a gate can say.
#
$script:FailedBuilds = @{}

function Invoke-TestExe {
    param([string]$Pattern, [string]$Label, [int]$TimeoutSeconds = 300, [string]$Arguments = '', [string]$RequiresBuild = '')

    if ($RequiresBuild -and $script:FailedBuilds.ContainsKey($RequiresBuild)) {
        Add-Result $Label 'SKIP' "$RequiresBuild failed to build; not running a stale binary"
        return $true
    }

    #
    # Newest hit across BOTH build roots, not the first enumeration hit:
    # a solution build writes to the repo root while a project-by-project
    # build writes under tests\<project>, both roots persist, and they can
    # hold binaries of different ages (README, "Build output lands in two
    # different places"). Enumeration order is directory order, so taking
    # the first hit has already run one stale exe past this gate.
    #
    $exe = Get-ChildItem $repoRoot -Filter $Pattern -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match [regex]::Escape("x64\$Configuration") } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $exe) {
        #
        # Not SKIP: we only get here when the corresponding build PASSED,
        # so a missing binary means it landed somewhere this glob cannot
        # see -- exactly the state a green result must never paper over.
        #
        Add-Result $Label 'FAIL' "$Pattern not found under any x64\$Configuration root despite a successful build"
        return $false
    }

    #
    # System.Diagnostics.Process directly rather than Start-Process:
    # Start-Process -PassThru hands back an object whose ExitCode stays
    # empty, so `-ne 0` is true for a process that exited 0 and every
    # passing test reads as a failure. That is the worst possible failure
    # mode for a gate -- it cries wolf until it gets ignored.
    #
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe.FullName
    if ($Arguments) { $psi.Arguments = $Arguments }
    if ($CoverageOnly) {
        $coveragePath = Join-Path $CoverageDirectory "$($Label -replace ':', '-').cov"
        Write-Host "==> $Label ($Pattern $Arguments)"
        $psi.FileName = $coverageTool
        $psi.Arguments = "--quiet --sources `"$repoRoot\src`" --export_type=binary:`"$coveragePath`" -- `"$($exe.FullName)`" $Arguments"
    }
    $psi.WorkingDirectory = Split-Path -Parent $exe.FullName
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = [System.Diagnostics.Process]::Start($psi)

    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()

    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch {}
        Add-Result $Label 'FAIL' "timed out after ${TimeoutSeconds}s"
        return $false
    }

    $output = $stdout.Result
    $errorOutput = $stderr.Result

    Set-Content -Encoding utf8 -Path (Join-Path $env:TEMP "$($Label -replace ':', '-').out") -Value $output
    if ($CoverageOnly) {
        Set-Content -Encoding utf8 -Path (Join-Path $CoverageDirectory "$($Label -replace ':', '-').out") -Value $output
        Set-Content -Encoding utf8 -Path (Join-Path $CoverageDirectory "$($Label -replace ':', '-').err") -Value $errorOutput
    }

    if ($proc.ExitCode -ne 0) {
        $tail = ($output -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 3) -join ' | '
        $errorTail = ($errorOutput -split "`n" | Where-Object { $_.Trim() } | Select-Object -Last 3) -join ' | '
        if ($errorTail) { $tail = "$tail | $errorTail" }
        Add-Result $Label 'FAIL' "exit $($proc.ExitCode): $tail"
        return $false
    }

    Add-Result $Label 'PASS'
    return $true
}

# ------------------------------------------------------------------- Fast

$ok = $true

$runsBuild = -not $CoverageOnly -and $Tier -in @('Build', 'Fast', 'Proof', 'All')
$runsTests = -not $CoverageOnly -and $Tier -in @('Fast', 'All')

#
# The exhaustive interleaving explorations and the fuzzers are verification
# runs, not regression tests, and they do not belong in the loop a person
# runs after every edit. Measured: the whole test set took 608 seconds, of
# which 546 was one sandbox and 518 was a single exhaustive proof. What is
# left after excluding them is around a second, which is the difference
# between a gate that gets run and one that gets skipped.
#
# Nothing is deleted. -Tier Proof runs exactly what Fast excludes, so the
# proofs stay one command away and CI can run both.
#
$runsProofs = -not $CoverageOnly -and $Tier -in @('Proof', 'All')

#
# gtest filter applied in Fast. *SchedTest.* is every systematic
# interleaving exploration; they exhaust schedule spaces in the tens of
# thousands and are what the Proof tier exists for.
#
$FastFilter = '--gtest_filter=-*SchedTest.*:*StressTest.*'

if ($runsBuild) {
    Write-Host "==> Build tier ($Configuration)" -ForegroundColor Cyan

    $ok = (Invoke-Build 'src\BlorgFS.vcxproj' 'build:driver') -and $ok

    foreach ($p in @(
        @('tests\TlsTest\TlsTest.vcxproj', 'build:TlsTest'),
        @('tests\TlsFuzzTest\TlsFuzzTest.vcxproj', 'build:TlsFuzzTest'),
        @('tests\TlsHandshakeTest\TlsHandshakeTest.vcxproj', 'build:TlsHandshakeTest'),
        @('tests\PerfHarness\PerfHarness.vcxproj', 'build:PerfHarness'),
        @('tests\sandbox\ClientSandbox.vcxproj', 'build:ClientSandbox'),
        @('tests\sandbox\ClientFuzz.vcxproj', 'build:ClientFuzz'),
        @('tests\sandbox\SocketSandbox.vcxproj', 'build:SocketSandbox'),
        @('tests\sandbox\NodeTableSandbox.vcxproj', 'build:NodeTableSandbox'),
        @('tests\sandbox\DispatchSandbox.vcxproj', 'build:DispatchSandbox'),
        @('tests\sandbox\TlsHandshakeSandbox.vcxproj', 'build:TlsHandshakeSandbox'),
        @('tests\VolumeTester\VolumeTester.vcxproj', 'build:VolumeTester'))) {
        $ok = (Invoke-Build $p[0] $p[1]) -and $ok
    }

    #
    # TlsTest checks the key schedule and AEAD against the RFC 8448
    # vectors, so it is the direct regression test for anything touching
    # Tls.c. TlsFuzzTest replays the seed corpus. TlsHandshakeTest is
    # deliberately NOT run here: it drives a real handshake against a live
    # openssl s_server, which makes it an integration test, not a gate.
    #
    if ($runsTests) {
        $ok = (Invoke-TestExe 'TlsTest.exe' 'test:rfc8448' 300 '' 'build:TlsTest') -and $ok
        $ok = (Invoke-TestExe 'TlsFuzzTest.exe' 'test:tlsfuzz' 300 '' 'build:TlsFuzzTest') -and $ok

        #
        # The HTTP client compiled into usermode (sandbox\SandboxDriver.h).
        # The scenario suite covers the shapes a well-behaved server never
        # produces; the fuzzer covers the ones nobody thought of. Both
        # report through the sandbox's own guards -- a pool overrun, a
        # leak, or a completion callback firing twice is a failure even if
        # the client returned a plausible status.
        #
        $ok = (Invoke-TestExe 'ClientSandbox.exe' 'test:client-scenarios' 300 '' 'build:ClientSandbox') -and $ok
        $ok = (Invoke-TestExe 'ClientFuzz.exe' 'test:client-fuzz' 60 "200 $FastFilter" 'build:ClientFuzz') -and $ok

        #
        # The real Socket.c against the kernel rule model (gtest). Covers
        # what only an executable model can: a watchdog firing on a peer
        # that never answers, a timeout DPC racing a real completion for
        # the same context, inline completions nesting at DISPATCH, the
        # pool under concurrent acquire/release, and -- in the same binary
        # -- the negative controls proving each of those rules is actually
        # enforced rather than merely documented.
        #
        $ok = (Invoke-TestExe 'SocketSandbox.exe' 'test:kernel-socket' 120 $FastFilter 'build:SocketSandbox') -and $ok

        #

        #
        # FCB/DCB lifetime: the claim that a node handed back by
        # BlorgNodeTableLookupPin is never freed while pinned. Driven with
        # real threads pinning and unpinning while the reap worker runs
        # underneath, so a premature free lands on the guarded pool rather
        # than on a corrupted list days later.
        #
        # The budget is generous because the revival proof inside is a
        # full exhaustive exploration (~150k replays, several minutes):
        # its schedule space grew when lock claims moved under the baton
        # and contenders started genuinely blocking. A timeout here means
        # the machine slowed or the space exploded, not a test failure.
        #
        $ok = (Invoke-TestExe 'NodeTableSandbox.exe' 'test:kernel-nodetable' 120 $FastFilter 'build:NodeTableSandbox') -and $ok

        #
        # Every remaining driver translation unit -- all of IRP dispatch,
        # the FSP work queue, the path cache, the statistics counters --
        # compiled and linked against the kernel model. Building it is
        # itself the check today: a dispatch handler that stops compiling
        # here has drifted from the shim, and a test cannot be written for
        # a file that does not build.
        #
        $ok = (Invoke-TestExe 'DispatchSandbox.exe' 'test:kernel-dispatch' 120 $FastFilter 'build:DispatchSandbox') -and $ok

        #
        # The real TlsHandshake.c against the same model, with a fake TLS
        # 1.3 server (TlsHandshakeKernelTest.cpp) standing in for
        # openssl s_server: it reads the driver's actual ClientHello back
        # off the wire (WskModelLastSendBytes) and answers with a
        # self-consistent ServerHello and encrypted flight built from the
        # same crypto primitives Tls.c uses, so this needs no live peer
        # and no network. Covers the handshake state machine end to end,
        # prioritising BlorgTlsCheckPin -- the actual security boundary against
        # a MITM'd certificate -- plus several malformed-flight shapes
        # TlsHandshakeTest.cpp's live-server integration test can't easily
        # produce on demand.
        #
        $ok = (Invoke-TestExe 'TlsHandshakeSandbox.exe' 'test:kernel-tlshandshake' 300 '' 'build:TlsHandshakeSandbox') -and $ok
    }
}

# ------------------------------------------------------------------ Proof

#
# Exactly what Fast excludes, so the two together are the old Fast and
# nothing has stopped being checked -- it has stopped being checked on
# every edit.
#
# These are the systematic interleaving explorations and the fuzzer. They
# exhaust schedule spaces rather than sampling them, which is what makes
# them proofs and also what makes them minutes rather than milliseconds.
# Run before landing anything that touches node lifetime, dispatch, the
# path cache, the socket watchdog, or the HTTP response parser.
#
if ($runsProofs) {
    Write-Host "==> Proof tier (exhaustive; minutes, not seconds)" -ForegroundColor Cyan

    $proofFilter = '--gtest_filter=*SchedTest.*:*StressTest.*'

    $ok = (Invoke-TestExe 'SocketSandbox.exe'   'proof:socket'    300  $proofFilter 'build:SocketSandbox') -and $ok
    $ok = (Invoke-TestExe 'DispatchSandbox.exe' 'proof:dispatch'  600  $proofFilter 'build:DispatchSandbox') -and $ok

    #
    # The long one: the revival proof alone exhausts ~56k schedules and
    # takes several minutes, and it grew when lock claims moved under the
    # baton and contenders began genuinely blocking. A timeout here means
    # the machine slowed or the space exploded, not a test failure.
    #
    $ok = (Invoke-TestExe 'NodeTableSandbox.exe' 'proof:nodetable' 1800 $proofFilter 'build:NodeTableSandbox') -and $ok

    #
    # The full corpus rather than the 500-iteration smoke Fast runs.
    #
    $ok = (Invoke-TestExe 'ClientFuzz.exe' 'proof:client-fuzz' 900 '50000' 'build:ClientFuzz') -and $ok
}

# Coverage measures existing Debug binaries after the build and Fast gate.
# Driver.c is not linked into these binaries. Node-table exhaustive proofs
# remain in the Proof tier; this profile adds dispatch/socket proofs and
# 2,000 client-fuzz iterations to the Fast test set.
if ($CoverageOnly) {
    foreach ($test in @(
        @('TlsTest.exe', 'coverage:tls', 300, ''),
        @('TlsFuzzTest.exe', 'coverage:tls-fuzz', 300, ''),
        @('ClientSandbox.exe', 'coverage:client', 600, $FastFilter),
        @('ClientFuzz.exe', 'coverage:client-fuzz', 600, "2000 $FastFilter"),
        @('SocketSandbox.exe', 'coverage:socket', 900, $FastFilter),
        @('NodeTableSandbox.exe', 'coverage:nodes', 600, $FastFilter),
        @('DispatchSandbox.exe', 'coverage:dispatch', 900, $FastFilter),
        @('SocketSandbox.exe', 'coverage:socket-proofs', 900, '--gtest_filter=*SchedTest.*:*StressTest.*'),
        @('DispatchSandbox.exe', 'coverage:dispatch-proofs', 1800, '--gtest_filter=*SchedTest.*:*StressTest.*'),
        @('TlsHandshakeSandbox.exe', 'coverage:handshake', 600, ''))) {
        $ok = (Invoke-TestExe $test[0] $test[1] $test[2] $test[3]) -and $ok
    }
    if ($ok) {
        $mergeArguments = @(Get-ChildItem $CoverageDirectory -Filter '*.cov' |
            ForEach-Object { "--input_coverage=$($_.FullName)" })
        $mergeArguments += @("--export_type=cobertura:$(Join-Path $CoverageDirectory 'coverage.xml')",
            "--export_type=html:$(Join-Path $CoverageDirectory 'html')")
        & $coverageTool @mergeArguments
        if ($LASTEXITCODE -ne 0) {
            Add-Result 'coverage:merge' 'FAIL' "OpenCppCoverage exited $LASTEXITCODE"
            $ok = $false
        }
        $manifest = @{
            Head = (& git -C $repoRoot rev-parse HEAD)
            Configuration = $Configuration
            CoverageToolVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($coverageTool).FileVersion
            Scope = 'src files linked into sandbox and TLS binaries; Driver.c excluded'
            Profile = 'Fast plus dispatch/socket Sched/Stress; ClientFuzz 2000; node-table Sched/Stress excluded'
            Isolation = 'Fast and scheduling suites use separate processes, matching the Fast/Proof tiers'
            Pins = @(& git -C $repoRoot submodule status)
        }
        $manifest | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $CoverageDirectory 'manifest.json')
    }
}

# ------------------------------------------------------------------- Perf

if ($Tier -eq 'Perf' -or $Tier -eq 'All') {
    Write-Host "==> Perf tier (drive ${PerfDrive}:)" -ForegroundColor Cyan

    $harness = Get-ChildItem $repoRoot -Filter 'PerfHarness.exe' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match [regex]::Escape("x64\$Configuration") } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $harness) {
        Add-Result 'perf' 'FAIL' 'PerfHarness.exe not built'
        $ok = $false
    }
    elseif (-not (Test-Path "${PerfDrive}:\")) {
        Add-Result 'perf' 'SKIP' "drive ${PerfDrive}: not mounted (load the driver, or run this inside the VM)"
    }
    elseif (-not $PerfFile) {
        Add-Result 'perf' 'SKIP' '-PerfFile not given'
    }
    else {
        New-Item -ItemType Directory -Force $BaselineDirectory | Out-Null
        $runDirectory = Join-Path $env:TEMP 'blorgfs-perf'
        New-Item -ItemType Directory -Force $runDirectory | Out-Null

        $workloads = @(
            @{ Name = 'seq'; Args = @('seq', $PerfFile) },
            @{ Name = 'rand'; Args = @('rand', $PerfFile, '64', '200') }
        )

        foreach ($w in $workloads) {
            $reportPath = Join-Path $runDirectory "$($w.Name).txt"

            #
            # Delete the previous run's report BEFORE running, not after
            # failing to find one: a harness that crashes or loses the
            # volume mid-run exits without writing anything, and Test-Path
            # below would otherwise present the last session's numbers as
            # this run's and compare them against the baseline.
            #
            Remove-Item $reportPath -Force -ErrorAction SilentlyContinue

            $harnessArgs = $w.Args + @('--report', $reportPath)

            & $harness.FullName @harnessArgs | Out-Null

            if ($LASTEXITCODE -ne 0) {
                Add-Result "perf:$($w.Name)" 'FAIL' "PerfHarness exited $LASTEXITCODE"
                $ok = $false
                continue
            }

            if (-not (Test-Path $reportPath)) {
                Add-Result "perf:$($w.Name)" 'FAIL' 'harness exited 0 but wrote no report'
                $ok = $false
                continue
            }

            $baselinePath = Join-Path $BaselineDirectory "$($w.Name).txt"

            if ($UpdateBaseline) {
                #
                # An accepted baseline must clear the invariant checks even
                # so: a run with unbalanced fetch accounting or failed
                # fetches is not a number worth measuring regressions
                # against, and accepting it silently poisons every later
                # comparison. Compare with no -Baseline runs invariants only.
                #
                & (Join-Path $PSScriptRoot 'Compare-BlorgMetrics.ps1') -Current $reportPath

                if ($LASTEXITCODE -ne 0) {
                    Add-Result "perf:$($w.Name)" 'FAIL' 'baseline rejected -- current run failed the invariant checks'
                    $ok = $false
                    continue
                }

                Copy-Item $reportPath $baselinePath -Force
                Add-Result "perf:$($w.Name)" 'BASELINE' "written to $baselinePath"
                continue
            }

            $compareArgs = @('-Current', $reportPath)
            if (Test-Path $baselinePath) { $compareArgs += @('-Baseline', $baselinePath) }

            & (Join-Path $PSScriptRoot 'Compare-BlorgMetrics.ps1') @compareArgs

            if ($LASTEXITCODE -ne 0) {
                Add-Result "perf:$($w.Name)" 'FAIL' 'see comparison output above'
                $ok = $false
            }
            else {
                $detail = if (Test-Path $baselinePath) { '' } else { 'no baseline yet -- invariants only' }
                Add-Result "perf:$($w.Name)" 'PASS' $detail
            }
        }
    }
}

# ----------------------------------------------------------------- summary

Write-Host "`n===== BlorgFS checks =====" -ForegroundColor Cyan

foreach ($r in $results) {
    $colour = switch ($r.Status) {
        'PASS' { 'Green' }
        'BASELINE' { 'Green' }
        'CLOCK' { 'Yellow' }
        'WARN' { 'Yellow' }
        'SKIP' { 'DarkGray' }
        default { 'Red' }
    }

    $line = "  {0,-9} {1}" -f $r.Status, $r.Name
    if ($r.Detail) { $line += "  --  $($r.Detail)" }

    Write-Host $line -ForegroundColor $colour
}

if (-not $ok) {
    Write-Host "`nFAILED" -ForegroundColor Red
    exit 1
}

Write-Host "`nOK" -ForegroundColor Green
exit 0
