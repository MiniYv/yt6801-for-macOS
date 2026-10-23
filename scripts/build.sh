#!/bin/bash
#
# build.sh - Build and package the Motorcomm YT6801 DriverKit driver (.dext)
#
# No xcodebuild: toolchain is CommandLineTools clang++ plus DriverKit.sdk and
# iig extracted from Xcode_26.2_Universal.xip (no Xcode install, no developer
# account, no network provisioning). Target is driverkit24.2 so macOS 15.2+
# loads the dext; the vendored SDK (26.x) is newer than the target, which is
# fine for clang.
#
# Usage:
#   ./scripts/build.sh            build + package + sign
#   ./scripts/build.sh clean      remove build/ and dist/
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# ---------- Paths ----------
# All toolchain paths can be overridden via environment (CI runners point them
# at Xcode's own DriverKit SDK instead of the vendored copy).
DKX="${DKX:-$ROOT/research/headers/dk_real/DriverKit.sdk}"
IIGBIN="${IIGBIN:-$ROOT/research/headers/tools/iig}"
CLANG="${CLANG:-/Library/Developer/CommandLineTools/usr/bin/clang++}"
STRIP="${STRIP:-/Library/Developer/CommandLineTools/usr/bin/strip}"
PLUTIL="/usr/bin/plutil"
CLT_LIB="${CLT_LIB:-/Library/Developer/CommandLineTools/usr/lib}"

BUILD="$ROOT/build"
GEN="$BUILD/gen"
OBJ="$BUILD/obj"
DIST="$ROOT/dist"
DEXT="$DIST/yt6801.dext"

SRC="$ROOT/source/yt6801"
NAME="yt6801"
BUNDLE_ID="io.miniyv.yt6801ethernet"
CLASS_NAME="io_miniyv_yt6801ethernet"

TARGET="x86_64-apple-driverkit24.2"
SYSINC="$DKX/System/DriverKit/usr/include"
FWROOT="$DKX/System/DriverKit/System/Library/Frameworks"

die() { echo "Error: $*" >&2; exit 1; }

if [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD" "$DIST"
    echo "Cleaned build/ and dist/"
    exit 0
fi

# ---------- Toolchain checks ----------
[ -x "$CLANG" ]  || die "clang++ not found: $CLANG (install Command Line Tools first)"
[ -x "$IIGBIN" ] || die "iig not found: $IIGBIN"
[ -d "$DKX" ]    || die "DriverKit SDK not found: $DKX"
[ -d "$FWROOT" ] || die "Frameworks directory missing in SDK"
[ -f "$CLT_LIB/libclang.dylib" ] || die "iig requires libclang.dylib: $CLT_LIB"

mkdir -p "$GEN/$BUNDLE_ID" "$OBJ" "$DEXT/Contents/MacOS"

# Runtime version string comes from the plist so the startup log and
# CFBundleVersion can never drift apart.
DRV_VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$SRC/Info.plist" 2>/dev/null)
[ -n "$DRV_VER" ] || die "cannot read CFBundleVersion from $SRC/Info.plist"

# ---------- Compile flags ----------
# -isysroot must point to DriverKit.sdk itself, not MacOSX.sdk: the DriverKit
# SDK is self-contained, and mixing in MacOSX.sdk/Kernel headers redefines
# UInt64/bool in MacTypes.h and clashes on ffs in libkern.
CFLAGS=(
    -target "$TARGET"
    -isysroot "$DKX"
    -I"$SYSINC"
    -F"$FWROOT"
    -I"$SRC"
    -I"$GEN"
    -std=c++17
    -O2
    -g
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fvisibility=hidden
    -Wno-deprecated-declarations      # those 10 pure-virtual methods are already marked __deprecated
    -Wno-nullability-completeness
    -Wno-unused-parameter
    -DYT6801_DRV_VERSION="\"$DRV_VER\""
)

# ---------- 1. iig ----------
echo "[1/5] Generating interface code with iig"
# The generated .iig.cpp hardcodes #include <$BUNDLE_ID/$CLASS_NAME.h>,
# so the header must land in that exact directory; --header and -I must match.
DYLD_LIBRARY_PATH="$CLT_LIB" "$IIGBIN" \
    --def "$SRC/$NAME.iig" \
    --header "$GEN/$BUNDLE_ID/$CLASS_NAME.h" \
    --impl "$GEN/$NAME.iig.cpp" \
    --framework-name "$BUNDLE_ID" \
    --deployment-target 24.2 \
    -- \
    -D__IIG=1 \
    -isysroot "$DKX" \
    -I"$SYSINC" \
    -F"$FWROOT" \
    -x c++ -std=c++17

# ---------- 2. Compile ----------
echo "[2/5] Compiling"
OBJS=()
for f in "$SRC/$NAME.cpp" "$SRC/${NAME}_datapath.cpp" "$GEN/$NAME.iig.cpp"; do
    base="$(basename "$f" .cpp)"
    printf '      %s.cpp\n' "$base"
    "$CLANG" "${CFLAGS[@]}" -c "$f" -o "$OBJ/$base.o"
    OBJS+=("$OBJ/$base.o")
done

# ---------- 3. Link ----------
# No -nostdlib: the SDK ships libSystem.tbd/libc++.tbd and clang picks up
# libc++.dylib; framework symbols stay undefined and are resolved by the
# DriverKit loader in-process.
echo "[3/5] Linking"
"$CLANG" \
    -target "$TARGET" \
    -isysroot "$DKX" \
    -F"$FWROOT" \
    -framework DriverKit \
    -framework NetworkingDriverKit \
    -framework PCIDriverKit \
    -o "$DEXT/Contents/MacOS/$NAME" \
    "${OBJS[@]}"

# ---------- 4. Assemble bundle ----------
echo "[4/5] Assembling bundle"
cp "$SRC/Info.plist" "$DEXT/Contents/Info.plist"
"$PLUTIL" -lint "$DEXT/Contents/Info.plist" >/dev/null
printf '%s' "APPL????" > "$DEXT/Contents/PkgInfo"

"$STRIP" -x "$DEXT/Contents/MacOS/$NAME" 2>/dev/null || true

# __osclassinfo must exist, or the loader cannot discover the driver class
if ! /Library/Developer/CommandLineTools/usr/bin/otool \
        -s __DATA_CONST __osclassinfo "$DEXT/Contents/MacOS/$NAME" \
        | grep -q "Contents of"; then
    die "No __osclassinfo section in binary; class registration is missing"
fi

# ---------- 5. Sign ----------
# Ad-hoc signing is enough: loading relies on dk=0x8001 to skip entitlement
# verification, not on a certificate chain.
echo "[5/5] Signing"
codesign --force --sign - \
    --entitlements "$SRC/$NAME.entitlements" \
    --timestamp=none \
    "$DEXT"

echo
echo "================ Build complete ================"
echo "Artifact : $DEXT"
echo "Arch     : $(file -b "$DEXT/Contents/MacOS/$NAME" | sed 's/.*: //')"
echo "Size     : $(du -sh "$DEXT" | cut -f1)"
echo
codesign -dvvv "$DEXT" 2>&1 | sed -n '2,6p' | sed 's/^/  /'
echo
echo "Next step: sudo ./scripts/install.sh"
