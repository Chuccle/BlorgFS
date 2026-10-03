# shellcheck shell=bash
# blorg guest ... -- the BlorgFS Windows test guest on a KVM/QEMU host.
# Sourced by tools/blorg. The one interface to the guest, for CI and for a
# person at a shell on the host alike; see AGENTS.md, "Cloud test guest".
#
# Commands:
#   up [--fresh]              boot the guest (a fresh disk from the golden
#                             image with --fresh or when none exists), wait
#                             for SSH
#   down [--graceful]         power off (hard by default: the disk is
#                             disposable; --graceful asks Windows first)
#   status                    is it running, and is SSH answering
#   wait [timeout-s]          block until SSH answers (default 600)
#   ssh [command...]          PowerShell in the guest; no command = interactive
#   ps <file.ps1> [args...]   copy a local script in and run it; exits with
#                             the script's exit code
#   push <local> <guest-path> copy into the guest   (guest paths: C:/x/y)
#   pull <guest-path> <local> copy out of the guest
#   reboot [timeout-s]        restart Windows and wait until it is back
#   boot-id                   the guest's last boot time (changes on any
#                             reboot, including a bugcheck's)
#   snapshot <name>           save a live snapshot (RAM + disk)
#   revert <name>             return to it
#   snapshots                 list them
#   screenshot <out.png>      what the guest's screen shows right now -- the
#                             way to see a bugcheck or a stuck boot
#   qga-exec <powershell>     run PowerShell through the QEMU guest agent,
#                             for when SSH is down but Windows is not
#   qmp <command> [json]      raw QMP, for anything else
#
#   host-setup                QEMU and /dev/kvm access (Debian/Ubuntu)
#   image [--iso ...]         build the golden image (guest-image.sh)
#   image-key                 hash of the image's inputs, for caching it
#   test --package DIR ...    one end-to-end runtime test run (guest-test.sh)
#   run SCRIPT [--out DIR]    run a bash script against the live guest,
#                             with `blorg` on PATH; transcript kept
#   selftest [--out DIR]      every channel above, once (CI runs it on
#                             changes to the rig)
#
# Settings. Everything is overridable from the environment, so CI, a cloud host and
# a developer's Linux box all drive the same guest the same way:
#
#   GUEST_HOME      state root (image + current run)
#   GUEST_GOLDEN    base image every run starts from (default: $GUEST_HOME/image/golden.qcow2)
#   GUEST_KEY       SSH key the image trusts (default: next to the image)
#   GUEST_CPUS      vCPUs              (default: min(nproc, 4))
#   GUEST_MEM_MB    RAM in MiB         (default: 4096)
#   GUEST_SSH_PORT  host port forwarded to the guest's sshd (default: 2222)
#   GUEST_ALLOW_TCG set to 1 to run without KVM -- works, but Windows is
#                   unusably slow under pure emulation; meant for smoke
#                   tests of the tooling itself, never for a real run.

GUEST_HOME="${GUEST_HOME:-${XDG_STATE_HOME:-$HOME/.local/state}/blorgfs-guest}"
GUEST_IMAGE_DIR="${GUEST_IMAGE_DIR:-$GUEST_HOME/image}"
GUEST_RUN_DIR="${GUEST_RUN_DIR:-$GUEST_HOME/run}"
GUEST_SSH_PORT="${GUEST_SSH_PORT:-2222}"
GUEST_MEM_MB="${GUEST_MEM_MB:-4096}"
if [[ -z "${GUEST_CPUS:-}" ]]; then
    GUEST_CPUS="$(nproc)"
    (( GUEST_CPUS > 4 )) && GUEST_CPUS=4
fi

GUEST_GOLDEN="${GUEST_GOLDEN:-$GUEST_IMAGE_DIR/golden.qcow2}"
GUEST_KEY="${GUEST_KEY:-$GUEST_IMAGE_DIR/id_ed25519}"
# The files of one running guest, under GUEST_RUN_DIR; called again by
# whatever points GUEST_RUN_DIR somewhere else (the image build).
guest_paths() {
    GUEST_DISK="$GUEST_RUN_DIR/disk.qcow2"
    GUEST_PIDFILE="$GUEST_RUN_DIR/qemu.pid"
    GUEST_QMP="$GUEST_RUN_DIR/qmp.sock"
    GUEST_QGA="$GUEST_RUN_DIR/qga.sock"
    GUEST_SERIAL="$GUEST_RUN_DIR/serial.log"
    GUEST_QEMU_LOG="$GUEST_RUN_DIR/qemu.log"
}
guest_paths

guest_accel() {
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
        echo "kvm"
    elif [[ "${GUEST_ALLOW_TCG:-0}" == "1" ]]; then
        note "no usable /dev/kvm; falling back to TCG emulation (very slow)"
        echo "tcg"
    else
        die "/dev/kvm is missing or not accessible. Run on a host with KVM (see blorg guest host-setup), or set GUEST_ALLOW_TCG=1 to emulate."
    fi
}

# Prints the QEMU argument list, one per line, for a guest booting from
# $1 (a qcow2). Extra arguments (CD-ROMs for the image build) follow.
#
# Device choices are deliberate:
#   - SeaBIOS (QEMU's default firmware): no Secure Boot, so test signing
#     can be switched on at all.
#   - q35's built-in AHCI + ide-hd and e1000e: both have inbox Windows
#     drivers. discard=unmap lets the guest's TRIM shrink the qcow2.
#   - virtio-serial port named org.qemu.guest_agent.0: the QEMU guest
#     agent's channel, a fallback way in when the guest network is down.
#   - user-mode networking with only sshd forwarded, and only on loopback:
#     nothing outside this host can reach the guest.
#   - std VGA, so QMP screendump can show a bugcheck screen.
guest_qemu_args() {
    local disk="$1"; shift
    local accel cpu
    accel="$(guest_accel)"
    if [[ "$accel" == "kvm" ]]; then
        # Hyper-V enlightenments: a large win for Windows under KVM and
        # invisible to the driver under test.
        cpu="host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time"
    else
        cpu="max"
    fi
    printf '%s\n' \
        -name blorgfs-guest \
        -machine "q35,accel=$accel" \
        -cpu "$cpu" \
        -smp "$GUEST_CPUS" \
        -m "$GUEST_MEM_MB" \
        -rtc base=utc,driftfix=slew \
        -drive "file=$disk,if=none,id=sys,format=qcow2,discard=unmap,detect-zeroes=unmap,cache=unsafe" \
        -device ide-hd,drive=sys,bus=ide.0,bootindex=1 \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$GUEST_SSH_PORT-:22" \
        -device e1000e,netdev=net0 \
        -device virtio-serial-pci \
        -chardev "socket,path=$GUEST_QGA,server=on,wait=off,id=qga0" \
        -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
        -qmp "unix:$GUEST_QMP,server=on,wait=off" \
        -serial "file:$GUEST_SERIAL" \
        -vga std \
        -display none \
        "$@"
}

guest_running() {
    [[ -f "$GUEST_PIDFILE" ]] && kill -0 "$(cat "$GUEST_PIDFILE")" 2>/dev/null
}

guest_qmp() { python3 "$BLORG_D/qmp.py" qmp "$GUEST_QMP" "$@"; }
guest_qga() { python3 "$BLORG_D/qmp.py" qga "$GUEST_QGA" "$@"; }

guest_ssh_opts() {
    printf '%s\n' \
        -i "$GUEST_KEY" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 \
        -o ServerAliveCountMax=4 \
        -o BatchMode=yes
}

GUEST_STAGE='C:/blorgfs-ci/stage'

ssh_guest() {
    local -a opts
    mapfile -t opts < <(guest_ssh_opts)
    ssh "${opts[@]}" -p "$GUEST_SSH_PORT" Administrator@127.0.0.1 "$@"
}

scp_guest() {
    local -a opts
    mapfile -t opts < <(guest_ssh_opts)
    scp -q -r "${opts[@]}" -P "$GUEST_SSH_PORT" "$@"
}

guest_wait() {
    local timeout="${1:-600}" start=$SECONDS
    while (( SECONDS - start < timeout )); do
        guest_running || die "QEMU is not running (see $GUEST_QEMU_LOG)"
        if ssh_guest 'exit 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 5
    done
    die "SSH did not answer within ${timeout}s; try: blorg guest screenshot /tmp/screen.png"
}

guest_up() {
    local fresh=0
    [[ "${1:-}" == "--fresh" ]] && fresh=1
    guest_running && { note "already running"; guest_wait; return; }
    [[ -f "$GUEST_GOLDEN" ]] || die "no golden image at $GUEST_GOLDEN; build one with: blorg guest image"
    mkdir -p "$GUEST_RUN_DIR"
    if (( fresh )) || [[ ! -f "$GUEST_DISK" ]]; then
        rm -f "$GUEST_DISK"
        # A thin overlay: the golden image is never written, so every fresh
        # run starts from exactly the same disk.
        qemu-img create -q -f qcow2 -F qcow2 -b "$GUEST_GOLDEN" "$GUEST_DISK"
    fi
    rm -f "$GUEST_QMP" "$GUEST_QGA" "$GUEST_PIDFILE"
    : > "$GUEST_SERIAL"
    local -a args
    mapfile -t args < <(guest_qemu_args "$GUEST_DISK")
    qemu-system-x86_64 "${args[@]}" -daemonize -pidfile "$GUEST_PIDFILE" >"$GUEST_QEMU_LOG" 2>&1 \
        || die "QEMU failed to start: $(cat "$GUEST_QEMU_LOG")"
    note "booting (cpus=$GUEST_CPUS mem=${GUEST_MEM_MB}MiB ssh=127.0.0.1:$GUEST_SSH_PORT)"
    guest_wait 900
    note "up"
}

guest_down() {
    guest_running || { note "not running"; return 0; }
    if [[ "${1:-}" == "--graceful" ]]; then
        guest_qmp system_powerdown >/dev/null || true
        local start=$SECONDS
        while guest_running && (( SECONDS - start < 180 )); do sleep 2; done
    fi
    if guest_running; then
        guest_qmp quit >/dev/null 2>&1 || kill "$(cat "$GUEST_PIDFILE")" 2>/dev/null || true
        local start=$SECONDS
        while guest_running && (( SECONDS - start < 30 )); do sleep 1; done
    fi
    rm -f "$GUEST_PIDFILE"
    note "down"
}

guest_status() {
    if ! guest_running; then echo "stopped"; return 1; fi
    if ssh_guest 'exit 0' >/dev/null 2>&1; then echo "running, ssh up"; else echo "running, ssh not answering"; fi
}

guest_boot_id() {
    ssh_guest "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')" | tr -d '\r'
}

guest_reboot() {
    local timeout="${1:-900}" before after start=$SECONDS
    before="$(guest_boot_id)"
    ssh_guest 'Restart-Computer -Force' >/dev/null 2>&1 || true
    # Wait for the boot id to change, not merely for SSH to answer: sshd
    # can still accept a connection in the seconds before shutdown begins.
    while (( SECONDS - start < timeout )); do
        sleep 10
        after="$(guest_boot_id 2>/dev/null || true)"
        if [[ -n "$after" && "$after" != "$before" ]]; then
            note "rebooted"
            return 0
        fi
    done
    die "guest did not come back within ${timeout}s"
}

guest_ps() {
    local script="${1:?usage: blorg guest ps <file.ps1> [args...]}"; shift
    [[ -f "$script" ]] || die "no such script: $script"
    ssh_guest "New-Item -ItemType Directory -Force -Path '$GUEST_STAGE' | Out-Null"
    local name
    name="$(basename "$script")"
    scp_guest "$script" "Administrator@127.0.0.1:$GUEST_STAGE/$name"
    local quoted=() a
    for a in "$@"; do quoted+=("'${a//\'/\'\'}'"); done
    # A child powershell so the script's own `exit N` becomes ssh's exit code.
    ssh_guest "powershell -NoProfile -ExecutionPolicy Bypass -File '$GUEST_STAGE/$name' ${quoted[*]:-}; exit \$LASTEXITCODE"
}

guest_snapshot() {
    local name="${1:?usage: blorg guest snapshot <name>}"
    guest_qmp hmp "savevm $name"
    note "saved snapshot '$name'"
}

guest_revert() {
    local name="${1:?usage: blorg guest revert <name>}"
    guest_qmp hmp "loadvm $name"
    note "reverted to '$name'"
}


# Prepares a Debian/Ubuntu machine to host the guest: QEMU, the image-build
# tools, and /dev/kvm for the current user. GitHub's ubuntu runners expose
# KVM; on a cloud VM pick a size with nested virtualization.
guest_host_setup() {
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        qemu-system-x86 qemu-utils xorriso openssh-client python3 curl openssl ca-certificates

    if [[ ! -e /dev/kvm ]]; then
        echo "blorg: /dev/kvm does not exist -- this machine has no hardware virtualization exposed." >&2
        echo "blorg: on a cloud VM, pick a size with nested virtualization (e.g. Azure Dsv5)." >&2
        return 1
    fi

    # GitHub's runners ship /dev/kvm root-only. A udev rule (rather than a
    # one-off chmod) keeps it usable if the device node is recreated.
    echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' \
        | sudo tee /etc/udev/rules.d/99-blorgfs-kvm.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=kvm
    sudo chmod 0666 /dev/kvm

    echo "blorg: ready ($(qemu-system-x86_64 --version | head -n1))"
}

# blorg guest run [--out DIR] SCRIPT: runs SCRIPT (bash, with `blorg` on
# PATH and $SESSION_OUT for files to keep) against the live guest -- the one
# a `blorg guest test --keep` left up, or a fresh one. Commands are echoed
# (bash -x) into DIR/session/transcript.txt. Exits with the script's code.
guest_run() (
    set +e -uo pipefail
    local out="$PWD/guest-results" script rc bindir
    while (( $# > 1 )); do
        case "$1" in
            --out) out="$2"; shift 2 ;;
            *) die "unknown guest run argument '$1'" ;;
        esac
    done
    script="${1:?usage: blorg guest run [--out DIR] SCRIPT}"
    [[ -f "$script" ]] || die "no such script '$script'"
    bindir="$(dirname "$BLORG")"
    export PATH="$bindir:$PATH"
    export SESSION_OUT="$out/session"
    mkdir -p "$SESSION_OUT"
    cp "$script" "$SESSION_OUT/script.sh"

    "$BLORG" guest status >/dev/null 2>&1 || "$BLORG" guest up --fresh

    bash -x "$script" 2>&1 | tee "$SESSION_OUT/transcript.txt"
    rc=${PIPESTATUS[0]}
    echo "exit=$rc" >> "$SESSION_OUT/transcript.txt"

    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        {
            echo "## Guest session: exit $rc"
            echo
            echo '```'
            tail -n 200 "$SESSION_OUT/transcript.txt"
            echo '```'
        } >> "$GITHUB_STEP_SUMMARY"
    fi
    exit "$rc"
)

# blorg guest selftest [--out DIR]: every way of acting in the guest, once,
# against a guest a test run left up (package installed, B: mounted).
guest_selftest() {
    local script
    script="$(mktemp)"
    cat > "$script" <<'SH'
set -euo pipefail
# Run a command, read its output.
blorg guest ssh '[Environment]::OSVersion.VersionString; hostname'
blorg guest ssh 'sc.exe query BlorgFS'
blorg guest ssh 'Get-ChildItem B:\ | Select-Object -ExpandProperty Name'

# Copy a file in, have the guest transform it, copy the result out.
echo "selftest $(date -u +%FT%TZ)" > "$SESSION_OUT/in.txt"
blorg guest push "$SESSION_OUT/in.txt" C:/blorgfs-ci/probe/in.txt
blorg guest ssh '(Get-Content C:/blorgfs-ci/probe/in.txt).ToUpper() | Set-Content C:/blorgfs-ci/probe/out.txt'
blorg guest pull C:/blorgfs-ci/probe/out.txt "$SESSION_OUT/out.txt"
[[ "$(tr -d '\r' < "$SESSION_OUT/out.txt")" == "$(tr '[:lower:]' '[:upper:]' < "$SESSION_OUT/in.txt")" ]]

# Read a file the driver serves, through B:.
blorg guest ssh '(Get-FileHash B:\names\no-extension -Algorithm SHA256).Hash'

# See the screen; use the guest agent, which works when SSH does not.
blorg guest screenshot "$SESSION_OUT/screen.png"
blorg guest qga-exec 'Get-Service sshd | Select-Object -ExpandProperty Status'

# Snapshot, change something, revert: the change must be gone.
blorg guest snapshot selftest
blorg guest ssh 'New-Item -ItemType File -Force C:/blorgfs-ci/probe/after-snapshot.txt | Out-Null'
blorg guest revert selftest
blorg guest wait 300
[[ "$(blorg guest ssh 'Test-Path C:/blorgfs-ci/probe/after-snapshot.txt' | tr -d '\r')" == "False" ]]

echo "selftest: every channel works"
SH
    guest_run "$@" "$script"
}

cmd_guest() {
    local sub="${1:-}"; shift || true
    set -e
    case "$sub" in
        up)         guest_up "$@" ;;
        down)       guest_down "$@" ;;
        status)     guest_status ;;
        wait)       guest_wait "$@" ;;
        ssh)        ssh_guest "$@" ;;
        ps)         guest_ps "$@" ;;
        push)       ssh_guest "New-Item -ItemType Directory -Force -Path (Split-Path '${2:?}') | Out-Null"
                    scp_guest "${1:?}" "Administrator@127.0.0.1:$2" ;;
        pull)       scp_guest "Administrator@127.0.0.1:${1:?}" "${2:?}" ;;
        reboot)     guest_reboot "$@" ;;
        boot-id)    guest_boot_id ;;
        snapshot)   guest_snapshot "$@" ;;
        revert)     guest_revert "$@" ;;
        snapshots)  guest_qmp hmp "info snapshots" ;;
        screenshot) guest_qmp screendump "{\"filename\": \"$(realpath -m "${1:?}")\", \"format\": \"png\"}" >/dev/null
                    note "wrote $1" ;;
        qga-exec)   guest_qga exec 600 "$@" ;;
        qmp)        guest_qmp "$@" ;;
        host-setup) guest_host_setup ;;
        image)      guest_image "$@" ;;
        image-key)  guest_image_key ;;
        test)       guest_test "$@" ;;
        run)        guest_run "$@" ;;
        selftest)   guest_selftest "$@" ;;
        *)          sed -n '2,/^$/p' "$BLORG_D/guest.sh" | sed 's/^# \{0,1\}//'; [[ -z "$sub" ]] ;;
    esac
}
