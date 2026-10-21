# YT6801 以太网驱动（macOS）

[English](README.md) | **简体中文** | [繁體中文](README.zh-TW.md)

面向 **Motorcomm YT6801 PCIe 千兆以太网控制器**（PCI `1f0a:6801`）的 macOS 用户态 DriverKit（DEXT）网络驱动。

本项目移植自厂商为同一芯片发布的 Linux 驱动（`fuxi-gmac` 驱动栈，源码收录于 [GitHub 镜像仓库](https://github.com/bartvdbraak/yt6801)，原始出处为 [Motorcomm 官网下载页](https://www.motor-comm.com/product/ethernet-control-chip)）。依照上游许可要求，本仓库以 **GPL-2.0 或更新版本** 协议开源。

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

本机 7×24 小时日常使用，有线链路、DHCP、拔线回落、睡眠唤醒均已实测稳定。**其他机型未测试**——理论上任何带 YT6801（`1f0a:6801`）PCIe 设备的机器都能用，但实际表现取决于平台（PCIe 布线、BIOS、VTD 配置）。

遇到问题请[提交 issue](../../issues)；有建议或指正，欢迎提 PR。

## 安装

### 黑苹果

1. 从 [Releases](../../releases) 下载 `YT6801-Installer-1.0.dmg` 并打开
2. 启动 **YT6801 Installer**，选语言、同意协议、点**安装**
3. 插上网线即可用，无需重启

大多数黑苹果环境（SIP 关闭 / AMFI 放宽）会静默激活驱动。如果激活被拦截，检查 SIP 是否关闭、boot-args 里是否有 `amfi_get_out_of_my_way=0x1`。

### 白苹果（免费方案）

本驱动是 ad-hoc 签名——正版 Mac 要求 Developer ID 签名 + 公证（$99/年），所以在白苹果上免费运行的办法是先放宽安全策略：

1. 进恢复模式，开终端执行 `csrutil disable`
2. boot-args 加上 `amfi_get_out_of_my_way=0x1 amfi=0x80` 后重启
3. 然后按上面流程安装

完全原版（SIP 开启）的白苹果无法加载未签名 dext，这一点没有免费绕法——需要付费开发者账号，而且 DriverKit 网络 entitlement 还要向苹果专项申请。

命令行方式：

```
"YT6801 Installer 1.0.app/Contents/MacOS/YT6801Installer" --install    # 或 --status / --uninstall
```

更多细节见 [docs/zh-CN/build-and-install.md](docs/zh-CN/build-and-install.md)，其他文档：[English](docs/en/build-and-install.md) · [繁體中文](docs/zh-TW/build-and-install.md)。

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
