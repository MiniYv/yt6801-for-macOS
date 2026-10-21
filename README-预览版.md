# YT6801 驱动 · 插网线预览版（1.1.1-wired）

这是**独立目录**，和 `~/WorkBuddy/yt6801 mac`（1.0.72 自环版）完全分开，两边互不覆盖。
`research/` 是指向主目录的软链接，只读复用，不占空间。

| 项目 | 值 |
|---|---|
| 目录 | `/Users/macos/WorkBuddy/yt6801-wired` |
| 版本 | `1.1.1`（内部串 `1.1.1-wired`） |
| bundle id | `com.example.driver.yt6801`（**和主版相同**，装上去是替换关系） |
| 变更补丁 | `docs/wired-1.1.1.patch`（489 行，相对 1.0.72） |
| 产物 | `dist/yt6801.dext` |

---

## 一、这一版改了什么

核心只有一件事：**先看有没有插网线，再决定要不要进回环。** 原版顺序是反的，所以插上线也永远走自环。

| # | 文件 | 改动 |
|---|---|---|
| 1 | `yt6801_ivars.h` | 新增 `wantLoopback`、`linkSpeedMbps`、`linkFullDuplex` |
| 2 | `yt6801.cpp` | 新增 `YTLinkState` / `yt_decode_link` / `yt_read_link` / `yt_wait_link`；`Start` 里在 `setupDatapath()` **之前**等自协商（最多 3 秒）并设 `wantLoopback` |
| 3 | `yt6801_datapath.cpp` | 回环自检整块加 `if (iv->wantLoopback)` 闸门；CARDR 游标对齐也一起闸（跳过自检时 CARDR 还是 0，按它反算会得到垃圾索引） |
| 4 | `yt6801_datapath.cpp` | `yt_sync_mac_speed()`：MAC 速率按 PHY 协商结果配，不再写死 1000M（百兆口上原版必不通） |
| 5 | `yt6801.cpp` | `pollLinkStatus` 去掉「回环中直接 return」；回环中一旦探到真链路，退出回环并清掉 `MAC_CR.LM` 与 `PFR.PR/RA` |
| 6 | `yt6801_datapath.cpp` | `TxDataAvailable` 在真实链路下加 ~300 µs 有界收包窗口（中断送不到，靠门限补） |

---

## 二、怎么装

```bash
cd "/Users/macos/WorkBuddy/yt6801-wired"
./scripts/build.sh
./scripts/install.sh
```

装完如果旧实例卡在 `terminating for upgrade`：

```bash
sudo ifconfig en4 down
sleep 5
sudo kill -TERM <旧PID>      # 它本来就在 terminating 状态
sudo ifconfig en4 up
```

全程不重启、不写 nvram、不弹窗。

---

## 三、到机房怎么验

插好网线，**重载一次 dext**（拔掉再装、或者 down/up 后走到 Start），然后看这几行：

```bash
/usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS "yt6801"' | grep "yt6801:"
```

按顺序对照：

| 日志 | 含义 | 判定 |
|---|---|---|
| `PHY 协商结果：1000M 全双工` | 自协商成立 | ✅ 线通了 |
| `启动链路判定：检测到真实链路 → 按真实链路启动，不碰回环` | 走的是真实链路 | ✅ 关键一步 |
| `MAC 速率已设 1000M 全双工 MAC_CR=0x00302000` | MAC 侧配置对 | ✅ |
| `上报链路 UP（media=0x... 真实链路 rc=0x0）` | 已告诉协议栈 | ✅ |
| `ifconfig en4` 出现 `inet 192.168.x.x` | DHCP 成功 | ✅ 能上网 |

失败时的对照：

| 现象 | 大概率原因 |
|---|---|
| 一直是 `无链路（3 秒内未 link up）` | 线/口坏、交换机没开、或端口要 802.1X 认证 |
| `检测到真实链路` 但 DHCP 拿不到 IP | 交换机端口隔离 / VLAN 不对 / 机房里没有 DHCP |
| 有 IP，ping 不通网关 | MAC 速率编码与端口不匹配（可看 `MAC_CR` 的 PS/FES 位） |
| ping 通但传文件断 | 无中断 + 单队列，RX 只能靠 TX 门铃带（见「已知限制」） |

---

## 四、已知限制（这一版没解决）

1. **没有中断**。`ConfigureInterrupts` 返回 0 但 `irqCount` 恒 0，MSI-X 在这台机器上送不到。
   收包靠 TX 门铃 + 有界自旋，纯接收流量（视频流、只下载不上传）会卡。
2. **拔线不会立刻反映**。链路变化靠 `MAC_ISR.PHYIS`，中断不来就跑不到 `pollLinkStatus`，
   接口会一直显示 UP，直到下次重载。
3. **MAC 是本地管理地址** `02:59:54:36:80:01`（EFUSE 没读），可正常用，
   但机房里如果做了 MAC 绑定就得先报备。
4. **无 offload**：`GetHardwareAssists` 返回 0，校验和、TSO 全走软件。
5. `ifconfig` 那行 `SIOCGIFXMEDIA: Input/output error` 还在，只影响显示。

---

## 五、怎么退回 1.0.72 自环版

```bash
cd "/Users/macos/WorkBuddy/yt6801 mac"
./scripts/build.sh && ./scripts/install.sh
```

或者直接用 `backup/installed-sext/` 里那份系统已装扩展恢复。

> 注意：两个版本 bundle id 相同，系统里同时只能装一个。
> 预览版源码不受影响，退回只是把系统里跑的那个换回去。
