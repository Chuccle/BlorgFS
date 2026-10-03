#!/usr/bin/env bash
#
# Runs tools/agent/blorg inside the blorg-agent image, against this
# checkout:   ci/agent/container/run.sh check
#
# Uses podman when installed, else docker (BLORG_ENGINE overrides). Builds
# the image on first use (BLORG_IMAGE to use a prebuilt one instead).
# With podman, --userns=keep-id keeps files written into the checkout
# owned by you.
#
# Mounted: the checkout at /src, a sibling ../server-rs when there is one,
# and a named volume for blorg's cache. GH_TOKEN, the BLORG_* settings and
# any proxy variables are passed through.

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
[[ "$engine" == podman ]] && args+=(--userns=keep-id)
if [[ -f "$REPO/../server-rs/Cargo.toml" ]]; then
    args+=(-v "$(cd "$REPO/../server-rs" && pwd):/server-rs" -e BLORG_SERVER_RS=/server-rs)
fi
while IFS='=' read -r name _; do
    args+=(-e "$name")
done < <(env | grep -E '^(GH_TOKEN|BLORG_[A-Z_]+|(HTTPS?|NO)_PROXY|(https?|no)_proxy)=' | grep -vE '^(BLORG_CACHE|BLORG_SERVER_RS)=')

exec "$engine" "${args[@]}" "$IMAGE" "$@"
