#!/usr/bin/env bash
#
# Runs tools/agent/blorg inside the blorg-agent image, against this
# checkout:   ci/agent/container/run.sh check
#             ci/agent/container/run.sh win build
#
# Uses podman when installed, else docker (BLORG_ENGINE overrides). Builds
# the image on first use (BLORG_IMAGE to use a prebuilt one instead).
#
# With podman the container runs rootless as you; --userns=keep-id keeps
# files written into the checkout owned by you, and keep-groups carries
# your kvm group membership in, so /dev/kvm works without root. Docker runs
# as root inside (its daemon is root anyway) and needs nothing extra.
#
# Mounted: the checkout at /src, a sibling ../server-rs when there is one,
# a named volume for blorg's cache, the guest images ($GUEST_HOME) when
# set, and /dev/kvm when the host has it. GH_TOKEN/BLORG_GH_TOKEN and the
# BLORG_*/GUEST_* settings are passed through. For BLORG_KVM_SSH, run
# blorg on the host instead: the work happens on the remote end anyway.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
IMAGE="${BLORG_IMAGE:-localhost/blorg-agent:latest}"

engine="${BLORG_ENGINE:-}"
if [[ -z "$engine" ]]; then
    if command -v podman >/dev/null; then engine=podman
    elif command -v docker >/dev/null; then engine=docker
    else echo "run.sh: neither podman nor docker is installed" >&2; exit 2; fi
fi

if ! "$engine" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "run.sh: building $IMAGE with $engine (first use)" >&2
    "$engine" build -t "$IMAGE" -f "$HERE/Containerfile" "$HERE" >&2
fi

args=(run --rm -i -v "$REPO:/src" -v blorg-cache:/var/cache/blorg -w /src)
[[ -t 0 && -t 1 ]] && args+=(-t)
if [[ "$engine" == podman ]]; then
    args+=(--userns=keep-id --group-add keep-groups)
fi
if [[ -e /dev/kvm ]]; then
    args+=(--device /dev/kvm)
fi
if [[ -f "$REPO/../server-rs/Cargo.toml" ]]; then
    args+=(-v "$(cd "$REPO/../server-rs" && pwd):/server-rs" -e BLORG_SERVER_RS=/server-rs)
fi
if [[ -n "${GUEST_HOME:-}" ]]; then
    args+=(-v "$GUEST_HOME:$GUEST_HOME" -e "GUEST_HOME=$GUEST_HOME")
fi
while IFS='=' read -r name _; do
    args+=(-e "$name")
done < <(env | grep -E '^(GH_TOKEN|BLORG_[A-Z_]+|GUEST_[A-Z_]+)=' | grep -vE '^(BLORG_CACHE|BLORG_SERVER_RS|GUEST_HOME)=')

exec "$engine" "${args[@]}" "$IMAGE" "$@"
