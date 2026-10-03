#!/usr/bin/env bash
#
# Warms a fresh Linux agent session for BlorgFS: runs `blorg setup` (flatcc
# and the generated headers, the WDK/SDK headers from NuGet, and -- when a
# server-rs checkout is present -- flatc and the Windows Rust target), then
# prints what the session can do.
#
# Use it as the setup script of a cloud agent environment, e.g.
#   Claude Code on the web: environment "Setup script"
#   Codex cloud:            environment "Setup script"
# with the line:  bash BlorgFS/tools/agent/session-start.sh
# (path relative to where the environment clones the repo). It never fails
# the session: everything it does, the blorg commands also do on demand.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BLORG="$REPO/tools/agent/blorg"

if [[ "$(uname -s)" != "Linux" ]]; then exit 0; fi

"$BLORG" setup >/tmp/blorg-setup.log 2>&1 || echo "blorg setup had failures; see /tmp/blorg-setup.log"
"$BLORG" doctor 2>/dev/null
echo
echo "BlorgFS agent toolchain: read 'Remote agents' in AGENTS.md; start with 'tools/agent/blorg check'."
exit 0
