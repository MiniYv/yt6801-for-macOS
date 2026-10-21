#!/bin/bash
#
# install.sh - Assemble the host app and activate the YT6801 DEXT (no root needed)
#
# Why no root is needed: /Applications is admin-group writable, and what
# actually needs root is sysextd itself (it shows the approval prompt), not
# this script. The host app lives in /Applications to pass sysextd's
# parent-bundle check.
#
# Key constraints (per Apple docs + the system's own dexts):
#   1. The dext bundle id must start with the host app's bundle id
#   2. The dext directory name must equal its bundle id (xxx.dext)
#   3. The driver_extension category requires OSBundleUsageDescription
#   4. OSBundleRequired must be DriverKit, not Root
#   5. Delegate callbacks arrive on the main queue; never block it
#
# No reboot, no nvram writes at any point.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DEXT_SRC="$ROOT/dist/yt6801.dext"
APP="/Applications/YT6801Driver.app"
SE="$APP/Contents/Library/SystemExtensions"
DEXT_ID="io.miniyv.yt6801ethernet"
LOG="/tmp/yt6801_activate.log"

green(){ printf '\033[32m%s\033[0m\n' "$*"; }
red()  { printf '\033[31m%s\033[0m\n' "$*"; }

[ -d "$DEXT_SRC" ] || { red "Cannot find $DEXT_SRC; run ./scripts/build.sh first"; exit 1; }

echo "==> Assembling host app: $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$SE"

cp "$ROOT/source/hostapp/Info.plist" "$APP/Contents/Info.plist"
printf '%s' 'APPL????' > "$APP/Contents/PkgInfo"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier io.miniyv.yt6801host" \
    "$APP/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Add :NSSystemExtensionUsageDescription string 'The YT6801 Ethernet driver requires a system extension to work.'" \
    "$APP/Contents/Info.plist" >/dev/null 2>&1

clang -fobjc-arc -O2 \
    -framework Foundation -framework SystemExtensions \
    -o "$APP/Contents/MacOS/yt6801_activate" \
    "$ROOT/source/hostapp/yt6801_activate.m" || exit 1

cp -R "$DEXT_SRC" "$SE/$DEXT_ID.dext"

# Sign the inner dext first, then the outer app
codesign --force --sign - --entitlements "$ROOT/source/yt6801/yt6801.entitlements" \
    --timestamp=none "$SE/$DEXT_ID.dext" 2>/dev/null
codesign --force --sign - --entitlements "$ROOT/source/hostapp/hostapp.entitlements" \
    --timestamp=none "$APP" 2>/dev/null

code_selfcheck() {
    codesign --verify --deep --strict "$APP" 2>&1 | sed 's/^/    /'
    local id
    id=$(plutil -extract CFBundleIdentifier raw "$SE/$DEXT_ID.dext/Contents/Info.plist")
    [ "$id" = "$DEXT_ID" ] || { red "Wrong dext bundle id: $id"; exit 1; }
}
echo "==> Signature self-check"
code_selfcheck

echo "==> Developer mode"
systemextensionsctl developer 2>/dev/null | sed 's/^/    /'

echo "==> Requesting activation (hard 3s timeout)"
# Hard 3s timeout: past "waiting for user approval" a human is slower than 3s
# and the request would linger as a stuck un-clickable pending entry.
: > "$LOG"
"$APP/Contents/MacOS/yt6801_activate" "$LOG" &
ACT_PID=$!
RC=124
for i in $(seq 1 30); do
    if ! kill -0 $ACT_PID 2>/dev/null; then
        wait $ACT_PID 2>/dev/null; RC=$?
        break
    fi
    sleep 0.1
done
if kill -0 $ACT_PID 2>/dev/null; then
    kill -9 $ACT_PID 2>/dev/null
    wait $ACT_PID 2>/dev/null
    echo "    Not done within 3s; gave up (no hanging wait for a click)"
    RC=124
fi
cat "$LOG" 2>/dev/null | sed 's/^/    /' | tail -6

echo
echo "==> Exit code $RC"
echo "==> Extension list"
systemextensionsctl list 2>/dev/null | sed 's/^/    /'
exit $RC
