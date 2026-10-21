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

本機 7×24 小時日常使用，有線鏈路、DHCP、拔線回落、睡眠喚醒均已實測穩定。**其他機型未測試**——理論上任何帶 YT6801（`1f0a:6801`）PCIe 裝置的機器都能用，但實際表現取決於平台（PCIe 佈線、BIOS、VTD 組態）。

遇到問題請[提交 issue](../../issues)；有建議或指正，歡迎提 PR。

## 安裝

### 黑蘋果

1. 從 [Releases](../../releases) 下載 `YT6801-Installer-1.0.dmg` 並開啟
2. 啟動 **YT6801 Installer**，選語言、同意協議、點**安裝**
3. 插上網路線即可用，無需重開機

大多數黑蘋果環境（SIP 關閉 / AMFI 放寬）會靜默啟用驅動。如果啟用被攔截，檢查 SIP 是否關閉、boot-args 裡是否有 `amfi_get_out_of_my_way=0x1`。

### 白蘋果（免費方案）

本驅動是 ad-hoc 簽名——正版 Mac 要求 Developer ID 簽名 + 公證（$99/年），所以在白蘋果上免費執行的辦法是先放寬安全政策：

1. 進復原模式，開終端機執行 `csrutil disable`
2. boot-args 加上 `amfi_get_out_of_my_way=0x1 amfi=0x80` 後重開機
3. 然後照上面流程安裝

完全原版（SIP 開啟）的白蘋果無法載入未簽名 dext，這一點沒有免費繞法——需要付費開發者帳號，而且 DriverKit 網路 entitlement 還要向蘋果專項申請。

命令列方式：

```
"YT6801 Installer 1.0.app/Contents/MacOS/YT6801Installer" --install    # 或 --status / --uninstall
```

更多細節見 [docs/zh-TW/build-and-install.md](docs/zh-TW/build-and-install.md)，其他文件：[English](docs/en/build-and-install.md) · [简体中文](docs/zh-CN/build-and-install.md)。

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
