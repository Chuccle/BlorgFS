# Debugging this VM: what's real and what's noise

This VM setup is flaky in ways that look alarming but usually aren't. This
document is a decision tree for telling the two apart, plus the specific
footguns that cost real time this session. Read this before treating
anything here as a BlorgFS bug — most "the VM is frozen/crashed" moments
this session were not.

**But read the next section first.** One very common "frozen VM" *is*
BlorgFS, and mistaking it for VIX flakiness costs the most time of
anything here.

## The guest freeze that IS BlorgFS: `KdBreakPoint()` in Debug builds

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
always-on in Release anyway (`README.md`), so nothing is lost. This is still
the right default even with the rejection-path breakpoints gone — a Debug
build also skews every performance number. Attach a debugger *before*
loading a Debug build so any remaining trap is diagnosable instead of fatal:

```powershell
.\deploy\Deploy-ToVM.ps1 -VmxPath <vmx> -GuestUser <u> -GuestPassword <p> `
    -VmPassword <vmpw> -Configuration Release -RemoteHost 10.0.50.17
```

## First move: is the guest actually unwell, or is it just VIX?

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
`deploy/README.md`.)

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
next section).

## Driving the guest: what works and what silently does not

Hard-won specifics for this VM. Each of these looked like a guest or driver
problem and was not.

- **This `.vmx` is config-encrypted.** Every guest operation needs
  `-vp <vm-password>` before the command verb, or it fails with *"A password
  is required for this operation"*. Bare `vmrun list` is the one exception,
  which makes the VM look reachable when nothing else is.
  `Deploy-ToVM.ps1` takes this as `-VmPassword`.
- **`cmd.exe` via `runProgramInGuest` hangs indefinitely; `powershell.exe`
  works.** Reproducible even for `cmd.exe /c exit` with no redirection,
  while `notepad.exe` and `powershell.exe -Command exit` return instantly
  on the same guest. Each attempt strands a `cmd.exe` in the guest, so a
  pile of them is a symptom of this and not of anything else. **Drive the
  guest with `powershell.exe` only.**
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

## Footgun: orphaned `kd.exe` processes break the build and freeze the guest

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

## Footgun: a kernel debugger attached during a bugcheck LOOKS like a frozen VM

`AutoReboot` is configured on this guest, but a bugcheck that happens while
a KD session is connected **halts and waits for the debugger** instead of
auto-rebooting — completely normal WinDbg behavior, but from outside (or on
the VM's console) it is indistinguishable from a genuinely hung VM: the
screen just sits there, unresponsive, forever.

**If a hang coincides with a debugger session being open, close that session
with `resume: true` (or send `g`) before concluding the VM itself is stuck.**
This alone explained more than one "it's frozen" moment this session.

## Footgun: `g` (resume) reports "Request timed out" and kills the session

This is expected, not a failure — the resume itself works, but the tool call
wrapping it doesn't get a response until the *next* break, so it times out
and the session id stops being valid. **Open a fresh `open_kd_session` to
check state after resuming** rather than reusing the old session id or
retrying `g` on it.

## Footgun: an unresolved break address isn't automatically suspicious

Not every break-in lands in `nt!DbgBreakPointWithStatus` by name — on one
boot this session, forced break-ins repeatedly landed at an address with no
resolvable symbol at all (`lm` showed nothing there), at both ~1 minute and
~31 minutes of uptime, with an identical kernel base both times. That
pattern (same address, same kernel base, across what should be a fresh
KASLR slide) points at the debugger's own break-in landing in a stable
idle-loop location for that particular boot, not a driver problem — confirm
via the same uptime-advancing check above rather than assuming an unnamed
address means something is wrong.

## Root-caused: service stop wedges in `STOP_PENDING` (no dismount handler)

`sc stop BlorgFS` leaves the service in `STOP_PENDING` indefinitely, after
which `sc start` fails `1056 (already running)` and a reinstall fails
*"The specified service has been marked for deletion"*. Only a guest reboot
clears it. Reproduces against a healthy, reachable backend after normal
successful use, so it is not the dead-backend socket-timeout case above.

**It is not a reference-count leak — every relevant count is already zero.**
Live KD state while wedged:

```
!drvobj \Driver\BlorgFS 2       -> DriverUnload: <non-null>
!devobj <volume DO>             -> RefCount 0
                                   ExtensionFlags (0x1) DOE_UNLOAD_PENDING
                                   AttachedDevice (Upper) ... \FileSystem\FltMgr
!devobj <BlorgDrive DO>         -> RefCount 0, DOE_UNLOAD_PENDING
dt nt!_VPB <vpb>                -> Flags 1 (VPB_MOUNTED), ReferenceCount 0
x BlorgFS!PrefetchRingCount     -> 0n1     (standing reference, never released)
x BlorgFS!HttpActiveRequests    -> 0n1     (standing reference, never released)
```

Both drain gates still reading their initial standing reference of 1 proves
`DriverUnload` **was never entered** — each drain releases that reference as
its first action. That rules out the two unbounded `KeWaitForSingleObject`
drains in `Prefetch.c`/`Client.c`, which are the intuitive suspects and the
wrong ones.

The actual chain:

1. `BlorgMountVolume` (`FsCtrl.c`) self-mounts: it creates the volume device
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
handle `IRP_MN_DISMOUNT_VOLUME`, tear down the FCB/DCB tree and the prefetch
rings, clear `VPB_MOUNTED`, and let FltMgr detach. Until then, **a guest
reboot between driver deploys is mandatory**, which `Deploy-ToVM.ps1` does
not currently do on its own.

## Known environmental issue: NMI_HARDWARE_FAILURE (bugcheck 0x80)

Confirmed pre-existing and unrelated to BlorgFS (occurs even with the driver
not loaded), and now confirmed to correlate with **host memory pressure**
(see below) and heavy guest load. Triage:

```
tools\Get-CrashVerdict.ps1 -DumpPath <path\to\dump> -SymbolServer
```

If the bucket matches `0x80_4F4454_AuthenticAMD_NOERRREC_IMAGE_AuthenticAMD.sys`
with `DriverOnStack: 0`, this is the known false alarm — `AuthenticAMD.sys`
is WinDbg's generic fallback name when it has no real driver to blame, and
`Arg1` decodes to the ASCII tag `'TDO'`, consistent with a
hypervisor/watchdog-injected NMI on an apparently-unresponsive guest rather
than a genuine fault. Full detail in `deploy/README.md`.

## Host memory pressure breaks VIX reliability, not just VM performance

**Below roughly 3-4GB of host free RAM, expect `vmrun` guest-automation
calls to fail or time out frequently — copies, `runProgramInGuest`, even
`checkToolsState` — while the guest kernel itself remains completely
healthy.** This was directly observed and measured this session:

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

## Footgun: a PowerShell-in-guest crash isn't necessarily your script's fault

`runProgramInGuest` occasionally reports exit code `-196608` on completely
trivial scripts (a bare `sc.exe query` + `Test-Path`) under host memory
pressure. This looks like a script bug but reproduces on scripts with no
possible bug — treat it as another host-pressure symptom, not something to
debug in the script itself.

## "The semaphore timeout period has expired" on `B:\` is the backend, not the FS

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

## Solved: the vendor IOCTLs returned `ERROR_INVALID_FUNCTION` (device type)

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

## Technique: diagnosing "my IOCTL returns the wrong status" via live KD

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
