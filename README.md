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
               differential correctness harness
deploy/        VM deploy pipeline (see AGENTS.md)
third_party/   submodules: flatcc, picohttpparser, schemas, googletest
```

## Building and testing

Run the tiered check script rather than a hand-rolled build:

```bash
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast
```

| Tier | What it does | Needs |
|---|---|---|
| `Build` | Compile + link everything with PREfast | nothing |
| `Fast` (default) | Build, plus RFC 8448 crypto vectors and the fuzz corpus | nothing |
| `Perf` | Fast, plus PerfHarness workloads compared against a stored baseline | driver loaded, backend reachable |
| `All` | every tier | as above |

Exit code is 0 only if everything in the tier passed. Run `-Tier Fast` before
calling any change done — cheaper tiers don't run the crypto tests that catch
a `Tls.c` regression.

Three GitHub Actions workflows cover the rest: `build.yml` gates every push
and PR to `master` at the Fast tier, `verify.yml` runs CBMC proofs and
extended fuzz/interleaving coverage nightly, and `codeql.yml` runs weekly
(and on PRs touching its own config) with the pinned Microsoft driver query
packs.

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
