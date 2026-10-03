#!/usr/bin/env bash
#
# One runtime test run, end to end, on a KVM host: boot a fresh guest from
# the golden image, deploy the blorg package into it, run the in-guest
# tests, collect results and diagnostics, and exit with the verdict.
#
#   run-guest-tests.sh --package DIR [--out DIR] [--suites DIR]
#                      [--no-verifier] [--kernel-dump] [--keep]
#
#   --package DIR   the unpacked blorg-package-windows-x64 artifact
#                   (manifest.json, driver/, server/)
#   --out DIR       where results land (default ./guest-results)
#   --suites DIR    extra suites (*.ps1) to run in the guest; see the suite
#                   contract in in-guest/Invoke-GuestTests.ps1
#   --no-verifier   run without Driver Verifier on BlorgFS.sys
#   --kernel-dump   bring MEMORY.DMP back too after a bugcheck (large)
#   --keep          leave the guest running afterwards, for an agent or a
#                   person to investigate with host/guestctl
#
# Exit: 0 pass, 1 fail (tests failed or the guest bugchecked), 2 the rig
# itself broke before a verdict existed.
#
# The same script runs in CI and by hand; nothing in it knows which.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GUESTCTL="$HERE/host/guestctl"

package="" out="$PWD/guest-results" suites="" verifier=1 kernel_dump=0 keep=0
while (( $# )); do
    case "$1" in
        --package)     package="$2"; shift 2 ;;
        --out)         out="$2"; shift 2 ;;
        --suites)      suites="$2"; shift 2 ;;
        --no-verifier) verifier=0; shift ;;
        --kernel-dump) kernel_dump=1; shift ;;
        --keep)        keep=1; shift ;;
        *) echo "run-guest-tests: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

rig_fail() { echo "run-guest-tests: $*" >&2; finish 2; }
# Collapsible sections in a GitHub Actions log; plain headers elsewhere.
step() {
    echo
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then echo "::group::$*"; else echo "==> $*"; fi
}
endstep() { [[ -z "${GITHUB_ACTIONS:-}" ]] || echo "::endgroup::"; }

finish() {
    local code="$1"
    # Host-side evidence, whatever happened in the guest.
    mkdir -p "$out/host"
    # shellcheck source=ci/guest/host/lib.sh
    ( source "$HERE/host/lib.sh"; cp -f "$GUEST_SERIAL" "$GUEST_QEMU_LOG" "$out/host/" 2>/dev/null || true )
    "$GUESTCTL" screenshot "$out/host/screen.png" >/dev/null 2>&1 || true
    if (( keep )); then
        echo "run-guest-tests: guest left running (--keep); drive it with $GUESTCTL"
    else
        "$GUESTCTL" down >/dev/null 2>&1 || true
    fi
    exit "$code"
}

[[ -f "$package/manifest.json" ]] || { echo "run-guest-tests: --package must be an unpacked blorg package (no manifest.json in '$package')" >&2; exit 2; }
mkdir -p "$out"
out="$(cd "$out" && pwd)"

step "Assembling the bundle"
bundle="$(mktemp -d)/bundle"
mkdir -p "$bundle/tools"
cp -r "$package" "$bundle/package"
cp -r "$HERE/in-guest" "$bundle/in-guest"
cp "$REPO/tools/Test-BlorgCorrectness.ps1" "$bundle/tools/"
if [[ -n "$suites" ]]; then
    mkdir -p "$bundle/suites"
    cp "$suites"/*.ps1 "$bundle/suites/" 2>/dev/null || echo "  (no *.ps1 in $suites)"
fi
python3 -c 'import json,sys; m=json.load(open(sys.argv[1], encoding="utf-8-sig")); print("  package", m.get("version"))' "$package/manifest.json"
endstep

step "Booting a fresh guest"
"$GUESTCTL" up --fresh || rig_fail "guest did not boot"
endstep

step "Deploying the bundle"
"$GUESTCTL" ssh "Remove-Item -Recurse -Force C:/blorgfs-ci/bundle, C:/blorgfs-ci/results -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force C:/blorgfs-ci | Out-Null" \
    || rig_fail "could not prepare the guest's staging directory"
"$GUESTCTL" push "$bundle" C:/blorgfs-ci/bundle || rig_fail "could not copy the bundle into the guest"
endstep

step "Preparing the guest"
prep_args=()
(( verifier )) || prep_args+=(-NoVerifier)
"$GUESTCTL" ssh "powershell -NoProfile -ExecutionPolicy Bypass -File C:/blorgfs-ci/bundle/in-guest/Prepare-Guest.ps1 ${prep_args[*]:-}; exit \$LASTEXITCODE"
rc=$?
if (( rc == 3010 )); then
    "$GUESTCTL" reboot || rig_fail "guest did not come back from the preparation reboot"
elif (( rc != 0 )); then
    rig_fail "Prepare-Guest.ps1 failed ($rc)"
fi
endstep

step "Running the tests in the guest"
boot_before="$("$GUESTCTL" boot-id)"
"$GUESTCTL" ssh "powershell -NoProfile -ExecutionPolicy Bypass -File C:/blorgfs-ci/bundle/in-guest/Invoke-GuestTests.ps1; exit \$LASTEXITCODE"
test_rc=$?
endstep

# ssh's own failures are 255. Anything else is the test runner's verdict.
if (( test_rc == 255 )); then
    step "Lost the guest mid-run; waiting for it to come back"
    "$GUESTCTL" screenshot "$out/screen-at-loss.png" >/dev/null 2>&1 || true
    "$GUESTCTL" wait 900 || rig_fail "guest never came back after losing it mid-run (see $out/screen-at-loss.png)"
    endstep
fi
boot_after="$("$GUESTCTL" boot-id 2>/dev/null || true)"

step "Collecting diagnostics"
diag_args=()
(( kernel_dump )) && diag_args+=(-IncludeKernelDump)
"$GUESTCTL" ssh "powershell -NoProfile -ExecutionPolicy Bypass -File C:/blorgfs-ci/bundle/in-guest/Get-GuestDiagnostics.ps1 ${diag_args[*]:-}" || true
rm -rf "$out/results"
"$GUESTCTL" pull C:/blorgfs-ci/results "$out/" || rig_fail "could not copy results out of the guest"
endstep

# The verdict: the runner's own, overridden by a bugcheck or an unexplained
# reboot -- a crash after the last step still counts.
python3 - "$out" "$test_rc" "$boot_before" "$boot_after" <<'PY'
import json, os, sys
out, rc, before, after = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
def load(p):
    try:
        with open(os.path.join(out, p), encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None
res = load("results/results.json") or {}
crash = load("results/diag/crash.json") or {}
reasons = []
if res.get("verdict") != "pass":
    reasons.append(f"tests: {res.get('verdict', 'no results')}")
if crash.get("bugchecked"):
    reasons.append("guest bugchecked")
if before and after and before != after:
    reasons.append("guest rebooted during the run")
if rc == 255:
    reasons.append("lost contact with the guest mid-run")
verdict = "fail" if reasons else "pass"
lines = [f"verdict={verdict}", f"package={res.get('package')}"]
lines += [f"reason={r}" for r in reasons]
for s in res.get("steps", []):
    lines.append(f"step.{s['name']}={s['status']}" + (f" ({s['detail']})" if s.get("detail") else ""))
with open(os.path.join(out, "verdict.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("\n".join(lines))

summary = os.environ.get("GITHUB_STEP_SUMMARY")
if summary:
    icon = {"pass": "✅", "fail": "❌", "skip": "⏭️"}
    with open(summary, "a", encoding="utf-8") as f:
        f.write(f"## Windows guest runtime tests: {verdict.upper()}\n\n")
        f.write(f"Package `{res.get('package')}`\n\n")
        for r in reasons:
            f.write(f"- **{r}**\n")
        f.write("\n| Step | Result | Time | Detail |\n|---|---|---|---|\n")
        for s in res.get("steps", []):
            f.write(f"| {s['name']} | {icon.get(s['status'], '')} {s['status']} | {s['seconds']}s | {s.get('detail') or ''} |\n")
sys.exit(0 if verdict == "pass" else 1)
PY
finish $?
