#!/usr/bin/env bash
#
# Prepares a Debian/Ubuntu machine to host the BlorgFS test guest: QEMU,
# the image-build tools, and access to /dev/kvm for the current user.
# Used by CI (GitHub's ubuntu runners expose KVM) and by the Azure host's
# cloud-init; on any other Linux box with KVM it is the whole setup.

set -euo pipefail

sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    qemu-system-x86 qemu-utils xorriso openssh-client python3 curl openssl ca-certificates

if [[ ! -e /dev/kvm ]]; then
    echo "install-host-deps: /dev/kvm does not exist -- this machine has no hardware virtualization exposed." >&2
    echo "install-host-deps: on a cloud VM, pick a size with nested virtualization (e.g. Azure Dsv5)." >&2
    exit 1
fi

# GitHub's runners ship /dev/kvm root-only. A udev rule (rather than a
# one-off chmod) keeps it usable if the device node is recreated.
echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' \
    | sudo tee /etc/udev/rules.d/99-blorgfs-kvm.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
sudo chmod 0666 /dev/kvm

echo "install-host-deps: ready ($(qemu-system-x86_64 --version | head -n1))"
