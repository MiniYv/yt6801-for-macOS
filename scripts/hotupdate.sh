#!/bin/bash
#
# hotupdate.sh - In-place hot update: no uninstall, no reinstall, no prompt
#
# `systemextensionsctl uninstall` deletes the approval record, so reinstalling
# prompts again even though the extension was already enabled. Apple's REPLACE
# semantics avoid that: same bundle id with a higher CFBundleVersion submitted
# as a replacement - sysextd verifies the higher version, requires the old
# dext process to exit first (it holds IOPCIDevice), and persists
# activated_enabled immediately without a prompt.
#
# Killing the old process needs root; the password comes from ~/.yt6801_sudo
# (mode 600). No reboot, no nvram writes at any point.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ID="io.miniyv.yt6801ethernet"
APP="/Applications/YT6801Driver.app"
SE="$APP/Contents/Library/SystemExtensions"
PLIST="$ROOT/source/yt6801/Info.plist"
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

pkill -f "yt6801_activate" 2>/dev/null
pkill -f "keep_pending"    2>/dev/null

# 1) Bump the version number - hard prerequisite for the REPLACE branch
CUR=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$PLIST" 2>/dev/null || echo "1.0.0")
NEW=$(echo "$CUR" | awk -F. '{ if (NF>=3) printf "%s.%s.%d", $1,$2,$3+1; else printf "%s.0", $1 }')
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $NEW"           "$PLIST" >/dev/null
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $NEW" "$PLIST" >/dev/null
echo "==> Version $CUR -> $NEW"

# 2) Build
echo "==> Building"
if ! ./scripts/build.sh > /tmp/yt_build.log 2>&1; then
    red "Build failed:"; tail -25 /tmp/yt_build.log; exit 1
fi

# 3) Assemble the app: replace the dext in place, approved identity unchanged
echo "==> Assembling and signing"
rm -rf "$SE/$ID.dext"
mkdir -p "$SE"
cp -R "$ROOT/dist/yt6801.dext" "$SE/$ID.dext"
codesign --force --sign - --entitlements "$ROOT/source/yt6801/yt6801.entitlements" \
    --timestamp=none "$SE/$ID.dext" 2>/dev/null
codesign --force --sign - --entitlements "$ROOT/source/hostapp/hostapp.entitlements" \
    --timestamp=none "$APP" 2>/dev/null

NEW_CD=$(codesign -dv --verbose=4 "$SE/$ID.dext" 2>&1 | awk -F= '/^CDHash=/{print $2}')
dim "New dext CDHash = $NEW_CD"

# 4) Kill the old dext process; IOKit will not instantiate the new version
#    until it exits
OLD_PIDS=$(pgrep -f "$ID.dext" | tr '\n' ' ')
if [ -n "${OLD_PIDS// /}" ]; then
    echo "==> Terminating old driver processes: $OLD_PIDS"
    sudo_do kill -TERM $OLD_PIDS
    for i in $(seq 1 20); do
        pgrep -f "$ID.dext" >/dev/null || break
        sleep 0.3
    done
    if pgrep -f "$ID.dext" >/dev/null; then
        dim "TERM ineffective; using KILL"
        sudo_do kill -9 $(pgrep -f "$ID.dext")
        sleep 1
    fi
else
    dim "No running old driver process"
fi

# 5) Submit the REPLACE request (an already-approved extension does not prompt)
STAMP=$(date +%Y%m%d_%H%M%S)
DRVLOG="$ROOT/logs/driver_${STAMP}.log"
mkdir -p "$ROOT/logs"
echo "$DRVLOG" > /tmp/last_drv_log.txt

echo "==> Submitting replacement request"
/usr/bin/log stream --level default --style compact \
    --predicate 'eventMessage CONTAINS "yt6801: "' > "${DRVLOG}" 2>&1 &
STREAM=$!
sleep 1

# Hard 3s timeout: a hanging request stays in the system as a stuck pending
# entry and stacks with the next request.
: > /tmp/yt6801_activate.log
"$APP/Contents/MacOS/yt6801_activate" /tmp/yt6801_activate.log > /dev/null 2>&1 &
APID=$!
RC=124
for i in $(seq 1 30); do
    if ! kill -0 $APID 2>/dev/null; then
        wait $APID 2>/dev/null; RC=$?
        break
    fi
    sleep 0.1
done
if kill -0 $APID 2>/dev/null; then
    kill -9 $APID 2>/dev/null
    wait $APID 2>/dev/null
    red "    Activation request did not finish within 3s; abandoned (no hanging wait)"
    RC=124
else
    grep -E "RESULT" /tmp/yt6801_activate.log | tail -1 | sed 's/^/    /'
fi
sleep 1
kill $STREAM 2>/dev/null; wait $STREAM 2>/dev/null

# 6) Result
echo
echo "==> Driver init log"
grep -F 'yt6801: ' "${DRVLOG}" 2>/dev/null \
    | sed 's/.*yt6801: /    yt6801: /' | grep -v '^    yt6801: $' | awk '!seen[$0]++'

STATE=$(systemextensionsctl list 2>/dev/null | grep "$ID" | tail -1 | sed 's/^\s*//')
echo
echo "==> Extension state"
echo "    $STATE"

# Prove "the new binary is running" via CDHash, not by trusting the state
sleep 1
RUN_CD=$(ioreg -r -c IOUserNetworkEthernet -w0 2>/dev/null | awk -F'"' '/IOUserServerCDHash/{print $4; exit}')
echo "==> Running CDHash: ${RUN_CD:-(no instance)}"
if [ -n "${RUN_CD:-}" ] && [ "$RUN_CD" = "${NEW_CD}" ]; then
    green "    OK: the running binary is the one just built"
else
    red "    FAILED: the running binary is not the new one (old=$RUN_CD new=${NEW_CD})"
fi

echo "==> Network interfaces"
for i in $(ifconfig -l); do
    case "$i" in en*) printf '    %-5s %s\n' "$i" "$(ifconfig $i 2>/dev/null | awk '/ether/{print $2}')";; esac
done

exit $RC
