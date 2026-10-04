# Windows test guest

A Windows guest under KVM/QEMU, driven entirely by the scripts here, for
loading the packaged driver into a real kernel and testing it. It runs on
any Linux host with `/dev/kvm`: your own Linux machine, a cloud VM with
nested virtualization, or a GitHub `ubuntu-latest` runner, which is what CI
uses. Everything CI does, you can do by hand with the same commands.

```
ci/guest/
  action.yml                  CI host setup shared by the two workflows
  image/build-image.sh        golden Windows image, unattended, once per host
  image/Setup-GoldenImage.ps1   (runs in the guest during that build)
  host/guestctl               the control CLI: boot, ssh, push/pull, reboot,
                              snapshot/revert, screenshot, guest-agent exec
  host/install-host-deps.sh   QEMU + /dev/kvm access on Debian/Ubuntu
  host/make-corpus.py         the deterministic tree server-rs serves
  in-guest/                   Prepare-Guest, Invoke-GuestTests,
                              Get-GuestDiagnostics
  run-guest-tests.sh          one full run: fresh guest -> package -> verdict
  run-session.sh              a script of guestctl commands, with a transcript
  session-probe.sh            example script: every channel, once
  infra/azure/main.bicep      optional long-lived KVM host on Azure
```

## Running the tests

```bash
ci/guest/host/install-host-deps.sh        # once per host
ci/guest/image/build-image.sh             # once per host (20-30 min under KVM)
ci/guest/run-guest-tests.sh --package <unpacked blorg-package-windows-x64>
```

The package is the `blorg-package-windows-x64` artifact from `build.yml`;
the guest never builds anything itself, so what it tests is what ships.
`run-guest-tests.sh` boots a fresh guest, pushes the package plus
`in-guest/` and `tools/Test-BlorgCorrectness.ps1`, prepares the guest
(test signing, Driver Verifier on `BlorgFS.sys` unless `--no-verifier`,
old dumps cleared; reboots if any of that needs it), runs
`Invoke-GuestTests.ps1`, collects diagnostics, pulls everything back to
`--out`, and powers the guest off (`--keep` leaves it up). Exit 0 is pass,
1 fail, 2 the rig broke before there was a verdict.

**Topology.** server-rs runs on the KVM host as the Linux build, the way
the product is deployed, and the guest reaches it at `10.0.2.2` (QEMU user
networking's address for the host). The driver's WSK traffic therefore
crosses a real (emulated e1000e) NIC, and the backend's filesystem is
case-sensitive. The binary is the package's static
`server/linux-x64/server-rs` (or `--server-bin`). The driver's TLS path is
not exercised yet.

What a run checks, in order: the package's files match `manifest.json`; a
deterministic corpus from `host/make-corpus.py` (sizes either side of page,
64 KiB, read-ahead granule and ceiling; 300-entry directory; awkward and
non-ASCII names; deep and empty directories) is served and answers
`/healthcheck` from inside the guest; `driver\Install-BlorgFS.ps1` installs
against it and `B:` mounts; the service is RUNNING; the tree on `B:`
matches the corpus path for path and size for size;
`Test-BlorgCorrectness.ps1` passes against the same server; any
`tests/guest-suites/*.ps1` pass; the service is still RUNNING. A bugcheck
or unexplained reboot anywhere fails the run even if every step before it
passed, and the minidumps come back in `results/diag/dumps` for
`Show-CrashAnalysis.ps1` (which runs `tools\Get-CrashVerdict.ps1`) on a
Windows machine with the debugging tools. CI does that itself: when a run
brings dumps back, a `windows-latest` job analyses them with the driver's
PDB from the same build and reports the bugcheck, faulting line and stack
in its log and as a `blorg guest-crash` check annotation.

`results/results.json` is rewritten after every step, so a run that died
mid-way still says how far it got. `verdict.txt` is the one-screen answer;
`host/screen.png` is what the guest's screen showed at the end.

## Adding a suite

Put `<name>.ps1` in `tests/guest-suites/`; CI passes that directory in. It
exits 0 on pass and is given whichever of `-Drive`, `-BackendUrl`,
`-CorpusManifest` and `-ResultsDir` its `param()` declares. The served tree
is on the host, so fixtures a suite needs on the volume go in
`tests/guest-suites/<name>.corpus/`; the host serves them as `<name>\` and
they are in the corpus manifest like everything else. Only top-level
`*.ps1` files run, so helpers can sit in subdirectories beside the suite.
The rig also copies `third_party/schemas/conformance/` (the wire-contract
probe) to `suites\contract\`, where the contract suite looks for it. Suites
run in Windows PowerShell 5.1 inside the guest, so no PowerShell 7 syntax,
and build non-ASCII strings from code points (a BOM-less script is read as
ANSI).

## Working in the guest

Everything goes through `host/guestctl` on the host:

| Do this | Command |
|---|---|
| Boot a fresh guest | `guestctl up --fresh` |
| Run PowerShell in it | `guestctl ssh 'Get-Service BlorgFS'` |
| Run a local script in it | `guestctl ps ./thing.ps1 -Arg value` |
| Copy in / out | `guestctl push ./dir C:/x/dir`, `guestctl pull C:/x/file .` |
| Reboot and wait | `guestctl reboot` |
| Checkpoint / go back | `guestctl snapshot clean`, `guestctl revert clean` |
| See the screen (bugcheck?) | `guestctl screenshot screen.png` |
| SSH is down, Windows is not | `guestctl qga-exec 'Get-NetAdapter'` |
| Power off | `guestctl down` |

The guest is reachable only from the host's loopback (SSH on
127.0.0.1:2222, key-only, as Administrator with PowerShell as the shell);
nothing else on the network can reach it. The SSH key lives next to the
golden image. Inline `guestctl ssh` commands pass through Windows argv
parsing on the way to `powershell.exe -c`, which mangles double quotes: use
single quotes inline, or `guestctl ps` for anything longer than a line.

Deploy with `Install-BlorgFS.ps1`, never by copying the `.sys`; and a
second deploy into the same boot needs a reboot or a revert, because
`sc stop BlorgFS` wedges in `STOP_PENDING`. `guestctl revert` to a
snapshot taken before the install is the fast way.

**Without a KVM host of your own**, dispatch `guest-session.yml` with a
`session` input: bash that runs against a live guest with the package
installed, with `guestctl` on `PATH` and `$SESSION_OUT` for files to bring
back. It is manual only and gates nothing; the CI verdict comes from
`guest-runtime.yml`, which takes no commands. The transcript (every command
echoed, with its output) comes back in the job log, the step summary and a
`blorg guest-session` check annotation; the `guest-session` artifact also
holds whatever the script wrote to `$SESSION_OUT`. `session-probe.sh` is a
worked example that uses every channel once (the input's default). For
example:

```bash
gh workflow run guest-session.yml -f session='
guestctl ssh "Get-Service BlorgFS"
guestctl pull C:/Windows/INF/setupapi.dev.log "$SESSION_OUT/"
guestctl screenshot "$SESSION_OUT/screen.png"'
```

## The golden image

Windows Server 2025 Standard **Server Core** (build 26100, the oldest
Windows the driver targets), from Microsoft's public 180-day evaluation ISO (override with `WINDOWS_ISO_URL`). Every input (the
ISO, virtio-win, the OpenSSH zip) is pinned by SHA-256 in `build-image.sh`,
so one recipe always builds from the same bytes. Built unattended:
`autounattend.xml` on a generated config ISO, then `Setup-GoldenImage.ps1`
on first logon installs OpenSSH (from the Win32-OpenSSH release zip, since
Server Core's own capability needs Windows Update) and the QEMU guest agent
(from the virtio-win ISO), turns test signing on and boot-failure recovery
off (a crash reboots straight back into Windows instead of waiting at a
recovery menu), sets Driver Verifier on `BlorgFS.sys`, keeps kernel dumps,
and turns Windows Update off. The build then boots the result once and
checks it over SSH before publishing it.

Device choices that matter, all in `host/lib.sh`: SeaBIOS, because Secure
Boot blocks `bcdedit /set testsigning on`; AHCI disk and e1000e NIC,
because both have inbox Windows drivers so setup needs no injected virtio
storage driver.

## CI

`guest-runtime.yml` runs after every successful `Build BlorgFS` run, on the
package that run produced; on PRs touching the rig, against the newest
package from that branch or master; and on demand (`package_run_id`,
`verifier`). It is a fixed test: the shipped package, an image from pinned
inputs, the suites in the commit, no commands from outside it. The verdict
is a `blorg guest-verdict` check annotation and `guest-results/verdict.txt`.
PRs touching the rig also run `session-probe.sh`, a fixed script, to check
every `guestctl` channel. Runs queue rather than cancel, so an image build
is never thrown away. It uploads `guest-results` (results, logs,
diagnostics, screenshot), and the image build's screens if a run fails. It
needs no secrets.

CI caches the image (`actions/cache`, keyed by the image recipe's hash and
the calendar quarter, so the evaluation never runs out under a cached
image). A cache miss costs one unattended install in that run. A
`workflow_run` triggered by a pull request runs in master's cache scope, so
it uses the default branch's rig scripts and never saves the image: a PR
(possibly from a fork) must not be able to plant a golden image that master
later boots. The image directory also holds the guest's SSH key; that key
only opens a guest bound to the runner's loopback.

## A host that outlives a CI job

`infra/azure/main.bicep` provisions an Ubuntu VM (`Standard_D4s_v5` by
default; Dsv5 supports nested virtualization) with QEMU installed and SSH
open only to `allowedSshSource`. Clone the repo there, build the image
once, and use `guestctl` as above; deallocate it when idle.
