# YT6801 Ethernet Driver for macOS

**English** | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md)

A user-space DriverKit (DEXT) network driver for the **Motorcomm YT6801 PCIe Gigabit Ethernet** controller (PCI `1f0a:6801`) on macOS. Built for Hackintosh (黑苹果 / 黑蘋果) laptops that shipped this chip with no driver at all.

**Developer: MiniYv (小禹)**

## About this project

**The Motorcomm YT6801 is the first consumer PCIe gigabit NIC chip from mainland China to reach mass production. This is the world's first open-source macOS driver for it. With it, a domestic PCIe wired NIC runs on macOS for the first time.**

The chip packs MAC, PHY and the PCIe controller into one 4×4 mm, 32-pin package on a single 3.3 V rail. Vendors use it in place of the Intel/Realtek part they used to buy, saving both BOM cost and board area. With the cost down, more laptops ship with one: MECHREVO, COLORFUL and TUXEDO all have current models (see *Hardware known to ship the YT6801*).

The silicon was never the problem. The software was. Intel and Realtek NICs have a shelf full of community kexts and dexts to pick from; machines built on this chip had nothing, and plugging in a cable did not get you a working interface.

The driver was written from scratch in user space, with no kernel extension required.

> **Built by AI, end to end.** The entire driver was generated with WorkBuddy (Vibe Coding), with no hand-written code, and verified on real hardware (see *Tested hardware and current status*). Several bugs surfaced along the way; two mattered most: throughput pinned at ~1% of line rate, and a link deadlock whenever the driver started without an active link. Details are in [CHANGELOG.md](CHANGELOG.md).

This driver is ported from the vendor's Linux driver for the same chip, the `fuxi-gmac` driver stack (source files mirrored on [GitHub](https://github.com/bartvdbraak/yt6801), originally from [Motorcomm's download page](https://www.motor-comm.com/product/ethernet-control-chip)). It inherits the **GNU GPL-2.0-or-later** license from the upstream project, as required.

## Features

- User-space DriverKit driver, no kernel extensions, works on macOS 15.2+
- DMA descriptor rings backed by self-managed `IODMACommand` buffers
- Link detection via periodic PHY polling (200 ms): cable pull/unplug is reported to macOS immediately, and the system can fall back to Wi-Fi
- MAC speed auto-sync after renegotiation (10/100/1000 Mbit)
- Gigabit line-rate throughput, measured: **894 Mbps sustained download** (~95% of the TCP ceiling), zero retransmissions
- MAC address read from EFUSE (with register and static fallbacks)
- Hardware self-loopback self-test
- Media type and vendor reporting (shows up as "Motorcomm YT6801 PCIe Gigabit Ethernet Controller")
- GUI installer (DMG) with silent activation and CLI mode

## Requirements

- macOS 15.2 or later (built and tested on macOS 26), Intel (x86_64)
- Motorcomm YT6801-based Ethernet controller (`1f0a:6801`)

## Tested hardware and current status

- **Mechrevo Jiaolong 16 Pro (Youwan Qingchun edition)**: Ryzen 9 8940HX + RTX 5060, Hackintosh
- **Mechrevo Yaoshi 16 Pro (Jiguang X Chaowan edition)**: i7-14650HX + RTX 4070, Hackintosh — tested by a second user on the exact same OS build and driver version; the activation failure hit on this machine is what v1.0.13 fixes
- Tested OS: **macOS 26.7.1 (Tahoe)**

**Status: usable, not yet proven.** This machine runs the driver daily over a wired link; DHCP, unplug/fallback and sleep/wake have all been exercised on it. That is not a stability guarantee. The first v1.0 carried more defects than anyone caught at the time and **was not actually stable**. Real use is what surfaced them: throughput pinned at ~1% of line rate first, then a link deadlock after a start without an active link. Today's build is considerably more usable; long-run behaviour is still the least tested part. A working driver, not a finished one.

**Other hardware and OS versions are untested.** The driver should work on any machine exposing a YT6801 (`1f0a:6801`) PCIe function, but results depend on your platform (PCIe wiring, BIOS, VTD setup). Some machines remain untested: if activation still fails even with SIP off and AMFI relaxed, add `dk=0x8001` to boot-args in `config.plist` as a fallback.

### Hardware known to ship the YT6801

| Brand | Model | Evidence |
|---|---|---|
| COLORFUL 七彩虹 | 将星 X16 Pro (i7-14650HX + RTX 5060) | Teardown report identifies 裕太微 YT6801 as the LAN controller |
| MECHREVO 机械革命 | 蛟龙16Pro: R9-9955HX and 潮玩青春版 R9-8940HX | Reported via `lspci`; the 8940HX unit is the machine this driver was written on |
| MECHREVO 机械革命 | 极光X (GM6PR0C), 耀世15Pro | Owner reports; the vendor INF carries subsystem `139A:1D05` for 极光X |
| MECHREVO 机械革命 | 耀世16Pro 极光X 潮玩版 (i7-14650HX + RTX 4070) | Owner-confirmed working with this driver on macOS 26.7.1 (see *Tested hardware*); v1.0.13 fixes the activation issue found on it |
| MECHREVO 机械革命 | 无界14X / WUJIE14X(A) | ArchWiki hardware table lists Ethernet `1f0a:6801` as working |
| TUXEDO | InfinityBook Pro 14 Gen9 (AMD), InfinityBook Pro 15 Gen10 | TUXEDO's own FAQ names the Motorcomm YT6801 and ships `tuxedo-yt6801` |
| Acer | unnamed models | Vendor INF: subsystem vendor `1025` (`1739`, `173C`, `1947`, `194F`) |
| Samsung | unnamed models | Vendor INF: subsystem `C1D9:144D` |
| Ninkear | N10 Pro mini PC (Intel N150) | `lspci` report on Umbrel OS |
| TongFang ODM boards | subsystem vendor `1D05` (`137D`, `137F`, `1380`, `1381`, `1382`, `1383`, `1384`, `1385`, `1388`, …) | Vendor INF annotates these as `tongfang`; MECHREVO and TUXEDO both build on TongFang barebones |

The same model name does not guarantee the same NIC: batches and sub-SKUs differ (other COLORFUL laptops ship Realtek 2.5GbE). Identify by PCI ID, not by brand.

**Check your own machine:**

- macOS: `ioreg -l -p IODeviceTree | grep -i 1f0a` prints `"pci1f0a,6801"` when the controller is present; the neighbouring `pci1d05,7011` is the subsystem (ODM) ID.
- Linux: `lspci -nn | grep 1f0a`
- Windows: Device Manager → the adapter's hardware ID contains `PCI\VEN_1F0A&DEV_6801`

If your model is missing from the table but the ID matches, it should work; please report the result in an issue.

Found a problem? Please [open an issue](../../issues). Suggestions and corrections are welcome as pull requests.

## FAQ

**Does macOS support the YT6801 without a driver?**
No. Apple ships nothing for `1f0a:6801`. Without this project the interface never shows up in Network settings and the cable does nothing, which is why these laptops are usually described as having no working Ethernet under macOS.

**Is this a kext?**
No. It is a DriverKit system extension (dext) running in user space. Nothing goes into the kernel, and `EFI/OC/Kexts` is not touched. You install an app and activate the extension from it, the same way a vendor driver works.

**Which macOS versions work?**
15.2 and later, including macOS 26 Tahoe. The DriverKit networking API this driver uses is not available before 15.2.

**Does it work on Apple Silicon?**
No. The dext is built for x86_64 only, and no Apple Silicon Mac ships a YT6801. It targets Intel and AMD hackintoshes.

**Do I have to disable SIP?**
On most hackintoshes SIP is already off or AMFI is relaxed, and activation just works. If activation is blocked, turning SIP off and adding `amfi_get_out_of_my_way=0x1` to boot-args is the usual fix.

**The interface shows up but keeps a 169.254 address.**
That is a DHCP failure, not a driver failure. Check the cable and the switch port first. If the link light is on and the address still does not arrive, collect `sudo log show --last 5m --predicate 'eventMessage CONTAINS "yt6801:"'` and open an issue.

**Does Ethernet replace Wi-Fi?**
No, both stay active. macOS follows the service order in Network settings, and unplugging the cable falls back to Wi-Fi on its own.

**How fast is it in practice?**
894 Mbps sustained TCP download on a gigabit link, about 95% of what the link can carry. Details in [CHANGELOG.md](CHANGELOG.md).

**Is the driver signed and notarized?**
It is ad-hoc signed. A Mac with SIP enabled will refuse to load it; that needs a Developer ID signature plus notarization, and the DriverKit networking entitlement needs Apple's approval.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) (English / 简体中文 / 繁體中文). Current release **v1.0.8** fixes the link deadlock that left the interface stuck on a `169.254` address when the driver started without an active link; v1.0.7 fixed a throughput bug present since v1.0 that capped transfers at ~1% of gigabit line rate, and that fix is in here too. With both in, the driver sustains **894 Mbps download** (~95% of the TCP ceiling) on a gigabit link.

## Install

### Hackintosh (OpenCore)

1. Download `YT6801-Installer-x.x.dmg` from the [Releases](../../releases) page and open it
2. Launch **YT6801 Installer**, pick a language, accept the license, click **Install**
3. Plug in the cable. No restart needed.

Most hackintosh setups (SIP disabled / AMFI relaxed) activate the driver silently. If activation is blocked, check that SIP is off and that `amfi_get_out_of_my_way=0x1` is in boot-args.

### Real Macs (free path)

The driver is ad-hoc signed. Real Macs require a Developer ID signature plus notarization, and Apple charges $99/year for that; the free way to run it on one is to relax the security policy first:

1. Boot into Recovery, open Terminal: `csrutil disable`
2. Add `amfi_get_out_of_my_way=0x1 amfi=0x80` to boot-args and reboot
3. Then install as above

On a fully stock, SIP-enabled Mac the unsigned dext will not load, and there is no free workaround: it needs a paid Apple Developer account, and the DriverKit networking entitlement needs Apple's special approval on top.

There is also a CLI: run the installer binary directly (from the mounted DMG, or after copying it to /Applications):

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

Requirements: macOS with Xcode (DriverKit 24.2 SDK or newer), plus a vendored copy of the DriverKit SDK.

```
./scripts/build.sh        # compile + sign the dext into dist/
./scripts/install.sh      # build host app and activate
./scripts/test.sh         # host-side verification suite
./scripts/nic_test.sh     # full NIC health/stability test against the live driver
```

All toolchain paths in `scripts/build.sh` (`DKX`, `IIGBIN`, `CLANG`, `STRIP`, `CLT_LIB`) can be overridden via environment variables; CI does exactly that.

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

This project is licensed under the **GNU General Public License v2.0 or later**. See [LICENSE](LICENSE).

It is derived from Motorcomm's Linux driver for the same chip:

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv (macOS DriverKit port)
```

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
