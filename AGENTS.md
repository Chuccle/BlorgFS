# AGENTS.md

Operating manual for coding agents working on BlorgFS, and the detailed
reference for human contributors too. Read [README.md](README.md) first for
what the project is and its repository layout; this document covers
everything needed to build, test, deploy, debug and modify the driver
correctly, plus the hard-won findings behind the current design.

Keep this to two markdown files — `README.md` and `AGENTS.md`. All agent
documentation and context, for any tool, lives here — don't create new
markdown files (a `CLAUDE.md` included) for findings; add a section here
instead (see "Documentation discipline" below).

**Start with the [Quick reference](#quick-reference) below** for the
condensed, current-state version of everything in this file — commands,
settings, and the top footguns, each linking to the full section for
detail and evidence. The rest of the document is organized rules-first,
reference-second: operating rules and conventions come early because they
apply to every task; the two large evidence sections (the read-ahead
investigation and the disk-cache design) are last because they are read
occasionally, not every session.

| Section | What it's for |
|---|---|
| [Quick reference](#quick-reference) | Condensed cheat sheet — read this first |
| [What is expected of you here](#what-is-expected-of-you-here) | The bar for changes in this codebase |
| [Changing behaviour: the full pass](#changing-behaviour-the-full-pass) | The checklist a behaviour change must clear |
| [Documentation discipline](#documentation-discipline) | Where findings go, and how to keep docs from going stale |
| [Conventions](#conventions) | Naming, style, and hard rules |
| [Build and test tiers](#build-and-test-tiers) | How to build and run the regression tiers |
| [Continuous integration](#continuous-integration) | What each CI workflow gates |
| [Sanitizers](#sanitizers) | ASan/KASAN requirements |
| [Deploying to a VM](#deploying-to-a-vm) | The deploy pipeline and its quirks |
| [Debugging the VM: what's real and what's noise](#debugging-the-vm-whats-real-and-whats-noise) | Decision tree for VM/debugger flakiness |
| [Measuring performance](#measuring-performance) | How to benchmark correctly |
| [Read-ahead policy: current state](#read-ahead-policy-current-state) | What the driver does today, and why, in one place |
| [Evidence trail: the playback-stutter investigation](#evidence-trail-the-playback-stutter-investigation) | Conclusions and reusable measurement lessons from the investigation behind that policy; full round-by-round history is in git log |
| [Future work: on-disk hot cache (not implemented)](#future-work-on-disk-hot-cache-not-implemented) | Design for a not-yet-started project — nothing in it exists in the codebase |

## Quick reference

Condensed, current-state facts. Each row links to the section with the
full detail and supporting evidence — read this first, go deeper only when
a task needs it.

### Commands

| Do this | Command | Detail |
|---|---|---|
| Build + test (default gate) | `powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast` | [Build and test tiers](#build-and-test-tiers) |
| Deploy to the dev VM | `.\deploy\Deploy-ToVM.ps1 -Configuration Release` | [Deploying to a VM](#deploying-to-a-vm) |
| Benchmark (Release, Verifier off) | `powershell -File deploy/Deploy-ToVM.ps1 -ForBenchmark` | [Measuring performance](#measuring-performance) |
| Accept new perf baseline | `powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Perf -PerfFile <path> -UpdateBaseline` | [Measuring performance](#measuring-performance) |
| KASAN driver build | `msbuild src\BlorgFS.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:EnableKASAN=true` | [Sanitizers](#sanitizers) |

### Always true

- **Run `-Tier Fast` before calling any change done.** Cheaper tiers don't
  run the crypto vectors that catch a `Tls.c` regression.
- **A behaviour change isn't done when the code works** — update the stale
  comment, add a regression test in the layer that can actually catch the
  bug, and update docs, in the same pass. See
  [Changing behaviour: the full pass](#changing-behaviour-the-full-pass).
- **Never create a new markdown file.** Findings go in a section of this
  file. See [Documentation discipline](#documentation-discipline).
- **Benchmark only on Release with Driver Verifier off**, and use
  `-ForBenchmark`. A Debug build and/or Verifier bias every number and have
  invalidated whole studies before. See
  [Measuring performance](#measuring-performance).
- **Never deploy by hand-copying `.sys` into `System32\drivers`.** It skips
  catalog validation and can leave a stale binary in place. Use
  `Deploy-ToVM.ps1`.
- **A guest reboot is mandatory between driver deploys.** `sc stop` wedges
  in `STOP_PENDING` — there is no dismount handler yet. See
  [Debugging the VM](#debugging-the-vm-whats-real-and-whats-noise).
- **Keep the kernel call tree flat and the stack usage small.** ~3 pages
  total, shared with everything the thread calls; running out is a
  bugcheck, not a recoverable error. See [Conventions](#conventions).
- **Data-oriented design; avoid false sharing; avoid atomics unless
  correctness requires them.** Lay data out for its access pattern
  (per-processor where contended), pad/align contended state to a cache
  line, and reach for a non-atomic or per-processor design before an
  `Interlocked*` op — this codebase has already removed a shared atomic
  gauge in favour of per-processor counters for exactly this reason. See
  [Conventions](#conventions).

### Top footguns (full detail in "Debugging the VM")

| Symptom | It usually means | Not |
|---|---|---|
| Guest looks completely frozen | `vmrun`/VIX host-side flakiness (often low host RAM), or a `KdBreakPoint()` trap in a **Debug** build | A real bugcheck — confirm via KD: is `System Uptime` advancing? |
| `sc stop BlorgFS` hangs in `STOP_PENDING` | No `IRP_MN_DISMOUNT_VOLUME` handler (known, unfixed) — reboot the guest | A refcount leak (already ruled out) |
| `B:\` gives "semaphore timeout period has expired" | Backend unreachable — the INF's default `RemoteHost` resolves to a dead host; use `10.0.50.17:8080` | A filesystem bug |
| Bugcheck `0x80 NMI_HARDWARE_FAILURE` | Known environmental false alarm (correlates with host memory pressure) — confirm bucket via `Get-CrashVerdict.ps1` | A BlorgFS defect |
| `cmd.exe` via `runProgramInGuest` hangs or returns exit 1 on a healthy guest | A known VIX quirk — drive the guest with `powershell.exe` only | A guest health problem |

### Read-ahead: current settings

Full derivation in [Read-ahead policy: current state](#read-ahead-policy-current-state).

| Setting | Value | Registry override |
|---|---|---|
| Starting granule | 128 KB (every file) | `ReadAheadGranularityKb` |
| Growth ceiling | 2 MB | `ReadAheadMaxGranularityKb` |
| Adaptive feedback loop | on | `ReadAheadAdapt=0` pins the granule |
| Slack-based growth | on | `ReadAheadSlackGrowth=0` disables it |

### Not implemented — don't assume it exists

The **on-disk hot cache is an unstarted future project** ([design
doc](#future-work-on-disk-hot-cache-not-implemented)). There is no
`DiskCache.c`/`.h` in the tree yet; nothing described there is live driver
behaviour.

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
4. **Adjust the documentation.** `README.md` for changes to what a human
   contributor needs — the project description, build/deploy invocations,
   layout. This file (`AGENTS.md`) for everything else: conventions, the
   deploy pipeline's mechanics, VM debugging, and performance methodology.
5. **Run `-Tier Fast`** before calling it done.

## Documentation discipline

Most of the expensive problems in this project were diagnosed once, in a
session that then ended. The write-ups are the only reason the next person
does not pay for them again — so treat them as part of the work, not an
epilogue.

**Where findings go.** There are exactly two markdown files in this repo:
`README.md` (a conventional, human-facing project overview) and `AGENTS.md`
(this file — build/test/deploy mechanics, VM and debugger findings,
performance methodology, and conventions). All agent documentation and
context, for any tool, lives in `AGENTS.md` — there is no per-tool file
(`CLAUDE.md` or otherwise). **Do not create new markdown files for
findings.** Add a section to `AGENTS.md` instead, so they stay useful to
whoever comes next regardless of tool.

**Write down what was *not* true, not just what was.** A ruled-out
hypothesis is worth as much as the root cause, because it is the one the
next person will otherwise re-derive from scratch. "Debugging the VM"
records, for example, that a wedged unload is *not* the drain code and
*not* a refcount leak — both plausible, both wrong, both already checked.

**Fix staleness in the same pass as the change.** A document describing code
you just altered is now wrong, and a confidently wrong document is worse
than none — it sends the next person down a path that no longer exists. When
you change behaviour, grep the docs for what described it and correct them
before you call the change done:

```bash
grep -rn "<symbol-or-behaviour>" README.md AGENTS.md
```

Two real instances: the `KdBreakPoint` calls documented in "Debugging the
VM" were removed but the paragraph describing them survived; and a memory entry carried a
confidently-argued but incorrect root cause for the statistics IOCTL (a
minifilter theory, proposing an architectural device split) that would have
sent the next agent on a substantial and unnecessary refactor. Delete or
correct superseded conclusions outright rather than leaving them to be
weighed against the truth.

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

  So `grep -n '^[A-Za-z].*Blorg'` is the export list, and a bare module
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
- **Keep the call tree flat, and be conservative with the kernel stack.** It
  is only about three pages, shared with everything the thread calls, and
  running out of it is a bugcheck, not a recoverable error. Avoid deeply
  nested calls between internal routines that each pass data on the stack,
  and bound the depth of any recursive routine explicitly rather than
  trusting the caller. Prefer system-space (pool) allocation over a large
  stack frame. `IoGetStackLimits`/`IoGetRemainingStackSize` can check
  remaining headroom and `KeExpandKernelStackAndCallout` can grow it, but
  neither is a substitute for keeping the tree shallow. The kernel stack can
  also be paged out while the thread is in a user-mode wait, so never pass a
  stack-based buffer (a local variable) to DMA or to a routine that can run
  at `>= DISPATCH_LEVEL`.
- **Data-oriented design.** Lay data out for how it is actually traversed —
  hot fields together, cold fields elsewhere, per-processor where
  contended, arrays over pointer-chasing, struct-of-arrays over
  array-of-structs when a hot path only touches a few fields of many.
  Shape the structure around the access pattern rather than around a
  conceptual object model, and default to measuring the access pattern
  (a counter, a trace) before restructuring rather than guessing it.
- **Avoid false sharing.** Two independently-written fields on the same
  cache line serialize their writers against each other even though the
  code never intended them to share anything. Pad or align per-processor
  or per-CPU state to a cache line boundary — `Statistics.h`'s
  `BLORGFS_STATISTICS` entries are one per processor, each padded to a
  multiple of 64 bytes, specifically so a CPU updates only its own entry
  with a plain `+=` and never dirties a line another core is touching.
  When fields are written by different threads at different rates, that's
  a sign they belong on different cache lines, not packed together for
  compactness.
- **Avoid atomics except where correctness genuinely requires them.** An
  `Interlocked*` op is a full memory barrier and a cache line the whole
  system can serialize on; on a hot or completion path it can cost more
  than the work it's guarding. Reach for a non-atomic, thread- or
  processor-confined design first — a per-processor counter summed by the
  reader, or reformulating a shared mutable gauge into two independent
  monotone counters whose difference the reader computes. `Statistics.h`
  did exactly that: a shared interlocked "fetches in flight" gauge was
  removed in favour of `FetchesIssued - (FetchesCompleted +
  FetchesFailed)`, each term a per-processor counter, because the gauge
  was redundant with counters that already existed and an interlocked op
  on that path was paid on every fetch for nothing. Where atomicity is
  genuinely required — a cross-thread reference count (`Fcb`/`Dcb`/
  `Vcb->RefCount`), a claim/idempotency flag two threads can race to set
  (`OnReapList`, `FspQueue.ThreadsActive`) — use `Interlocked*`, not a
  lock, and say in a comment why the operation has to be atomic so the
  next person doesn't mistake it for a leftover habit.
- **Minimise syscalls and blocking calls.** Batch and amortise instead of
  repeating a call per item; prefer an async completion over anything that
  waits. A blocking call on a hot or completion path is a design bug, not a
  detail — it occupies a worker for the whole duration.
- **Collapse branches with overlapping or shared predicates.** If two
  conditions test the same thing, evaluate it once and branch once. Repeated
  or nested tests of the same predicate should be merged rather than left
  parallel — it is both faster and the only way the invariant stays obvious.

## Build and test tiers

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

(In Claude Code specifically, a `Stop` hook in a local `.claude/settings.json`
— a personal, gitignored setup, not something checked into this repo — may
run the `Build` tier automatically after each turn as a non-blocking
backstop. It is not a substitute for running `Fast` yourself.)

### Where builds land, and why it matters

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

## Deploying to a VM

BlorgFS is a kernel-mode filesystem driver, so it is developed against a
throwaway Windows VM rather than the build machine. This section is
tool-agnostic — nothing here assumes any particular editor, agent, or IDE.

**If something looks broken while testing — a frozen VM, a mysterious
timeout, a bugcheck — read "Debugging the VM" below first.** Most
alarming-looking failures in this environment are known, diagnosable, and
not BlorgFS bugs; that section is a decision tree for telling the difference
before spending time chasing the wrong thing.

**Do not deploy by copying `BlorgFS.sys` into `System32\drivers` by hand.**
It skips catalog validation entirely and can silently leave a stale binary in
place from a previous iteration, so the thing you are debugging is not the
thing you just built. Symptoms of that mistake look like driver bugs
(unexplained hangs, behaviour that does not match the source) and cost far
more time than the deploy script does.

### The short version

Copy `deploy/blorgfs.env.example` to `deploy/blorgfs.env`, fill it in once,
and then deploying takes no arguments at all:

```powershell
.\deploy\Deploy-ToVM.ps1 -Configuration Release
```

`blorgfs.env` holds the VM path, the guest account, the `.vmx`
config-encryption password, the snapshot to revert to, the backend address,
and the KDNET key. It is **gitignored** -- every one of those is a
credential for the debug VM, and the KDNET key in particular lets anyone on
the network attach a kernel debugger to that guest. The committed
`blorgfs.env.example` is the template and carries no real values.

The point of it is that a session starting from nothing can deploy and debug
without first being handed five settings by hand. Explicit arguments still
win over the file, so nothing is locked in:

```powershell
.\deploy\Deploy-ToVM.ps1 `
    -VmxPath "C:\path\to\Win11.vmx" `
    -GuestUser <user> -GuestPassword <pass> `
    -SnapshotName <clean-snapshot> `
    -Configuration Debug
```

That builds, copies the driver package into the guest, and runs
`Install-BlorgFS.ps1` there. Reverting to a known-good snapshot first is
strongly recommended — a buggy driver load can bugcheck the guest, and a
half-installed package is worse to debug than a clean one.

For benchmarking:

```bash
powershell -File deploy/Deploy-ToVM.ps1 -ForBenchmark
```

`-ForBenchmark` deploys Release, clears Driver Verifier, reboots so the
change actually applies, and waits for the guest to go idle before
reporting success — see "Measuring performance" below for why each of those
matters. `-Configuration` still wins if given explicitly.

### How the install actually works

`BlorgFS.inf` is a Windows 10 1903+ **primitive driver** INF: the driver
creates its own device objects in `DriverEntry` rather than being enumerated
by a bus, so there is no hardware ID, no `[Manufacturer]`/`[Models]` section,
and it never appears in Device Manager. Installing it correctly is a two-step
sequence, and **neither step alone is sufficient**:

```powershell
# 1. Stage the package into the Driver Store
pnputil.exe /add-driver <inf> /install

# 2. Run the section that does CopyFiles + AddService
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall.NTamd64 132 <inf>
```

Why both:

- `BlorgFS.inf` sets `DestinationDirs = 13`, meaning **run from Driver
  Store** (driver package isolation). `InstallHinfSection`'s plain
  `CopyFiles` engine cannot write into the protected Driver Store on its
  own. Run without staging first, it fails the copy — interactively as a
  *"Setup cannot copy the file BlorgFS.sys"* dialog that misleadingly blames
  the **source** path (the source is fine; the destination model is the
  problem), and non-interactively as a **silent no-op**: exit code 0, and
  nothing written to `setupapi.dev.log`.
- `pnputil /install` alone stages the package but never runs
  `[DefaultInstall.NTamd64.Services]`. Its `/install` flag installs against
  *matching devices*, and a primitive driver has no hardware ID to match, so
  it reports success, stages the files, and leaves the service unregistered.

Other things that bite here:

- **The section is `DefaultInstall.NTamd64`, not `DefaultInstall`.** The INF
  only defines the architecture-decorated variant. Naming the undecorated one
  matches nothing and no-ops silently.
- **Deploy the *staged* INF, not the repo-root one.** MSBuild's `stampinf`
  step fills in `DriverVer` on the copy at
  `x64\<Config>\BlorgFS\BlorgFS.inf` *before* `Inf2Cat` hashes it. The
  repo-root `BlorgFS.inf` deliberately leaves `DriverVer` blank, so its bytes
  — and therefore its hash — differ from what the catalog contains. Deploying
  it fails with *"The hash for the file is not present in the specified
  catalog file. The file is likely corrupt or the victim of tampering."*
  That message sounds like binary corruption or a signing failure; it
  actually just means the wrong copy of the INF was deployed.
- **The registered service name is `BlorgFS`.** Not `BLORG`, not anything
  from an ad-hoc `sc create`. Verify with `sc query BlorgFS`; a `1060` from
  `sc query` means the `AddService` step never ran.
- Test-signing must be on (`bcdedit /set testsigning on`) and the driver's
  test certificate trusted, both of which `Install-BlorgFS.ps1` handles.
  Enabling test-signing requires a **reboot** before any test-signed driver
  will load; the script exits with status 2 to signal exactly that, and
  `Deploy-ToVM.ps1` reboots and retries once automatically.

Confirm a good install:

```powershell
sc query BlorgFS      # STATE : 4 RUNNING
Test-Path B:\         # True  -- the driver self-mounts, there is no mount step
```

### VMware / `vmrun` quirks

- The `.vmx` used for this project is **encrypted at the VM-config level**,
  which is separate from any guest OS login. Every `vmrun` invocation against
  it needs `-vp <vm-password>` before the command verb — including read-only
  commands like `list`. Without it: *"A password is required for this
  operation."*
- `vmrun`'s argument order is rigid:
  `vmrun [flags] COMMAND <vmx-path> [command-args...]`. The vmx path goes
  **immediately after the command name**, not at the end. Getting this wrong
  produces confusing errors like *"Cannot open VM: C:\some\other\path,
  unknown file suffix"* — it is trying to interpret your argument as the vmx.
- Guest credentials go in `-gu` / `-gp`, and VMware Tools must be running in
  the guest for any guest command to work. `vmrun checkToolsState <vmx>`
  is the quick health check; it can transiently report tools as down and
  recover on its own.
- `runProgramInGuest` defaults to a **non-interactive, Session-0** context.
  Anything that needs the interactive desktop (notably classic setup UI) will
  silently do nothing there. Pass `-interactive` to run on the console
  session. The two-step install above avoids needing this, but it is
  essential for *seeing* a setup dialog when diagnosing why an install
  failed.
- `runProgramInGuest` does not surface the guest process's exit code.
  `Install-BlorgFS.ps1` works around this by writing its status to a file
  that `Deploy-ToVM.ps1` copies back and reads — and deletes any stale status
  file first, so a crashed run cannot be misread as the previous run's
  success.

### Avoiding manual guest login

Windows leaves the guest at the lock/login screen after any reboot
(including the auto-reboot after a bugcheck), and pre-login the network
profile tends to sit as "Public" — which blocks ICMP by default, so `ping`
and some guest-automation calls look like the guest is unreachable when it is
actually just sitting at the lock screen waiting for a human. Set Windows
auto-logon once per VM image so this stops being a recurring interruption:

```powershell
$k = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"
Set-ItemProperty $k AutoAdminLogon "1"
Set-ItemProperty $k DefaultUserName "<user>"
Set-ItemProperty $k DefaultPassword "<pass>"
Set-ItemProperty $k DefaultDomainName $env:COMPUTERNAME
Set-ItemProperty $k ForceAutoLogon "1"   # re-applies after a manual lock, not just cold boot
```

Also worth disabling for any long-running test session, so an idle timeout
does not re-lock the session mid-run:

```powershell
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
Set-ItemProperty "HKCU:\Control Panel\Desktop" ScreenSaveActive "0"
```

This needs to be set once per golden snapshot — a `revertToSnapshot` to an
older snapshot taken before these were set will need it redone.

### Kernel debugging (KDNET)

The guest boots with `debug Yes` and a KDNET transport. Check its settings
from inside the guest with `bcdedit /dbgsettings` (key, port, host IP).

Connecting to an **already-running** target requires an explicit `target=`
with the guest's IP:

```
net:port=50000,key=<key>,target=<guest-ip>
```

`vmrun getGuestIPAddress <vmx>` gets the IP. Omitting `target=` only works
against a target actively announcing itself (e.g. right at boot); against a
running guest it simply times out.

See "Debugging the VM" below for what a normal break-in looks like, the
`g` (resume) timeout, and the `KdBreakPoint()` gotcha in Debug builds — all
of it applies here too.

### Testing inside the guest

The `Perf` tier of `tools\Invoke-BlorgChecks.ps1` must run **inside the
guest**, where `B:` is mounted and the HTTP backend is reachable. See
"Measuring performance" below for what the counters mean and how baselines
are updated.

## Debugging the VM: what's real and what's noise

This VM setup is flaky in ways that look alarming but usually aren't. This
section is a decision tree for telling the two apart, plus the specific
footguns that have cost real time. Read it before treating anything here as
a BlorgFS bug — most "the VM is frozen/crashed" moments were not.

**But read the next part first.** One very common "frozen VM" *is* BlorgFS,
and mistaking it for VIX flakiness costs the most time of anything here.

### The guest freeze that IS BlorgFS: `KdBreakPoint()` in Debug builds

**Largely fixed on 2026-08-22 — read this before blaming a Debug-build
freeze on the environment, and before re-adding a breakpoint to a rejection
path.**

`KdBreakPoint()` is `#if DBG`-gated by the WDK headers: an `int 3` in Debug,
nothing in Release. On a debug-enabled guest that `int 3` traps, and **with
no debugger attached the guest simply stops dead** — indistinguishable from
the VIX/host-memory flakiness described below. `vmrun` calls hang, the
console is unresponsive, the VM looks wedged. The tell is that resuming from
a debugger un-freezes it, and it re-freezes the moment something touches
`B:` again.

`Create.c` used to carry six unconditional `KdBreakPoint()` calls on its
access-mask and disposition rejection paths, so **any** process opening a
file on `B:` with an unsupported access mask froze the whole guest. That
fires constantly without you doing anything: Explorer, Defender (`WdFilter`
attaches to `\Device\BlorgDrive`) and `SearchHost` all probe files with
write masks, and anything registering a library or saving settings (Steam,
game launchers) opens for write as a matter of course.

**Five of the six are now gone** — refusing a write on a read-only volume is
an expected outcome, not an anomaly worth trapping (see the comment above
`CheckFileAccess`). **Do not re-add them.** One deliberately remains, on the
terminal `STATUS_INVALID_DEVICE_REQUEST` fallthrough at the end of
`BlorgVolumeCreate`, which is a genuine "should not get here" -- the create
matched no case at all. Its own reason is recorded in that function's header
comment too, so grepping for `KdBreakPoint` and landing on the survivor
turns up the explanation without having to find this file first.

**Confirm it in one command.** Attach KD, break in, and resolve the
instruction pointer:

```
ln <rip>
```

Landing anywhere inside the `BlorgFS` module — `BlorgFS!CheckFileAccess+0x46`
was the original signature — is a driver breakpoint, not a debugger
break-in artifact. Contrast with `nt!DbgBreakPointWithStatus`, which is the
normal forced break-in (see below). Check the module range with
`lm m Blorg*` if the symbol does not resolve. Since the fix this should only
resolve to the `BlorgCreate` fallthrough; anything else means a breakpoint
was reintroduced.

**Use a Release build for any correctness or performance testing.**
`KdBreakPoint()` compiles to nothing there, and the statistics counters are
always-on in Release anyway (see "Measuring performance" below), so nothing
is lost. This is still the right default even with the rejection-path
breakpoints gone — a Debug build also skews every performance number.
Attach a debugger *before* loading a Debug build so any remaining trap is
diagnosable instead of fatal:

```powershell
.\deploy\Deploy-ToVM.ps1 -VmxPath <vmx> -GuestUser <u> -GuestPassword <p> `
    -VmPassword <vmpw> -Configuration Release -RemoteHost 10.0.50.17
```

### First move: is the guest actually unwell, or is it just VIX?

`vmrun`'s guest-automation channel (VIX) and the guest kernel are two
different things, and they fail independently. A `vmrun` call timing out or
returning *"The VMware Tools are not running in the virtual machine"*
tells you **nothing** about whether the guest is actually broken — check the
guest directly before believing it.

**Fastest ground truth: attach a kernel debugger and look at two things.**

```
net:port=<port>,key=<key>,target=<guest-ip>
```

(`vmrun getGuestIPAddress <vmx>` gets the IP; omitting `target=` only works
against a target actively announcing itself, e.g. right at boot — see
"Kernel debugging (KDNET)" above.)

1. **Does the register/stack state show a real bugcheck**, or the debugger's
   own break-in artifact? Landing in `nt!DbgBreakPointWithStatus` (or, on
   some boots, a different but equally unnamed low address — see below)
   with a short 2-3 frame garbage stack is the **normal, expected** result
   of forcing a break-in on an otherwise-idle or -running system. It is not
   evidence of anything in BlorgFS. A **real** bugcheck shows an actual call
   chain: `nt!KeBugCheckEx` → `nt!HalpNMIHalt` → `nt!HalBugCheckSystem` (this
   session's specific signature — see the NMI section below).
2. **Is `System Uptime` advancing** across repeated break-ins a few seconds
   apart? If yes, the kernel is alive and running normally — whatever
   `vmrun` is complaining about is a host-side VIX problem, not a guest
   problem.

If both check out, the guest is fine. Stop chasing a "frozen VM" and go
fix whatever is actually making VIX unreliable (almost always host memory —
see below).

### Driving the guest: what works and what silently does not

Hard-won specifics for this VM. Each of these looked like a guest or driver
problem and was not.

- **`cmd.exe` via `runProgramInGuest` hangs indefinitely; `powershell.exe`
  works.** Reproducible even for `cmd.exe /c exit` with no redirection,
  while `notepad.exe` and `powershell.exe -Command exit` return instantly
  on the same guest. Each attempt strands a `cmd.exe` in the guest, so a
  pile of them is a symptom of this and not of anything else. **Drive the
  guest with `powershell.exe` only.**

  It does not always hang: it also returns **exit 1** on a completely
  healthy guest, for both `cmd.exe /c exit 0` and `cmd.exe /c ver`, while
  `powershell.exe -Command 'exit 0'` returns 0 in the same second. That
  makes `cmd.exe` uniquely bad as a *health probe*, which is the one job it
  looks perfect for: the probe reports every guest as dead, and any
  recovery escalation behind it then fires against a guest that was fine.
  On 2026-08-29 a sweep did exactly that, hard-resetting a working VM and
  producing a black console that was then read as evidence of a driver
  hang. The rule above already said not to do this.
- **`shutdown /r /t 0` never returns through `runProgramInGuest`.**
  `runProgramInGuest` blocks until the guest program exits, and with `/t 0`
  the OS tears the process down before it can exit, so vmrun waits forever
  on a status that will never arrive. The guest reboots normally and sits
  there idle while the host-side script hangs -- twelve minutes of apparent
  "slow measurement" that was one stuck call. Use **`shutdown /r /t 5`**:
  `shutdown.exe` schedules and returns, PowerShell exits, vmrun returns,
  and the reboot fires afterwards.
- **Pass scripts as files, not as `-Command` strings.** Bash mangles `$_`
  and `$($...)` before PowerShell ever sees them, which silently corrupts
  the script — the usual symptom is an empty output file rather than an
  error. Write the script locally, `copyFileFromHostToGuest`, then run it
  with `-ExecutionPolicy Bypass -NoProfile -File`.
- **`runProgramInGuest` does not return the guest's stdout.** Redirect
  inside the guest (`*> C:\...\out.txt`) and copy the file back. Do the
  copy-back even when the run reports a non-zero exit — that is usually
  where the actual error message is.
- **MSBuild from Bash mangles `/p:` switches into paths.** Run it from
  PowerShell with `-p:` form instead.
- **`C:\BlorgFS-Deploy` in the guest accumulates files across sessions.**
  There are results in there from days ago. Always write to a fresh
  filename or delete first, or you will read a stale file back and believe
  it is this run's output.

### Footgun: orphaned `kd.exe` processes break the build and freeze the guest

A `kd` session whose resume timed out (see below) leaves the **process
alive** even though the tooling has dropped the session id. Two things
follow, neither of which points at its cause:

- It holds `x64\Debug\BlorgFS.pdb` open, so the next driver build dies with
  `LNK1201: error writing to program database`. Every other project in the
  solution still builds, which makes it look like a driver-specific code
  problem.
- It is still attached to the target, so it can hold the guest halted —
  looking exactly like the VIX hang above.

Check for strays and clear the ones from your own session:

```powershell
Get-Process kd -ErrorAction SilentlyContinue | Select-Object Id,StartTime
```

Match `StartTime` against when you opened sessions before killing anything;
a `kd` predating your session may be someone else's live debugger.

### Footgun: a kernel debugger attached during a bugcheck LOOKS like a frozen VM

`AutoReboot` is configured on this guest, but a bugcheck that happens while
a KD session is connected **halts and waits for the debugger** instead of
auto-rebooting — completely normal WinDbg behavior, but from outside (or on
the VM's console) it is indistinguishable from a genuinely hung VM: the
screen just sits there, unresponsive, forever.

**If a hang coincides with a debugger session being open, close that session
with `resume: true` (or send `g`) before concluding the VM itself is stuck.**
This alone explained more than one "it's frozen" moment.

### Footgun: `g` (resume) reports "Request timed out" and kills the session

This is expected, not a failure — the resume itself works, but the tool call
wrapping it doesn't get a response until the *next* break, so it times out
and the session id stops being valid. **Open a fresh `open_kd_session` to
check state after resuming** rather than reusing the old session id or
retrying `g` on it.

### Footgun: an unresolved break address isn't automatically suspicious

Not every break-in lands in `nt!DbgBreakPointWithStatus` by name — on one
boot, forced break-ins repeatedly landed at an address with no resolvable
symbol at all (`lm` showed nothing there), at both ~1 minute and ~31 minutes
of uptime, with an identical kernel base both times. That pattern (same
address, same kernel base, across what should be a fresh KASLR slide) points
at the debugger's own break-in landing in a stable idle-loop location for
that particular boot, not a driver problem — confirm via the same
uptime-advancing check above rather than assuming an unnamed address means
something is wrong.

### Root-caused: service stop wedges in `STOP_PENDING` (no dismount handler)

`sc stop BlorgFS` leaves the service in `STOP_PENDING` indefinitely, after
which `sc start` fails `1056 (already running)` and a reinstall fails
*"The specified service has been marked for deletion"*. Only a guest reboot
clears it. Reproduces against a healthy, reachable backend after normal
successful use, so it is not the dead-backend socket-timeout case below.

**It is not a reference-count leak — every relevant count is already zero.**
Live KD state while wedged:

```
!drvobj \Driver\BlorgFS 2       -> DriverUnload: <non-null>
!devobj <volume DO>             -> RefCount 0
                                   ExtensionFlags (0x1) DOE_UNLOAD_PENDING
                                   AttachedDevice (Upper) ... \FileSystem\FltMgr
!devobj <BlorgDrive DO>         -> RefCount 0, DOE_UNLOAD_PENDING
dt nt!_VPB <vpb>                -> Flags 1 (VPB_MOUNTED), ReferenceCount 0
x BlorgFS!HttpActiveRequests    -> 0n1     (standing reference, never released)
```

Both drain gates still reading their initial standing reference of 1 proves
`DriverUnload` **was never entered** — each drain releases that reference as
its first action. That rules out the two unbounded `KeWaitForSingleObject`
drains in `Client.c`, which is the intuitive suspect and the
wrong ones.

The actual chain:

1. `FsCtrlMountVolume` (`FsCtrl.c`) self-mounts: it creates the volume device
   object and sets `VPB_MOUNTED`.
2. Filter Manager attaches minifilters to the now-mounted volume — `fltmc
   instances` shows `WdFilter`, `UCPD`, `applockerfltr`, `bfs` and
   `FileInfo` on `\Device\BlorgDrive`.
3. On stop, `IopUnloadDriver` walks the driver's device objects. The volume
   device has `AttachedDevice != NULL` (FltMgr sitting above it), so it
   cannot be deleted: the I/O manager sets `DOE_UNLOAD_PENDING` and
   **defers `DriverUnload`**.
4. **`IRP_MN_DISMOUNT_VOLUME` has no handler.** `FsCtrl.c` cases
   `IRP_MN_USER_FS_REQUEST` and `IRP_MN_MOUNT_VOLUME`; dismount falls into
   `default:` and returns `STATUS_INVALID_DEVICE_REQUEST`. Nothing else will
   initiate a dismount either, because the driver mounted itself rather than
   being mounted by a storage stack.
5. The volume therefore never dismounts, FltMgr never detaches, the deferred
   delete never completes, and `DriverUnload` never runs.

Fix direction (not yet implemented): give the volume a real dismount path —
handle `IRP_MN_DISMOUNT_VOLUME`, tear down the FCB/DCB tree, clear
`VPB_MOUNTED`, and let FltMgr detach. Until then, **a guest
reboot between driver deploys is mandatory**, which `Deploy-ToVM.ps1` does
not currently do on its own.

### Known environmental issue: NMI_HARDWARE_FAILURE (bugcheck 0x80)

This VMware/AMD-virtualization setup occasionally bugchecks with
`0x00000080 NMI_HARDWARE_FAILURE`, confirmed unrelated to BlorgFS -- it
happens even with the driver not loaded -- and confirmed to correlate with
**host memory pressure** (see below) and heavy guest load. Two independent
occurrences triaged with `tools\Get-CrashVerdict.ps1 -SymbolServer` both
landed on the identical bucket:

```
0x80_4F4454_AuthenticAMD_NOERRREC_IMAGE_AuthenticAMD.sys
DriverOnStack: 0
```

`Arg1` decodes to the ASCII tag `'TDO'` and the captured stack is just
`nt!KeBugCheckEx` with nothing underneath -- a minidump limitation, not a
real call chain. `AuthenticAMD.sys` is WinDbg's generic fallback name when it
has no real driver to blame, not an actual faulting module. This pattern
matches a hypervisor/watchdog-injected NMI forcing a crash dump on a guest
that looked unresponsive, not a genuine hardware or driver fault.

**Before treating a `0x80` bugcheck as a BlorgFS finding**, triage it:

```
tools\Get-CrashVerdict.ps1 -DumpPath <path\to\dump> -SymbolServer
```

and check `DriverOnStack` — if it's `0` and the bucket matches the one
above, this is the known false alarm, not a new bug.

Heavier allocation load (e.g. a broad recursive directory enumeration)
seems to correlate with triggering this — plausibly because **Driver
Verifier's special pool** turns every allocation into extra TLB-flush IPI
traffic, which is exactly the kind of load that would expose a marginal
hypervisor-level IPI-ack timing issue. Consider disabling Verifier
(`verifier /reset`, then reboot) for routine correctness/perf runs and
reserving it for targeted memory-safety sessions, if this keeps recurring
under load.

### Host memory pressure breaks VIX reliability, not just VM performance

**Below roughly 3-4GB of host free RAM, expect `vmrun` guest-automation
calls to fail or time out frequently — copies, `runProgramInGuest`, even
`checkToolsState` — while the guest kernel itself remains completely
healthy.** This was directly observed and measured:

| Host free RAM | VIX behavior |
|---|---|
| 1.24 GB | Guest looked completely unresponsive; every call failed |
| 2.6 GB | Frequent failures, needed 2-3 retries per call |
| 3.2 GB | Frequent timeouts on trivial commands (`sc query`) |
| 8.1 GB | Fully reliable, no retries needed |

Check host memory before assuming a guest problem:

```powershell
Get-CimInstance Win32_OperatingSystem |
    Select-Object @{n='FreeGB';e={[math]::Round($_.FreePhysicalMemory/1MB,2)}}
```

`vmware-vmx` itself is worth checking too — it has been observed using
noticeably more working set than the VM's own configured `memsize` (e.g.
6.13GB against a 4GB `memsize`), plausibly `mem.hotadd` overhead; disabling
that (requires a VM power-off to edit the `.vmx`) is an untried but
plausible lever if this keeps recurring.

**Don't burn cycles on rapid VIX retries under memory pressure** — it just
adds more contention. Either wait for more host RAM to free up, or fall back
to the KD-based ground-truth check above, which doesn't depend on VIX at
all.

### Footgun: a PowerShell-in-guest crash isn't necessarily your script's fault

`runProgramInGuest` occasionally reports exit code `-196608` on completely
trivial scripts (a bare `sc.exe query` + `Test-Path`) under host memory
pressure. This looks like a script bug but reproduces on scripts with no
possible bug — treat it as another host-pressure symptom, not something to
debug in the script itself.

### "The semaphore timeout period has expired" on `B:\` is the backend, not the FS

Enumerating `B:\` failing with *"The semaphore timeout period has expired"*
(an `IOException` from `Get-ChildItem`) means the driver loaded and mounted
fine and is simply not reaching its HTTP backend.

The INF-seeded default `RemoteHost` is `blorgfs.blorg.lan`, which **resolves
in this environment to `10.0.60.10` and is not reachable on port 8080**. The
working backend is **`10.0.50.17:8080`**. Deploy with `-RemoteHost
10.0.50.17` (or fix `HKLM:\SYSTEM\CurrentControlSet\Services\BlorgFS\Parameters\RemoteHost`
and restart the service).

Confirm which side is at fault from inside the guest before touching driver
code:

```powershell
Resolve-DnsName blorgfs.blorg.lan
Test-NetConnection -ComputerName 10.0.50.17 -Port 8080
```

Note that a driver pointed at a dead backend also makes service **stop**
pathological: every in-flight request has to burn its full socket timeout
(connect 15s / send 15s / receive 30s, `Socket.c`) before unload can
proceed, so `STOP_PENDING` can persist for minutes and look like a hang.

### Solved: the vendor IOCTLs returned `ERROR_INVALID_FUNCTION` (device type)

`IOCTL_BLORGFS_QUERY_STATISTICS` / `RESET_STATISTICS` / `SET_TLS_PIN` all
failed with `ERROR_INVALID_FUNCTION` (error 1) from usermode — the
long-standing "statistics IOCTL broken" issue. **Root cause: they were
declared `CTL_CODE(FILE_DEVICE_FILE_SYSTEM, ...)`.**

The I/O manager routes an IOCTL by the device type baked into its
`CTL_CODE`: `FILE_DEVICE_FILE_SYSTEM` becomes `IRP_MJ_FILE_SYSTEM_CONTROL`
(an FSCTL), everything else becomes `IRP_MJ_DEVICE_CONTROL`. All three are
implemented in `DevIoCtrl.c` under `IRP_MJ_DEVICE_CONTROL`, so they never
arrived; they fell into `FsCtrl.c`'s unhandled-FSCTL `default`, which
returns `STATUS_INVALID_DEVICE_REQUEST` → `ERROR_INVALID_FUNCTION`. Fixed
by declaring them `FILE_DEVICE_UNKNOWN`.

Two traps worth knowing if this ever regresses:

- **The sandbox tests cannot catch it.** They call `BlorgDeviceControl`
  directly, so the routing decision never happens and every test passes
  with the broken device type. `DevIoCtrlTest.VendorIoctlsAreNotRoutedAsFsctls`
  asserts on the device type specifically for this reason.
- **`PerfHarness.exe` embeds the IOCTL codes at compile time.** After
  changing them you must rebuild it, or a stale binary keeps sending the old
  codes and still reports error 1 against a fixed driver. Note also that a
  **Debug** `PerfHarness.exe` will not start in the guest (missing debug CRT
  DLLs, exit code `-1073741515`) — deploy the Release one.

### Technique: diagnosing "my IOCTL returns the wrong status" via live KD

When a custom vendor IOCTL fails with a status that doesn't match anything
in the driver's own dispatch code, the fastest way to find out whether the
IRP is even reaching your dispatch routine is to inspect the live device
object stack, not just read source:

```
!devobj \BlorgFS
```

`AttachedDevice (Upper) ... \FileSystem\FltMgr` in the output means a
minifilter stack sits above your device and gets first look at every IRP —
relevant for any device created as `FILE_DEVICE_DISK_FILE_SYSTEM` and passed
to `IoRegisterFileSystem`, since Filter Manager treats it as a real
filesystem eligible for minifilter attachment regardless of whether the
driver intended it purely as a control device. Cross-reference with, from
inside the guest:

```powershell
fltmc filters      # which minifilters are loaded at all
fltmc instances    # which volumes/devices each one is actually attached to
```

Note: `Set-MpPreference -DisableRealtimeMonitoring $true` can silently no-op
under Tamper Protection even from an admin token — don't trust it without
checking `Get-MpComputerStatus` afterward.

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
reset -- it wedges in `STOP_PENDING` (see "Debugging the VM" above).

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

## Read-ahead policy: current state

This is the driver's read-ahead behaviour as it stands today, stated
without the history. The full derivation — every dead end, reversal and
measurement that produced these rules — is the evidence trail immediately
below; read it before changing any of this, since several of these rules
exist specifically because a simpler version of them measurably failed.

**Lookahead is entirely Cc's built-in read-ahead**, sized per file object
via `CcSetReadAheadGranularity`. The driver's own prefetcher and chunk
budget are gone (git history only) and are not coming back as-is — see
"Future work: on-disk hot cache" for the replacement direction.

| Rule | Detail |
|---|---|
| Starting granule | **128 KB**, every file. Registry: `ReadAheadGranularityKb` (0 = never call `CcSetReadAheadGranularity`, leaves Cc's own default of `PAGE_SIZE`). |
| Shrink | A window that fetched more than **2x** what it consumed (one granule of lead allowed). Applies to every pattern; does not consult the consumer. |
| Grow | Only when **all** of: the consumer never idles (`ReadIsGreedy` in `Read.c`, idle ticks under 25% of a window); the transport is quiet (fewer than `READ_AHEAD_ADAPT_QUIET_DEPTH` fetches in flight, fixed at **6**); 16 consecutive exactly-adjacent reads on the *current* stream (`ReadCurrentStreak`/`ReadLastStreamIndex` — not the longest streak across trackers, which oscillated); and Cc is still honouring the granule last given (largest paging read in the window against the granule asked for — not a rate or a clock). |
| Growth ceiling | **2 MB** (`ReadAheadMaxGranularityKb`), chosen because Cc itself caps around ~1.1 MB on this rig and going further bought nothing. |
| Feedback loop | On by default; `ReadAheadAdapt=0` pins the granule (useful for A/B measurement). |
| Slack-based growth | On by default; `ReadAheadSlackGrowth=0` disables it. |
| **Removed**: "loaded transport grows the granule" | Was in the tree, measured to be up to **12x worse** on paced/deadline workloads, deleted outright. Growth today is slack-only. |

**What this gets right, measured:** a greedy sequential reader (file copy)
reaches ~1.0x a usermode HTTP client on the same link at the same moment;
amplification stays under 1.2x on every pattern including sparse/random
access; paced playback at realistic bitrates misses effectively no
deadlines.

**What is not solved, and isn't a granularity problem:** eight paced
consumers demanding the *entire* link at once miss 0.5%-33% of deadlines in
every configuration tried, pinned or adaptive. That's bandwidth
starvation, not something a granule choice can fix. When those streams
starve at the ceiling they also stop idling and get misclassified as
"greedy" by the slack signal — recorded as a known, harmless (net) edge
case, not fixed.

**The identified next lever, untried:** raise the number of fetches in
flight for a sequential reader without raising the granule. The
512 KB-vs-128 KB gap traces to too few fetches in flight to amortise
time-to-first-byte at the smaller size (depth ~2.7 at the driver's typical
operating point), not to the granule being inherently slower.

## Evidence trail: the playback-stutter investigation

The policy in "Read-ahead policy: current state" above didn't come from
first principles — it came from a specific investigation, several dead
ends, and two reverted implementation attempts. This section keeps the
conclusions and the reusable lessons, compressed; the full round-by-round
measurement tables and every superseded hypothesis along the way are not
reproduced here but are recoverable from this file's git history, per the
[documentation discipline](#documentation-discipline) above of writing
down what was *not* true, not just what was.

### The investigation, compressed

Reported symptom (2026-08-28, master, Release, Verifier off): video and
subtitle lag watching media off `B:`. The file that reproduces it does
0.33 MB/s sustained against a driver that does 17-28 MB/s, so throughput was
never the question — **the trigger is enabling subtitles**, and until they
are on, nothing about this driver is visible to the player at all. A local
control (the same file copied to the guest's own disk, same player, same
seeking) played smoothly with zero reads recorded by this driver's
counters, which exonerates decode and CPU capacity as independent
explanations.

Four candidate causes were tested and ruled out — see "What it is not"
below. What was left: subtitles make the demuxer jump around the file
(sequential paging reads fall from 100% to ~70%), which defeats Cc's
read-ahead prediction for a third of reads and forces demand fetches the
player blocks on. A demand fetch is dominated by time-on-the-wire (~65% of
it is body bytes crossing the link), not by anything this driver does with
the CPU.

Comparing against NTFS (which reads in ~98 KB clusters against this
driver's ~675 KB) pointed at `READ_AHEAD_GRANULARITY` as the lever. A sweep
across four fixed granules (4 KB to 512 KB) showed **no single constant
works**: a large granule wins on throughput and loses badly on tail latency
(a one-second stall on a filesystem whose purpose is playback), a small one
is the reverse, and which one wins depends on load — a lone reader wants a
small granule, a saturated transport wants a large one. That ruled out a
constant and pointed at an adaptive, per-file-object policy, made possible
because `CcSetReadAheadGranularity` turned out to be safely re-callable
mid-stream (undocumented, confirmed by measurement).

The adaptive policy went through two real bugs before landing on today's
rules. First cut: growth was gated only by an amplification check, which
an operator-precedence mistake (`else if`) silently disabled — a bursty
read pattern inheriting a grown granule from a preceding sequential phase
amplified 2.82x before the waste test was moved back in front of the grow
test. Second: growth keyed off the *longest* streak seen across all stream
trackers on the FCB, so a stale streak from an earlier sequential phase
kept growth armed indefinitely and the granule oscillated; keying off the
*current* stream's own streak (`ReadCurrentStreak`) fixed it.

The growth ceiling (2 MB) also took two failed attempts at self-tuning
before settling on a fixed, measured constant — see "Reverted experiments"
below. A "grow when the transport is loaded" rule was in the tree for a
while and measured to make paced/deadline workloads up to ~12x worse before
being removed outright; growth today is gated on consumer slack (idle vs.
busy) only, not transport load.

One gap remains open, and is a limit rather than a defect: eight paced
consumers demanding the entire link at once miss 0.5%-33% of deadlines in
every configuration tried, pinned or adaptive. That's bandwidth starvation,
which no granule choice controls. The identified next lever — raising the
number of fetches in flight for a sequential reader rather than the granule
size, since the real constraint at the driver's typical depth (~2.7) is too
few fetches to amortise time-to-first-byte — has not been attempted.

### What it is not

Tested and dead, recorded so they are not re-proposed:

- **Not the MKV Cues index.** The theory was that each seek drags a read to
  the index at the end of the file. `end-of-file` reads: **0**.
- **Not metadata.** The theory was that the player re-opens or re-stats on
  seek. `creates` and `file info`: **0 and 0**.
- **Not decode or CPU capacity.** The local-disk control above played
  smoothly with zero reads recorded by this driver, on the same two vCPUs.
- **Not seeking.** Twenty-five scripted seeks at two-second intervals, the
  rate the reporter used, produced zero reads over a frame interval. An
  earlier version of this investigation wrongly concluded seeking was the
  cause, from a synchronous-read simulation that exhibited the problem it
  was built to look for rather than the real, asynchronous access pattern.

### Reverted experiments

Three things were built, measured, and pulled back out. Recorded so they
are not re-proposed without new evidence:

- **A self-tuning growth ceiling** (two versions: keep-only-if-10%-better,
  revert-only-if-10%-worse). Both regressed sequential throughput and
  neither shipped. The reason generalizes: per-window throughput can't
  resolve an ~11% marginal effect through a link whose own noise is ~30%,
  and no threshold fixes that — loose enough to survive the noise is loose
  enough to never fire. The ttfb/body ratio was the obvious alternative
  signal and doesn't work either, since ttfb grows with the granule too.
  What shipped instead: a fixed 2 MB constant, and the byte-count-based
  "did Cc honour the granule" signal used elsewhere in the policy, which
  doesn't depend on a stable link to measure.
- **Pipelining the receive** (posting the WSK receive at send-issue instead
  of send-completion, to shrink time-to-first-byte). Measured consistently
  *worse* across interleaved pairs — the send-completion DPC was never what
  the request was waiting on. Also surfaced a real use-after-free in the
  sandbox (a receive can complete synchronously and free the context before
  the send is issued) with a clear fix if this is ever revisited: from the
  moment the receive is posted it owns the context, and the send completion
  may only stamp a timestamp and record status — never fail, retry,
  complete or kick.
- **Splitting one fetch into concurrent range requests** over partial MDLs,
  to get a short stall and a large granule at once. Measured worse at every
  slice count tried — this guest has two vCPUs, so concurrent WSK receives
  and HTTP parses serialize rather than overlap, and splitting just adds
  per-slice overhead. `READ_AHEAD_PARAMETERS.PipelinedRequestSize` in
  `ntifs.h` describes exactly this and would invite trying it again; the
  answer may differ on a guest with more processors, but does not on this
  one.

### Lessons learned: measuring this driver

Every one of these cost a wrong conclusion before it was understood, and
they apply to any future performance work here, not just read-ahead.

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
no burst (2.07x at 1% sequential, too scattered to arm anything). What
reproduces the captured trace is `demux`: several cursors, each reading a
short adjacent burst then skipping a stride, which is what a container's
interleaved tracks look like from one file object.

**Calibrating on one statistic is not validation.** The `demux` model above
was tuned until its amplification matched the trace and its sequential
share was close, and the match was treated as licence to sweep. It reads in
4 KB blocks; the player issues ~262 KB. Matching one number while missing
another by two orders of magnitude is the same class of error as matching
fetch latency and calling it what the user feels.

**Measure interleaved or not at all.** An ordered A-then-B comparison hands
any host/link drift to whichever setting ran second — the same 128 KB
setting measured 20.2 MB/s at 0.16% in one session and 12.6 MB/s at 1.8% an
hour later. Alternate configurations with a reboot between runs, and rotate
which arm goes first between rounds, or a warm-up bias reads as a real
effect.

**Compare shares, not counts, across runs of different length.** Runs with
different file sizes or durations produce different read totals, so an
absolute count of reads over a frame ranks the longest run worst.
`UserReadsOverFrameShare` exists for this — it is already a percentage, so
multiplying it again yields impossible values above 100% and, worse,
preserves the ordering while destroying the magnitude.

## Future work: on-disk hot cache (not implemented)

> **Not implemented. This is a future project, not current driver
> behaviour.** There is no `DiskCache.c`/`DiskCache.h` in the tree, no
> registry keys for it, and nothing described below exists yet. Don't
> reference this section as if the code is present, and don't be surprised
> the symbols below don't grep-hit anywhere else in the repo — that's
> expected until the project starts.

The prefetch ring was removed rather than replaced. What follows is the
design for its replacement, which is **not an in-memory prefetcher** -- that
experiment is finished and its evidence is in git history.

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

