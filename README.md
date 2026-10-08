# BlorgFS

Kernel-mode Windows filesystem driver that presents an HTTP backend as a
mounted, read-only volume (`B:`). Built on async WSK networking, an optional
hand-rolled TLS 1.3 client, and a keep-alive connection pool.

## Repository layout

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
               differential correctness harness; blorg, the Linux
               entrypoint for packaging and the test guest
  guest/         what runs inside the Windows test guest
deploy/        VM deploy pipeline (see AGENTS.md)
third_party/   submodules: flatcc, picohttpparser, schemas, googletest,
               and server-rs (the backend this driver ships with)
VERSION        the package version
rust-toolchain.toml  the Rust toolchain that builds server-rs
```

## Building and testing

Run the tiered check script rather than a hand-rolled build:

```bash
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast
```

| Tier | What it does | Needs |
|---|---|---|
| `Build` | Compile + link everything with PREfast | nothing |
| `Fast` (default) | Build, sandbox regression suites, RFC 8448 vectors and fuzz smoke tests | nothing |
| `Proof` | Build, exhaustive interleaving tests and the full client fuzz corpus | nothing |
| `Perf` | Fast, plus PerfHarness workloads compared against a stored baseline | driver loaded, backend reachable |
| `All` | every tier | as above |

Exit code is 0 only if everything in the tier passed. Run `-Tier Fast` before
calling any change done — cheaper tiers don't run the crypto tests that catch
a `Tls.c` regression.

After a Debug Fast run, collect line coverage with OpenCppCoverage installed:

```powershell
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast -Configuration Debug -CoverageOnly -CoverageDirectory C:\Temp\blorg-coverage
```

The output directory must be empty. The report records its head and schema/server
pins. It measures real sources linked into the sandbox/TLS binaries, excludes
`Driver.c`, and does not measure native kernel or guest coverage. The profile
adds dispatch/socket scheduling tests and 2,000 client-fuzz iterations to Fast;
node-table exhaustive tests remain in `Proof`. A manual `build.yml` run with
`coverage=true` collects the same report as an artifact.

GitHub Actions covers the rest: `build.yml` gates every push and PR to
`master` at the Fast tier, builds the package and tests it in a Windows
guest (`guest.yml`), `verify.yml` runs CBMC proofs and
extended fuzz/interleaving coverage nightly, and `codeql.yml` runs weekly
(and on PRs touching its own config) with the pinned Microsoft driver query
packs.

## Package and test guest

BlorgFS ships with its backend, [server-rs](https://github.com/Chuccle/server-rs),
pinned as `third_party/server-rs`. Every build produces one package,
`blorg-package-windows-x64`: the test-signed driver, server-rs for Linux
and Windows, and a `manifest.json` naming every commit. A Windows Server
guest under KVM then installs that driver and tests it against that server,
so a driver/server mismatch fails the build. A `v*` tag matching `VERSION`
publishes the package as a release.

The Linux side has one entrypoint, the same in CI and by hand:

```bash
git submodule update --init --recursive third_party/server-rs

# A package from a driver build (x64/Release, from msbuild or a build's
# BlorgFS-Release-x64 artifact), then a test of it in a local guest
# (any Linux host with KVM; the first two guest commands once per host):
tools/blorg server           # the pinned server-rs, into out/server
tools/blorg package --driver x64/Release
tools/blorg guest host-setup
tools/blorg guest image
tools/blorg guest test --package out/package
```

`tools/blorg help` lists everything; [AGENTS.md](AGENTS.md#package-and-test-guest)
has the detail.

## Deploying to a VM

BlorgFS is a kernel driver, so it's developed and tested against a throwaway
Windows VM rather than the build machine. Copy `deploy/blorgfs.env.example`
to `deploy/blorgfs.env` and fill it in once; after that, deploying takes no
arguments:

```powershell
.\deploy\Deploy-ToVM.ps1 -Configuration Release
```

For benchmarking, use `-ForBenchmark` instead, which deploys Release, clears
Driver Verifier, and waits for the guest to go idle before reporting success.

## Documentation for contributors and agents

**[AGENTS.md](AGENTS.md)** is the detailed reference: coding conventions,
the required steps when changing behaviour, sanitizers, CI, the VM deploy
pipeline and its quirks, a debugging decision tree for the test VM, and the
performance-measurement methodology (with the findings behind the current
read-ahead tuning). It's written for coding agents but is equally the
reference for human contributors — read it before making non-trivial
changes. This includes tool-specific notes (e.g. Claude Code); there is no
separate per-tool file.
