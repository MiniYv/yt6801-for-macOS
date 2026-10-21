#!/bin/bash
#
# uninstall.sh - Uninstall the YT6801 driver (requires root: sudo ./scripts/uninstall.sh)
#
# Note the teamID '-': the driver is ad-hoc signed with an empty Team ID and
# systemextensionsctl rejects empty arguments, so '-' must be passed.
#
set -euo pipefail

BUNDLE_ID="io.miniyv.yt6801ethernet"
HOSTAPP="/Applications/YT6801Driver.app"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { red "Root required: sudo $0"; exit 1; }

echo "==> Deactivating System Extension"
systemextensionsctl uninstall - "$BUNDLE_ID" || true

echo "==> Removing host app"
rm -rf "$HOSTAPP"

echo "==> Collecting orphaned extensions"
systemextensionsctl gc || true

echo
green "Uninstalled"
echo
echo "To also remove dk=0x8001 from boot-args:"
echo "    sudo nvram boot-args=\"\$(nvram boot-args | sed 's/boot-args[[:space:]]*//; s/ dk=0x8001//')\""
