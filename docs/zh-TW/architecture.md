# 架構說明

## 這是什麼

一個跑在使用者態的 DriverKit（DEXT）驅動，接管 Motorcomm YT6801 PCIe 千兆
乙太網路控制器（`1f0a:6801`），把它註冊成 macOS 網路堆疊（Skywalk）裡的普通
`enX` 乙太網路介面。核心裡沒有一行驅動程式碼：驅動執行在獨立行程裡，透過映射的
PCIe BAR 操作晶片，當機只會重啟行程，核心不受影響。

移植自 Motorcomm 的 Linux 驅動（`fuxi-gmac`），繼承 GPL-2.0+ 授權。

## 分層

```
macOS 網路堆疊（Skywalk / IOUserNetworkEthernet）
        |  四佇列（TX/RX 提交 + 完成）+ 封包池
dext 行程（使用者態）
   - Start/Stop、介面註冊、鏈路上報
   - 硬體初始化：BAR0+0x2000 暫存器區塊、MDIO/PHY、DMA 重置
   - 資料面：門鈴驅動的 TX/RX 描述符環
        |  PCIe
YT6801 MAC（DWC 相容）-> PHY
```

## 暫存器佈局

- MAC/DMA/MTL/PHY 暫存器相對 **BAR0 + 0x2000**（Linux 原始碼裡的
  `FXGMAC_MAC_REGS_OFFSET`）
- MGMT 區塊（PHY 重置線、EFUSE 0x1500 區）直接相對 **BAR0**，不加偏移

## DMA 設計：自建緩衝

最關鍵的設計決定：`IOUserNetworkPacket::getDataIOVirtualAddress()` 給出的
封包池 IOVA 在這顆 MAC 上**不可用**——描述符回寫和封包長度看起來完全正常，但
DMA 實際讀寫的負載是別的記憶體。因此收發兩個方向都用自建的 2 KB 緩衝
（每方向 128 槽，共約 512 KB），經 `IODMACommand` 映射，每幀多一次 memcpy。
描述符只指向 `txSlotPhys[i]` / `rxSlotPhys[i]`。

環的要求：實體連續（單段 DMA）、64 位元定址（`maxAddressBits = 64`）、
不跨 4 GB 邊界（TDTR/RDTR 只有低 32 位元，4 KB 頁對齊天然滿足）。

## 啟動順序

1. PCI 匹配（`IOPCIPrimaryMatch 0x68011f0a`）、BAR0 映射
2. 晶片重置重載 EFUSE 補丁表、釋放 PHY 重置線
3. MAC 位址三級來源：EFUSE 補丁表 → MACA0 暫存器 → 靜態保底
4. 等 PHY 自動協商（熱插拔 1–2 秒，冷啟動可能 15 秒以上）
5. 偵測到網路線就跳過回環自檢，直接進真實模式並按協商速率配 MAC；
   沒有線才進回環自測
6. 描述符環在通道停止時寫基址（先寫環再置 ST/SR），SR 置位後再推 RX 尾指標

## 鏈路處理

實測 MSI-X 送不到（`irqCount` 恆 0），鏈路狀態和收包由 5 ms 計時器維持
（有效輪詢間隔 200 ms；回環態 2 s）。拔線靠輪詢發現並上報系統，
系統自動回落 Wi-Fi；每次速率變化都重新同步 MAC 的 PS/FES/DM 位。

## 已實作 / 未實作

已驗證可用：

- 端到端收發、幀內容正確、千兆鏈路、DHCP
- EFUSE 真實 MAC、多播雜湊過濾（IPv6 ND 可用）
- 拔線偵測與自動回落、睡眠喚醒存活

未實作（明確上報「不支援」，不謊報）：

- 硬體校驗和 offload、TSO、VLAN offload
- 多段描述符、多佇列/RSS、中斷合併
- MSI-X 中斷（用計時器輪詢代替）

已知外觀問題：`ifconfig` 可能印出 `SIOCGIFXMEDIA: Input/output error`——
媒體表是主動推送的，但堆疊從不呼叫舊的查詢路徑，不影響收發。
