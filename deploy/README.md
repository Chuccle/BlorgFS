# Deploying and testing BlorgFS in a VM

BlorgFS is a kernel-mode filesystem driver, so it is developed against a
throwaway Windows VM rather than the build machine. This document covers the
deploy pipeline, the VM/debugger plumbing around it, and the quirks that cost
real time to rediscover. It is tool-agnostic — nothing here assumes any
particular editor, agent, or IDE.

**If something looks broken while testing — a frozen VM, a mysterious
timeout, a bugcheck — read [`DEBUGGING.md`](DEBUGGING.md) first.** Most
alarming-looking failures in this environment are known, diagnosable, and
not BlorgFS bugs; that document is a decision tree for telling the
difference before spending time chasing the wrong thing.

## The short version

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

**Do not deploy by copying `BlorgFS.sys` into `System32\drivers` by hand.**
It skips catalog validation entirely and can silently leave a stale binary in
place from a previous iteration, so the thing you are debugging is not the
thing you just built. Symptoms of that mistake look like driver bugs
(unexplained hangs, behaviour that does not match the source) and cost far
more time than the deploy script does.

## How the install actually works

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

## VMware / `vmrun` quirks

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

## Avoiding manual guest login

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

## A known false alarm: NMI_HARDWARE_FAILURE (bugcheck 0x80)

This VMware/AMD-virtualization setup occasionally bugchecks with
`0x00000080 NMI_HARDWARE_FAILURE`, unrelated to BlorgFS -- confirmed to
happen even with the driver not loaded. Two independent occurrences
triaged with `tools\Get-CrashVerdict.ps1 -SymbolServer` both landed on the
identical bucket:

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

**Before treating a `0x80` bugcheck as a BlorgFS finding**, triage it and
check `DriverOnStack` — if it's `0` and the bucket matches the one above,
this is the known false alarm, not a new bug.

**A kernel debugger left attached across this bugcheck makes the guest look
frozen instead of rebooting.** `AutoReboot` is configured, but a bugcheck
that happens while KDNET is connected halts and waits for the debugger
instead of auto-rebooting — normal WinDbg behaviour, but from outside it is
indistinguishable from a genuinely hung VM: the screen just sits there.
If a hang coincides with a debugger session being open, close that session
with `resume: true` (or send `g`) before assuming the VM itself is stuck.

Heavier allocation load (e.g. a broad recursive directory enumeration)
seems to correlate with triggering this — plausibly because **Driver
Verifier's special pool** turns every allocation into extra TLB-flush IPI
traffic, which is exactly the kind of load that would expose a marginal
hypervisor-level IPI-ack timing issue. Consider disabling Verifier
(`verifier /reset`, then reboot) for routine correctness/perf runs and
reserving it for targeted memory-safety sessions, if this keeps recurring
under load.

## Kernel debugging (KDNET)

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

Two behaviours that look alarming and are not:

- **Landing at `nt!DbgBreakPointWithStatus` with a short, garbage-looking
  2–3 frame stack immediately on connect is normal.** That is the debugger's
  own forced break-in interrupting whatever the CPU was doing — usually the
  idle thread — not a breakpoint in BlorgFS. To tell a live kernel from a
  frozen one, break in repeatedly and check that `System Uptime` is
  advancing.
- The resume command (`g`) frequently reports a timeout and invalidates the
  session, even though the resume itself worked. Expect to open a fresh
  debugger session to inspect state afterwards rather than reusing the old
  session id.

`KdBreakPoint()` is `#if DBG`-gated by the WDK headers: it compiles to
`__debugbreak()` (an `int 3`) in **Debug** builds and to **nothing** in
Release. In a Debug build on a debug-enabled guest, hitting one traps into
the debugger; with kernel debugging enabled but no debugger attached, it
will appear to freeze the guest. This is a real difference in
Debug-vs-Release behaviour to keep in mind when a Debug build appears to
hang where a Release build does not — attach a debugger before loading, and
the trap becomes diagnosable instead of fatal.

`Create.c` used to carry several such calls on its access-mask and
disposition rejection paths, which froze the guest whenever anything opened
a file on `B:` for write. Those were removed on 2026-08-22; only the
terminal `BlorgCreate` fallthrough still traps. See the `KdBreakPoint`
section in [`DEBUGGING.md`](DEBUGGING.md) before reintroducing one.

## Testing tiers

Run from the repo root on the build machine:

```powershell
powershell -File tools/Invoke-BlorgChecks.ps1 -Tier Fast
```

| Tier | What it does | Needs |
|---|---|---|
| `Build` | Compile + link everything with PREfast | nothing |
| `Fast` (default) | Build, plus RFC 8448 crypto vectors and the fuzz corpus | nothing |
| `Perf` | Fast, plus PerfHarness workloads compared to a stored baseline | driver loaded, backend reachable |
| `All` | every tier | as above |

Exit code is 0 only if everything in the tier passed.

The `Perf` tier must run **inside the guest**, where B: is mounted and the
HTTP backend is reachable. See the performance section in `CLAUDE.md` for
what the counters mean and how baselines are updated.
