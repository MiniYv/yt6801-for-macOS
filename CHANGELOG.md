# Changelog

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

- Exiting loopback now writes `BMCR=0x1340` (bit9 restart auto-negotiation, no
  soft reset), so auto-negotiation really restarts.
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

- 退出回环改写 `BMCR=0x1340`（bit9 重启自协商，不置软复位位），自协商真正重启。
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

- 退出回環改寫 `BMCR=0x1340`（bit9 重啟自動協商，不置軟復位位），自動協商真正重啟。
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
