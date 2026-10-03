#!/usr/bin/env bash
#
# Runs an agent's script against the live guest and keeps everything it
# produced: the way a remote agent with no KVM of its own acts in the guest.
# The agent dispatches guest-runtime.yml with a `session` script, and reads
# the transcript and files back from the guest-results artifact, or from
# the job log, which carries the transcript and any screenshots (base64).
#
#   run-session.sh --out DIR SCRIPT
#
# SCRIPT is bash with guestctl on PATH and $SESSION_OUT (a directory whose
# contents are returned) set. It runs against the guest a test run left up
# (run-guest-tests.sh --keep): the package installed, B: mounted, the
# server answering. If no guest is running, a fresh one is booted.
# Commands are echoed (bash -x) into the transcript, so it reads as a log
# of what the agent did and what came back.
#
# Exit: the script's own exit code.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

out="$PWD/guest-results"
while (( $# > 1 )); do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        *) echo "run-session: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
script="${1:?usage: run-session.sh --out DIR SCRIPT}"
[[ -f "$script" ]] || { echo "run-session: no such script '$script'" >&2; exit 2; }

export PATH="$HERE/host:$PATH"
export SESSION_OUT="$out/session"
mkdir -p "$SESSION_OUT"
cp "$script" "$SESSION_OUT/script.sh"

guestctl status >/dev/null 2>&1 || guestctl up --fresh

bash -x "$script" 2>&1 | tee "$SESSION_OUT/transcript.txt"
rc=${PIPESTATUS[0]}
echo "exit=$rc" >> "$SESSION_OUT/transcript.txt"

# Images into the job log too, base64: an agent that can read the log but
# not download the artifact (an egress proxy in the way) still sees them.
for png in "$SESSION_OUT"/*.png; do
    [[ -f "$png" ]] && echo "SCREEN $(basename "$png") png-base64 $(base64 -w0 "$png")"
done

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
        echo "## Agent session: exit $rc"
        echo
        echo '```'
        tail -n 200 "$SESSION_OUT/transcript.txt"
        echo '```'
    } >> "$GITHUB_STEP_SUMMARY"
fi
exit "$rc"
