#!/bin/bash
#
# redeploy.sh - Build -> uninstall old instance -> install new version ->
#               capture driver startup logs
#
# Uninstall first because on same-ID replacement sysextd only adds the new
# bundle to DextRecordTable while the old dext process keeps holding
# IOPCIDevice; the kernel never instantiates a second IOUserNetworkEthernet
# for the same PCI device, so the new code never runs. Uninstall terminates
# the old process and releases the device. No reboot, no nvram writes.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ID="io.miniyv.yt6801ethernet"
STAMP=$(date +%Y%m%d_%H%M%S)
LOG="$ROOT/logs/driver_${STAMP}.log"

[ "${1:-}" = "nobuild" ] || ./scripts/build.sh >/dev/null 2>&1 || {
    echo "Build failed"; ./scripts/build.sh 2>&1 | tail -20; exit 1; }

echo "==> Uninstalling old instance"
systemextensionsctl uninstall - "$ID" >/dev/null 2>&1
for i in $(seq 1 20); do
    pgrep -f "$ID.dext" >/dev/null || break
    sleep 0.5
done
if pgrep -f "$ID.dext" >/dev/null; then
    echo "!! Old instance still running; cannot continue"; exit 1
fi

echo "==> Starting log capture: $LOG"
/usr/bin/log stream --level default --style compact \
    --predicate 'eventMessage CONTAINS "yt6801"' > "$LOG" 2>&1 &
STREAM=$!
sleep 1

echo "==> Installing new version"
./scripts/install.sh > /tmp/install_last.txt 2>&1
tail -6 /tmp/install_last.txt | sed 's/^/    /'

sleep 3
kill $STREAM 2>/dev/null
pkill -f "log stream" 2>/dev/null

echo "==> Driver init log"
grep -F 'yt6801: ' "$LOG" | sed 's/.*yt6801: /    yt6801: /' | awk '!seen[$0]++'

echo
echo "==> Extension state"
systemextensionsctl list 2>/dev/null | tail -2 | sed 's/^/    /'
echo "==> Interfaces"
ifconfig -l | tr ' ' '\n' | tail -4 | sed 's/^/    /'
echo "$LOG" > /tmp/last_drv_log.txt
