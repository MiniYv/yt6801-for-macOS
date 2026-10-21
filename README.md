# YT6801 Ethernet Driver for macOS

**English** | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

A user-space DriverKit (DEXT) network driver for the **Motorcomm YT6801 PCIe Gigabit Ethernet** controller (PCI `1f0a:6801`) on macOS.

This driver is a port of the vendor's Linux driver ([motorcomm yt6801 / fuxi-gmac](https://github.com/motorcomc/yt6801-linux-driver)), re-implemented on Apple's DriverKit framework. It inherits the **GNU GPL-2.0-or-later** license from the upstream project.

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

## Install

Download `YT6801-安装器-1.0.dmg` from the [Releases](../../releases) page, open it, drag the installer to Applications if you like, launch it and click **Install**. The app activates the system extension and brings up the interface — no restart needed.

CLI mode:

```
"/Applications/YT6801 安装器.app/Contents/MacOS/YT6801Installer" --install    # or --status / --uninstall
```

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
