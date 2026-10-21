# YT6801 Ethernet Driver for macOS

**English** | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

A user-space DriverKit (DEXT) network driver for the **Motorcomm YT6801 PCIe Gigabit Ethernet** controller (PCI `1f0a:6801`) on macOS.

**Developer: MiniYv (小禹)**

This driver is ported from the vendor's Linux driver for the same chip — the `fuxi-gmac` driver stack (source files mirrored on [GitHub](https://github.com/bartvdbraak/yt6801), originally from [Motorcomm's download page](https://www.motor-comm.com/product/ethernet-control-chip)). It inherits the **GNU GPL-2.0-or-later** license from the upstream project, as required.

## Features

- User-space DriverKit driver — no kernel extensions, works on macOS 15.2+
- DMA descriptor rings backed by self-managed `IODMACommand` buffers
- Link detection via periodic PHY polling (200 ms): cable pull/unplug is reported to macOS immediately, and the system can fall back to Wi-Fi
- MAC speed auto-sync after renegotiation (10/100/1000 Mbit)
- MAC address read from EFUSE (with register and static fallbacks)
- Hardware self-loopback self-test
- Media type and vendor reporting (shows up as "Motorcomm YT6801 PCIe Gigabit Ethernet Controller")
- GUI installer (DMG) with silent activation and CLI mode

## Requirements

- macOS 15.2 or later (built and tested on macOS 26), Intel (x86_64)
- Motorcomm YT6801-based Ethernet controller (`1f0a:6801`)

## Tested hardware

- **Mechrevo Jiaolong 16 Pro (Youwan Qingchun edition)** — Ryzen 9 8940HX + RTX 5060, Hackintosh
- Tested OS: **macOS 26.7.1 (Tahoe)**

This machine runs the driver 24/7 with wired link, DHCP, unplug/fallback and sleep/wake all verified stable. **Other hardware and OS versions are untested** — the driver should work on any machine exposing a YT6801 (`1f0a:6801`) PCIe function, but results depend on your platform (PCIe wiring, BIOS, VTD setup).

Found a problem? Please [open an issue](../../issues). Suggestions and corrections are welcome as pull requests.

## Install

### Hackintosh

1. Download `YT6801-Installer-x.x.dmg` from the [Releases](../../releases) page and open it
2. Launch **YT6801 Installer**, pick a language, accept the license, click **Install**
3. Plug in the cable — no restart needed

Most hackintosh setups (SIP disabled / AMFI relaxed) activate the driver silently. If activation is blocked, check that SIP is off and that `amfi_get_out_of_my_way=0x1` is in boot-args.

### Real Macs (free path)

The driver is ad-hoc signed — Apple charges $99/year for the Developer ID + notarization that real Macs require, so the free way to run it on a real Mac is to relax the security policy first:

1. Boot into Recovery, open Terminal: `csrutil disable`
2. Add `amfi_get_out_of_my_way=0x1 amfi=0x80` to boot-args and reboot
3. Then install as above

On a fully stock, SIP-enabled Mac the unsigned dext will not load — there is no free workaround for that; it requires a paid Apple Developer account, and the DriverKit networking entitlement additionally needs Apple's special approval.

CLI mode — run the installer binary directly (from the mounted DMG, or after copying it to /Applications):

```
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --uninstall
```

Once the driver is installed, the host app in /Applications can activate or deactivate the extension without the installer:

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
```

More details: [docs/en/build-and-install.md](docs/en/build-and-install.md) · Documentation also available in [简体中文](docs/zh-CN/build-and-install.md) and [繁體中文](docs/zh-TW/build-and-install.md).

## Build from source

Requirements: macOS with Xcode 26 (DriverKit 26 SDK), plus a vendored copy of the DriverKit SDK.

```
./scripts/build.sh        # compile + sign the dext into dist/
./scripts/install.sh      # build host app and activate
./scripts/test.sh         # host-side verification suite
./scripts/nic_test.sh     # full NIC health/stability test against the live driver
```

All toolchain paths in `scripts/build.sh` (`DKX`, `IIGBIN`, `CLANG`, `STRIP`, `CLT_LIB`) can be overridden via environment variables — CI does exactly that.

## Scripts

| Script | Purpose |
|---|---|
| `build.sh` | Compile, sign and package the dext |
| `install.sh` / `uninstall.sh` | Install / remove the driver and host app |
| `update.sh` / `hotupdate.sh` / `redeploy.sh` | Version-bump upgrade helpers |
| `test.sh` | Host-side logic verification (ring simulator) |
| `loopback_test.sh` | PHY self-loopback test against the live driver |
| `watch_activate.sh` | Watch system-extension activation state |
| `nic_test.sh` | Link, latency, error counters, throughput and soak test |
| `installer/scripts/build.sh` | Build the GUI installer DMG/ZIP |

## License

This project is licensed under the **GNU General Public License v2.0 or later** — see [LICENSE](LICENSE).

It is derived from Motorcomm's Linux driver for the same chip:

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv (macOS DriverKit port)
```

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
