#!/usr/bin/env bash
#
# One-time setup of a Linux KVM host for agent builds and tests: QEMU, the
# golden test image, the build VM layer on top of it, and (optionally) a
# self-hosted GitHub Actions runner, so a cloud agent that can reach only
# GitHub can still use the host through agent-remote.yml.
#
#   setup-kvm-host.sh [--runner-token TOKEN] [--runner-name NAME]
#
# Run it as the (non-root, sudo-capable) user that will own the images and
# run the runner, from a BlorgFS checkout that carries ci/guest/. Any host
# with /dev/kvm works: a Linux box, or the Azure VM from
# ci/guest/infra/azure (Dsv5, nested virtualisation).
#
# --runner-token is a repository runner registration token (Settings >
# Actions > Runners > New self-hosted runner, or
#   gh api -X POST repos/Chuccle/BlorgFS/actions/runners/registration-token --jq .token
# ). The runner gets the label `blorg-kvm`, which is what agent-remote.yml
# asks for with runner=kvm.
#
# BlorgFS is a public repository. A self-hosted runner there runs whatever a
# workflow on any branch or fork PR tells it to, so before registering one
# set Settings > Actions > General > "Require approval for all external
# contributors", and keep this host for nothing but this rig.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

runner_token="" runner_name="$(hostname)-blorg-kvm"
while (( $# )); do
    case "$1" in
        --runner-token) runner_token="$2"; shift 2 ;;
        --runner-name)  runner_name="$2"; shift 2 ;;
        *) echo "setup-kvm-host: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[[ $EUID -ne 0 ]] || { echo "setup-kvm-host: run as the user who will own the images, not root" >&2; exit 2; }
[[ -x "$REPO/ci/guest/host/install-host-deps.sh" ]] || { echo "setup-kvm-host: ci/guest/ is not in this checkout" >&2; exit 2; }

echo "==> host packages and /dev/kvm"
"$REPO/ci/guest/host/install-host-deps.sh"
sudo apt-get install -y -qq --no-install-recommends rsync git jq >/dev/null

echo "==> golden test image"
# shellcheck source=/dev/null
source "$REPO/ci/guest/host/lib.sh"
if [[ -f "$GUEST_GOLDEN" ]]; then
    echo "  present: $GUEST_GOLDEN"
else
    "$REPO/ci/guest/image/build-image.sh"
fi

echo "==> build VM layer"
"$REPO/ci/agent/build-vm/build-layer.sh"

if [[ -n "$runner_token" ]]; then
    echo "==> GitHub Actions runner ($runner_name, label blorg-kvm)"
    dir="$HOME/actions-runner"
    mkdir -p "$dir"
    if [[ ! -x "$dir/config.sh" ]]; then
        ver="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | jq -r .tag_name | sed 's/^v//')"
        curl -fsSL "https://github.com/actions/runner/releases/download/v$ver/actions-runner-linux-x64-$ver.tar.gz" | tar -xz -C "$dir"
    fi
    (cd "$dir" && ./config.sh --unattended --replace --url https://github.com/Chuccle/BlorgFS \
        --token "$runner_token" --name "$runner_name" --labels blorg-kvm --work _work)
    # The runner's jobs find the images where this script put them.
    echo "GUEST_HOME=$GUEST_HOME" >> "$dir/.env"
    (cd "$dir" && sudo ./svc.sh install "$USER" && sudo ./svc.sh start)
fi

echo
echo "KVM host ready. From here:      tools/agent/blorg win build && tools/agent/blorg win test"
echo "From an agent with SSH access:  BLORG_KVM_SSH=$USER@<this host> tools/agent/blorg win build"
[[ -n "$runner_token" ]] && echo "From an agent with only GitHub: tools/agent/blorg ci remote --runner kvm"
exit 0
