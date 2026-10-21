# YT6801 以太网驱动（macOS）

[English](README.md) | **简体中文** | [繁體中文](README.zh-TW.md)

面向 **Motorcomm YT6801 PCIe 千兆以太网控制器**（PCI `1f0a:6801`）的 macOS 用户态 DriverKit（DEXT）网络驱动。

本项目移植自厂商的 Linux 驱动（[motorcomm yt6801 / fuxi-gmac](https://github.com/motorcomc/yt6801-linux-driver)），基于 Apple DriverKit 框架重新实现。依照上游项目要求，本仓库以 **GPL-2.0 或更新版本** 协议开源。

## 功能

- 用户态 DriverKit 驱动——不依赖内核扩展（kext），支持 macOS 15.2+
- DMA 描述符环 + 自建 `IODMACommand` 缓冲
- 定时轮询链路（200ms）：拔线立刻上报系统，可自动回落到 Wi-Fi
- 重新协商后自动同步 MAC 速率（10/100/1000M）
- 从 EFUSE 读取出厂 MAC 地址（寄存器 / 静态地址兜底）
- 硬件自环自检
- 上报媒体类型与厂商信息（系统识别为 "Motorcomm YT6801 PCIe Gigabit Ethernet Controller"）
- GUI 安装器（DMG），支持静默激活与命令行模式

## 环境要求

- macOS 15.2 或更高版本（开发与测试环境为 macOS 26），Intel（x86_64）
- 基于 Motorcomm YT6801 的以太网控制器（`1f0a:6801`）

## 测试机型

- **机械革命 蛟龙16Pro 潮玩青春版**——Ryzen 9 8940HX + RTX 5060，黑苹果

## 安装

从 [Releases](../../releases) 下载 `YT6801-安装器-1.0.dmg`，打开后运行安装器，点击**安装**即可——系统扩展自动激活，无需重启。

命令行方式：

```
"/Applications/YT6801 安装器.app/Contents/MacOS/YT6801Installer" --install    # 或 --status / --uninstall
```

## 从源码构建

依赖：装有 Xcode 26（含 DriverKit 26 SDK）的 macOS，以及一份 DriverKit SDK 副本。

```
./scripts/build.sh        # 编译 + 签名，产物在 dist/
./scripts/install.sh      # 构建宿主 app 并激活
./scripts/test.sh         # 宿主侧逻辑验证
./scripts/nic_test.sh     # 对运行中的驱动做全量网卡健康测试
```

`scripts/build.sh` 中所有工具链路径（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）均可用环境变量覆盖——CI 正是这么做的。

## 脚本一览

| 脚本                                           | 用途                 |
| -------------------------------------------- | ------------------ |
| `build.sh`                                   | 编译、签名、打包 dext      |
| `install.sh` / `uninstall.sh`                | 安装 / 卸载驱动与宿主 app   |
| `update.sh` / `hotupdate.sh` / `redeploy.sh` | 抬版本号升级的辅助脚本        |
| `test.sh`                                    | 宿主侧逻辑验证（描述符环模拟器）   |
| `loopback_test.sh`                           | 对运行中的驱动做 PHY 自环测试  |
| `watch_activate.sh`                          | 监视系统扩展激活状态         |
| `nic_test.sh`                                | 链路、延迟、错误计数、吞吐与压力测试 |
| `installer/scripts/build.sh`                 | 构建 GUI 安装器 DMG/ZIP |

## 许可证

本项目以 **GNU General Public License v2.0 或更新版本** 发布——见 [LICENSE](LICENSE)。

本项目衍生自 Motorcomm 面向同一芯片的 Linux 驱动：

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv（macOS DriverKit 移植）
```

本程序按"现状"分发，不含任何担保；包括对适销性和特定用途适用性的默示担保。详见 GNU 通用公共许可证。
