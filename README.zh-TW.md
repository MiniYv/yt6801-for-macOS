# YT6801 乙太網路驅動程式（macOS）

[English](README.md) | [简体中文](README.zh-CN.md) | **繁體中文**

適用於 **Motorcomm YT6801 PCIe Gigabit 乙太網路控制器**（PCI `1f0a:6801`）的 macOS 使用者態 DriverKit（DEXT）網路驅動程式。

本專案移植自原廠的 Linux 驅動程式（[motorcomm yt6801 / fuxi-gmac](https://github.com/motorcomc/yt6801-linux-driver)），以 Apple DriverKit 框架重新實作。依上游專案要求，本儲存庫以 **GPL-2.0 或更新版本**授權釋出。

## 功能

- 使用者態 DriverKit 驅動——不依賴核心擴充（kext），支援 macOS 15.2+
- DMA 描述符環 + 自建 `IODMACommand` 緩衝區
- 定時輪詢鏈路（200ms）：拔線立即回報系統，可自動切換至 Wi-Fi
- 重新協商後自動同步 MAC 速率（10/100/1000M）
- 從 EFUSE 讀取出廠 MAC 位址（暫存器 / 靜態位址備援）
- 硬體自我回環自檢
- 回報媒體類型與廠商資訊（系統顯示為 "Motorcomm YT6801 PCIe Gigabit Ethernet Controller"）
- GUI 安裝程式（DMG），支援靜默啟用與命令列模式

## 系統需求

- macOS 15.2 或更新版本（開發與測試環境為 macOS 26），Intel（x86_64）
- 採用 Motorcomm YT6801 的乙太網路控制器（`1f0a:6801`）

## 測試機型

- **機械革命 蛟龍16Pro 潮玩青春版**——Ryzen 9 8940HX + RTX 5060，黑蘋果

## 安裝

從 [Releases](../../releases) 下載 `YT6801-安裝器-1.0.dmg`，開啟後執行安裝程式，點擊**安裝**即可——系統擴充功能自動啟用，無需重新開機。

命令列方式：

```
"/Applications/YT6801 安裝器.app/Contents/MacOS/YT6801Installer" --install    # 或 --status / --uninstall
```

## 從原始碼建置

需求：已安裝 Xcode 26（含 DriverKit 26 SDK）的 macOS，以及一份 DriverKit SDK 副本。

```
./scripts/build.sh        # 編譯 + 簽署，產物在 dist/
./scripts/install.sh      # 建置宿主 app 並啟用
./scripts/test.sh         # 宿主端邏輯驗證
./scripts/nic_test.sh     # 對執行中的驅動程式進行完整網卡健康測試
```

`scripts/build.sh` 中所有工具鏈路徑（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）皆可用環境變數覆寫——CI 正是如此運作。

## 腳本一覽

| 腳本 | 用途 |
|---|---|
| `build.sh` | 編譯、簽署、打包 dext |
| `install.sh` / `uninstall.sh` | 安裝 / 解除安裝驅動程式與宿主 app |
| `update.sh` / `hotupdate.sh` / `redeploy.sh` | 遞增版本號升級的輔助腳本 |
| `test.sh` | 宿主端邏輯驗證（描述符環模擬器） |
| `loopback_test.sh` | 對執行中的驅動程式進行 PHY 自環測試 |
| `watch_activate.sh` | 監看系統擴充功能啟用狀態 |
| `nic_test.sh` | 鏈路、延遲、錯誤計數、吞吐與壓力測試 |
| `installer/scripts/build.sh` | 建置 GUI 安裝程式 DMG/ZIP |

## 授權條款

本專案以 **GNU General Public License v2.0 或更新版本**釋出——詳見 [LICENSE](LICENSE)。

本專案衍生自 Motorcomm 針對同一晶片的 Linux 驅動程式：

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv（macOS DriverKit 移植）
```

本程式係依「現狀」散布，不提供任何明示或默示之擔保，包含但不限於適售性與特定用途適用性之擔保。詳見 GNU 通用公共授權條款。
