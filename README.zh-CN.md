# YT6801 以太网驱动（macOS）

[English](README.md) | **简体中文** | [繁體中文](README.zh-TW.md)

面向 **Motorcomm YT6801 PCIe 千兆以太网控制器**（PCI `1f0a:6801`）的 macOS 用户态 DriverKit（DEXT）网络驱动。

**开发者：MiniYv（小禹）**

## 关于本项目

**Motorcomm YT6801，中国大陆第一款量产商用的消费级 PCIe 千兆网卡芯片。这是全世界第一个开源的 YT6801 macOS 驱动。有了它，主板上的国产PCIe网卡第一次在 macOS 上跑起来了。**

芯片把 MAC、PHY 和 PCIe 控制器做进一颗 4×4 mm、32 引脚的封装，单 3.3V 供电。整机厂用它替掉原先板载的 Intel、Realtek 方案，BOM 和布板面积一起省下来。成本降了，用它的笔记本也就多了。机械革命、七彩虹、TUXEDO 的现售机型里都能找到（见「已知使用 YT6801 的机型」）。

硬件从来不缺，缺的是软件。Intel、Realtek 的网卡在黑苹果社区有一堆现成的 kext 和 dext 可挑；换上这颗国产芯片的机器一片空白，插上网线，macOS 也不认。

驱动从零写起，跑在用户态，不依赖内核扩展。

> **全程 AI 生成。** 整个驱动由 WorkBuddy（Vibe Coding）生成，没有一行人工手写代码，并在本机实测跑通（见「测试机型与当前状态」）。开发过程中修掉的问题里有两个要紧：吞吐被钉在线速 1%；驱动在无链路状态下启动后，链路会卡死。详见 [CHANGELOG.md](CHANGELOG.md)。

本项目移植自厂商为同一芯片发布的 Linux 驱动（`fuxi-gmac` 驱动栈，源码收录于 [GitHub 镜像仓库](https://github.com/bartvdbraak/yt6801)，原始出处为 [Motorcomm 官网下载页](https://www.motor-comm.com/product/ethernet-control-chip)）。依照上游许可要求，本仓库以 **GPL-2.0 或更新版本** 协议开源。

## 功能

- 用户态 DriverKit 驱动，不依赖内核扩展（kext），支持 macOS 15.2+
- DMA 描述符环 + 自建 `IODMACommand` 缓冲
- 定时轮询链路（200ms）：拔线立刻上报系统，可自动回落到 Wi-Fi
- 重新协商后自动同步 MAC 速率（10/100/1000M）
- 千兆线速吞吐实测：**下载持续 894 Mbps**（约为 TCP 上限的 95%），零重传
- 从 EFUSE 读取出厂 MAC 地址（寄存器 / 静态地址兜底）
- 硬件自环自检
- 上报媒体类型与厂商信息（系统识别为 "Motorcomm YT6801 PCIe Gigabit Ethernet Controller"）
- GUI 安装器（DMG），支持静默激活与命令行模式

## 环境要求

- macOS 15.2 或更高版本（开发与测试环境为 macOS 26），Intel（x86_64）
- 基于 Motorcomm YT6801 的以太网控制器（`1f0a:6801`）

## 测试机型与当前状态

- **机械革命 蛟龙16Pro 潮玩青春版**：Ryzen 9 8940HX + RTX 5060，黑苹果
- **机械革命 耀世16Pro**：i7-14650HX + RTX 4070，黑苹果——用户实测，系统版本与上面相同；测试中遇到了激活问题，v1.0.13 修复了部分问题
- 测试系统：**macOS 26.7.1（Tahoe）**

**状态：可用，但谈不上久经考验。** 本机长期挂着有线链路日常使用，DHCP、拔线回落、睡眠唤醒都在这台机器上跑过。这不构成稳定性承诺。最初的 v1.0 带的缺陷比当时发现的更多，**实际并不稳定**，全靠真实使用一个个露出来：吞吐先被钉在线速约 1%，接着是无链路启动后链路卡死。现在明显好用了，长久运行仍然最缺测试。它是个能用的驱动，还算不上完工。

**其他机型与系统版本未测试。** 理论上任何带 YT6801（`1f0a:6801`）PCIe 设备的机器都能用，实际表现取决于平台（PCIe 布线、BIOS、VTD 配置）。另有部分机型未测试：如果关掉 SIP、放宽 AMFI 后仍不能直接用，可在 config.plist 的 boot-args 里加 `dk=0x8001` 兜底。

### 已知使用 YT6801 的机型

| 品牌 | 机型 | 依据 |
|---|---|---|
| 七彩虹 COLORFUL | 将星 X16 Pro（i7-14650HX + RTX 5060） | 拆解报告：网卡芯片为裕太微 YT6801 |
| 机械革命 MECHREVO | 蛟龙16Pro（R9-9955HX、潮玩青春版 R9-8940HX） | `lspci` 实测；其中 8940HX 这台就是本驱动的开发机 |
| 机械革命 MECHREVO | 极光X（GM6PR0C）、极光X 潮玩版、耀世15Pro | 用户实测；厂商 INF 里极光X 对应子系统 `139A:1D05` |
| 机械革命 MECHREVO | 耀世16Pro（i7-14650HX + RTX 4070） | 用户实测可用；v1.0.13 修复了该机遇到的部分问题 |
| 机械革命 MECHREVO | 无界14X / WUJIE14X(A) | ArchWiki 硬件表记录以太网 `1f0a:6801` 可用 |
| TUXEDO | InfinityBook Pro 14 Gen9（AMD）、InfinityBook Pro 15 Gen10 | TUXEDO 官方 FAQ 明确写有 Motorcomm YT6801，并发布 `tuxedo-yt6801` 驱动包 |
| 宏碁 Acer | 未公开具体型号 | 厂商 INF：子系统厂商 `1025`（`1739`、`173C`、`1947`、`194F`） |
| 三星 Samsung | 未公开具体型号 | 厂商 INF：子系统 `C1D9:144D` |
| Ninkear | N10 Pro 迷你主机（Intel N150） | Umbrel OS 上的 `lspci` 报告 |
| 同方 TongFang 模具 | 子系统厂商 `1D05`（`137D`、`137F`、`1380`、`1381`、`1382`、`1383`、`1384`、`1385`、`1388` 等） | 厂商 INF 标注为 `tongfang`；机械革命与 TUXEDO 都基于同方模具 |

同一个型号名不等于同一颗网卡，批次和子型号会换料（七彩虹另有笔记本用 Realtek 2.5GbE）。判断按 PCI ID，不按品牌。

**查自己这台机器：**

- macOS：`ioreg -l -p IODeviceTree | grep -i 1f0a`，命中会打印 `"pci1f0a,6801"`；旁边的 `pci1d05,7011` 是子系统（ODM）ID。
- Linux：`lspci -nn | grep 1f0a`
- Windows：设备管理器 → 网卡的硬件 ID 里含 `PCI\VEN_1F0A&DEV_6801`

型号不在表里但 ID 对得上，理论上就能用，欢迎到 issue 里报个结果。

遇到问题请[提交 issue](../../issues)；有建议或指正，欢迎提 PR。

## 常见问题

**macOS 自带 YT6801 的驱动吗？**
没有。苹果不给 `1f0a:6801` 提供任何驱动。没有这个项目，网卡在「网络」设置里根本不出现，插上网线也不会有反应，这类机器在 macOS 下通常被直接说成「没有有线网」。

**这是 kext 吗？**
不是。它是 DriverKit 系统扩展（dext），跑在用户态。内核里什么都不注入，`EFI/OC/Kexts` 也不会被动到。装一个 app，从 app 里激活扩展，和厂商驱动的装法一样。

**支持哪些 macOS 版本？**
15.2 及以后的版本，包含 macOS 26 Tahoe。驱动用的 DriverKit 网络接口在 15.2 之前没有。

**支持 Apple Silicon 吗？**
不支持。dext 只编了 x86_64，而且没有哪台 M 系列 Mac 用 YT6801。目标是 Intel、AMD 黑苹果。

**必须关 SIP 吗？**
多数黑苹果本来就关着 SIP 或放宽了 AMFI，激活直接过。如果被系统拦下来，关掉 SIP 并在 boot-args 里加 `amfi_get_out_of_my_way=0x1`，一般就能解决。

**接口出来了，但一直是 169.254 地址。**
这是没拿到 DHCP，不是驱动坏了。先查网线和交换机口。如果链路灯是亮的、地址还是不来，收集 `sudo log show --last 5m --predicate 'eventMessage CONTAINS "yt6801:"'` 后提 issue。

**插网线后 Wi-Fi 会被顶掉吗？**
不会，两个服务并存。macOS 按「网络」设置里的服务顺序选出口，拔掉网线会自动回落到 Wi-Fi。

**实际能跑多快？**
千兆链路下 TCP 下载持续 894 Mbps，约合链路能力的 95%。细节见 [CHANGELOG.md](CHANGELOG.md)。

**驱动签名了吗，要公证吗？**
ad-hoc 签名。SIP 开启的白苹果会拒绝加载。要让它加载需要 Developer ID 签名加公证，DriverKit 网络 entitlement 还得苹果单独批准。

## 更新日志

见 [CHANGELOG.md](CHANGELOG.md)（English / 简体中文 / 繁體中文）。当前版本 **v1.0.8**：修掉了驱动在无链路状态下启动后链路卡死、接口停在 `169.254` 地址的毛病；v1.0.7 那版修的是自 v1.0 起就存在的吞吐缺陷（此前实际传输速度被钉在千兆线速的约 1%），这版也带上了。两处修完，千兆链路实测**下载持续 894 Mbps**（约为 TCP 上限的 95%）。

## 安装

### 黑苹果（OpenCore）

1. 从 [Releases](../../releases) 下载 `YT6801-Installer-x.x.dmg` 并打开
2. 启动 **YT6801 Installer**，选语言、同意协议、点**安装**
3. 插上网线即可用，无需重启

大多数黑苹果环境（SIP 关闭 / AMFI 放宽）会静默激活驱动。如果激活被拦截，检查 SIP 是否关闭、boot-args 里是否有 `amfi_get_out_of_my_way=0x1`。

### 白苹果（免费方案）

本驱动是 ad-hoc 签名。正版 Mac 要 Developer ID 签名加公证（$99/年），免费在白苹果上跑的办法，是先把安全策略放宽：

1. 进恢复模式，开终端执行 `csrutil disable`
2. boot-args 加上 `amfi_get_out_of_my_way=0x1 amfi=0x80` 后重启
3. 然后按上面流程安装

完全原版（SIP 开启）的白苹果无法加载未签名 dext，这一点没有免费绕法：得有付费开发者账号，DriverKit 网络 entitlement 还得单独向苹果申请。

也可以走命令行，直接运行安装器可执行文件（在挂载的 DMG 中，或复制到 /Applications 之后）：

```
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --uninstall
```

驱动安装后，也可以不经过安装器，直接用 /Applications 下的宿主 app 激活或停用扩展：

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
```

更多细节见 [docs/zh-CN/build-and-install.md](docs/zh-CN/build-and-install.md)，其他文档：[English](docs/en/build-and-install.md) · [繁體中文](docs/zh-TW/build-and-install.md)。

## 从源码构建

依赖：装有 Xcode（含 DriverKit 24.2 或更新 SDK）的 macOS，以及一份 DriverKit SDK 副本。

```
./scripts/build.sh        # 编译 + 签名，产物在 dist/
./scripts/install.sh      # 构建宿主 app 并激活
./scripts/test.sh         # 宿主侧逻辑验证
./scripts/nic_test.sh     # 对运行中的驱动做全量网卡健康测试
```

`scripts/build.sh` 中所有工具链路径（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）都能用环境变量覆盖，CI 就是这么做的。

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

本项目以 **GNU General Public License v2.0 或更新版本** 发布，见 [LICENSE](LICENSE)。

本项目衍生自 Motorcomm 面向同一芯片的 Linux 驱动：

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv（macOS DriverKit 移植）
```

本程序按"现状"分发，不含任何担保；包括对适销性和特定用途适用性的默示担保。详见 GNU 通用公共许可证。
