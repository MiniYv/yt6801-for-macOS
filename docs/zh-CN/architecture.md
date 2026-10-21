# 架构说明

## 这是什么

一个跑在用户态的 DriverKit（DEXT）驱动，接管 Motorcomm YT6801 PCIe 千兆
以太网控制器（`1f0a:6801`），把它注册成 macOS 网络栈（Skywalk）里的普通
`enX` 以太网接口。内核里没有一行驱动代码：驱动运行在独立进程里，通过映射的
PCIe BAR 操作芯片，崩溃只会重启进程，内核不受影响。

移植自 Motorcomm 的 Linux 驱动（`fuxi-gmac`），继承 GPL-2.0+ 协议。

## 分层

```
macOS 网络栈（Skywalk / IOUserNetworkEthernet）
        |  四队列（TX/RX 提交 + 完成）+ 包池
dext 进程（用户态）
   - Start/Stop、接口注册、链路上报
   - 硬件初始化：BAR0+0x2000 寄存器块、MDIO/PHY、DMA 复位
   - 数据面：门铃驱动的 TX/RX 描述符环
        |  PCIe
YT6801 MAC（DWC 兼容）-> PHY
```

## 寄存器布局

- MAC/DMA/MTL/PHY 寄存器相对 **BAR0 + 0x2000**（Linux 源码里的
  `FXGMAC_MAC_REGS_OFFSET`）
- MGMT 块（PHY 复位线、EFUSE 0x1500 区）直接相对 **BAR0**，不加偏移

## DMA 设计：自建缓冲

最关键的设计决定：`IOUserNetworkPacket::getDataIOVirtualAddress()` 给出的
包池 IOVA 在这颗 MAC 上**不可用**——描述符回写和包长看起来完全正常，但
DMA 实际读写的载荷是别的内存。因此收发两个方向都用自建的 2 KB 缓冲
（每方向 128 槽，共约 512 KB），经 `IODMACommand` 映射，每帧多一次 memcpy。
描述符只指向 `txSlotPhys[i]` / `rxSlotPhys[i]`。

环的要求：物理连续（单段 DMA）、64 位寻址（`maxAddressBits = 64`）、
不跨 4 GB 边界（TDTR/RDTR 只有低 32 位，4 KB 页对齐天然满足）。

## 启动顺序

1. PCI 匹配（`IOPCIPrimaryMatch 0x68011f0a`）、BAR0 映射
2. 芯片复位重载 EFUSE 补丁表、释放 PHY 复位线
3. MAC 地址三级来源：EFUSE 补丁表 → MACA0 寄存器 → 静态保底
4. 等 PHY 自协商（热插拔 1–2 秒，冷启动可能 15 秒以上）
5. 检测到网线就跳过回环自检，直接进真实模式并按协商速率配 MAC；
   没有线才进回环自测
6. 描述符环在通道停止时写基址（先写环再置 ST/SR），SR 置位后再推 RX 尾指针

## 链路处理

实测 MSI-X 送不到（`irqCount` 恒 0），链路状态和收包由 5 ms 定时器维持
（有效轮询间隔 200 ms；回环态 2 s）。拔线靠轮询发现并上报系统，
系统自动回落 Wi-Fi；每次速率变化都重新同步 MAC 的 PS/FES/DM 位。

## 已实现 / 未实现

已验证可用：

- 端到端收发、帧内容正确、千兆链路、DHCP
- EFUSE 真实 MAC、多播哈希过滤（IPv6 ND 可用）
- 拔线检测与自动回落、睡眠唤醒存活

未实现（明确上报"不支持"，不谎报）：

- 硬件校验和 offload、TSO、VLAN offload
- 多段描述符、多队列/RSS、中断合并
- MSI-X 中断（用定时器轮询代替）

已知外观问题：`ifconfig` 可能打印 `SIOCGIFXMEDIA: Input/output error`——
媒体表是主动推送的，但栈从不调旧的查询路径，不影响收发。
