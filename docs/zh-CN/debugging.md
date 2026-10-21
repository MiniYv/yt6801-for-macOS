# 排错笔记

这里只记"删了会再踩"的非显然事实。代码注释只写"做什么"，这里记"为什么"。

## 寄存器与总线

- MAC 寄存器全部相对 BAR0 + 0x2000。漏掉偏移的症状：`MAC_VR` 读成
  0xfee00000、MDIO 永远 busy、MAC 地址读成 01:00:00:00:01:00。MGMT 块
  （PHY 复位 0x1004、EFUSE 0x1500）不加偏移。
- PCIDriverKit 的 `MemoryRead32`/`Write32` 返回 void，出错时读到全 1
  （0xFFFFFFFF）——寄存器访问永远"成功"。MDIO 读到全 1 会卡死 busy 位，
  只能靠超时退出。
- 芯片级复位（SYS_RESET_REG 0x152c bit31，自清零）会重载 EFUSE 补丁表、
  重新拉起 PHY 复位线、并清掉 BAR0 0x1000–0x1300 的非粘性寄存器（先存后恢复）。
- PHY 复位线（0x1004）保持复位时，MDIO 仍应答但 PHY 不输出 MII 时钟——
  DMA 停滞、线上无帧，症状极具欺骗性。释放序列：写 0 → 等 1.5 ms → 写 1。
- PHY 软复位会擦掉全部扩展寄存器（AFE 配置），每次软复位后都要重写。
- SPEC_STATUS 位布局：bit10 链路、bit13 双工、bit14:15 速率。bit11 是另一
  个寄存器的定义，套错会把百兆读成千兆。

## MAC 地址

- 三级来源：EFUSE 补丁表 → MACA0 寄存器 → 静态保底。全 0/全 FF/多播位
  一律视为无效落下一级。
- 最终地址必须写回 MAC 地址过滤 0（MACA0LR/HR）——寄存器里残留的非法值
  会让 MAC 丢掉所有入帧，表现为"RX 永远没包"。
- EFUSE 行读：CTRL_0 写行号+START，轮询 CTRL_1 DONE，数据在 CTRL_1
  bit31:24。

## 描述符与 DMA

- 描述符必须指向驱动自建的 `IODMACommand` 缓冲，绝不能指向包池 IOVA
  （见 architecture.md）。幽灵帧（OWN 回写、PL 正确、载荷没动）必须丢弃；
  EtherType 为 0 也是脏缓冲的特征。
- 包池必须同时带 `PoolFlagMapToDext` 和 `PoolFlagToDevice`。
- 描述符环必须物理连续。
- RDES2 的 B1L 必须等于缓冲大小，RCR.RBSZ 替代不了它。
- TX 的 desc3 FL 必须写，只写 B1L 不够。
- TX 长度上限 = mtu + 14 + 4，栈可能投递聚合出来的大包。
- OWN 永远最后写，且写之前 `OSSynchronizeIO()`。

## 环状态机

- 满空判据用显式 pending 计数，不能用指针相等（回绕后指针相等会被读成"空"）。
- 环最多挂 RING-2 个描述符；RX 尾指针回绕到环基址时 DMA 会认为环空。
- RX 尾指针必须在 SR 置位之后再推；启动通道会把 CARDR/RDTR 复位到环首。
- 环基址必须在通道停止时写，然后再置 ST/SR。
- 卡死排查先看 CATDR/CARDR：指针不动就是 DMA 没跑，别先去翻描述符内容。
- 丢包判据看 MMC 硬件计数：MMC 在涨且 CRC 错为零说明线路干净，丢包在上游。

## 中断与时序

- MSI-X 配置成功但永远送不到；PacketPoller 队列绑定也被拒。最终用 5 ms
  `IOTimerDispatchSource` 收割。
- DriverKit 的 MMIO 比 Linux 快得多；MDIO 轮询必须显式 `IODelay(10)` 循环，
  否则等于没等。

## 链路

- 先定回环/真实模式，再做任何 PHY 操作：自检的软复位会拆掉已建立的链路。
- PHY 自协商在后台跑：热插拔 1–2 秒起链，冷启动可能 15 秒以上。
- MAC 速率不能写死 1000M；按协商结果配 PS/FES/DM，否则百兆口 link 灯亮
  但收发全废。
- ANLPAR/STAT1000 要在链路起来之后读（复位后 1 秒内读到的一律是 0）。

## macOS 协议栈

- 栈从不调用 `getSupportedMediaArray`；注册成功后要用
  `ReportAvailableMediaTypes` 主动推送媒体类型。
- 在 `Start` 里过早 reportLink 会被丢弃；借 `SetInterfaceEnable`
  （`ifconfig up/down` 触发）补报。
- 四个包队列都要显式 `setEnable`，少一个栈就不投递。
