# Changelog

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
