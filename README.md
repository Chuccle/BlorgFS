# BlorgFS

Kernel-mode Windows filesystem driver presenting an HTTP backend as a mounted
volume (B:). Read-only. Async WSK networking, an optional hand-rolled TLS 1.3
client and a keep-alive connection pool.

## Layout

```
src/           the driver, and only the driver -- one .vcxproj, its INF,
               and the sources that compile into BlorgFS.sys
tests/         everything that verifies it
  sandbox/       usermode targets that compile the real driver sources
                 against a kernel model, plus the systematic scheduler
                 and the CBMC harnesses under verification/
  TlsTest/       RFC 8448 vectors        TlsFuzzTest/  record layer under ASan
  TlsHandshakeTest/  live openssl handshake
  PerfHarness/   workload driver and counter reader
  VolumeTester/  volume-level behaviour against a mounted drive
tools/         tiered check runner, metric comparison, crash triage,
               differential correctness harness
deploy/        VM deploy pipeline and the debugging notes
third_party/   submodules: flatcc, picohttpparser, schemas, googletest
```

**Build output lands in two different places, and it matters.** A *solution*
build writes to the repo root (`x64\<Config>\`), because `$(SolutionDir)` is
defined; building a project on its own -- which is what
`tools\Invoke-BlorgChecks.ps1` does -- writes to `tests\<project>\x64\<Config>\`
instead. Neither is wrong, but both exist at once and can hold binaries of
different ages.

Assume the one you want is the stale one until you have checked. This split
has caused three separate failures: a test running against a binary with no
ASan runtime beside it, `Invoke-TestExe` picking a stale exe by glob, and a
32-minute-old `PerfHarness` deployed to the VM measuring nothing. CI works
around it by searching both roots (`build.yml`).

The sandbox projects put their own directory on the include path so
`src/Driver.h` can pull in `SandboxPrelude.h` without the driver naming a
test directory.

## Checking for regressions

**Run `tools\Invoke-BlorgChecks.ps1` — do not hand-roll a build command.**

```bash
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast
```

Tiers, cheapest first:

| Tier | What it does | Needs |
|---|---|---|
| `Build` | Compile + link everything with PREfast | nothing |
| `Fast` (default) | Build, plus RFC 8448 crypto vectors and the fuzz corpus | nothing |
| `Perf` | Fast, plus PerfHarness workloads compared to a stored baseline | driver loaded, backend reachable |
| `All` | every tier | as above |

Exit code is 0 only if everything in the tier passed, so it gates cleanly.

Run `-Tier Fast` before calling any change done — the crypto tests are what
catch a `Tls.c` regression, and cheaper tiers do not run them.

(For agent-driven sessions, a `Stop` hook in `.claude/settings.json` runs the
`Build` tier automatically after each turn as a non-blocking backstop. It is
not a substitute for running `Fast` yourself.)

### Build gotchas

- **Use the 64-bit MSBuild.** The WDK NuGet picks its PREfast and ApiValidator
  directory from the MSBuild process's own architecture; the 32-bit one selects
  an x86 directory where those tools are missing, silently losing the static
  analysis. `Invoke-BlorgChecks.ps1` already resolves the right one.
- **`inf2cat` "postdated DriverVer" -- fixed, but know the shape.** `BlorgFS.inf`
  leaves `DriverVer` empty, so stampinf fills it from local time. inf2cat used
  to validate against UTC, so between local midnight and the UTC offset the
  two disagreed and catalog generation failed on a tree that compiled fine
  (`inf2cat.exe exited with code -2`). `Inf2CatUseLocalTime` in
  `BlorgFS.vcxproj` now points the validator at the same clock the stamp came
  from. If it ever returns, that mismatch is where to look; the check script
  reports it as `CLOCK`, not `FAIL`.

## Continuous integration

Three workflows, split by what a failure should cost you.

| Workflow | Runs on | What it does |
|---|---|---|
| `build.yml` | push and PR to master | Both configurations, Fast tier. The merge gate. |
| `verify.yml` | 03:00 UTC daily, or on demand | CBMC proofs and extended fuzz/interleaving runs. |
| `codeql.yml` | Saturdays 23:41 UTC, on demand, and on any PR touching its own config | CodeQL with the pinned Microsoft driver query packs. |

The daily and weekly ones are deliberately not gates: a CBMC regression or
a new CodeQL finding is worth waking up to, not worth blocking a merge that
PREfast and the Fast tier already cleared.

`codeql.yml`'s third trigger is the one worth understanding. Scheduled
workflows only ever run on the default branch, so a change to what CodeQL
analyses -- above all a query-pack pin in `.github/codeql/codeql-config.yml`
-- could otherwise only be merged unrun, and would first show up as a
changed finding set the following Saturday. A PR touching that config or
the workflow runs the analysis it is changing. `workflow_dispatch` covers
the rest: re-scanning after a pack bump or a batch of fixes, without a
seven-day wait.

Both packs are pinned on purpose. A pack release must not silently change
what a scheduled run reports -- but a pin that is never reviewed is lost
coverage, and the windows-drivers pack is the one carrying the
driver-specific IRQL and annotation queries. Bump it deliberately, let the
PR trigger run it, and re-triage: previous false-positive verdicts do not
carry across a pack version.

## Sanitizers

The usermode sandbox targets build with **ASan** (`EnableASAN`) — it owns
memory-safety there, and the gate runs under it. Three things every ASan
target needs, and each fails in its own unhelpful way if missed:

| Requirement | Symptom when missing |
|---|---|
| `/Zi`, not `/ZI` | ASan and Edit-and-Continue are incompatible |
| `LinkIncremental=false` in a *configuration* PropertyGroup | `LNK4300: ignoring '/INCREMENTAL' because input module contains ASAN metadata` on every link. Setting it inside `ClCompile` is silently ignored. |
| A `CopyAsanRuntime` post-build target | The exe exits `0xC0000135` before `main`, so the suite reports a bare non-zero exit and no output |

That last one is per project and easy to get wrong, because a *solution*
build puts every binary in one directory — so a target with no copy step
still runs, using the DLL some other project deposited next to it.
`tools\Invoke-BlorgChecks.ps1` builds project-by-project instead, where
output is project-local, and there the missing copy is fatal. A target can
therefore pass in CI and fail in the gate, or the reverse, purely on which
build shape ran last.

The driver itself builds with **KASAN**:

```bash
msbuild src\BlorgFS.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:EnableKASAN=true
```

`kasan.lib` ships in the WDK and the instrumented `.sys` is roughly double
the size — that size jump is the quickest check that it actually applied.
It is opt-in, so the normal build and the gate are unaffected. Running it
needs `bcdedit /set kasan on` in the guest and a reboot.

Note `-fsanitize=thread` is **unsupported** for `x86_64-pc-windows-msvc`;
there is no TSan on this platform. Interleaving coverage comes from the
systematic scheduler instead (`tests\sandbox\Scheduler.h`).

The scheduler's lock contract is **claim-under-the-baton**: a primitive waits
via `KmSchedWaitUntilClaim`, and its claim callback runs while the caller still
holds the baton, immediately after the predicate that justified it. Claiming
anywhere else reopens a TOCTOU window between check and claim -- the spin-lock
double-grant and the ERESOURCE double-hold were both exactly that window.
A deadlocked schedule drains its parked threads serially through the baton
rather than releasing them all at once, so an abandoned run exits cleanly
instead of corrupting every replay after it. `SchedulerAudit` in
`NodeTableSchedTest.cpp` pins both properties.

## Measuring performance

**Benchmark on an optimised Release build with Driver Verifier disabled.**
Anything else measures the instrumentation, not the driver, and the two
mistakes compound.

`deploy\Deploy-ToVM.ps1` defaults to `-Configuration Debug`. A Debug driver
is unoptimised and compiles `BLORGFS_PRINT` in (it is a runtime check on
`global.LogLevel`, not a no-op), so it is the wrong binary to time:

```bash
powershell -File deploy/Deploy-ToVM.ps1 -ForBenchmark
```

`-ForBenchmark` is the whole answer: it deploys Release, clears Driver
Verifier, reboots so the change actually applies, and then **waits for the
guest to go idle** before reporting success. Use it for every performance
run. `-Configuration` still wins if given explicitly.

That last step is not politeness. A freshly booted Windows guest runs
Defender, SearchIndexer, Windows Update and TiWorker for minutes, which on a
2-vCPU guest is both cores saturated -- it steals the CPU the driver needs
*and* makes `vmrun` calls slow enough to look wedged. A run taken inside that
window measured its usermode control at 25.63 MB/s before the driver and
14.04 MB/s after: a 45% collapse in one cycle. Bracket every driver
measurement with the control run immediately before and after it, and throw
the point away when the two disagree.

Driver Verifier is worse, because it is invisible in every output the
harness produces -- and because **it lives in the snapshot**. `Deploy-ToVM`
reverts before deploying, so verifier comes back on every deploy regardless
of what was done to the running guest. That is why clearing it is part of
the deploy rather than something to remember.

The driver reports its own build flavour (`BLORGFS_STATS_FLAG_CHECKED_BUILD`),
so `PerfHarness` refuses to print workload results from a checked driver
without saying so first. Nothing reports the verifier state, so check it:

```bash
verifier.exe /querysettings
```

If `BlorgFS.sys` is listed, the driver is running under whatever flags are
shown -- typically special pool, force IRQL checking, pool tracking, I/O
verification, deadlock detection, DMA checking, security checks and DDI
compliance checking. Every allocation is on its own guarded page and every
DDI call is wrapped. Disable it, reboot, measure, and put it back:

```bash
verifier.exe /reset
verifier.exe /flags 0x001209bb /driver BlorgFS.sys
```

Record the flag word before resetting; the value above is this VM's, not a
universal one. `-ForBenchmark` saves it to `verifier-before.txt` in the guest
deploy directory before resetting.

Faster still, set `BenchSnapshotName` in `blorgfs.env` to a snapshot whose
guest already has verifier cleared, and `-ForBenchmark` reverts to that
instead -- saving a reboot per run. Take it by hand: boot, `verifier.exe
/reset`, reboot, power off, snapshot. vmrun cannot snapshot an encrypted VM
while it is running.

This is not hypothetical. An entire performance investigation was run
against a Debug driver under that full verifier flag set and compared
against an *unverified usermode HTTP client*. The comparison was
systematically biased against the driver by an unknown but certainly large
factor, which invalidated a "2.8x the CPU per MB/s" conclusion outright and
left every ratio in the study unsafe to quote. Verifier belongs on for
correctness work and off for measurement, and which one is in force must be
stated alongside the numbers.

Counters are **always on**, including in release builds (`Statistics.h`/`.c`),
stored per-processor and read two ways:

- `fsutil fsinfo statistics B:` — the standard `FILESYSTEM_STATISTICS` /
  `FAT_STATISTICS` surface, via `FSCTL_FILESYSTEM_GET_STATISTICS(_EX)`.
- `PerfHarness.exe` — the driver-specific counters (read dispatch mix,
  chunk-fetch latency histogram, connection pool, TLS) over `IOCTL_BLORGFS_QUERY_STATISTICS` on `\\.\BlorgFS`.

Two things to know before reading read numbers:

- **Buffered and unbuffered measure different systems.** Buffered goes
  through Cc, which supplies all of this driver's read-ahead, and is what
  playback looks like. `streams <dir> <n> <secs> unbuffered` bypasses Cc so
  every read reaches the driver, which is what to use when comparing the
  transport against a usermode HTTP client.
- **Warm runs are not measurements.** A second run against the same files is
  served from the Windows cache at thousands of MB/s with zero paging reads.
  Reboot the guest between points, and check that `paging reads` is non-zero
  before believing a number.

```bash
PerfHarness.exe seq B:\media\big.mkv --report run.txt
```

**One stream is not the workload.** Several readers at once is the normal
case -- a video and its subtitle track, a game streaming assets while its own
data file is open, a library browse overlapping playback -- and it behaves
differently enough that single-stream numbers can look healthy while the
system is starving streams outright.

```bash
PerfHarness.exe streams B:\ 8 30
powershell -File tools/Measure-BlorgScaling.ps1 -OutputPath baseline.txt
```

`streams` gives each reader its own file and thread and reports the **latency
tail** (p50/p95/p99/max) and a **fairness** ratio alongside aggregate
throughput. Aggregate alone is the wrong metric: a stream that stalls for a
second has failed even when the total looks fine, and equal throughput split
unequally is a different system from one that shares.

`Measure-BlorgScaling.ps1` sweeps stream counts and adds scaling efficiency
against the single-stream result. **It reboots the guest between runs**, which
is not hygiene: the Windows cache holds the previous run's files, and a
counter reset does not touch it. A second run against the same files is
served from RAM at thousands of MB/s with no paging reads at all, so a sweep
without a real reset measures that from the second row onward. `sc stop` cannot provide that
reset -- it wedges in `STOP_PENDING` (see `deploy/DEBUGGING.md`).

Workload commands reset the counters first, so the numbers are attributable to
the workload. `--report` writes flat `key=value` metrics for
`tools\Compare-BlorgMetrics.ps1`, which checks both correctness invariants
(every inline paging read must have a fetch; fetch issues must balance
terminations) and perf deltas against a baseline in `tools\baselines\`.

Accept new numbers deliberately, never silently:

```bash
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Perf -PerfFile B:\media\big.mkv -UpdateBaseline
```

The driver runs in a VM for testing — `deploy\Deploy-ToVM.ps1` builds, copies,
and installs it via vmrun. Run the `Perf` tier inside the guest, where the
volume is mounted.

## Playback stutter: subtitles, and why the obvious measurements missed it

Measured 2026-08-28 against master, Release, Driver Verifier off, on the
reported symptom: video and subtitle lag watching media off `B:`.

The file that reproduces it is H.264 + AAC + `S_TEXT/UTF8` at 2.6 Mbit/s --
0.33 MB/s sustained against a driver that does 17-28 MB/s. Throughput was
never the question. **The trigger is enabling subtitles**, and until they
are on, nothing about this driver is visible to the player at all.

### What subtitles change

Same file, same player, same session; the only difference is the subtitle
track:

| | subs off, 25 seeks | subs on |
|---|---|---|
| user reads | 75 | 1211 |
| mean | 713 us | 5794 us |
| max | 16.2 ms | 258 ms |
| **over one frame (41 ms)** | **0 (0.00%)** | **86 (7.10%)** |
| paging reads sequential | 100% | **70.4%** |
| user bytes | 19.6 MB | **428 MB** |

Three things happen at once. Sequential paging reads fall to 70.4%, so the
cache manager -- which only predicts forward-sequential access -- stops
prefetching for roughly a third of them. The player reads about 22 times
more data than the bitrate needs, because a text subtitle track is sparse
and interleaved and following it drags the demuxer across ranges the video
stream never touches. And the resulting latency distribution is bimodal
with nothing between the modes: 863 reads at 16-64 us because the bytes
were resident, 111 reads at 16-131 ms because they were not.

### It is this driver, on a clean control

The same file was copied to the guest's own disk and played by the same
player, same subtitle track, same seeking, same two vCPUs. **It played
smoothly, and this driver's counters recorded zero reads for the duration**,
so the control is clean rather than merely plausible.

That exonerates decode -- identical decode and compositing work, no
stutter -- and retires CPU capacity as an independent explanation, since
two cores are demonstrably enough for this content.

It also makes the outlier records legible. The six worst fetches show 88 to
159 ms in `send`, for a request of a couple of hundred bytes, with
`FetchesActive` of 0 or 1: nothing else in flight, and no physical way a
send of that size takes that long. Those are the driver's own completions
being delayed by the driver's own work. Playing from this volume adds
428 MB through WSK receive, HTTP parsing and copies on top of decode, where
the local path adds close to nothing.

So both driver-side costs share one root, and both scale with over-fetch:

- a cold read costs a round trip plus a 512 KB to 1 MB body, against
  roughly 0.1 ms for the same read locally, and
- moving those bytes costs CPU that delays the completions of the very
  fetches being waited on.

### Concurrency cannot fix this

`FetchesActive` is 0 on five of the six worst fetches and 1 on the other.
At the moment of every stall the driver had nothing else in flight, so the
pipeline was idle, not saturated.

That is the shape of the problem. Concurrency scales throughput; this is
latency on a serial dependency chain. The demuxer cannot issue the next
read until the current one returns, because its contents determine the next
offset, and the cache manager only parallelises access it can predict.
Adding width buys nothing here, and would cost something -- more in-flight
completions competing for the two cores already delaying the ones we have.

Which is why the mitigations that matter attack latency: fetch less per
cold read, or do not go to the network at all.

### NTFS as the reference

Measured through the same standard FSCTL this driver implements, same
workload (256 KB requests over a cold region of the same 1.2 GB file):

| | average read the cache manager issues |
|---|---|
| NTFS (`C:`) | **98 KB** |
| BlorgFS (`B:`) | **675 KB** |

NTFS issues roughly one disk read per request, sized just above it -- 2,677
user reads produced 2,681 disk reads -- and does not batch aggressively even
while streaming sequentially at 771 MB/s. This driver clusters about seven
times larger.

That reframes `READ_AHEAD_GRANULARITY`. The evidence table in `Driver.h`
compared 256 KB, 512 KB and 1 MB and picked the middle, but every option
tested was already far outside where the reference filesystem sits, and all
three were measured on eight concurrent streams -- a throughput workload,
not the latency-shaped one that stalls. It is a well-evidenced choice among
outliers.

The counter-argument is real and has to be measured rather than assumed:
NTFS reads from a local disk where a request costs about 0.1 ms, so small
reads cost it nothing, while this driver pays a ~12.7 ms round trip per
request. Shrinking granularity trades stall latency for more round trips on
sequential streaming. `ReadAheadGranularityKb` in the service's
`Parameters` key exists to sweep that trade without a rebuild per point;
zero means never call `CcSetReadAheadGranularity` and leave Cc's default.

### The granularity sweep, and why there is no right constant

That trade has now been measured rather than assumed, and the answer is
that no value of `READ_AHEAD_GRANULARITY` is free.

First, the fact that decides how to read the table. **Cc's default is not
adaptive.** Between `CcInitializeCacheMap` and `CcSetReadAheadGranularity`
the granularity of a cached file is `PAGE_SIZE` -- a constant, 4 KB. So
"leave the default" is not letting Cc choose, it is pinning the granule to
one page, the smallest value in the sweep. Every point below is a constant;
the sweep has no adaptive arm in it at all.

Granularity is the rounding unit for read-ahead, so a small read drags its
whole aligned granule, and a large granule means a demand read waits on a
large fetch. That single mechanism produces both columns: throughput rises
with granule size because fetches get larger and fewer, and the tail rises
with it because each stall is longer.

Measured on one cold guest per matrix, a dataset no other cell touches, and
the granularity assignment alternated within each workload so file identity
cannot track the setting. Sequential rows are the mean of two runs with the
assignment reversed; the others are single runs.

| granule | seq MB/s | seq tail | streams x8 MB/s | streams x8 tail | demux MB/s | demux amplification | demux tail |
|---|---|---|---|---|---|---|---|
| 512 KB (committed) | **27.1** | 3.26% | **32.2** | 13.3% | 0.75 | 28.1x | 4.2% |
| 128 KB | 20.8 | **0.11%** | **32.1** | 29.2% | 0.64 | 20.6x | 0.44% |
| 64 KB (FastFat) | 16.8 | 0.13% | 28.2 | 15.6% | **0.86** | 10.8x | **0.22%** |
| 4 KB (`PAGE_SIZE`) | 16.1 | **0.09%** | 28.7 | 14.0% | 0.82 | **1.4x** | 1.0% |

"Tail" is the share of application-visible reads longer than a 24 fps frame
interval -- the number a viewer can see, not the mean.

The shape is monotone and the endpoints both regress:

- **512 KB costs the tail.** Sequential buffered reads spend 3.26% of their
  reads over a frame against 0.11% at 128 KB, a factor of thirty. At four
  concurrent streams its worst read was **1087 ms** against 118 ms at
  `PAGE_SIZE`. On a filesystem whose stated purpose is playback, a
  one-second read is the whole complaint.
- **`PAGE_SIZE` costs throughput.** Sequential buffered drops from 27.1 to
  16.1 MB/s, **-41%**, because the same bytes now take 4096 fetches instead
  of 832 -- five times the round trips at ~12.7 ms each.

Two controls back the rig up. Unbuffered sequential reads, where Cc is out
of the path, move -5% with amplification 1.00 at both ends -- no effect
where none is possible. Buffered 4 KB random reads are flat at 0.39 MB/s
and amplification 1.00, because Cc's read-ahead is pattern-triggered and
never arms on uniformly random access, so granularity is inert there.

**Unresolved, and load-bearing before anything is committed:**

- `streams x8` at 128 KB reported a 29.2% tail, roughly double every other
  setting, from a single run against a different directory. That is either
  the one real objection to 128 KB or it is noise, and one sample cannot
  tell.
- The `meta` storm produced no usable result at any setting, so
  metadata-heavy work is unmeasured.
- The demux workload reads in 4 KB blocks while Media Player issues ~262 KB
  reads. The granule that minimises amplification is the one matching the
  read size, so the playback column is the row most flattering to small
  values and the one least entitled to decide this.

### Why a constant is the wrong shape

The two regressions above are not in tension by accident. Large granules
win when a file object is being read forwards, because the fetch amortises;
they lose when it is being picked at, because a small read waits for a large
fetch. Those are different file objects, often at the same moment -- a video
track streaming while a subtitle track is picked at -- and one constant has
to serve both.

The driver already knows which is which. `READ_STREAM_TRACKER` in the FCB
exists to answer exactly that question and `BlorgReadIsSequential` already
reports 54-73% sequential on these workloads. So the shape that fits the
evidence is not a better constant but a granularity that starts small and
is raised for a file object whose reads have proven sequential -- Cc's
per-file-object setting is the right granularity of control for it, and the
adaptivity Cc does not provide is adaptivity this driver has the state to
provide itself.

`CcSetReadAheadGranularity` is re-callable, which the design depends on and
nothing documents. It is described as setting the value for a cached file,
FastFat calls it once at cache-map time, and a silently-ignored second call
would have killed the idea. Measured rather than assumed: starting a demux
run at 512 KB and switching to `PAGE_SIZE` partway through moved
amplification from 27.5x to **18.7x** on the same file and the same
setting, with the switch point accounting for the size of the move (it
fires after 300 paging reads, and a 512 KB run only produces about 420, so
roughly 71% of the run stayed at 512 KB: 0.71 x 27.5 + 0.29 x 1.4 is about
20). Run-to-run variation on that cell is ~2%, so a 32% drop is the
re-call, not noise. Speculative reads rose 167 to 246 and demand reads 245
to 361 across the same switch, which is what smaller granules produce.

What is still unestablished is the microscopic mechanism. Read-ahead is
demonstrably what costs the slow reads -- suppressing it with
FILE_FLAG_RANDOM_ACCESS drops reads over a frame from 46/43 to 2/4 across
replicates, with ReadsSpeculative confirming the suppression actually
happened -- but whether the damage is collision (a reader faulting into a
range an in-flight read-ahead already claimed, and inheriting its latency),
transport congestion from fetching 27x the bytes, or priority inversion is
not settled by aggregates. An earlier reading of this section claimed
collision on the grounds that the worst application read tracked the worst
speculative fetch. That was wrong: the maximum sits at 58-66 ms in every
configuration measured, including with read-ahead entirely suppressed, so
it is a floor common to all of them and not evidence of anything. The
metric that does discriminate is the count of reads over a frame, not the
maximum.

### The granule depends on load, which is why no constant worked

Reads over a frame interval are roughly the share of application reads that
have to wait for a fetch, times the chance that wait exceeds the frame. A
granule serves many reads, so halving it roughly doubles how many wait and
halves how long each waits. Which way that trades depends entirely on where
fetch latency sits relative to 41.67 ms -- and load moves it:

| | fetch mean | reads over a frame |
|---|---|---|
| one reader, 128 KB | 19.8 ms | 0.345% |
| one reader, 512 KB | 58.7 ms | 3.40% |
| eight streams, 128 KB | 41.8 ms | 28.9% |
| eight streams, 512 KB | 102.5 ms | 13.7% |

Unloaded, a 128 KB fetch lands comfortably inside the frame, so the extra
waits cost nothing and the small granule wins tenfold. Saturated, a 128 KB
fetch takes 41.8 ms -- sitting exactly on the threshold -- so nearly every
extra wait becomes visible and the large granule wins by making far fewer
reads wait at all. Throughput is identical at that point (31.3 against 31.8
MB/s), so at eight streams this is a latency decision and not a bandwidth
one, and the benchmark that originally chose 512 KB could not have seen it.

That is the whole reason no constant worked. The variable is load, and
neither a fixed value nor an amplification-driven policy can observe it.
The driver can: fetches in flight run about 1.6 to 2.8 for a single reader
and 21 to 28 at eight streams.

So growth requires a sequential reader **and** a loaded transport. Sequential
alone is not sufficient and that is the trap -- a lone reader walking a file
forwards has the same long streak and the same 1.0 amplification as eight of
them, and growing for it is exactly the 3.40% case. Files start at 128 KB,
shrink on measured waste, and climb to 512 KB only when depth crosses the
threshold.

Measured against starting at 512 KB, each cell rebooted with its own dataset
and the configurations alternated:

| workload | start at 512 KB | load-aware, start at 128 KB |
|---|---|---|
| one sequential reader | 3.39% over a frame, 27.4 MB/s | **0.03%**, 19.5 MB/s |
| eight streams | 15.06%, 31.8 MB/s | **14.47%**, 32.4 MB/s |
| demux (playback) | 0.83%, 0.99 MB/s | **0.08%**, 1.17 MB/s |

It takes the better of the two constants in each case rather than splitting
the difference, and the eight-stream cell recorded sixteen grows -- eight
file objects each doubling twice -- which is the load gate doing exactly
what it was built for, per file object.

The cost is 29% of throughput for a lone sequential reader, 19.5 MB/s
against 27.4. That is a file-copy shape rather than a playback one, 19.5
MB/s remains an order of magnitude above any bitrate this volume serves, and
it is the price of keeping a lone reader's fetches inside the frame budget.
Single runs per cell; the underlying constants were replicated, this
comparison was not.

**Measure interleaved or not at all.** Every earlier comparison here was
A-then-B in time, and the host drifts: the same 128 KB setting measured 20.2
MB/s at 0.16% in one session and 12.6 MB/s at 1.8% an hour later. An ordered
comparison hands that drift to whichever setting ran second. The numbers
above alternate configurations with a reboot per run; the fixed-granule
replicates came back at 3.37/3.42% and 0.34/0.35%, tight enough to decide
on, which none of the earlier ordered runs were. The reversal controls used
before this protected against dataset bias and did nothing about drift.

### Why the trade exists at all: too few fetches in flight to cover ttfb

Everything above tunes *around* per-fetch cost. A large granule is only
better because it amortises a fixed cost over more bytes, so the size of
that fixed cost decides how sharp the trade is.

This section previously claimed that 35% of a fetch was this driver's own
overhead, from a phase split against a usermode client:

| per fetch | driver, 196 KB avg | usermode, same bytes |
|---|---|---|
| ttfb | 10.10 ms | 5.33 ms |
| body transfer | 9.52 ms | ~7.5 ms |

**That comparison was invalid and the conclusion drawn from it was wrong.**
The driver column was measured at a pipeline depth of ~2.7 fetches in
flight; the usermode column was a strictly serial client, one request at a
time. Sweeping the usermode client across concurrency, with completion
stamped by polling the async handles so that no response is charged the
drain time of its predecessors:

| concurrency | 1 | 2 | 3 | 4 | 6 |
|---|---|---|---|---|---|
| usermode ttfb, in guest | 6.05 ms | 9.78 ms | 12.46 ms | 17.73 ms | 22.41 ms |

At the driver's own depth of ~2.7 a usermode client sits near 11.6 ms
against the driver's 9.5. There is no 35% gap. The driver is at or slightly
better than a usermode client at matched depth, and the projected "41 MB/s
at parity" was never available -- it was above the physical ceiling, which
should have been the tell.

The rise is not queueing inside the server either. Sweeping response body
size from the host, so that neither this driver nor the guest network stack
is in the path:

| body | conc 1 | conc 2 | conc 3 | conc 4 | conc 6 |
|---|---|---|---|---|---|
| 256 B | 3.80 ms | 2.35 | 2.44 | 1.99 | 2.31 |
| 8 KB | 2.05 ms | 2.11 | 2.70 | 3.17 | 3.48 |
| 64 KB | 2.33 ms | 4.37 | 6.24 | 6.24 | 7.78 |

At 256 bytes ttfb is flat in concurrency; the climb appears only as bodies
grow, and minimum ttfb stays 1.3-2.3 ms at every point. A file small enough
to be served from the backend's resident cache queues identically to one
large enough to be streamed per request, so it is not the server's file
handling. Concurrent response bodies contend for link bandwidth and delay
later responses' headers. Time to first byte under load is a property of
the link, not of this driver and not of the backend.

### The ceiling: ~24-29 MB/s, and it is the network

| measurement | MB/s |
|---|---|
| single stream, from the guest | 23.96 |
| single stream, from the host | 23.70 |
| 2 streams from the guest, aggregate | 21.30 |
| 4 streams from the guest, aggregate | 21.21 |
| 8 MB ranges, advancing (cold) | 28.45 |
| 8 MB ranges, same range repeated (warm) | 29.30 |

Host and guest are indistinguishable, so it is not the guest's virtual NIC.
Aggregate does not rise with stream count -- four streams get a quarter each
-- so it is not per-connection. Warm and cold ranges are identical, so it is
not the backend's disk. It is the network path, and it is the same from
anywhere.

This agrees with the ~30 MB/s ceiling recorded under "Why this, and not more
lookahead" below, measured independently at 16 streams, and extends it: the
wall is already reached by a *single* connection, so concurrency divides it
rather than growing it.

It also settles a contradiction this document was carrying. That section
measured the driver cold at 0.93-1.01x a usermode client while the section
above claimed a 35% deficit against one. The 0.93-1.01x figure was right;
the 35% came from comparing unequal pipeline depths.

This bounds every throughput number here. The 512 KB constant's 27.4 MB/s on
a lone sequential reader was already taking ~96% of the wall; there was
never 40 MB/s to find.

### What that leaves: depth, not granule

Per-stream throughput is `granule / (ttfb + granule/bandwidth)`, multiplied
by the number of fetches in flight and capped by the link:

| granule | body at 28 MB/s | + ttfb | per slot | x2.7 depth | observed |
|---|---|---|---|---|---|
| 512 KB | 18.3 ms | 28.3 ms | 18.1 MB/s | capped at wall | 27.4 |
| 128 KB | 4.6 ms | 14.6 ms | 8.8 MB/s | 23.7 MB/s | 19.5 |

The small granule does not lose throughput because it is small. It loses
because at a depth of ~2.7 there are not enough fetches in flight to cover
each one's ttfb, so a fixed ~10 ms is amortised over 128 KB instead of over
512 KB. At depth 4 a 128 KB granule reaches the wall on the arithmetic
above, with every stall still one small granule long.

That is the shape of "best of both worlds", and it is not the same lever as
the fetch splitting removed below. Splitting divided a single *demanded*
fetch into concurrent slices that were all needed at once, so the pieces
only shared bandwidth with each other. Depth adds *speculative* fetches
further ahead in the file, whose bytes will be consumed anyway. Concurrency
on bytes that are already wanted is close to free; concurrency on bytes
wanted right now just divides the same bandwidth finer.

This is not in tension with "Why this, and not more lookahead" below, which
rejects lookahead as a way to *add* throughput. It is rejected there because
cold reads at a 512 KB granule already sit at the wall, and nothing on the
fetch path can beat the wire. Depth here does not aim past the wall; it aims
to reach it at a granule small enough to keep stalls short. The 128 KB
configuration is at 19.5 MB/s against a ~28 MB/s ceiling, so unlike the
512 KB case there is a gap, and it is the gap the small granule opened.

Lookahead today is entirely Cc's read-ahead -- the driver's own prefetcher
and its chunk budget are gone -- and Cc puts about 4 fetches in flight for a
single stream, ~2.7 measured on average. Raising effective depth for a
sequential reader without raising the granule is the untried lever, and it
is the one the arithmetic points at. Nothing in this section has been
attempted.

### The workload with a deadline, which the harness did not have

Every workload here reads flat out. `seq`, `rand`, `demux` and `streams` all
consume as fast as the driver delivers, and a reader going flat out is
permanently at the read-ahead frontier: it takes each granule the instant Cc
produces it, so it is maximally exposed to fetch latency. The "over one
frame (41 ms)" figure reported for such a reader is hypothetical -- it asks
whether a viewer would have seen a hitch, of a reader that has no deadline.

That matters because the whole adaptive granularity trade was arbitrated on
that figure. `play` is the missing workload: blocks read on a fixed
schedule, block n due at `start + (n+1) * block / rate`, counting blocks
that missed their due time and by how much.

At 3072 KB/s in 128 KB blocks -- a 41.67 ms frame at roughly a Blu-ray
bitrate:

| granule | missed | worst late | app mean | fetch mean / max | speculative : demand |
|---|---|---|---|---|---|
| 128 KB | 0 / 600 | 0.00 ms | 92 us | 16.1 / 66.8 ms | 340 : 1 |
| 512 KB | 0 / 600 | 0.00 ms | 63 us | 41.0 / 55.1 ms | 115 : 1 |

At 8192 KB/s, a 15.62 ms interval:

| granule | missed | worst late | app mean | fetch mean / max | speculative : demand |
|---|---|---|---|---|---|
| 128 KB | 1 / 1600 | 1.35 ms | 59 us | 16.4 / 73.7 ms | 499 : 2 |
| 512 KB | **11 / 1600** | **64.15 ms** | 128 us | 42.9 / 106.1 ms | 187 : 2 |

Two things fall out, and they point opposite ways.

**Cc absorbs almost everything.** 340 speculative fetches against one demand
fetch: the path a user waits on is essentially never on the wire. The driver
issued fetches of 66-106 ms throughout while the application saw means of
59-128 us. This is the same effect recorded when the user-read timer was
added -- 67 ms fetches behind a reader seeing 0.11 ms -- and it is why fetch
latency is the wrong layer to draw conclusions from.

**The tail protection is nonetheless real, above a rate.** At 3 MB/s the
granule does not matter; both are perfect and 512 KB is marginally better.
At 8 MB/s the 512 KB granule produces 11 missed deadlines with 64 ms worst
lateness, against one miss 1.35 ms late at 128 KB. That is a hitch a viewer
would see, on a consumer that genuinely has a deadline. The hypothesis that
the greedy harness invented the tail is **wrong** above about 3 MB/s.

So the adaptive policy is defensible, and the remaining compromise is
narrower than it looked. It is one case: the greedy sequential reader --
a file copy -- which wants 512 KB for 27.4 MB/s against 19.5, and which has
no deadline to protect. The policy refuses to grow for it because it cannot
tell a copy from a lone player, and it is right to be cautious given the
8 MB/s row above.

The signal that separates them is not sequentiality, amplification or
transport load, all of which they share. It is **slack**: a paced consumer
idles between reads -- 99% of wall clock at both rates above, since a read
served from cache costs microseconds against a 15-41 ms interval -- and a
copy never idles at all. Measured per stream as the gap between one read
completing and the next arriving, that distinguishes "has a deadline" from
"wants throughput" directly, where every signal the policy uses today is a
proxy for it. Growing only for a stream with no slack would take the file
copy to 512 KB while leaving both paced rates where they are.

Measured, as instrumentation only -- `ReadIdle*` in the statistics block,
the gap from one application-visible read completing on a file to the next
arriving on it, reported by the harness as an idle share of wall clock:

| workload | idle share | idle p50 |
|---|---|---|
| greedy sequential, buffered | 0.09% | <=2 us |
| streams x8 | 0.03% | <=2 us |
| demux, 4 tracks, burst 4 | 0.53% | -- |
| rand 64 KB buffered | 2.07% | <=2 us |
| paced 8 MB/s | 94.99% | <=16 ms |
| paced 3 MB/s | 99.85% | <=65 ms |

Three orders of magnitude apart with nothing in between. Every greedy
workload lands under 2.1% and every paced one above 94%, so any threshold
from 10% to 90% classifies all six correctly. As a discriminator this is
about as clean as a runtime signal gets, and it is the only one measured
here that separates a file copy from a player -- sequentiality,
amplification and transport load are all shared between them.

Nothing reads these to make a decision. Three things have to be answered
first, and two of them are new:

- **A starving player stops idling.** Its rate rises until it is asking
  continuously, at which point it looks exactly like a copy -- and that is
  when a large granule hurts it most. A slack rule needs a floor a
  struggling player cannot fall through, which probably means pairing slack
  with whether demand rate responds to delivery rate: a copy's does, a
  player's does not.

  How close that gets is now measurable rather than hypothetical: eight
  players missing 6.57% of their deadlines at the link ceiling still measure
  92.06% idle, because a consumer that is occasionally late is still mostly
  waiting on its own schedule. The signal degrades toward greedy only under
  sustained starvation, which leaves more headroom above the threshold than
  the worry assumed -- but the direction of the failure is unchanged.

- **The harness had to start representing production before the policy
  could key on this**, and now does. `demux` and `streams` take an optional
  `pace=<KBps>`, sharing one `Pacer` with `play` so that paced means one
  thing across all three. Paced, they land where the things they stand for
  do:

  | workload | idle share | missed | worst late |
  |---|---|---|---|
  | demux, unpaced | 0.10% | -- | -- |
  | demux, paced 3072 KB/s | 99.91% | 1 / 3000 (0.03%) | 0.05 ms |
  | streams x8, unpaced | 0.02% | -- | -- |
  | streams x8, paced 1536 KB/s each | 98.94% | 9 / 4800 (0.19%) | 13.41 ms |
  | streams x8, paced 3072 KB/s each | 92.06% | 631 / 9600 (6.57%) | 208.87 ms |

  The last row is eight players demanding 24 MB/s of a link that delivers
  24-29. It delivered 23.98 at fairness 1.00 and every one of the eight
  still stuttered, which is what running a deadline workload at the wall
  looks like -- and it is the case where the granule choice matters most,
  so it is the one a policy change has to be judged on.

  Two harness bugs had to be fixed to get those numbers, and both had
  produced confident nonsense first:

  - An FCB outlives its handles on the delayed close list, so the first read
    of a run was charged the gap since the *previous* run's last read on
    that file. Unpaced demux reported 99.70% idle from one 402-second
    sample, in a run lasting seconds. The stamp is now cleared on open.

  - Eight paced streams started together came due at the same instant, which
    is one burst of eight demands per interval rather than eight players.
    In phase they missed 7.33% of deadlines with a 500 ms worst case while
    asking for 12 MB/s of a link that had just delivered 21.7. Staggered by
    `i/N` of the interval, the same configuration misses 0.19% at 13.41 ms.

  The second is worth keeping in mind beyond this harness: synchronised
  arrivals make a link look saturated at half its capacity.

- **Run-to-run variance is larger than the effect on some cells.** The
  8 MB/s paced case missed 1 deadline in 1600 in one run and 65 in another
  at the same granule, and greedy sequential measured 12.56 MB/s here
  against 19.5 elsewhere. Any decision taken on these numbers needs the
  interleaved-with-reboot shape, not two runs compared across a session.

The amplification shrink rule composes with all of this and is unaffected:
wasted bytes are wasted whether or not anyone has a deadline, which is what
keeps the demux pattern protected however it is classified.

### Growing on slack: on by default

The adaptive policy grows the granule for a consumer that never idles as
well as for a loaded transport. `ReadIsGreedy` in Read.c compares a window's
idle ticks against its service ticks, greedy below 25%, and it is consulted
only while fewer than `READ_AHEAD_ADAPT_QUIET_DEPTH` fetches are in flight.
`ReadAheadSlackGrowth` (registry) still switches it off.

Two rounds with the switch alternating, a guest reboot between every run,
**and the arm order alternating between rounds**:

| workload | slack | MB/s | missed | worst | grows |
|---|---|---|---|---|---|
| seq, greedy | 0 | 19.04 | -- | -- | 0.0 |
| seq, greedy | 1 | **22.52** | -- | -- | 2.0 |
| play 3072 | 0 | 3.00 | 0.00% | 0.00 ms | 0 |
| play 3072 | 1 | 3.00 | 0.00% | 0.00 ms | 0 |
| demux paced 3072 | 0 | 3.00 | 0.78% | 89.22 ms | 0 |
| demux paced 3072 | 1 | 3.00 | 0.17% | 50.95 ms | 0 |
| streams x8 paced 3072 | 0 | 23.73 | 27.81% | 1347.86 ms | 8.0 |
| streams x8 paced 3072 | 1 | 23.95 | 6.11% | 397.48 ms | 11.0 |

The sequential case is what this exists for: +18%, `min(slack=1) = 22.44`
above `max(slack=0) = 19.73` so the arms do not overlap, and the mechanism
observed rather than inferred -- exactly two grows in every switch-on run
and none in any switch-off run. Paced playback misses no deadline in either
arm and never grows.

An earlier sweep put the same gain at +38%, from four rounds that alternated
the arms but always ran slack=0 first. Fixing the order took it to +18%, so
**half of that first number was warm-up**. It is recorded because the
correction is the useful part: alternating the configuration is not enough
if the order within a round is fixed.

Two rows carry no claim. Demux differs in slack's favour with **zero grows
in both arms**, so the policy cannot have caused it. And the eight-stream
grow counts range from 0 to 16 *within a single arm*, so that workload's
variance swamps any difference between arms; the gate means slack cannot
fire while the transport is busy, which is where that workload lives, but
indistinguishable in noisy data is not the same as identical.

### The loaded rule is settled, and it is gone

The policy also grew the granule whenever the transport was busy. That rule
predated slack and its case was this, from the granularity sweep:

|  | 128 KB | 512 KB |
|---|---|---|
| one stream | 0.345% over a frame | 3.40% |
| eight streams | 28.9% | 13.7% |

Loaded, the large granule looked like it won by a factor of two. Every
number in that table came from a reader going flat out.

Pinning the granule -- `ReadAheadAdapt=0` in the registry, so the feedback
loop cannot confound the arms -- and running consumers that read to a
schedule reverses it. Two sweeps, two rounds each, arm order rotating:

| paced workload | pin 128 KB | pin 512 KB |
|---|---|---|
| play 3072 | 0.00% missed | 0.04% |
| demux paced 3072 | 0.39% | 4.76% |
| streams x8 paced 1536 | 0.33% | 1.73% |

Twelve times worse on the demux pattern, five times worse on eight paced
streams at half the link. So the rule is removed, and growth now happens for
exactly one reason: a sequential consumer that never idles, on a quiet
transport.

With it gone, the adaptive policy records **zero grows on every paced
workload that is not at the link ceiling** -- it is pin128 for those --
while still growing twice for a greedy sequential reader:

| | adaptive | pin 128 KB | pin 512 KB |
|---|---|---|---|
| seq, greedy, cold | **22.52 MB/s** | 16.48 | 20.26 |
| play 3072 | 0.00% | 0.00% | 0.00% |
| demux paced 3072 | 0.17% | 0.41% | 1.02% |
| streams x8 paced 1536 | 0.26% | 0.11% | 1.00% |

**One workload is not tuned for and cannot be.** Eight paced streams
demanding the whole link miss between 0.5% and 33% of their deadlines in
every arm -- pinned or adaptive, small granule or large:

| arm | per-run missed, pooled over both sweeps |
|---|---|
| pin 128 KB | 2.30%, 3.48%, 0.52%, 33.12% |
| pin 512 KB | 19.31%, 26.92%, 3.43%, 1.68% |

A consumer asking for 100% of a link is unstable however it is fetched, and
granularity does not control it. An earlier reading of two runs per arm made
the large granule look eight times worse there and it was written up that
way; the second sweep put the same arm at 2.56% and the claim was withdrawn.
Two runs of a workload whose spread is 0.5% to 33% is not a measurement.

That row is also the one place the slack rule misfires, and it is worth
being exact about. Eight paced streams run only 3 to 8 fetches in flight --
they idle 92% of the time, so they are nowhere near the 21-28 a greedy
eight-stream load puts up -- which straddles `READ_AHEAD_ADAPT_QUIET_DEPTH`.
When they starve at the ceiling they stop idling, and the policy grows: 8.5
times a run, against zero on every other paced workload. This is exactly the
failure predicted when slack was first measured -- a player that cannot keep
up looks like a copy -- and depth cannot separate the two here, because a
lone copy runs 2.7 to 4 in flight and these run 3 to 8.

It does not show up as harm: adaptive missed 10.07% on that row against
pin128's 16.82% and pin512's 2.56%, all three inside the same 0.5-33% spread.
So it is recorded rather than fixed. Fixing it needs a signal that separates
one starving consumer from one greedy one, and neither slack nor depth is
that signal.

### Reaching the ceiling, without paying for it anywhere else

Growth stopped at 512 KB, which is where a sequential reader stopped too.
`ReadAheadMaxGranularityKb` makes that bound configurable, and sweeping it
one boot and one cold file per arm -- the Windows cache survives a counter
reset, so any workload after the first on a file measures Cc rather than
this driver:

| cap | seq MB/s | grows |
|---|---|---|
| 512 KB | 23.05 | 2 |
| 1 MB | 27.34 | 3 |
| 2 MB | **28.38** | 4 |
| 4 MB | 27.91 | 5 |

4 MB buys nothing, so 2 MB is the default now.

**This was written up as reaching 99.8% of the link and that was wrong.**
The 28.38 came from the guest in one sweep; the 28.45 "ceiling" came from
the host in a different session. The path to the backend is WiFi -- an
Intel AX200 at a 649 Mbps link rate, 2 ms away -- so the ceiling is a
variable medium, and comparing two numbers taken hours apart says nothing.
That is the same cross-session error this document warns about twice
elsewhere.

Measured properly, by alternating the driver and a usermode client within
one session so each pair sees the same medium, on a different cold file
each time:

| pair | driver | usermode | ratio |
|---|---|---|---|
| ep 01 | 20.74 MB/s | 25.31 | 0.82 |
| ep 02 | 19.83 | 23.94 | 0.83 |
| ep 03 | 18.87 | 23.82 | 0.79 |

**The driver reaches about 0.8 of what a usermode client gets on the same
link at the same moment**, consistently across three pairs. The granule
sweep above is still valid -- it was internally consistent, one boot per arm
-- and 2 MB is still the right cap. But the remaining gap is roughly 20%,
not nothing.

The shape of it is worth stating because it is not mysterious. The usermode
client issues ONE request for the whole file and pays time-to-first-byte
once. The driver pays it per granule: at 2 MB and ~25 MB/s that is an 80 ms
body behind a ~10 ms ttfb, about 11% overhead, which is the right order for
the gap. Closing it means overlapping a fetch's ttfb with the previous
fetch's body rather than making the granule larger -- 4 MB already showed no
gain.

The check that matters is not the sequential column but the other two: the
sparse demux pattern fetched **223 MB at every one of those caps** and the
random pattern **85 MB**, both with zero grows. A higher ceiling is
unreachable for them because they never satisfy the streak condition, so
raising it cannot cost them anything.

Confirmed against the previous default across every workload, each on its
own cold file:

| workload | 512 KB cap | 2 MB cap | fetched |
|---|---|---|---|
| seq, greedy | 19.91 MB/s | **26.59 MB/s** | 466 MB both |

| play 3072 | 0.00% missed | 0.00% missed | 76 MB both |
| demux sparse, stride 1024 | 10.77 MB/s | 10.82 | 223 MB both |
| rand 64 KB buffered | 10.30 MB/s | 10.39 | 85 MB both |
| streams x8 paced 3072 | 2.62% missed, 144 ms | 1.23%, 95 ms | -- |

### Amplification, and why it stopped being a problem

The granule was once a constant, and a constant large enough for sequential
reads cost the demux pattern 27.5x amplification -- 512 KB fetched to serve
a fraction of it. That is what made the trade look unavoidable.

It is not a trade any more, because the large granule is no longer a
constant. 128 KB is the floor every file starts at, and the only reader that
ever leaves it is a sequential one that never idles. Measured against
consumption, on cold files:

| pattern | fetched | consumed | amplification |
|---|---|---|---|
| sparse demux, 64 KB blocks, 1024 KB stride | 223 MB | 187.5 MB | 1.19x |
| random, 64 KB blocks | 85 MB | 93.75 MB | 0.91x |
| sequential, greedy | 466 MB | 465.6 MB | 1.00x |

Random reads fetch *less* than they consume, because some of what they ask
for is already resident. Nothing here is above 1.2x, and none of it moved
when the growth ceiling was raised four-fold.

### What made the ceiling portable: asking Cc, not the clock

Both rate-based tuners failed on the same thing -- an 11% marginal effect
cannot be seen through 30% noise. The signal that works is not a rate at
all.

Cc does not read ahead in whatever size it is told. It caps somewhere of its
own choosing, around 1.1 MB here, and the cap sweep shows it plainly: the
same 465 MB file was fetched in 418, 413, 421 and 412 requests at ceilings
of 2, 4, 8 and 16 MB. Four ceilings spanning 8x, and the request count does
not move. Everything above ~1 MB was inert.

So growth now stops when Cc declines to honour the granule -- the largest
paging read seen in the window against the granule asked for. Two byte
counts, no clock, no noise:

| | cap 2 MB | cap 16 MB |
|---|---|---|
| seq, ratio to usermode | **1.01** | **1.01** |
| seq grows / shrinks | 4 / 0 | 5 / 0 |
| play 3072 missed | 0.09% | 0.00% |
| demux sparse fetched | 223 MB, 0 grows | 223 MB, 0 grows |
| rand 64 KB fetched | 85 MB, 0 grows | 85 MB, 0 grows |
| streams x8 paced missed | 1.58% | 1.94% |

An eight-fold change in the configured bound moves the ratio not at all, and
the grow count by one step instead of the three the fixed ceiling took
(4, 5, 6, 7 at 2, 4, 8, 16 MB). The ceiling is now mostly a safety bound
rather than the operative limit, which is what portability means here: on a
system where Cc reads further, the policy follows it, and on one where it
reads less, the policy stops earlier -- without either being configured.

It is not perfectly bound-independent. The test uses the largest paging read
in the window, which is permissive: one oversized read admits another
doubling, which is why 16 MB reached five grows and 2 MB four. Both landed
at ratio 1.01, so the extra step cost nothing here, and a stricter statistic
would be tuning against a single environment again.

**Amplification cannot regress through this, and not by luck.** Growth
requires a greedy consumer AND sixteen consecutive exactly-adjacent reads.
A seeking reader has neither, which is why demux and random fetched
byte-identical totals with zero grows at every ceiling tried across the
whole sweep series. The new gate only narrows growth further, so it cannot
reach them.

### A self-tuning ceiling: built twice, measured, reverted

The growth ceiling is a constant fitted to one link, and where a sequential
reader stops gaining is a property of the link rather than of this driver.
2 MB suits a 2 ms, ~28 MB/s WiFi path; a 50 ms path needs a granule several
times larger before time-to-first-byte is amortised at all. So the ceiling
should be measured, not configured.

It was built that way: after each doubling, the next window's rate is
compared against the window before it, and a doubling that fails is stepped
back, ending the climb for that file. Rates compare as `bytes1*ticks2`
against `bytes2*ticks1`, exact in integers with no counter frequency needed.

**Both versions regressed sequential throughput and neither is in the tree.**

| | seq MB/s | ratio to usermode | grows | shrinks |
|---|---|---|---|---|
| fixed 2 MB ceiling | ~32 | 1.02 | 4-7 | 0 |
| keep only if 10% better | 18.20 / 18.89 | 0.68 / 0.76 | 1.0 | 1.0 |
| revert only if 10% worse | 19.21 / 23.87 | 0.77 / 0.80 | 1.5-2.0 | 1.0 |

The first version had the test backwards, and the cap sweep says why: 512 KB
to 2 MB is two doublings worth 23% together, so the marginal gain is about
11% per step. Requiring more than 10% improvement per doubling, against a
link whose own noise is tens of percent, fails every trial on its first
judgement -- growth stopped after one step, 40% down.

Inverting it to stop only on measured harm should have been safe, because
its failure mode is climbing to the bound, which is what the fixed ceiling
already does. It still settled after one or two steps, because a 10% dip
between adjacent windows happens by chance: the same configuration measured
14.36 and 24.07 MB/s in consecutive rounds.

**The conclusion is about the signal, not the threshold.** Per-window
throughput cannot resolve an 11% marginal effect on a medium that moves by
30%, and no margin fixes that -- a threshold loose enough to survive the
noise is loose enough to never fire. Locating the knee needs either a stable
link or a signal that is not end-to-end throughput.

The ttfb/body ratio was the obvious alternative and does not work either.
It stays near 2.5 whether the granule is 512 KB or 2 MB, because
time-to-first-byte grows with the granule too -- concurrent fetches queue
behind each other's bodies, so the thing that would signal "stop growing"
scales with the growth.

What this leaves is an honest constant with its provenance recorded:
2 MB, measured on this link, with the sweep that chose it and a registry
override (`ReadAheadMaxGranularityKb`) for a link where it is wrong. The
work is in `git stash` if a stable link ever makes the measurement possible.

### Pipelining the receive: built, measured, reverted

The phase split says a fetch spends about 9.5 ms before its first byte
against a usermode client's 5.3 ms, and the obvious suspect was ordering:
the receive was posted from the send *completion*, so a send-completion DPC
stood between a response already sitting in the socket buffer and anyone
asking for it. Posting the receive as soon as the send was issued should
have removed it.

It did not. Measured with a runtime switch so the two orderings could
alternate with a reboot each -- the only comparison shape that has held up
on this host:

| | ttfb | fetch | MB/s | reads over a frame |
|---|---|---|---|---|
| receive posted at send-issue | 9.54 ms | 20.38 ms | 16.28 | 0.498% |
| receive posted at send-completion | **8.96 ms** | **19.48 ms** | **17.64** | **0.448%** |

Consistently worse across both interleaved pairs, and the `wait` phase --
the one it exists to shrink -- went up rather than down (7.93/9.61 against
7.23/9.19). A pre-posted receive that does not reduce time-to-headers means
the send-completion DPC was never what the request was waiting on. Reverted.

Two things worth keeping from it. The sandbox caught a use-after-free in the
first version within one run: posting the receive *before* the send reads as
tidier and is wrong, because a receive can complete synchronously -- the
scripted peer does, and WSK may -- running the request to completion and
freeing the context before the send is issued. ASan named the line, and 23
allocation-failure scenarios failed beside it.

And if this is ever attempted again, the invariant that made two outstanding
operations safe without reference counting the whole state machine was
ownership: from the moment the receive is posted it owns the context, and
the send completion may only stamp a timestamp and record a status -- never
fail, retry, complete or kick. That is cheaper than a refcount and it is
checkable by reading one routine.

The gap it was aimed at turned out not to exist. The 9.5 ms against 5.3 ms
compared this driver at a depth of ~2.7 against a serial usermode client;
at matched depth the usermode client is slower. Ordering was excluded by
the measurement above, and the premise was withdrawn by the one before it.
Pipelining was aimed at a target that was not there, which is why it moved
nothing.

### Splitting the fetch: built, measured, removed

Splitting one large fetch into concurrent range requests over partial MDLs
was built to get a short stall and a large granule at once. It measured
worse than not splitting, at every factor tried: mean fetch latency rose
from 46.1 ms to 51.2 ms at four slices, and to 50-57 ms at two.

The connection counters exclude the obvious explanation -- 100% pool reuse,
no fresh connects, no retries, no timeouts -- and the arithmetic gives the
real one. 51.2 ms across four slices is about 12.8 ms each, which is serial
rather than concurrent. This guest has two processors, and four
simultaneous WSK receives each running a completion and an HTTP parse
cannot overlap on them. Concurrency is not a free lever here, and the code
was removed rather than kept as something that costs requests and returns
nothing.

Recorded because the idea is a natural one to have again, and because
READ_AHEAD_PARAMETERS.PipelinedRequestSize in ntifs.h describes exactly
this and would invite it. On a guest with more processors the answer may
differ; on this one it does not.

The same section previously claimed the tail was a reader colliding with an
in-flight read-ahead and inheriting its latency, on the grounds that the
worst application read tracked the worst speculative fetch. That was two
constants sharing a floor -- the maximum sits at 58-66 ms in every
configuration, including with read-ahead suppressed outright -- and it is
withdrawn. The count of reads over a frame is what discriminates; the
maximum never did.

### What it is not

Tested and dead, recorded so they are not re-proposed:

- **Not the MKV Cues index.** The theory was that each seek drags a read to
  the index at the end of the file. `end-of-file` reads: **0**.
- **Not metadata.** The theory was that the player re-opens or re-stats on
  seek. `creates` and `file info`: **0 and 0**.
- **Not decode.** See the local control above.
- **Not seeking.** Twenty-five scripted seeks at two second intervals, the
  rate the reporter used, produced zero reads over a frame interval.

An earlier version of this section concluded that seeking was the cause, on
a 1:1 seek-to-stall figure taken from a paced *simulation* that issued
synchronous 64 KB reads and blocked on each. Media Player reads ~262 KB
asynchronously behind its own buffer, and paging reads stay 100% sequential
across 25 seeks. The simulation exhibited the problem it was built to look
for.

### How to measure this, and four ways to get it wrong

Every one of these cost a wrong conclusion before it was understood.

**Driver-side latency is not what a user feels.** The cache manager sits in
front of all of it and exists to hide exactly those numbers -- driven at
playback rate the driver was issuing 67 ms fetches while the reader saw
0.11 ms. `UserReadLatency*` in `BLORGFS_STATISTICS` is the number to read;
the chunk-fetch block explains it but is not it.

**Most application reads never become an IRP.** Buffered synchronous reads
of a cached file are served by fast I/O, so timing `IRP_MJ_READ` produced
one sample against a reader's 780. `FastIoRead` is wrapped
(`BlorgFastIoRead`) for this reason: `FsRtlCopyRead` is where a caller
blocks, because it calls `CcCopyRead` inline.

**Validate instrumentation against something independent before trusting
it.** The user-read timer was checked against a usermode reader measuring
itself on the other side of the syscall boundary: 780 samples against 780,
max 31.174 ms against 31.4 ms. Without that step its first version -- which
saw one read in 780 -- would have looked like a finding.

**A simulation of a player is not a player.** Read pace, read size, and
whether reads are synchronous all change the answer, and a reader written
to exhibit a hypothesis will exhibit it. Drive the real application:
`PerfHarness reset`, run it, `PerfHarness stats`.

Two further traps already paid for: reads at full speed measure read-ahead
working rather than playback feeling slow, so pace a simulated reader at
the real bitrate; and every `*MaxUs` field was summed across processors
rather than max-reduced until 2026-08-28, so any maximum taken from this
driver before then is inflated by roughly the processor count.

**A workload that cannot exhibit the effect will report that there is no
effect.** Four candidate workloads were tried against read-ahead
granularity before one could see it, and each was rejected on its own
measurement rather than on argument: unbuffered random (amplification
1.000, Cc is not in the path), buffered random (1.000, read-ahead is
pattern-triggered and never arms), a demuxer model whose cursors advanced
by their own block size (1.002 at 99.8% sequential -- `seq` in disguise,
read-ahead working perfectly), and the same model with a large stride but
no burst (2.07x at 1% sequential, too scattered to arm anything). Sweeping
any of the four would have produced a flat line and the confident wrong
conclusion that granularity does not matter. What reproduces the captured
trace is `demux`: several cursors, each reading a short adjacent burst then
skipping a stride, which is what a container's interleaved tracks look like
from one file object. The driver's own sequential test is exact adjacency,
so the trace's 70.4% sequential is itself the evidence that the real
pattern contains runs, not scattered reads.

**Calibrating on one statistic is not validation.** That `demux` model was
tuned until its amplification matched the trace (24.8x against 22x) and its
sequential share was close, and the match was treated as licence to sweep.
It reads in 4 KB blocks; the player issues ~262 KB. Matching one number
while missing another by two orders of magnitude is the same class of error
as matching fetch latency and calling it what the user feels.

**Compare shares, not counts, across runs of different length.** Runs with
different file sizes or durations produce different read totals, so an
absolute count of reads over a frame ranks the longest run worst.
`UserReadsOverFrameShare` exists for this. It is already a percentage --
`SafeRatio` multiplies by 100 -- so multiplying again yields impossible
values above 100% and, worse, preserves the ordering while destroying the
magnitude.


## TODO: on-disk hot cache

The prefetch ring was removed rather than replaced. What follows is the
design for its replacement, which is **not an in-memory prefetcher** -- that
experiment is finished and its evidence is in git history. Nothing in this
section is implemented yet.

**It lives in the driver, as a module.** A usermode helper owning the store
was the other candidate, on the argument that it removed a re-entrancy
problem by construction. That argument was weaker than it looked: keeping
the store off this volume removes the recursion just as completely, since
nothing in NTFS's completion path calls back into this driver. What is left
is deadlock through the memory manager, which is a discipline problem the
helper would not have solved either -- it is the same problem every
filesystem has when it touches another one.

### Why this, and not more lookahead

Measured on the reference rig, 16 concurrent streams, 512 KB range GETs,
interleaved against a usermode HTTP client:

| | throughput |
| --- | --- |
| network path ceiling, cold or warm | ~30 MB/s |
| driver, cold | 27-30 MB/s (0.93-1.01x the usermode client) |
| driver, warm in the Windows cache | **4800-6800 MB/s** |
| local guest disk, write / read | **601 MB/s / 4.3 GB/s** |

Cold reads already sit at the link ceiling, so nothing on the fetch path can
add throughput: every delivered byte has to cross a ~30 MB/s wire. The only
way past it is to **not cross the wire**. RAM caching already demonstrates
the payoff and is bounded by RAM -- at four concurrent streams the working
set stopped fitting and a re-read fell from 6677 MB/s to 131 MB/s. Local
disk is ~20x the network on write, ~140x on read, with ~100x the capacity of
RAM.

### Architecture: a driver module

`src/DiskCache.c` / `DiskCache.h`, owning a block store in **one ordinary
file on an ordinary live volume**. Location and maximum size configurable
through the registry alongside `RemoteHost`/`RemotePort` in `Parameters`.

The module boundary is deliberately narrow, and every entry point is either
pure memory or explicitly asynchronous:

```
BlorgDiskCacheInitialize / BlorgDiskCacheDrain   startup, unload
BlorgDiskCacheLookup(FileId, BlockIndex)         IN MEMORY ONLY, no I/O
BlorgDiskCacheReadAsync(Slot, Mdl, Completion)   serves a hit
BlorgDiskCacheAdmitAsync(FileId, Block, Mdl)     fire-and-forget write-behind
BlorgDiskCacheInvalidate(FileId)                 validator changed
```

`BlorgDiskCacheLookup` touching no I/O is what makes the rest safe: the read
dispatch path can ask "is this cached?" while holding whatever it holds, and
only then decide which asynchronous path to take.

**The read path keeps the shape it already has.** A paging read today
returns `STATUS_PENDING` and is completed later from a network completion
(`Read.c`). A cache hit is the same shape with a different source:

1. `BlorgDiskCacheLookup` — in-memory index, no I/O, no blocking.
2. **Hit**: queue a cache read; a worker fills `Irp->MdlAddress` and
   completes the IRP.
3. **Miss**: issue the HTTP fetch exactly as now. On completion, complete the
   IRP *first*, then queue the write-behind from the buffer already in hand.

The reader never waits on the cache in either direction. A miss costs
nothing it did not already cost, and a write-behind failure is invisible.

### What the store being off-volume does, and does not, buy

**The store must not live on this volume**, checked at open by comparing the
target's volume device object with ours. That single rule is what removes
*recursion*: with the store on NTFS, nothing in the completion path of a
`ZwReadFile` calls back into BlorgFS. There is no cycle in the call graph,
and the usermode-helper alternative bought nothing here that this check does
not.

What is left is not recursion, and calling it that obscures the actual
risks. Two remain, both mediated by memory manager:

- **Deadlock through MM, not a nested call.** If a thread holds an FCB
  resource and, inside a cache read, memory pressure makes MM trim that
  file's pages, MM calls this driver's `AcquireForLazyWrite` and blocks on
  the resource the thread is still holding. So: **never hold an FCB resource
  across a cache call.** Enqueue, release, return pending.
- **The paging path is the dangerous one.** A paging read can originate from
  MM while it is already short of memory. Dependent I/O issued from that
  thread can wait on the reclaim that is waiting on us. So: **no cache I/O
  on the calling thread** -- all `ZwReadFile`/`ZwWriteFile` happen on the
  module's own PASSIVE workers, and the dispatch path only ever enqueues.

That second rule costs nothing the design was not already paying. The IRP is
completed asynchronously either way, so moving the I/O to a worker changes
which thread finishes it and nothing else.

**IRQL** is a hard constraint rather than a judgement call: `ZwReadFile` and
`ZwWriteFile` are PASSIVE-only, while network completions run at
`<= DISPATCH`, so an admit queued from a completion reaches PASSIVE through
a work item. That is the same rule the removed prefetcher lived by, and the
one thing from it worth keeping.

**Open the store `FILE_NO_INTERMEDIATE_BUFFERING`**, for two reasons that
are worth stating accurately. It avoids double-caching bytes Cc already
holds for this volume, and it keeps the store from adding cache-manager
memory pressure at exactly the moment the driver is serving a paging read.
The cost is sector alignment, which a fixed-block store gives for free.

An earlier draft of this section justified the flag by claiming that
cache-manager pressure from our own store could re-enter this driver through
its own cache callbacks. That is not true for a store on another volume, and
the flag is worth setting anyway for the two reasons above.

### Store layout, index and recovery

Fixed-size blocks, each preceded by its own header: magic, file-identity
hash, block index, backend validator, byte length, and the MAC below. Slots
are addressed by index, never by cluster or LCN, so fragmentation, extension
and defragmentation are all transparent.

**The index is rebuilt from the block headers at startup, not persisted.**
A separate index file is faster to load and introduces a whole failure class
this does not need — an index that disagrees with the store, torn across a
crash, and confidently wrong. Header scan cannot desync because the headers
*are* the store. At 512 KB blocks a 30 GB store is ~61k headers; reading
only the header of each is a few hundred MB against a local disk measured at
4.3 GB/s, so a second or so of startup, off the mount path.

Persisting an index is a later optimisation, and only worth it if that
startup cost ever shows up as a complaint.

### Concurrency

- Slot allocation from a free list under a leaf lock; nothing else is
  acquired under it.
- Per-slot reference count so eviction cannot reclaim a slot with a read in
  flight -- the same protocol the node table already uses, and the one the
  systematic scheduler is set up to explore.
- A per-block "fetch in flight" marker so two readers missing the same block
  do not both fetch it and both write it.

### Cluster pinning: not worth it

`FSCTL_MARK_HANDLE` with `MARK_HANDLE_PROTECT_CLUSTERS` marks a file so the
defragmenter will not move it. **The conclusion is not to use it.** The
intuition that this is over-engineering is right, and for reasons stronger
than "SSDs do not care about seeks":

- It solves a problem this design does not have. Protection matters when
  something maps a file by LCN and needs that mapping to stay valid --
  hibernation files, page files, block-level VM disks. This store is
  addressed through the filesystem by offset, so a moved extent is
  transparent.
- It is NTFS-only and volume-specific, and the cache is meant to live
  wherever the user points it.
- Marking a large file unmovable is antisocial: it permanently constrains the
  volume's own defragmenter on behalf of a cache that is by definition
  disposable.
- The cost it avoids is seek cost, which on SSD is near zero and on spinning
  media is still small against the ~30 ms network fetch it is competing with.

Revisit only if profiling ever shows extent-map lookup -- not seek time --
dominating cache reads, which would be a surprise.

### Security model

**Cache integrity is a security boundary.** A process that can write the
store can inject bytes this filesystem then serves as authoritative file
content: a straightforward data-poisoning primitive against every reader of
the share.

- **ACL the store to SYSTEM and Administrators only**, deny everyone else,
  and create it with an explicit security descriptor rather than inheriting
  the parent directory's. A cache under a user-writable path with inherited
  ACLs is the default-insecure outcome to avoid.
- **Refuse a store whose ownership or ACL is not what was expected**, at
  open, rather than repairing it -- repairing races the attacker.
- **ACLs alone are not sufficient.** They do not cover an offline attack
  (booting another OS, mounting the volume elsewhere), an administrator-level
  compromise, or ordinary corruption. Blocks must carry their own integrity
  check.
- **Per-block keyed integrity, verified before use.** Each block records a
  MAC over (file identity, block index, backend validator, contents). A
  mismatch discards the block and falls back to the network. A plain
  checksum detects corruption but not tampering, and the threat here is
  tampering.

  The key has to persist for the cache to survive a reboot, and it has to
  live somewhere the store does not -- otherwise an attacker who can rewrite
  blocks can recompute the tags and the MAC proves nothing. The service's
  own registry key is the natural home: same trust boundary as the driver's
  configuration, already SYSTEM-only, and already what `TlsPin` uses
  (`Driver.c`). Generate it on first use, never log it, and treat a missing
  key as an empty cache rather than an error.

  Running in kernel does not change the threat model here. The attacker of
  interest is a process that can write the file, not one that can call the
  driver -- and MAC verification happens on the module's worker before any
  cached byte reaches an IRP, so a forged block is discarded on the same
  path that would have discarded a corrupt one.
- **Bind blocks to a backend validator.** `server-rs` returns `etag` and
  `last_modified`; a block whose validator does not match the current
  response is stale and must not be served. Without this the cache serves
  yesterday's bytes for a file that changed.
- **Tampering, truncation or wholesale replacement** must be
  indistinguishable in effect from a cold cache: verification fails, blocks
  are discarded, reads go to the network.

The bar is explicit: **the cached path must not be weaker than the uncached
path.** A design step that cannot meet that does not ship.

### Admission and eviction: start small

The literature is consistent that the largest wins come from **admission**
control rather than clever eviction, and that the specific thing to avoid is
caching one-hit-wonders -- for a disk-backed store that is wasted write
bandwidth and, on SSD, wasted endurance.

- **Admit on second miss, not first.** This is the CDN answer to one-hit
  wonders, costs a few bytes of state per candidate, and is the single
  highest-value policy decision available.
- **Prefer sequential streams.** The `READ_STREAM_TRACKER` array on the FCB
  (`Structs.h`) already carries the streak -- it survived the prefetch
  removal partly for this. A streaked reader is exactly the case where the
  following blocks are worth having.
- **Evict with segmented LRU.** Cheap, well understood, and resistant to one
  large scan flushing the whole store.
- **Do not start with TinyLFU/W-TinyLFU or ARC.** W-TinyLFU is the strongest
  general result in the literature and is the right thing to *grow into* if
  measurement justifies it; its frequency sketch costs about a page. But it
  earns its keep on skewed, high-cardinality, small-object workloads -- CDN
  edges, key-value caches -- and this workload is a handful of very large,
  sequentially-read files. Second-hit admission plus SLRU captures most of
  the benefit at a fraction of the complexity, and the counters will show
  whether anything more is warranted.

Measure hit rate, bytes served from cache, and write amplification before
tuning any of it.

Sources: [TinyLFU (ACM ToS)](https://dl.acm.org/doi/10.1145/3149371),
[size-aware admission for CDN memory caches (CMU)](http://reports-archive.adm.cs.cmu.edu/anon/2016/CMU-CS-16-120.pdf),
[CacheSack: admission optimization for Google datacenter caches (USENIX ATC 22)](https://www.usenix.org/system/files/atc22-yang-tzu-wei.pdf),
[FSCTL_MARK_HANDLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ni-ntifs-fsctl_mark_handle),
[Defragmenting Files](https://learn.microsoft.com/en-us/windows/win32/fileio/defragmenting-files).


## Deploying to the VM

**See `deploy\README.md`** — the full pipeline, the two-step INF install this
primitive driver requires, the staged-vs-repo-root INF hash trap, `vmrun`
argument/password quirks, and KDNET connection notes all live there. That
document is deliberately tool-agnostic; keep new deployment findings in it
rather than here.

The one rule worth repeating: **never deploy by copying `.sys` into
`System32\drivers` by hand.** It skips catalog validation and can leave a
stale binary in place, so what you debug is not what you built. Use
`deploy\Deploy-ToVM.ps1`.

## What is expected of you here

This is a kernel filesystem driver on a hot path, so the bar is higher than
"it compiles and the tests pass":

- **Scrutinise for correctness and performance, actively.** Do not wait to be
  pointed at a bug. Read the surrounding code when you touch a file and
  question it — the double-counted `NonCachedReads`, the vendor IOCTLs that
  could never route, and the unload that never ran were all found by looking,
  not by being asked.
- **Prefer optimisations that also simplify.** The best change removes code
  and instructions at the same time. Collapsing two branches that test the
  same predicate, deleting a redundant pass, replacing pointer-chasing with a
  flat array — these are wins twice over. A "fast" change that adds
  complexity needs to justify itself with a measurement.
- **Fix style inconsistencies as you find them.** The conventions below are
  opinionated and deliberate. Code that drifts from them is a defect; correct
  it in passing rather than matching the drift.
- **Measure before and after** anything performance-related. `Statistics.h`
  counters are always on, `PerfHarness` reports them, and
  `tools\Compare-BlorgMetrics.ps1` diffs against a baseline. An unmeasured
  performance claim does not land.

## Changing behaviour: the full pass

A behaviour change is not done when the code works. Every one of these, in
the same pass:

1. **Update the comments that described the old behaviour.** The header
   comment above the function, the struct-field comment, the file-header
   contract — whichever described what you just changed. A stale comment
   actively misleads, which is worse than silence.
2. **Add or update a regression test, in the layer that can actually catch
   it.** This is the part most often got wrong. The statistics IOCTL bug
   passed every existing dispatch test because they call
   `BlorgDeviceControl` directly and so never exercise the I/O manager's
   routing decision — the only test that could catch it asserts on the
   `CTL_CODE` device type itself. Ask what layer the bug lives in, and put
   the test there: crypto vectors for `Tls.c`, the usermode sandbox for
   `Client.c` logic, the kernel rule-model for IRQL/lock-order, the
   systematic scheduler for interleavings, `Test-BlorgCorrectness.ps1` for
   anything only the real driver plus real backend can show.
3. **Comment the test too**, to the same standard — say what it is defending
   against and why the obvious cheaper test would not catch it.
4. **Adjust the documentation files.** `README.md` for build/test/convention
   changes, `deploy\DEBUGGING.md` for anything about diagnosing the running
   system, `deploy\README.md` for deployment mechanics.
5. **Run `-Tier Fast`** before calling it done.

## Documentation discipline

Most of the expensive problems in this project were diagnosed once, in a
session that then ended. The write-ups are the only reason the next person
does not pay for them again — so treat them as part of the work, not an
epilogue.

**Where findings go.** Environment, VM and debugger findings belong in
`deploy\DEBUGGING.md`; deployment mechanics in `deploy\README.md`; build,
testing and convention material here. Keep them out of any single tool's
memory so they stay useful to whoever comes next.

**Write down what was *not* true, not just what was.** A ruled-out
hypothesis is worth as much as the root cause, because it is the one the
next person will otherwise re-derive from scratch. `DEBUGGING.md` records,
for example, that a wedged unload is *not* the drain code and *not* a
refcount leak — both plausible, both wrong, both already checked.

**Fix staleness in the same pass as the change.** A document describing code
you just altered is now wrong, and a confidently wrong document is worse
than none — it sends the next person down a path that no longer exists. When
you change behaviour, grep the docs for what described it and correct them
before you call the change done:

```bash
grep -rn "<symbol-or-behaviour>" README.md CLAUDE.md deploy/
```

Two real instances: the `KdBreakPoint` calls documented in
`deploy/README.md` were removed but the paragraph describing them survived;
and a memory entry carried a confidently-argued but incorrect root cause for
the statistics IOCTL (a minifilter theory, proposing an architectural device
split) that would have sent the next agent on a substantial and unnecessary
refactor. Delete or correct superseded conclusions outright rather than
leaving them to be weighed against the truth.

**Prefer evidence to assertion.** Record the command and its output — the
`ln <rip>` that identifies a breakpoint, the counter that read exactly 2x,
the `x BlorgFS!...` that proved a function never ran. A claim someone can
re-verify in one command survives; a bare conclusion does not.

## Conventions

These are opinionated and consistently applied. Match them exactly, and
correct drift when you find it.

### Naming and layout

- **Parameters are `PascalCase`; locals are `camelCase`.** This is the one
  people get wrong most often, and it is load-bearing for readability: at any
  line you can tell what came from the caller and what is yours.
  `static NTSTATUS HttpParseHeaders(HTTP_CONTEXT* Ctx)` with
  `SIZE_T bodyOffset = Ctx->BodyOffset;` inside.
- **Functions are `PascalCase` with a module prefix, and the prefix says
  whether the name leaves the file.** A file-static helper takes the bare
  module name (`ReadClaimStream`, `HttpFail`, `TlsHandshakeFail`). Anything
  with external linkage takes `Blorg`, with no exceptions for layer or
  ancestry -- `BlorgSendWskAsync`, `BlorgTlsSha256`, `BlorgFspDispatch`,
  `BlorgVolumeRead`. The fastfat-inherited names (`FsdPostRequest`,
  `PrePostIrp`, `OplockComplete`) were renamed along with everything else;
  they are this driver's functions now, not the reference implementation's.

  So `grep -n '^[A-Za-z].*Blorg'` is the export list, and a bare module
  name at file scope is a promise that it is `static`.
- **File-scope statics are `PascalCase` with the module prefix**
  (`SocketMaxPoolSize`, `HttpActiveRequests`). Driver-wide mutable state
  lives in the `global` struct rather than as loose globals — prefer adding
  a field there to introducing a new one.
- **Struct fields are `PascalCase`; macros and constants are
  `SCREAMING_SNAKE`** (`READ_AHEAD_GRANULARITY`, `SOCKET_CONNECT_TIMEOUT_MS`).
- **Empty parameter lists are `(VOID)`, never `()`.**
- **Allman braces**, including on `switch` cases, which get their own braced
  block. Every `if` gets braces, even single-statement ones.
- **Casts go through `C_CAST`,** not bare C-style casts.
- **Guard clauses and early returns** over nesting — the happy path stays at
  the lowest indentation level.

### Rules

- **Comments only at top-of-file, above a function, or beside a struct field.**
  Never inside a function body — naming carries the meaning there.
- **Padding:** never widen a field's type to satisfy `CHECK_PADDING_END`; add an
  explicit named `Reserved[N]` instead.
- **`ProbeForRead` for all user-buffer validation,** including output buffers.
  `ProbeForWrite` is legacy and writes every page.

  Two places deliberately do not probe, both following fastfat: the cached
  read path in `Read.c` hands `Irp->UserBuffer` to `CcCopyReadEx` under SEH,
  and `Security.c` lets `SeQuerySecurityDescriptorInfo` write into
  `Irp->UserBuffer` directly. In both the fault is caught rather than
  prevented. Do not "fix" either without reading why fastfat does the same.
- **Never `%wZ`/`%Z` in a `DbgPrint` that can run above `PASSIVE_LEVEL`** —
  the formatting touches paged code and bugchecks. In practice that means
  anywhere on a completion chain (`HttpFail`, `HttpComplete`,
  `Blorg*Complete`) or in anything one can call. It is fine, and used
  freely, on the PASSIVE-only dispatch paths.
- IRQL is load-bearing throughout the async paths; the file-header comments in
  `Client.c` and `Socket.h` state each path's contract. Read
  them before changing anything on a completion chain.
- **Data-oriented design.** Lay data out for how it is actually traversed —
  hot fields together, per-processor where contended, arrays over
  pointer-chasing. Shape the structure around the access pattern rather than
  around a conceptual object model.
- **Minimise syscalls and blocking calls.** Batch and amortise instead of
  repeating a call per item; prefer an async completion over anything that
  waits. A blocking call on a hot or completion path is a design bug, not a
  detail — it occupies a worker for the whole duration.
- **Collapse branches with overlapping or shared predicates.** If two
  conditions test the same thing, evaluate it once and branch once. Repeated
  or nested tests of the same predicate should be merged rather than left
  parallel — it is both faster and the only way the invariant stays obvious.
