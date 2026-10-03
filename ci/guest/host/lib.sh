# shellcheck shell=bash disable=SC2034 # the settings are read by the scripts that source this
#
# Shared settings and the QEMU command line for the BlorgFS test guest.
# Sourced by guestctl and build-image.sh; not meant to be run directly.
#
# Everything is overridable from the environment, so CI, a cloud host and
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
GUEST_DISK="$GUEST_RUN_DIR/disk.qcow2"
GUEST_PIDFILE="$GUEST_RUN_DIR/qemu.pid"
GUEST_QMP="$GUEST_RUN_DIR/qmp.sock"
GUEST_QGA="$GUEST_RUN_DIR/qga.sock"
GUEST_SERIAL="$GUEST_RUN_DIR/serial.log"
GUEST_QEMU_LOG="$GUEST_RUN_DIR/qemu.log"

GUEST_HOST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

die()  { echo "guest: $*" >&2; exit 1; }
note() { echo "guest: $*" >&2; }

guest_accel() {
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
        echo "kvm"
    elif [[ "${GUEST_ALLOW_TCG:-0}" == "1" ]]; then
        note "no usable /dev/kvm; falling back to TCG emulation (very slow)"
        echo "tcg"
    else
        die "/dev/kvm is missing or not accessible. Run on a host with KVM (see host/install-host-deps.sh), or set GUEST_ALLOW_TCG=1 to emulate."
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

guest_qmp() { python3 "$GUEST_HOST_DIR/qmp.py" qmp "$GUEST_QMP" "$@"; }
guest_qga() { python3 "$GUEST_HOST_DIR/qmp.py" qga "$GUEST_QGA" "$@"; }

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
