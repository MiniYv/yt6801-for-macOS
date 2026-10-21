#!/bin/bash
#
# update.sh - One command: edit code -> build -> install -> view driver logs
#
# No approval prompts: repeated activate requests pile up pending entries in
# System Settings and stacked dialogs block each other, so this script kills
# leftover activation processes and swaps via uninstall + install (no root,
# no GUI approval, no reboot, no nvram writes).
#
# Live log streaming only: the unified log history store is unusable on this
# machine (log show reports invalid log archive), so a log stream is started
# before install and stopped afterwards.
#
# Usage:
#   ./scripts/update.sh            build + swap + print logs
#   ./scripts/update.sh nobuild    skip build, swap only (pure verification)
#   ./scripts/update.sh logs       capture 15s of live logs only
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ID="io.miniyv.yt6801ethernet"
PASSFILE="$HOME/.yt6801_sudo"

green(){ printf '\033[32m%s\033[0m\n' "$*"; }
red()  { printf '\033[31m%s\033[0m\n' "$*"; }
dim()  { printf '\033[2m%s\033[0m\n' "$*"; }

sudo_do() {
    if [ -f "$PASSFILE" ]; then
        sudo -S -p '' "$@" < "$PASSFILE" 2>/dev/null
    else
        sudo "$@"
    fi
}

# ---------- Logs-only mode ----------
if [ "${1:-}" = "logs" ]; then
    echo "Capturing 15s of live logs (trigger some NIC traffic to test)..."
    /usr/bin/log stream --level default --style compact \
        --predicate 'eventMessage CONTAINS "yt6801: "' 2>&1 &
    P=$!; sleep 15; kill $P 2>/dev/null
    exit 0
fi

# ---------- 0. Kill leftover activation processes ----------
# They keep an OSSystemExtensionRequest hanging -> stuck pending entries.
STALE=$(pgrep -f "yt6801_activate" 2>/dev/null | tr '\n' ' ')
if [ -n "${STALE// /}" ]; then
    dim "Killing leftover activation processes: $STALE"
    pkill -f "yt6801_activate" 2>/dev/null
    sleep 1
fi
pkill -f "keep_pending" 2>/dev/null

# ---------- 1. Build ----------
if [ "${1:-}" != "nobuild" ]; then
    echo "==> Building"
    if ! ./scripts/build.sh > /tmp/yt_build.log 2>&1; then
        red "Build failed:"
        tail -25 /tmp/yt_build.log
        exit 1
    fi
    grep -E "^(Artifact|Size)" /tmp/yt_build.log | sed 's/^/    /'
fi

# ---------- 2. Uninstall the old instance ----------
# Required, not just cleanliness: on same-ID replacement sysextd only registers
# the new bundle in DextRecordTable; the old dext process keeps holding
# IOPCIDevice and the new code never runs. Uninstall terminates it and
# releases the PCI device.
echo "==> Uninstalling old instance (no password, no prompt)"
if ! systemextensionsctl uninstall - "$ID" >/dev/null 2>&1; then
    dim "Plain call failed; retrying with sudo"
    sudo_do systemextensionsctl uninstall - "$ID" >/dev/null 2>&1
fi

for i in $(seq 1 24); do
    pgrep -f "$ID.dext" >/dev/null || break
    sleep 0.5
done
if pgrep -f "$ID.dext" >/dev/null; then
    red "Old instance still running; aborting (installing anyway would keep old behavior)"
    exit 1
fi
dim "Old instance fully exited; PCI device released"

# ---------- 3. Capture logs + install new version ----------
STAMP=$(date +%Y%m%d_%H%M%S)
DRVLOG="$ROOT/logs/driver_${STAMP}.log"
mkdir -p "$ROOT/logs"
echo "$DRVLOG" > /tmp/last_drv_log.txt

echo "==> Installing (log: ${DRVLOG})"
/usr/bin/log stream --level default --style compact \
    --predicate 'eventMessage CONTAINS "yt6801: "' > "${DRVLOG}" 2>&1 &
STREAM=$!
sleep 1

./scripts/install.sh > /tmp/yt_install.log 2>&1
RC=$?

sleep 3
kill $STREAM 2>/dev/null
wait $STREAM 2>/dev/null

STATE=$(systemextensionsctl list 2>/dev/null | grep "$ID" | tail -1)

# ---------- 4. Result ----------
echo
echo "==> Driver init log"
grep -F 'yt6801: ' "${DRVLOG}" 2>/dev/null \
    | sed 's/.*yt6801: /    yt6801: /' \
    | grep -v '^    yt6801: $' | awk '!seen[$0]++'

echo
echo "==> Extension state"
echo "    $STATE" | sed 's/^\s*//'
echo "==> Network interfaces"
for i in $(ifconfig -l); do
    case "$i" in en*) echo "    $i  $(ifconfig $i 2>/dev/null | awk '/ether/{print $2}')";; esac
done

if echo "$STATE" | grep -q "waiting for user"; then
    echo
    red "Extension is waiting for user approval:"
    echo "    System Settings -> General -> Login Items & Extensions -> Driver Extensions (i) -> yt6801"
    echo "    (Approving once is enough; afterwards the script uses uninstall/install and never stacks requests)"
fi

exit $RC
