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

### Why the trade exists at all: 35% of a fetch is our own overhead

Everything above tunes *around* per-fetch cost. A large granule is only
better because it amortises a fixed cost over more bytes, so the size of
that fixed cost decides how sharp the trade is. Measured, most of it is
ours.

Decomposing a fetch (`FetchTtfb*`, `FetchBody*`, now in `--report`) against
the same backend, from the same guest, with a usermode HTTP client doing
128 KB keep-alive range GETs beside it:

| per fetch | driver, 196 KB avg | usermode, same bytes |
|---|---|---|
| ttfb | 10.10 ms | 5.33 ms |
| of which send | 0.88 ms | ~0 |
| of which wait on peer | 9.21 ms | |
| body transfer | 9.52 ms | ~7.5 ms |
| **total** | **19.61 ms** | **~12.8 ms** |

The driver's own measurable work is 12 us -- socket acquire 6 us, request
build 6 us -- so none of the gap is there. It is concentrated before the
first byte arrives: 1.9x on ttfb against 1.27x on transfer. The backend is
the same in both columns, so the extra is this driver's path to posting a
receive and observing the response, not the server thinking.

**That is where "no regression on every workload" is, and it is not in the
granule.** At usermode parity a 196 KB fetch would cost about 12.8 ms
instead of 19.6, which at the measured pipeline depth of ~2.7 is roughly 41
MB/s -- above what 512 KB achieves today -- while leaving every fetch far
inside the 41.67 ms frame budget, which is what the tail is made of. The
trade between throughput and stalls exists because fetches are expensive; it
shrinks as they get cheaper and would not need arbitrating at parity.

Candidates, in the order the measurement points at them: the receive is
posted only after the send completes, which adds a scheduling hop on a
two-processor guest; a fetch costs four pool allocations (context, receive
buffer, request buffer, encoded path) plus an IRP and an MDL per socket
operation; and the 0.88 ms send of a ~200 byte request is itself an order of
magnitude more than it should be. None of that has been attempted -- it is
recorded here because tuning the granule further is optimising the wrong
term.

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
