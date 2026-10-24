# Changelog

## v1.0.13 — 2026-10-24

### English

**Fixed: the driver failed to attach on machines whose ACPI marks the NIC as built-in (`provider entitlements check failed`)**

On platforms where the motherboard firmware (DSDT) injects the `built-in`  
property into the onboard Ethernet controller, the kernel requires the driver  
extension to carry the `com.apple.developer.driverkit.builtin` entitlement  
before instantiating it. That entitlement string must also precede the  
`IOPCIPrimaryMatch` dictionary inside the `transport.pci` array — the matcher  
stops at the first matching descriptor and never reads entries placed after  
it. The entitlement is now declared before the match dictionary, and the  
match key uses Apple's documented `IOPCIPrimaryMatch`. On such machines the  
driver now attaches with no boot-arg workaround at all (previously only  
`dk=0x8001` could bypass the check).

### 简体中文

**修复：在 ACPI 把网卡标记为内置设备的主板上，驱动无法附加（`provider entitlements check failed`）**

在主板固件（DSDT）给板载以太网控制器注入 `built-in` 属性的机器上，内核要求  
驱动扩展必须携带 `com.apple.developer.driverkit.builtin` 授权才允许实例化。  
同时该授权字符串必须放在 `transport.pci` 数组中 `IOPCIPrimaryMatch` 字典  
**之前**——匹配器命中字典后即停止遍历，放在后面的条目永远不会被读取。现在  
授权已前置于匹配字典，并把匹配键改为 Apple 文档标准的 `IOPCIPrimaryMatch`。  
此类机器无需任何 boot-arg 变通（此前只有 `dk=0x8001` 能绕过该检查）即可正常  
加载。

### 繁體中文

**修復：在 ACPI 將網卡標記為內建裝置的主機板上，驅動無法附加（`provider entitlements check failed`）**

在主機板韌體（DSDT）為板載乙太網路控制器注入 `built-in` 屬性的機器上，核心  
要求驅動擴充功能必須攜帶 `com.apple.developer.driverkit.builtin` 授權才允許  
實體化。同時該授權字串必須放在 `transport.pci` 陣列中 `IOPCIPrimaryMatch`  
字典**之前**——比對器命中字典後即停止走訪，放在後面的項目永遠不會被讀取。  
現在授權已前置於比對字典，並將比對鍵改為 Apple 文件標準的  
`IOPCIPrimaryMatch`。此類機器無需任何 boot-arg 變通（先前只有 `dk=0x8001`  
能繞過該檢查）即可正常載入。

## v1.0.9 — 2026-10-24

### English

**Fixed: the driver entered loopback and reported a fake link whenever no link  
was present at start**

Before this version, if the driver started without an active link (cable  
unplugged, or the peer port not yet up), it fell back to PHY loopback plus MAC  
internal loopback and reported the link "up" at 1000M full duplex. Frames sent  
in that state never reached the wire, and a cable plugged in later was detected  
only after the probe window expired. Loopback is a diagnostic mode, not a  
resting state — the driver no longer enters it on its own:

- No link now means an honest link-down report: the interface shows  
  `status: inactive` with the cable unplugged, no fake carrier.
- A cable plugged in later is picked up by the link poll directly.
- Leaving the self-test loopback restores auto-negotiation with  
  `BMCR=0x1340` (restart ANEG, no soft reset), so the PHY is never left pinned  
  to forced 100M.

**Fixed: the RX ring stopped permanently after roughly 30 received frames**

When the RX ring was armed to its full capacity, the tail pointer wrapped onto  
the ring base — behind the hardware's fetch cursor — and the DMA engine halted  
for good: `CARDR` froze, received-packet counters stopped, and nothing revived  
it. The ring now always keeps one free slot, and after refilling, the tail  
register is written with the address of the last armed descriptor, matching the  
vendor's Linux driver (`fxgmac_rx_refresh`).

**Changed: system requirement lowered to macOS 15.2**

The DriverKit version requirement drops from 25.0 to 24.2, so macOS 15.2 and  
later can load the driver. On macOS 15.x the previous builds were rejected by  
the system with a "newer SDK required" error, even though the hardware matched.

**Measured** (direct Ethernet connection between two computers):

- Link negotiates to 1000BASE-T full duplex; ARP resolves in both directions  
  within seconds of plugging in.
- With the cable unplugged the interface reports `status: inactive`.

### 简体中文

**修复：启动时没有链路，驱动就进入回环并上报假链路**

在此版本之前，如果驱动启动时没有活动链路（网线未插，或对端端口未起来），它会  
退回 PHY 回环加 MAC 内部回环，并把链路上报为「已连接 1000M 全双工」。这种状态下  
发出的帧根本不上网线，之后才插上的网线也要等探测窗过期才被发现。回环是诊断模式，  
不是待机状态——驱动不再自行进入回环：

- 没有链路就如实上报断开：不插线时接口显示 `status: inactive`，没有假载波。
- 之后插上网线，由链路轮询直接发现，不需要任何等待窗。
- 退出自检回环时写 `BMCR=0x1340`（重启自协商，不置软复位位），PHY 不会再被  
  钉死在强制 100M。

**修复：RX 环在收到约 30 帧后永久停摆**

RX 环挂满全部描述符时，尾指针会绕回环首、落在硬件游标后面，DMA 引擎就此永久  
停住：`CARDR` 冻结、收包计数不再增长，无法自行恢复。现在环上始终保留一个空槽，  
补挂描述符后尾指针写「最后一个已挂上的描述符」的地址，与厂商 Linux 驱动  
（`fxgmac_rx_refresh`）的写法一致。

**变更：系统要求降至 macOS 15.2**

DriverKit 版本要求从 25.0 降到 24.2，macOS 15.2 及以后都能加载。此前在  
macOS 15.x 上安装会被系统以「需要更新的 SDK」拒绝——尽管硬件完全匹配。

**实测**（两台电脑网线直连）：

- 链路协商到 1000BASE-T 全双工；插线后数秒内 ARP 双向解析成功。
- 拔线后接口如实显示 `status: inactive`。

### 繁體中文

**修復：啟動時沒有鏈路，驅動就進入回環並上報假鏈路**

在此版本之前，如果驅動啟動時沒有活動鏈路（網路線未插，或對端埠未起來），它會  
退回 PHY 回環加 MAC 內部回環，並把鏈路上報為「已連線 1000M 全雙工」。這種狀態下  
發出的封包根本不上網路線，之後才插上的網路線也要等偵測窗過期才被發現。回環是  
診斷模式，不是待機狀態——驅動不再自行進入回環：

- 沒有鏈路就如實上報斷開：不插線時介面顯示 `status: inactive`，沒有假載波。
- 之後插上網路線，由鏈路輪詢直接發現，不需要任何等待窗。
- 退出自我測試回環時寫 `BMCR=0x1340`（重啟自動協商，不置軟復位位），PHY 不會  
  再被釘死在強制 100M。

**修復：RX 環在收到約 30 個封包後永久停擺**

RX 環掛滿全部描述符時，尾指標會繞回環首、落在硬體遊標後面，DMA 引擎就此永久  
停住：`CARDR` 凍結、收包計數不再增長，無法自行恢復。現在環上始終保留一個空槽，  
補掛描述符後尾指標寫「最後一個已掛上的描述符」的位址，與原廠 Linux 驅動  
（`fxgmac_rx_refresh`）的寫法一致。

**變更：系統需求降至 macOS 15.2**

DriverKit 版本需求從 25.0 降到 24.2，macOS 15.2 及以後都能載入。此前在  
macOS 15.x 上安裝會被系統以「需要更新的 SDK」拒絕——儘管硬體完全匹配。

**實測**（兩台電腦網路線直連）：

- 鏈路協商到 1000BASE-T 全雙工；插線後數秒內 ARP 雙向解析成功。
- 拔線後介面如實顯示 `status: inactive`。

## v1.0.8 — 2026-10-22

### English

**Fixed: wired interface stuck on a 169.254 address when the driver started without an active link**

If the driver started while the wire was silent (cable not seated, or the switch  
port not yet up), it waited 30 s, ran the hardware self-test, and entered PHY  
loopback. Leaving loopback wrote `BMCR=0x9140` — bit15 is a soft reset — and the  
PHY came back as `0x0140`, i.e. **bit12 (auto-negotiation enable) cleared**. From  
that point the PHY was pinned to forced 100M, every 500 ms link probe was deaf,  
and a cable plugged in later could never be detected. The MAC kept looping DHCP  
frames internally, so the interface sat on a self-assigned `169.254.x.x` address  
while its link was reported "up" in self-loop mode.

**Fix.**

- The loopback probe exit path now writes `BMCR=0x1340` (bit9 restart  
  auto-negotiation, no soft reset), so auto-negotiation really restarts. Two  
  other restore paths still wrote the soft-reset value in this release; all  
  exit paths are unified to `0x1340` in v1.0.9.
- The loopback probe is non-blocking: it opens an 8 s window for  
  auto-negotiation instead of blocking 500 ms inside the poll timer, and only  
  re-enters loopback after that window expires.

**Measured** (gigabit switch port, cable plugged after the driver was already up):

- Link reaches **1000BASE-T full duplex in ~2.2 s** (2.0 s auto-negotiation plus  
  ≤200 ms poll interval); from loopback mode the worst case is ~4 s.
- 3.5 min soak after re-plug: zero link flaps, 300/300 pings to the gateway,  
  0 % loss, 0 input/output errors, DHCP bound on the first attempt.

### 简体中文

**修复：驱动在无链路状态下启动后，有线接口卡在 169.254 地址**

如果驱动启动时线上没有信号（网线没插到底，或交换机端口尚未起来），它会等满  
30 秒、跑完硬件自检后进入 PHY 回环。退出回环时写入 `BMCR=0x9140`——bit15 是软复位，  
PHY 回读成 `0x0140`，即 **bit12（自协商使能）被清掉**。从此 PHY 被钉死在强制  
100M，之后每 500ms 一次的链路探测全是聋的，晚插的网线永远发现不了；MAC 又把  
DHCP 报文关在内部回环里，于是接口停在自动分配的 `169.254.x.x`，而链路还以  
自环模式上报为"已连接"。

**修复内容**

- 回环探测退出路径改写 `BMCR=0x1340`（bit9 重启自协商，不置软复位位），自协商  
  真正重启。另两个恢复路径在本版本仍写软复位值，已在 v1.0.9 统一为 `0x1340`。
- 回环探测改为非阻塞：开一个 8 秒的自协商窗口，不再在轮询定时器里阻塞 500ms，  
  窗口过期后才重新进回环。

**实测**（千兆交换机端口，驱动已启动后才插线）

- **约 2.2 秒**协商到 1000BASE-T 全双工（2.0 秒自协商 + ≤200ms 轮询）；若当时在  
  回环态，最坏约 4 秒。
- 重插后连续 3.5 分钟：零链路翻转，ping 网关 300/300，丢包 0%，输入输出错误 0，  
  DHCP 一次绑上。

### 繁體中文

**修復：驅動在無鏈路狀態下啟動後，有線介面卡在 169.254 位址**

如果驅動啟動時線上沒有訊號（網路線沒插到底，或交換器埠尚未起來），它會等滿  
30 秒、跑完硬體自我測試後進入 PHY 回環。退出回環時寫入 `BMCR=0x9140`——bit15 是軟復位，  
PHY 回讀成 `0x0140`，即 **bit12（自動協商啟用）被清除**。從此 PHY 被釘死在強制  
100M，之後每 500ms 一次的鏈路偵測全是聾的，晚插的網路線永遠發現不了；MAC 又把  
DHCP 封包關在內部回環裡，於是介面停在自動指派的 `169.254.x.x`，而鏈路還以  
自環模式回報為「已連線」。

**修復內容**

- 回環偵測退出路徑改寫 `BMCR=0x1340`（bit9 重啟自動協商，不置軟復位位），自動  
  協商真正重啟。另兩個恢復路徑在本版本仍寫軟復位值，已在 v1.0.9 統一為  
  `0x1340`。
- 回環偵測改為非阻塞：開一個 8 秒的自動協商視窗，不再在輪詢計時器裡阻塞 500ms，  
  視窗過期後才重新進回環。

**實測**（千兆交換器埠，驅動已啟動後才插線）

- **約 2.2 秒**協商到 1000BASE-T 全雙工（2.0 秒自動協商 + ≤200ms 輪詢）；若當時在  
  回環態，最壞約 4 秒。
- 重插後連續 3.5 分鐘：零鏈路翻轉，ping 閘道 300/300，丟包 0%，輸入輸出錯誤 0，  
  DHCP 一次綁上。

## v1.0.7 — 2026-10-22

### English

**Fixed: severe throughput bug present since v1.0**

The v1.0 release (including all of its minor patched builds) carried a data-path  
bug that capped real-world throughput at roughly 1–3 MB/s — about 1% of gigabit  
line rate — in **both** directions, while everything else (link, DHCP, latency,  
error counters) appeared completely normal. This made large transfers such as  
downloads or file shares painfully slow.

**Root cause.** The TX doorbell handler (`TxDataAvailable`), which fires for  
every outgoing packet, ended with a leftover "bounded receive window"  
busy-wait loop from the early development builds: up to 1000 iterations of a  
15 µs hardware delay plus receive servicing, with a hard-coded minimum of  
about 134 iterations (~2 ms) per doorbell. Doorbell callbacks run serially on  
the driver's work loop, so this busy-wait serialized the entire data path —  
at ~200 doorbells per second the loop was saturated with delays and packets  
could only be serviced in tiny batches (measurements showed 6–11 packets per  
82 ms), pinning TCP throughput to the megabit range despite a healthy gigabit  
link.

**Fix.**

- Removed the per-doorbell busy-wait entirely. RX harvesting is already  
  covered by three independent mechanisms (adaptive 1 ms / 5 ms poll timer,  
  packet poller, and the RX doorbell), so no busy-wait fallback is needed.
- Removed the per-doorbell diagnostic register dumps (logging in the hot path  
  also serialized the data path).
- Shortened the TX ring-full spin from ~2 ms to ~300 µs; the 1 ms poll timer  
  re-runs the TX service if the ring is still full.

**Measured after the fix** (gigabit link, 1000BASE-T full duplex, zero TCP  
retransmissions, zero CRC errors):

- Download: **894 Mbps sustained** — about 95% of the ~940 Mbps practical TCP  
  ceiling for gigabit.

> Note: intermediate builds 1.0.1–1.0.6 were internal debug/test versions used  
> to locate this bug and were never published as releases. The public release  
> line therefore jumps from v1.0 directly to v1.0.7.

### 简体中文

**修复：自 v1.0 起就存在的严重吞吐缺陷**

v1.0 正式版（含其所有小规模调整版本）存在一个数据路径缺陷：实际传输速度被  
钉死在约 1–3 MB/s——仅为千兆线速的 **1% 左右**，且**双向都是如此**；而链路、  
DHCP、延迟、错误计数等一切指标看起来完全正常。这导致下载、文件共享等大流量  
传输慢得难以忍受。

**根因。** TX 门铃处理函数 `TxDataAvailable`（每发送一个包都会触发）的尾部  
残留了早期开发阶段的一段"有界接收窗口"忙等循环：最多 1000 次、每次 15 µs  
的硬件延时加收包服务，且硬编码了每轮至少约 134 次（约 2 ms）才允许提前退出。  
门铃回调在驱动工作循环上**串行执行**，因此这段忙等把整条数据路径串行化了：  
每秒约 200 次门铃时工作循环被延时占满，包只能以极小的批次被处理（实测每  
82 ms 仅推进 6–11 个包），TCP 吞吐被钉死在 Mbps 量级——尽管链路本身千兆  
完全健康。

**修复内容。**

- 彻底移除逐门铃忙等。收包收割已由三套相互独立的机制覆盖（1 ms / 5 ms 自适应  
  轮询定时器、packet poller、RX 门铃），无需忙等兜底。
- 移除逐门铃的诊断寄存器转储（热路径里的日志同样会串行化数据路径）。
- TX 环满时的自旋等待从约 2 ms 缩短到约 300 µs；若环仍满，由 1 ms 轮询定时器  
  重新触发 TX 服务。

**修复后实测**（千兆链路 1000BASE-T 全双工，TCP 零重传、零 CRC 错误）：

- 下载：**持续 894 Mbps** —— 约为千兆 TCP 实际上限（约 940 Mbps）的 95%。

> 说明：中间版本 1.0.1–1.0.6 是用于定位该缺陷的内部调试/测试版本，从未作为  
> Release 发布。公开版本号因此从 v1.0 直接跳到 v1.0.7。

### 繁體中文

**修復：自 v1.0 起就存在的嚴重吞吐缺陷**

v1.0 正式版（含其所有小規模調整版本）存在一個資料路徑缺陷：實際傳輸速度被  
釘死在約 1–3 MB/s——僅為千兆線速的 **1% 左右**，且**雙向都是如此**；而鏈路、  
DHCP、延遲、錯誤計數等一切指標看起來完全正常。這導致下載、檔案共用等大流量  
傳輸慢得難以忍受。

**根因。** TX 鈴鐺處理函式 `TxDataAvailable`（每傳送一個封包都會觸發）的尾部  
殘留了早期開發階段的一段「有界接收視窗」忙等迴圈：最多 1000 次、每次 15 µs  
的硬體延遲加收包服務，且硬編碼了每輪至少約 134 次（約 2 ms）才允許提前退出。  
鈴鐺回呼在驅動工作迴圈上**序列執行**，因此這段忙等把整條資料路徑序列化了：  
每秒約 200 次鈴鐺時工作迴圈被延遲佔滿，封包只能以極小的批次被處理（實測每  
82 ms 僅推進 6–11 個封包），TCP 吞吐被釘死在 Mbps 量級——儘管鏈路本身千兆  
完全健康。

**修復內容。**

- 徹底移除逐鈴鐺忙等。收包收割已由三套相互獨立的機制覆蓋（1 ms / 5 ms 自適應  
  輪詢定時器、packet poller、RX 鈴鐺），無需忙等兜底。
- 移除逐鈴鐺的診斷暫存器傾印（熱路徑裡的日誌同樣會序列化資料路徑）。
- TX 環滿時的自旋等待從約 2 ms 縮短到約 300 µs；若環仍滿，由 1 ms 輪詢定時器  
  重新觸發 TX 服務。

**修復後實測**（千兆鏈路 1000BASE-T 全雙工，TCP 零重傳、零 CRC 錯誤）：

- 下載：**持續 894 Mbps** —— 約為千兆 TCP 實際上限（約 940 Mbps）的 95%。

> 說明：中間版本 1.0.1–1.0.6 是用於定位該缺陷的內部除錯/測試版本，從未作為  
> Release 發布。公開版本號因此從 v1.0 直接跳到 v1.0.7。
