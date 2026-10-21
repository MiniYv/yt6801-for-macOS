# DEXT 与 kext：两条路的真实成本

这份文档只回答一个问题：**在你这台机器上，哪条路能以最小代价让 YT6801 真正工作。**

先纠正一个我之前说错的结论。

---

## 一、DEXT 本身不需要重启

这是事实，有官方出处：

**WWDC19 Session 702《System Extensions and DriverKit》**

> No installer or package necessary — Extensions stay in your app bundle.
> Use the new SystemExtensions framework — `activationRequest` API to make an extension available.
> **Extension lifecycle is managed by the system — Starts whenever needed, e.g. Driver Extensions start when a matching device is connected.**

**Apple 官方示例 `DriverKitUserClientSample`** 的实现说明：

> To build **without entitlements**:
> 1. Temporarily turn off SIP (`csrutil status` 确认)
> 2. `systemextensionsctl developer on`
> 3. Code Signing Identity 设为 **"Sign to Run Locally"**

**RTL812xLucy**（真实第三方 DEXT 网卡驱动，已发布）：

> Signed with Developer ID and notarized, so it installs on a Mac with full security enabled:
> open the app, approve the driver once in System Settings, done.
> **No recoveryOS, no security changes.**

结论：**正常的 DEXT 加载全程不重启**。App 发一次 `OSSystemExtensionRequest`，用户在系统设置里点一次允许，驱动就开始匹配设备。

我之前把「我这个没签名的 DEXT 需要 `dk=0x8001`」说成「DEXT 需要重启」，是错的。`dk=0x8001` 是 **boot-arg**，只有它才需要重启。

---

## 二、那为什么我这个 DEXT 绕不开重启

因为 `dk=0x8001` 的作用是**关闭 DriverKit 的 entitlement 校验**（Apple DTS 在开发者论坛确认：

> Run the command below can disable DriverKit-specific entitlement checks:
> `sudo nvram boot-args="dk=0x8001"`

boot-arg 只在**内核启动时读一次**，没有任何用户态手段能在运行时改。所以：

| 条件 | 是否需要重启 |
|---|---|
| 有 Apple 开发者账号 + Developer ID 签名 + 公证 | **不需要** |
| 无账号，走 SIP 关闭 + `developer on` + adhoc 签名 | 需要（关 SIP 要进恢复模式） |
| 无账号，走 `dk=0x8001` | 需要（改 boot-arg） |

我选的是第三条，所以需要重启。这不是 DEXT 的属性，是我的签名条件的属性。

---

## 三、kext 那条路：不是"不用重启"，是"重启方式不同"

先纠正另一半：**kext 也需要重启**。

macOS 11 起，`/Library/Extensions` 里的第三方 kext 必须先被 `kmutil configure` 收进 **AuxKC（Auxiliary Kernel Collection）**，而 **AuxKC 只在启动时构建**。所以 `kextload` 一个不在 KC 里的 kext，会被拒绝。

你的机器上 Lilu、VirtualSMC、AMFIPass 那一排，`kmutil showloaded` 里能看到它们的依赖链 —— 那些**不是运行时 kextload 进去的**，是 OpenCore 在 boot 阶段直接注入 kernel cache 的。

**这就是关键差别：**

| 方式 | 改什么 | 风险 | 回滚 |
|---|---|---|---|
| DEXT + `nvram boot-args` | 主板 nvram | 写错可能开不了机 | 要再进系统改回来 |
| DEXT + OC 注入 boot-args | EFI 里的 config.plist | 文件坏了有备份 | 换回备份文件 |
| kext 放 `/Library/Extensions` + AuxKC | 系统卷 + KC | 要改 SIP、可能被拒 | 删文件 + 重建 KC |
| **kext + OpenCore `Kernel > Add`** | **EFI 里的 config.plist + Kexts 目录** | **文件级，有备份** | **换回备份 + 删 kext** |

最后一行才是这台机器该走的路。理由：

1. **不碰主板 nvram** —— 只改 EFI 分区里的两个文件
2. **不需要 `csrutil disable`** —— OC 注入不经过 AuxKC 与 SIP 检查
3. **不需要 `dk=0x8001`** —— 那是 DEXT 的东西
4. **跟现有生态一致** —— Lilu / VirtualSMC / AMFIPass 全走这条路
5. **回滚是文件操作** —— 一条命令，不用进恢复模式

**代价仍然是重启一次。** 这一点我不再含糊：任何能让驱动碰到硬件的路，都要一次重启。

---

## 四、当前产物

### kext（推荐，已完成）

```
build/YT6801Ethernet.kext/                85 KB
└── Contents/MacOS/yt6801                 Mach-O 64-bit kext bundle x86_64
    Signature=adhoc
    Identifier=com.example.driver.yt6801
```

依赖（`kextlibs` 实测）：

```
com.apple.iokit.IONetworkingFamily = 3.4
com.apple.iokit.IOPCIFamily        = 2.9
com.apple.kpi.bsd                  = 25.6
com.apple.kpi.iokit                = 25.6
com.apple.kpi.libkern              = 25.6
```

源码：

| 文件 | 行数 | 内容 |
|---|---|---|
| `source/kext/yt6801_hw.h` | 210 | 硬件层接口、描述符结构、寄存器读写内联 |
| `source/kext/yt6801_hw.cpp` | 700 | MDIO、MAC、DMA 环、PHY 回环、TX/RX、中断应答 |
| `source/kext/yt6801_eth.h` | 85 | `IOEthernetController` 子类声明 |
| `source/kext/yt6801_eth.cpp` | 600 | 网络栈接线、中断、链路轮询、过滤器 |

寄存器定义直接复用 `source/yt6801/yt6801_reg.h`（纯宏，409 行），**单一真相源，不复制**。

### DEXT（备选，已完成）

```
dist/yt6801.dext/                         122 KB
```

需要 `dk=0x8001`，见下面第五节。

---

## 五、怎么装（两种，选一种）

### A. kext + OpenCore（推荐）

```bash
# 1. 构建
bash scripts/build_kext.sh

# 2. 看会改什么（不落盘）
sudo bash scripts/oc_inject.sh --dry-run

# 3. 注入（自动备份 config.plist）
sudo bash scripts/oc_inject.sh

# 4. 重启
# 5. 验证
kmutil showloaded --list-only | grep -i yt6801
ifconfig -l
log show --last 2m --predicate 'eventMessage CONTAINS "yt6801"' --info
```

回滚：

```bash
sudo bash scripts/oc_inject.sh --revert
```

脚本做的事：
- 找到含 `OC/config.plist` 的 EFI 分区（可能有多块盘，逐个试）
- 备份成 `config.plist.bak.<时间戳>`
- 在 `Kernel > Add` 追加一项（**幂等**：重复跑不会重复添加）
- `plutil -lint` 校验，失败自动还原
- 复制 kext 到 `OC/Kexts/`

**它不动 nvram，不动 SIP，不动任何其它 OC 配置。**

### B. DEXT + OC 注入 boot-args（备选）

如果你更想用 DEXT，也不要写 nvram。在 `config.plist` 里加：

```
NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 > boot-args
  在现有字符串末尾追加 " dk=0x8001"
```

然后重启，跑 `sudo bash scripts/install.sh`。

这样 `dk=0x8001` 只存在于 EFI 文件里，**不写进主板 nvram**，改回来就是删掉那几个字符。

---

## 六、装了之后能看到什么

启动后日志里应该有这些（`log show --predicate 'eventMessage CONTAINS "yt6801"'`）：

```
yt6801: BAR0 映射成功：16384 字节 @ 0x...
yt6801: MAC = xx:xx:xx:xx:xx:xx (EFUSE)
yt6801: PHY ID = 0x0000:0x8614（期望 0x0000:0x8614）
yt6801: BMCR 原值 = 0x1140
yt6801: 回环位已生效：BMCR = 0x8140
yt6801: PHY 回环自检：PASS（结果位图 0x0）
yt6801: 环已建立：TX 128 条 (2048 B) @ phys 0x...
yt6801: 硬件初始化完成（环 128/128，RBSZ 2048，RIWT 0x10）
yt6801: 启动完成：... 环 128/128 PHY 0000:8614
```

然后 `ifconfig -l` 里多一个 `enX`，`ifconfig enX up`，插网线 ping。

**注意：不插网线时链路是 DOWN**，这是正常的 —— PHY 回环自检验证的是芯片内部通路，不是外部链路的替代品。回环 PASS 说明"寄存器值对、MDIO 时序对、PHY 认这些值"，这是自检能做到的全部。

---

## 七、仍然没做的

不隐瞒：

- **多段 / TSO 帧**：`getMaxPacketSize` 返回 1518，jumbo 与 TSO 都没写
- **多播哈希表**（`MAC_HTR0..7`）：`setMulticastList` 降级成全多播，日志会明确说明
- **全部 offload**：校验和 / VLAN / TSO 都没做，`MAC_CR.IPC` 未开
- **WoL**：`getPacketFilters` 返回 0，不谎报
- **中断合并调优**：`RIWT=0x10` 是 Linux 默认值，高吞吐下可能需要再调

这些不影响"能 ping 通"，影响的是极限吞吐与 CPU 占用。
