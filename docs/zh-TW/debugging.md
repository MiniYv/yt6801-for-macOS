# 排錯筆記

這裡只記「刪了會再踩」的非顯然事實。程式碼註釋只寫「做什麼」，這裡記「為什麼」。

## 暫存器與匯流排

- MAC 暫存器全部相對 BAR0 + 0x2000。漏掉偏移的症狀：`MAC_VR` 讀成
  0xfee00000、MDIO 永遠 busy、MAC 位址讀成 01:00:00:00:01:00。MGMT 區塊
  （PHY 重置 0x1004、EFUSE 0x1500）不加偏移。
- PCIDriverKit 的 `MemoryRead32`/`Write32` 回傳 void，出錯時讀到全 1
  （0xFFFFFFFF）——暫存器存取永遠「成功」。MDIO 讀到全 1 會卡死 busy 位，
  只能靠逾時退出。
- 晶片級重置（SYS_RESET_REG 0x152c bit31，自清零）會重載 EFUSE 補丁表、
  重新拉起 PHY 重置線、並清掉 BAR0 0x1000–0x1300 的非黏性暫存器（先存後恢復）。
- PHY 重置線（0x1004）保持重置時，MDIO 仍應答但 PHY 不輸出 MII 時脈——
  DMA 停滯、線上無幀，症狀極具欺騙性。釋放序列：寫 0 → 等 1.5 ms → 寫 1。
- PHY 軟重置會擦掉全部擴充暫存器（AFE 組態），每次軟重置後都要重寫。
- SPEC_STATUS 位佈局：bit10 鏈路、bit13 雙工、bit14:15 速率。bit11 是另一
  個暫存器的定義，套錯會把百兆讀成千兆。

## MAC 位址

- 三級來源：EFUSE 補丁表 → MACA0 暫存器 → 靜態保底。全 0/全 FF/多播位
  一律視為無效落下一級。
- 最終位址必須寫回 MAC 位址過濾 0（MACA0LR/HR）——暫存器裡殘留的非法值
  會讓 MAC 丟掉所有入幀，表現為「RX 永遠沒封包」。
- EFUSE 列讀：CTRL_0 寫列號+START，輪詢 CTRL_1 DONE，資料在 CTRL_1
  bit31:24。

## 描述符與 DMA

- 描述符必須指向驅動自建的 `IODMACommand` 緩衝，絕不能指向封包池 IOVA
  （見 architecture.md）。幽靈幀（OWN 回寫、PL 正確、負載沒動）必須丟棄；
  EtherType 為 0 也是髒緩衝的特徵。
- 封包池必須同時帶 `PoolFlagMapToDext` 和 `PoolFlagToDevice`。
- 描述符環必須實體連續。
- RDES2 的 B1L 必須等於緩衝大小，RCR.RBSZ 替代不了它。
- TX 的 desc3 FL 必須寫，只寫 B1L 不夠。
- TX 長度上限 = mtu + 14 + 4，堆疊可能投遞聚合出來的大封包。
- OWN 永遠最後寫，且寫之前 `OSSynchronizeIO()`。

## 環狀態機

- 滿空判據用顯式 pending 計數，不能用指標相等（回繞後指標相等會被讀成「空」）。
- 環最多掛 RING-2 個描述符；RX 尾指標回繞到環基址時 DMA 會認為環空。
- RX 尾指標必須在 SR 置位之後再推；啟動通道會把 CARDR/RDTR 重置到環首。
- 環基址必須在通道停止時寫，然後再置 ST/SR。
- 卡死排查先看 CATDR/CARDR：指標不動就是 DMA 沒跑，別先去翻描述符內容。
- 丟包判據看 MMC 硬體計數：MMC 在漲且 CRC 錯為零說明線路乾淨，丟包在上游。

## 中斷與時序

- MSI-X 組態成功但永遠送不到；PacketPoller 佇列綁定也被拒。最終用 5 ms
  `IOTimerDispatchSource` 收割。
- DriverKit 的 MMIO 比 Linux 快得多；MDIO 輪詢必須顯式 `IODelay(10)` 迴圈，
  否則等於沒等。

## 鏈路

- 先定回環/真實模式，再做任何 PHY 操作：自檢的軟重置會拆掉已建立的鏈路。
- PHY 自動協商在背景跑：熱插拔 1–2 秒起鏈，冷啟動可能 15 秒以上。
- MAC 速率不能寫死 1000M；按協商結果配 PS/FES/DM，否則百兆口 link 燈亮
  但收發全廢。
- ANLPAR/STAT1000 要在鏈路起來之後讀（重置後 1 秒內讀到的一律是 0）。

## macOS 協定堆疊

- 堆疊從不呼叫 `getSupportedMediaArray`；註冊成功後要用
  `ReportAvailableMediaTypes` 主動推送媒體類型。
- 在 `Start` 裡過早 reportLink 會被丟棄；借 `SetInterfaceEnable`
  （`ifconfig up/down` 觸發）補報。
- 四個封包佇列都要顯式 `setEnable`，少一個堆疊就不投遞。
