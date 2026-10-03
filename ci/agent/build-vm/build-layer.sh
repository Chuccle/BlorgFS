#!/usr/bin/env bash
#
# Builds the build VM image: the golden test image plus the Windows build
# toolchain (Install-BuildToolchain.ps1), as a qcow2 overlay next to it.
#
#   build-layer.sh [--force]
#
# Output: $GUEST_IMAGE_DIR/build.qcow2, backed by golden.qcow2. The golden
# image is never written, so the test guest keeps testing exactly what CI
# shipped; only the build VM boots this layer. Rebuild it whenever the
# golden image is rebuilt (its backing file changes underneath it).
#
# Takes roughly 20-40 minutes, nearly all of it the Build Tools install.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
GUESTCTL="$REPO/ci/guest/host/guestctl"
[[ -x "$GUESTCTL" ]] || { echo "build-layer: ci/guest/ (the test-guest rig) is not in this checkout" >&2; exit 2; }
# shellcheck source=/dev/null
source "$REPO/ci/guest/host/lib.sh"

layer="$GUEST_IMAGE_DIR/build.qcow2"
golden="$GUEST_GOLDEN"
[[ -f "$golden" ]] || die "no golden image at $golden; build it first with ci/guest/image/build-image.sh"
if [[ -f "$layer" && "${1:-}" != "--force" ]]; then
    note "build layer already exists at $layer (--force to rebuild)"
    exit 0
fi

# Its own run directory and port, so this never touches a running test or
# build guest.
export GUEST_RUN_DIR="$GUEST_HOME/layer-build"
export GUEST_SSH_PORT="${LAYER_SSH_PORT:-2224}"
export GUEST_MEM_MB="${GUEST_MEM_MB:-8192}"
rm -rf "$GUEST_RUN_DIR"

note "booting a fresh guest from the golden image"
"$GUESTCTL" up --fresh
note "installing the build toolchain"
if ! "$GUESTCTL" ps "$HERE/Install-BuildToolchain.ps1"; then
    "$GUESTCTL" screenshot "$GUEST_HOME/layer-build-failure.png" || true
    "$GUESTCTL" down
    die "Install-BuildToolchain.ps1 failed (screen: $GUEST_HOME/layer-build-failure.png)"
fi
"$GUESTCTL" down --graceful

# The run disk is already an overlay on the golden image; it becomes the
# layer as is.
mv -f "$GUEST_RUN_DIR/disk.qcow2" "$layer"
rm -rf "$GUEST_RUN_DIR"
note "build layer ready: $layer ($(du -h "$layer" | cut -f1) on top of the golden image)"
