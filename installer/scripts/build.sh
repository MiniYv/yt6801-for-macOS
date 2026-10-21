#!/bin/bash
#
# build.sh — assemble the two deliverables
#
#   dist/YT6801Driver.app        headless host (carries the dext, copied to
#                                /Applications during install)
#   dist/YT6801 安装器.app        GUI installer (embeds the host template;
#                                itself never needs installing)
#
#   The host template ships inside the installer as
#   Resources/HostTemplate.apppayload — not .app, otherwise LaunchServices
#   registers the nested app and the app list shows two YT6801Driver entries.
#
# Usage:
#   ./scripts/build.sh                 # use $ROOT/../dist/yt6801.dext
#   ./scripts/build.sh <some.dext>     # build with any DEXT (for testing other versions)
#   FORCE_VERSION=1.5.0 ./scripts/build.sh <old.dext>
#                                      # rollback testing (see below)
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DEXT_SRC="${1:-$ROOT/../dist/yt6801.dext}"
HOST="$ROOT/dist/YT6801Driver.app"
INST="$ROOT/dist/YT6801 安装器.app"
DEXT_ID="io.miniyv.yt6801ethernet"
HOST_ID="io.miniyv.yt6801host"

green(){ printf '\033[32m%s\033[0m\n' "$*"; }
red()  { printf '\033[31m%s\033[0m\n' "$*"; }

# dext dir name may carry a version suffix; normalize to the bundle id
if [ -f "$DEXT_SRC/Contents/Info.plist" ]; then
    DEXT_BUNDLE="$DEXT_SRC"
elif [ -d "$(dirname "$DEXT_SRC")/$DEXT_ID.dext" ]; then
    DEXT_BUNDLE="$(dirname "$DEXT_SRC")/$DEXT_ID.dext"
else
    red "DEXT not found: $DEXT_SRC"
    exit 1
fi

VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$DEXT_BUNDLE/Contents/Info.plist" 2>/dev/null)
[ -n "$VERSION" ] || { red "cannot read CFBundleVersion"; exit 1; }

echo "==> DEXT   : $DEXT_BUNDLE"
echo "==> Version: $VERSION"

# ---------------------------------------------------------------- ① headless host
echo "==> Building host (headless)"
rm -rf "$HOST"
mkdir -p "$HOST/Contents/MacOS" "$HOST/Contents/Library/SystemExtensions"

clang -fobjc-arc -O2 \
    -framework Foundation -framework SystemExtensions -framework Security \
    -o "$HOST/Contents/MacOS/yt6801host" \
    "$ROOT/source/hostmain.m" || { red "host build failed"; exit 1; }

cp "$ROOT/source/host_Info.plist" "$HOST/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$HOST/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$HOST/Contents/Info.plist" >/dev/null
printf '%s' 'APPL????' > "$HOST/Contents/PkgInfo"

cp -R "$DEXT_BUNDLE" "$HOST/Contents/Library/SystemExtensions/$DEXT_ID.dext"

# ---- rollback testing: raise the dext's CFBundleVersion above the installed one ----
# sysextd only replaces an installed system extension when the version
# increases; same version is silently skipped, downgrade rejected.
HOST_DEXT="$HOST/Contents/Library/SystemExtensions/$DEXT_ID.dext"
if [ -n "${FORCE_VERSION:-}" ]; then
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $FORCE_VERSION" \
        "$HOST_DEXT/Contents/Info.plist" >/dev/null
    /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${FORCE_VERSION}-rollback" \
        "$HOST_DEXT/Contents/Info.plist" >/dev/null
    /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $FORCE_VERSION" \
        "$HOST/Contents/Info.plist" >/dev/null
    echo "==> Version override: ${VERSION} -> ${FORCE_VERSION} (rollback testing only, source untouched)"
    VERSION="$FORCE_VERSION"
fi

echo "==> Signing host (dext first, then host)"
codesign --force --sign - --entitlements "$ROOT/source/dext.entitlements" \
    --timestamp=none "$HOST_DEXT" 2>&1 | sed 's/^/    /'
codesign --force --sign - --entitlements "$ROOT/source/hostapp.entitlements" \
    --timestamp=none "$HOST" 2>&1 | sed 's/^/    /'
codesign --verify --deep --strict "$HOST" && green "    host signature OK" || { red "    host signature failed"; exit 1; }

# ---------------------------------------------------------------- ② GUI installer
echo "==> Building installer GUI"
rm -rf "$INST"
mkdir -p "$INST/Contents/MacOS" "$INST/Contents/Resources"

clang -fobjc-arc -O2 \
    -DYT_BUILTIN_VERSION="\"$VERSION\"" \
    -framework Cocoa -framework SystemExtensions -framework Security \
    -o "$INST/Contents/MacOS/YT6801Installer" \
    "$ROOT/source/main.m" || { red "installer build failed"; exit 1; }

cp "$ROOT/source/Info.plist" "$INST/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$INST/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$INST/Contents/Info.plist" >/dev/null
printf '%s' 'APPL????' > "$INST/Contents/PkgInfo"

# Host template: copied wholesale to /Applications on "Install". The dir name
# deliberately has no .app suffix — LaunchServices registers any nested .app
# as a standalone app (duplicate YT6801Driver in Launchpad); the installer
# renames it to YT6801Driver.app when copying.
ditto --rsrc --extattr --noqtn "$HOST" "$INST/Contents/Resources/HostTemplate.apppayload"

echo "==> Signing installer (including embedded host template)"
codesign --force --sign - --timestamp=none "$INST" 2>&1 | sed 's/^/    /'
codesign --verify --deep --strict "$INST" && green "    signature OK" || { red "    signature verification failed"; exit 1; }

# ---------------------------------------------------------------- ③ verification
ID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$HOST/Contents/Library/SystemExtensions/$DEXT_ID.dext/Contents/Info.plist")
[ "$ID" = "$DEXT_ID" ] && green "    dext id OK (${ID})" || { red "    dext id mismatch: $ID"; exit 1; }
HID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$HOST/Contents/Info.plist")
[ "$HID" = "$HOST_ID" ] && green "    host id OK (${HID})" || { red "    host id mismatch: $HID (sysextd treats it as a new app, re-approval may be required)"; exit 1; }
IID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$INST/Contents/Info.plist")
[ "$IID" = "io.miniyv.yt6801.installer" ] && green "    installer id OK (${IID})" || { red "    installer id mismatch: $IID"; exit 1; }

# Strip leftover quarantine attributes so Gatekeeper doesn't block the first launch
xattr -cr "$HOST" "$INST" 2>/dev/null || true

# ---------------------------------------------------------------- ③ packaging
# .app bundles exist only briefly while packaging DMG/ZIP and are renamed to
# .app.bak right after; not a bundle, so Spotlight never indexes a second YT6801.
echo "==> Packaging"
DMG="$ROOT/dist/YT6801-安装器-${VERSION}.dmg"
ZIP="$ROOT/dist/YT6801-安装器-${VERSION}.zip"
rm -f "$DMG" "$ZIP"

STAGE=$(mktemp -d)
ditto --rsrc --extattr --noqtn "$INST" "$STAGE/YT6801 安装器.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "YT6801 安装器" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null 2>&1 \
    && green "    DMG OK" || { red "    DMG packaging failed"; rm -rf "$STAGE"; exit 1; }
rm -rf "$STAGE"

# ZIP must be written with Python zipfile: the system zip/ditto never set the
# UTF-8 flag for non-ASCII names, so the Chinese filenames garble on extract.
/usr/bin/python3 - "$INST" "$ZIP" <<'PY' || { red "    ZIP packaging failed"; exit 1; }
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dp, dn, fn in os.walk(src):
        dn.sort(); fn.sort()
        rel = os.path.relpath(dp, os.path.dirname(src))
        zi = zipfile.ZipInfo(rel + "/"); zi.create_system = 3
        zi.external_attr = (0o40755 << 16) | 0x10
        zi.compress_type = zipfile.ZIP_STORED
        z.writestr(zi, b"")
        for f in fn:
            fp = os.path.join(dp, f)
            zi = zipfile.ZipInfo(os.path.join(rel, f)); zi.create_system = 3
            zi.external_attr = (os.stat(fp).st_mode & 0xFFFF) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(fp, "rb") as fh:
                z.writestr(zi, fh.read())
PY
green "    ZIP OK"

# ---------------------------------------------------------------- ④ wrap-up
# Leave no bare app behind: zip it, then delete the directory. LaunchServices
# detects an app by Contents/Info.plist (CFBundlePackageType=APPL) regardless
# of extension, so renaming to .app.bak alone is not enough.
mkdir -p "$ROOT/dist/build"
rm -rf "$ROOT/dist/build/YT6801Driver.zip" "$ROOT/dist/build/YT6801-安装器.zip" \
       "$ROOT/dist/build/YT6801Driver.app.bak" "$ROOT/dist/build/YT6801 安装器.app.bak"
ditto -c -k --sequesterRsrc --keepParent "$HOST" "$ROOT/dist/build/YT6801Driver.zip"
ditto -c -k --sequesterRsrc --keepParent "$INST" "$ROOT/dist/build/YT6801-安装器.zip"
rm -rf "$HOST" "$INST"

# The dirs existed briefly during the build and may have been registered; unregister them
LSREG=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$LSREG" -u "$HOST" 2>/dev/null || true
"$LSREG" -u "$INST" 2>/dev/null || true

echo
green "================ Build complete ================"
echo "Version: $VERSION"
echo "DMG  : $DMG"
echo "ZIP  : $ZIP"
du -sh "$DMG" "$ZIP" 2>/dev/null | sed 's/^/Size : /'
echo "Bare app: dist/build/*.zip (zipped, not recognized as apps)"
