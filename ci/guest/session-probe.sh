# shellcheck shell=bash
# The example session: touches every way of acting in the guest once,
# and fails if any of them does not work. Run by guest-runtime.yml on PRs
# that change the rig, and a worked example of a `session` script.
# Runs under run-session.sh (guestctl on PATH, $SESSION_OUT set).

set -euo pipefail

# Run a command, read its output. The driver's own state is the tests'
# business, not the probe's: those lines show it but cannot fail the probe.
guestctl ssh '[Environment]::OSVersion.VersionString; hostname'
guestctl ssh 'sc.exe query BlorgFS' || true
guestctl ssh 'Get-ChildItem B:\ | Select-Object -ExpandProperty Name' || true

# Copy a file in, have the guest transform it, copy the result out.
echo "probe $(date -u +%FT%TZ)" > "$SESSION_OUT/in.txt"
guestctl push "$SESSION_OUT/in.txt" C:/blorgfs-ci/probe/in.txt
guestctl ssh '(Get-Content C:/blorgfs-ci/probe/in.txt).ToUpper() | Set-Content C:/blorgfs-ci/probe/out.txt'
guestctl pull C:/blorgfs-ci/probe/out.txt "$SESSION_OUT/out.txt"
[[ "$(tr -d '\r' < "$SESSION_OUT/out.txt")" == "$(tr '[:lower:]' '[:upper:]' < "$SESSION_OUT/in.txt")" ]]

# Read a file the driver serves, through B:.
guestctl ssh '(Get-FileHash B:\names\no-extension -Algorithm SHA256).Hash' || true

# See the screen.
guestctl screenshot "$SESSION_OUT/screen.png"

# The guest-agent channel, which works when SSH does not.
guestctl qga-exec 'Get-Service sshd | Select-Object -ExpandProperty Status'

# Snapshot, change something, revert: the change must be gone.
guestctl snapshot probe
guestctl ssh 'New-Item -ItemType File -Force C:/blorgfs-ci/probe/after-snapshot.txt | Out-Null'
guestctl revert probe
guestctl wait 300
[[ "$(guestctl ssh 'Test-Path C:/blorgfs-ci/probe/after-snapshot.txt' | tr -d '\r')" == "False" ]]

echo "session probe: every channel works"
