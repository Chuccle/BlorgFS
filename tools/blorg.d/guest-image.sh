# shellcheck shell=bash
# blorg guest image | image-key
# Builds the golden Windows image the BlorgFS test guest boots from.
# Sourced by tools/blorg.
#
# Unattended end to end: Windows Setup runs from the install ISO with
# autounattend.xml from a generated config ISO, Invoke-BlorgGuest.ps1
# -Step Image does the guest-side configuration and powers off, then this
# script boots the result once to prove SSH and test signing work before
# publishing it.
#
#   blorg guest image [--iso PATH|URL] [--virtio-iso PATH|URL]
#                     [--image-index N]
#
# Defaults build Windows Server 2025 Standard (Server Core) from Microsoft's
# public 180-day evaluation ISO: build 26100, the oldest Windows the driver
# loads on (src/BlorgFS.vcxproj targets NTDDI 0x0A000010, 24H2; on 2022 it
# fails to start with "procedure not found"). Server Core because a
# filesystem driver test needs no desktop, and the image is a fraction of
# the size to build, cache and boot. The evaluation clock starts when the
# image is built, so CI keys its image cache by quarter and rebuilds well
# inside 180 days.
#
# Environment (all optional):
#   WINDOWS_ISO_URL      install ISO (default: Server 2025 evaluation, en-US)
#   WINDOWS_IMAGE_INDEX  index in install.wim (default 1: Standard Core)
#   VIRTIO_ISO_URL       virtio-win ISO, for the QEMU guest agent; set to
#                        "none" to skip the agent
#   OPENSSH_ZIP_URL      Win32-OpenSSH release zip
#   *_SHA256             the pin for each of the three (below)
#   BUILD_TIMEOUT_MIN    give up on Windows Setup after this (default 45)
#   STALL_MIN            give up sooner if the guest's screen has not
#                        changed for this long (default 20): it is stuck
# plus the GUEST_* settings in guest.sh.
#
# Every input is pinned by SHA-256, so the same recipe always builds from
# the same bytes and a moved download fails the build instead of silently
# changing the guest. Overriding a URL means overriding its pin too; an
# empty pin (VAR=) uses the input unchecked and prints its hash.
#
# Output: $GUEST_IMAGE_DIR/golden.qcow2 and the SSH key it trusts,
# $GUEST_IMAGE_DIR/id_ed25519.

# blorg guest image-key: a hash of everything the image is built from (this
# recipe, the answer file and Invoke-BlorgGuest.ps1's Image step), and
# nothing else, so changing how tests run never forces a rebuild.
guest_image_key() {
    {
        cat "$BLORG_D/guest-image.sh" "$REPO/tools/guest/autounattend.xml.in"
        sed -n '/^function Invoke-ImageStep/,/^function /p' "$REPO/tools/guest/Invoke-BlorgGuest.ps1"
    } | sha256sum | cut -c1-16
}

guest_image() (
    set -euo pipefail
WINDOWS_ISO_URL="${WINDOWS_ISO_URL:-https://software-static.download.prss.microsoft.com/dbazure/888969d5-f34g-4e03-ac9d-1f9786c66749/26100.1742.240906-0331.ge_release_svc_refresh_SERVER_EVAL_x64FRE_en-us.iso}"
WINDOWS_IMAGE_INDEX="${WINDOWS_IMAGE_INDEX:-1}"
VIRTIO_ISO_URL="${VIRTIO_ISO_URL:-https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/archive-virtio/virtio-win-0.1.302-1/virtio-win-0.1.302.iso}"
OPENSSH_ZIP_URL="${OPENSSH_ZIP_URL:-https://github.com/PowerShell/Win32-OpenSSH/releases/latest/download/OpenSSH-Win64.zip}"
WINDOWS_ISO_SHA256="${WINDOWS_ISO_SHA256-d0ef4502e350e3c6c53c15b1b3020d38a5ded011bf04998e950720ac8579b23d}"
VIRTIO_ISO_SHA256="${VIRTIO_ISO_SHA256-303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d}"
# Win32-OpenSSH publishes no stable per-version URL here; the pin is what
# keeps "latest" from changing the image underneath the recipe.
OPENSSH_ZIP_SHA256="${OPENSSH_ZIP_SHA256-23f50f3458c4c5d0b12217c6a5ddfde0137210a30fa870e98b29827f7b43aba5}"
BUILD_TIMEOUT_MIN="${BUILD_TIMEOUT_MIN:-45}"
STALL_MIN="${STALL_MIN:-20}"

iso_src="$WINDOWS_ISO_URL"
virtio_src="$VIRTIO_ISO_URL"
while (( $# )); do
    case "$1" in
        --iso)         iso_src="$2"; shift 2 ;;
        --virtio-iso)  virtio_src="$2"; shift 2 ;;
        --image-index) WINDOWS_IMAGE_INDEX="$2"; shift 2 ;;
        *) die "unknown argument '$1'" ;;
    esac
done

for tool in qemu-system-x86_64 qemu-img xorriso ssh-keygen curl python3 openssl; do
    command -v "$tool" >/dev/null || die "missing $tool (run: blorg guest host-setup)"
done
guest_accel >/dev/null

WORK="$GUEST_IMAGE_DIR/build"
DOWNLOADS="$GUEST_HOME/downloads"
mkdir -p "$WORK" "$DOWNLOADS"

# Local path or URL -> local path, checked against its pin. URLs are cached
# under downloads/ so a retried build does not fetch a 5 GB ISO twice.
fetch() {
    local src="$1" name="$2" pin="$3" path="$1" from="$1"
    if [[ ! -f "$src" ]]; then
        path="$DOWNLOADS/$name"
        if [[ ! -s "$path" ]]; then
            note "downloading $name"
            # A stalled transfer (under 100 KB/s for a minute) fails and is
            # retried rather than hanging the build.
            from="$(curl -fL --retry 4 --retry-delay 5 --speed-limit 102400 --speed-time 60 \
                    -o "$path.part" -w '%{url_effective}' "$src")"
            mv "$path.part" "$path"
        fi
    fi
    local sum
    sum="$(sha256sum "$path" | cut -d' ' -f1)"
    note "input $name sha256=$sum from $from"
    if [[ -z "$pin" ]]; then
        echo "::warning title=Unpinned image input::$name sha256=$sum is not pinned in tools/blorg.d/guest-image.sh" >&2
    elif [[ "$sum" != "$pin" ]]; then
        die "$name is not the pinned input (sha256 $sum, pinned $pin)"
    fi
    echo "$path"
}

iso="$(fetch "$iso_src" windows.iso "$WINDOWS_ISO_SHA256")"
virtio=""
if [[ "$virtio_src" != "none" ]]; then
    virtio="$(fetch "$virtio_src" virtio-win.iso "$VIRTIO_ISO_SHA256")"
fi
openssh_zip="$(fetch "$OPENSSH_ZIP_URL" OpenSSH-Win64.zip "$OPENSSH_ZIP_SHA256")"

# A fresh key and password per image. Both only ever reach a guest that
# listens on this host's loopback, but there is no reason to share them
# between images either.
rm -f "$WORK/id_ed25519" "$WORK/id_ed25519.pub"
ssh-keygen -q -t ed25519 -N '' -C blorgfs-guest -f "$WORK/id_ed25519"
# Base64 plus a fixed tail: always meets Windows' complexity rule.
password="$(openssl rand -base64 18 | tr -d '\n')Aa1!"

cfg="$WORK/config"
rm -rf "$cfg"; mkdir -p "$cfg"
xml="$(<"$REPO/tools/guest/autounattend.xml.in")"
xml="${xml//@ADMIN_PASSWORD@/$password}"
xml="${xml//@IMAGE_INDEX@/$WINDOWS_IMAGE_INDEX}"
printf '%s\n' "$xml" > "$cfg/autounattend.xml"
# Setup silently ignores an answer file that is not well-formed XML and
# waits at its language screen, which only shows up as a timeout.
python3 -c 'import sys, xml.dom.minidom as m; m.parse(sys.argv[1])' "$cfg/autounattend.xml" \
    || die "autounattend.xml is not well-formed XML"
cp "$REPO/tools/guest/Invoke-BlorgGuest.ps1" "$cfg/"
cp "$WORK/id_ed25519.pub" "$cfg/authorized_keys"
cp "$openssh_zip" "$cfg/OpenSSH-Win64.zip"
xorriso -as mkisofs -quiet -J -joliet-long -r -V BLORGCFG -o "$WORK/config.iso" "$cfg"

disk="$WORK/disk.qcow2"
rm -f "$disk"
qemu-img create -q -f qcow2 "$disk" 64G

# Install pass. Same machine as a test run, plus the install and config
# CDs and, when there is one, the virtio-win CD.
GUEST_RUN_DIR="$WORK/install"
guest_paths
mkdir -p "$GUEST_RUN_DIR/screens"
rm -f "$GUEST_QMP" "$GUEST_QGA" "$GUEST_PIDFILE"

# shellcheck disable=SC2054 # commas are QEMU option syntax, not separators
cds=(
    -drive "file=$iso,media=cdrom,if=none,id=cd0,readonly=on" -device ide-cd,drive=cd0,bus=ide.1,bootindex=2
    -drive "file=$WORK/config.iso,media=cdrom,if=none,id=cd1,readonly=on" -device ide-cd,drive=cd1,bus=ide.2
)
if [[ -n "$virtio" ]]; then
    # shellcheck disable=SC2054
    cds+=(-drive "file=$virtio,media=cdrom,if=none,id=cd2,readonly=on" -device ide-cd,drive=cd2,bus=ide.3)
fi
mapfile -t args < <(guest_qemu_args "$disk" "${cds[@]}")
qemu-system-x86_64 "${args[@]}" -daemonize -pidfile "$GUEST_PIDFILE" >"$GUEST_QEMU_LOG" 2>&1 \
    || die "QEMU failed to start: $(cat "$GUEST_QEMU_LOG")"

# Windows Setup reboots several times on its own; the run is over when
# Invoke-BlorgGuest.ps1 -Step Image powers off and QEMU exits. A screenshot
# every two minutes shows where a build that hangs stopped (a setup dialog,
# a missing driver), so keep them -- CI uploads the directory on failure.
note "installing Windows (timeout ${BUILD_TIMEOUT_MIN} min, or ${STALL_MIN} min on one screen); screenshots in $GUEST_RUN_DIR/screens"
# Why an install was abandoned, as a check annotation: readable through the
# API from places where the log and the screenshots are not.
install_failed() {
    local why="$1" tail_serial agent_log
    guest_qmp screendump "{\"filename\": \"$GUEST_RUN_DIR/screens/failed.png\", \"format\": \"png\"}" >/dev/null 2>&1 || true
    tail_serial="$(tail -c 600 "$GUEST_SERIAL" 2>/dev/null | tr -cd '[:print:]\n')"
    agent_log="$(timeout 60 python3 "$BLORG_D/qmp.py" qga "$GUEST_QGA" exec 45 \
        "Get-Content C:\\blorgfs-image.log -Tail 15 -ErrorAction SilentlyContinue" 2>/dev/null | tail -c 1500)"
    guest_qmp quit >/dev/null 2>&1 || true
    local body="$why
setup transcript (C:\\blorgfs-image.log, via the guest agent): ${agent_log:-unreachable (not installed yet, or Windows is not up)}
serial: ${tail_serial:-empty}"
    [[ -n "${GITHUB_ACTIONS:-}" ]] &&
        echo "::error title=blorg image-build::$(printf '%s' "$body" | sed 's/%/%25/g' | awk 'BEGIN{ORS="%0A"} {print}')"
    die "$why; screens in $GUEST_RUN_DIR/screens"
}
start=$SECONDS
last_change=$SECONDS
shot=0
while guest_running; do
    if (( SECONDS - start > BUILD_TIMEOUT_MIN * 60 )); then
        install_failed "Windows Setup did not finish within ${BUILD_TIMEOUT_MIN} min"
    fi
    if (( SECONDS - last_change > STALL_MIN * 60 )); then
        install_failed "the guest's screen has not changed for ${STALL_MIN} min ($(( (SECONDS - start) / 60 )) min into the install): stuck"
    fi
    if (( (SECONDS - start) / 120 >= shot )); then
        png="$GUEST_RUN_DIR/screens/$(printf %03d "$shot").png"
        guest_qmp screendump "{\"filename\": \"$png\", \"format\": \"png\"}" >/dev/null 2>&1 || true
        # Also into the log, base64, whenever the screen changed: a log is
        # readable from places an artifact download is not (behind an
        # egress proxy), and a stuck install is diagnosed by its screen.
        if [[ -s "$png" ]]; then
            sum="$(sha256sum "$png" | cut -c1-16)"
            if [[ "$sum" != "${last_sum:-}" ]]; then
                echo "SCREEN $(printf %03d "$shot") t=$(( SECONDS - start ))s png-base64 $(base64 -w0 "$png")"
                last_sum="$sum"
                last_change=$SECONDS
            fi
        fi
        # A heartbeat every two minutes, so a slow install and a stuck one
        # read differently in the log.
        note "installing: $(( (SECONDS - start) / 60 )) min, screen unchanged for $(( (SECONDS - last_change) / 60 )) min"
        shot=$(( shot + 1 ))
    fi
    sleep 10
done
note "install finished in $(( (SECONDS - start) / 60 )) min"

# Verify pass: boot the result through blorg guest exactly as a test run will,
# and check what Invoke-BlorgGuest.ps1 -Step Image was meant to leave behind.
verify_env=(GUEST_GOLDEN="$disk" GUEST_KEY="$WORK/id_ed25519" GUEST_RUN_DIR="$WORK/verify")
env "${verify_env[@]}" "$BLORG" guest up --fresh
# A script file, not an inline command: OpenSSH hands an inline command to
# powershell.exe through Windows argv parsing, which mangles quotes.
cat > "$WORK/check.ps1" <<'PS1'
$f = @()
if ((bcdedit /enum '{current}' | Out-String) -notmatch 'testsigning\s+Yes') { $f += 'testsigning off' }
if ((Get-Service sshd).StartType -ne 'Automatic') { $f += 'sshd not automatic' }
$info = Get-Content C:\blorgfs-ci\image-info.json -Raw | ConvertFrom-Json
if ($info.failedSteps) { $f += "setup steps failed: $($info.failedSteps -join ', ')" }
$info | ConvertTo-Json -Compress
if ($f) { Write-Output ('IMAGE CHECK FAILED: ' + ($f -join '; ')); exit 1 }
exit 0
PS1
set +e
info="$(env "${verify_env[@]}" "$BLORG" guest ps "$WORK/check.ps1" | tr -d '\r')"
rc=$?
set -e
env "${verify_env[@]}" "$BLORG" guest down --graceful
echo "$info"
(( rc == 0 )) || die "golden image failed verification (C:\\blorgfs-image.log in the guest has the setup transcript)"

# Publish: verification booted an overlay, so $disk itself is untouched.
# Compressed, because CI caches it.
note "compressing"
# zstd rather than zlib: several times faster to decompress, which is
# paid on every warm run's first reads; -m/-W parallelise the compression.
qemu-img convert -c -O qcow2 -o compression_type=zstd -m 8 -W "$disk" "$GUEST_GOLDEN.tmp"
mv "$GUEST_GOLDEN.tmp" "$GUEST_GOLDEN"
mv "$WORK/id_ed25519" "$GUEST_KEY"
mv "$WORK/id_ed25519.pub" "$GUEST_KEY.pub"
printf '%s\n' "$info" | head -n1 > "$GUEST_IMAGE_DIR/image-info.json"
rm -rf "$WORK"
note "golden image ready: $GUEST_GOLDEN ($(du -h "$GUEST_GOLDEN" | cut -f1))"
)
