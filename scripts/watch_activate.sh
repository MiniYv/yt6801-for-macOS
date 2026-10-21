#!/bin/bash
#
# watch_activate.sh - Watch the DEXT approval state and run follow-up checks
#                     automatically once approved. Approval is a GUI toggle
#                     only a human can click; everything after it is automatic.
#
# Usage: ./scripts/watch_activate.sh &
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

STAMP=$(date +%Y%m%d_%H%M%S)
LOG="logs/dext_watch_$STAMP.log"
DEXT_ID="io.miniyv.yt6801ethernet"

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG"; }

say "Watching approval state of $DEXT_ID (every 2s, up to 3h)"

deadline=$(( $(date +%s) + 10800 ))
last=""
approved=0

while [ "$(date +%s)" -lt "$deadline" ]; do
    line=$(systemextensionsctl list 2>/dev/null | grep "$DEXT_ID" || true)
    state=""
    case "$line" in
        *"waiting for user"*)      state="waiting" ;;
        *"activated enabled"*)     state="enabled" ;;
        *"activated disabled"*)    state="disabled" ;;
        *"terminated"*)            state="terminated" ;;
        "")                        state="absent" ;;
        *)                         state="other" ;;
    esac

    if [ "$state" != "$last" ]; then
        say "State changed to: $state"
        [ -n "$line" ] && say "  $line"
        last="$state"
    fi

    if [ "$state" = "enabled" ]; then
        approved=1
        break
    fi
    sleep 2
done

if [ "$approved" -ne 1 ]; then
    say "Timed out / not approved; exiting"
    exit 1
fi

say "======== Approved, extension is enabled; starting automatic checks ========"

sleep 3
say "---- 1. Extension state ----"
systemextensionsctl list 2>&1 | grep -E "$DEXT_ID|driver_extension" | tee -a "$LOG"

say "---- 2. dext process ----"
ps aux | grep -i "[y]t6801" | tee -a "$LOG"

say "---- 3. Our service in IORegistry ----"
ioreg -r -c IOUserNetworkEthernet -l 2>/dev/null | grep -E "IOClass|IOUserClass|IOUserServerName|BSD Name|IOMACAddress" | head -40 | tee -a "$LOG"

say "---- 4. Network interfaces ----"
ifconfig -l 2>&1 | tee -a "$LOG"

say "---- 5. Driver's own logs (last 5 minutes) ----"
log show --last 5m --info --predicate 'eventMessage CONTAINS "yt6801"' 2>/dev/null | tail -40 | tee -a "$LOG"

say "======== Automatic checks finished; see $LOG for results ========"
