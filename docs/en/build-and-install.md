# Build & Install

## Prerequisites

- macOS 15.2+ (build tested on macOS 26), Intel x86_64
- Xcode with the DriverKit 24.2 SDK or newer (`xcrun -sdk driverkit24.2 --show-sdk-path`)
- No Apple Developer account required: the dext is ad-hoc signed

## Build the driver

```
./scripts/build.sh
```

Produces `dist/yt6801.dext` (compile → link → assemble → sign). All toolchain
paths (`DKX`, `IIGBIN`, `CLANG`, `STRIP`, `CLT_LIB`) can be overridden via
environment variables; CI does exactly that.

Host-side offline verification (ring simulator, PHY loopback sequence):

```
./scripts/test.sh
```

## Install with the GUI installer

Build the installer DMG:

```
cd installer && ./scripts/build.sh    # dist/YT6801-Installer-<version>.dmg
```

Open the DMG and launch **YT6801 Installer**:

1. Pick a language
2. Accept the license agreement
3. Click **Install** (or **Uninstall** — the same dialog offers both when a
   driver is already present)

The installer handles the host app in /Applications, system-extension
activation and privilege escalation for removal. No restart needed. The host
app `/Applications/YT6801Driver.app` is a background agent with no Dock icon;
do not delete it — upgrades and removals go through it.

## CLI mode

Installer app:

```
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --uninstall
```

Host app (also usable standalone):

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
```

## Upgrading the driver

sysextd replaces an installed dext only when `CFBundleVersion` in the dext
`Info.plist` strictly increases — same-version installs are silently skipped.
Bump the version, rebuild, reinstall; the running instance is replaced without
a reboot.

## Uninstall

Use the installer's **Uninstall** button (admin password required). It waits
until `systemextensionsctl` reports the extension is gone before touching any
files. The symlink-free manual way is `--deactivate` plus removing
`/Applications/YT6801Driver.app`.

## CI

`.github/workflows/release.yml` builds the dext and installer on a GitHub
macOS runner and attaches the DMG/ZIP to a GitHub Release for every `v*` tag.

## Verify a running driver

```
systemextensionsctl list | grep yt6801     # activated enabled
ifconfig                                   # enX with the EFUSE MAC address
/usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS "yt6801"'
./scripts/nic_test.sh                      # full health/stability suite
```
