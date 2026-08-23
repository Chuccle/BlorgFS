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
- **`inf2cat` "postdated DriverVer" is the clock, not your change.** `BlorgFS.inf`
  leaves `DriverVer` empty, so stampinf fills it from local time while inf2cat
  validates against UTC. Between local midnight and the UTC offset they
  disagree and catalog generation fails on a tree that compiled fine. The check
  script reports this as `CLOCK`, not `FAIL`. Confirm with
  `Inf2Cat.exe /driver:x64\Debug\BlorgFS /os:10_x64 /uselocaltime`.

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

`-ForBenchmark` is the whole answer: it deploys Release and clears Driver
Verifier, then reboots so the change actually applies. Use it for every
performance run. `-Configuration` still wins if given explicitly.

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

## TODO: on-disk hot cache

The prefetch ring was removed rather than replaced. What follows is the
design seam for its replacement, which is **not an in-memory prefetcher** --
that experiment is finished and its evidence is in git history. Nothing in
this section is implemented.

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

### Architecture

A block store in **one ordinary file on an ordinary live volume**, plus a
separate index. Location and maximum size configurable through the registry,
alongside `RemoteHost`/`RemotePort` in `Parameters`.

- **Block-granular, not whole-file.** A 6 GB film watched for ten minutes
  must not admit 6 GB. Fixed-size blocks with a per-file present-bitmap;
  evict blocks, not files.
- **No contiguity assumptions.** The store is addressed by block index
  through the filesystem, never by cluster or LCN, so it must survive being
  fragmented, extended, or moved.
- **Asynchronous both ways, never on the foreground path.** A miss issues
  the HTTP fetch immediately and completes the read from it; the write-behind
  into the store happens afterwards, and its failure is invisible to the
  reader. A hit reads asynchronously and, on any error or timeout, abandons
  the cache and falls back to the network.
- **Advisory only.** Corruption, truncation, eviction, a missing file, a full
  volume or an unreadable index all degrade to the authoritative path. There
  must be no state in which the cache can make a read fail that would
  otherwise have succeeded.

**The re-entrancy hazard is the main design risk**, and it is why this is a
seam rather than an implementation. The driver would be calling into another
filesystem from inside its own `IRP_MJ_READ`; synchronous cache I/O on that
path can recurse through memory pressure back into this driver. Two ways out,
to be chosen before any code is written:

1. A kernel-side store with strictly asynchronous, non-blocking I/O and a
   hard rule that no foreground read ever waits on it.
2. A usermode helper service owning the store, with the driver querying it.
   This removes the cross-filesystem hazard entirely, at the cost of an IPC
   round trip per lookup -- which, against a ~30 ms network fetch, is noise.

Option 2 looks better on risk against benefit and should be the working
assumption until something argues otherwise.

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
  mismatch discards the block and falls back to the network. The key is
  generated per store instance and kept somewhere the store is not --
  otherwise an attacker who can rewrite blocks can recompute the tags. A
  plain checksum detects corruption but not tampering, and the threat here
  is tampering.
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
