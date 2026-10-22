# YT6801 乙太網路驅動程式（macOS）

[English](README.md) | [简体中文](README.zh-CN.md) | **繁體中文**

適用於 **Motorcomm YT6801 PCIe Gigabit 乙太網路控制器**（PCI `1f0a:6801`）的 macOS 使用者態 DriverKit（DEXT）網路驅動程式。

**開發者：MiniYv（小禹）**

## 關於本專案

**Motorcomm YT6801，中國大陸第一款量產商用的消費級 PCIe 千兆網卡晶片。這是全世界第一個開源的 YT6801 macOS 驅動程式。有了它，國產的PCIe網卡第一次在 macOS 上跑起來了。**

晶片把 MAC、PHY 與 PCIe 控制器做進一顆 4×4 mm、32 引腳的封裝，單 3.3V 供電。整機廠用它替掉原先板載的 Intel、Realtek 方案，BOM 與佈板面積一起省下來。成本降了，用它的筆電也就多了。機械革命、七彩虹、TUXEDO 的現售機型裡都能找到（見「已知使用 YT6801 的機型」）。

硬體從來不缺，缺的是軟體。Intel、Realtek 的網卡在黑蘋果社群有一堆現成的 kext 與 dext 可挑；換上這顆國產晶片的機器一片空白，插上網路線，macOS 也不認。

驅動程式從零寫起，跑在使用者態，不依賴核心擴充。

> **全程 AI 生成。** 整個驅動程式由 WorkBuddy（Vibe Coding）生成，沒有一行人工手寫程式碼，並在本機實測跑通（見「測試機型與目前狀態」）。開發過程中修掉的問題裡有兩個要緊：吞吐被釘在線速 1%；驅動在無鏈路狀態下啟動後，鏈路會卡死。詳見 [CHANGELOG.md](CHANGELOG.md)。

本專案移植自原廠為同一晶片發布的 Linux 驅動程式（`fuxi-gmac` 驅動堆疊，原始碼收錄於 [GitHub 鏡像儲存庫](https://github.com/bartvdbraak/yt6801)，原始出處為 [Motorcomm 官網下載頁](https://www.motor-comm.com/product/ethernet-control-chip)）。依上游授權要求，本儲存庫以 **GPL-2.0 或更新版本**授權釋出。

## 功能

- 使用者態 DriverKit 驅動，不依賴核心擴充（kext），支援 macOS 15.2+
- DMA 描述符環 + 自建 `IODMACommand` 緩衝區
- 定時輪詢鏈路（200ms）：拔線立即回報系統，可自動切換至 Wi-Fi
- 重新協商後自動同步 MAC 速率（10/100/1000M）
- 千兆線速吞吐實測：**下載持續 894 Mbps**（約為 TCP 上限的 95%），零重傳
- 從 EFUSE 讀取出廠 MAC 位址（暫存器 / 靜態位址備援）
- 硬體自我回環自檢
- 回報媒體類型與廠商資訊（系統顯示為 "Motorcomm YT6801 PCIe Gigabit Ethernet Controller"）
- GUI 安裝程式（DMG），支援靜默啟用與命令列模式

## 系統需求

- macOS 15.2 或更新版本（開發與測試環境為 macOS 26），Intel（x86_64）
- 採用 Motorcomm YT6801 的乙太網路控制器（`1f0a:6801`）

## 測試機型與目前狀態

- **機械革命 蛟龍16Pro 潮玩青春版**：Ryzen 9 8940HX + RTX 5060，黑蘋果
- 測試系統：**macOS 26.7.1（Tahoe）**

**狀態：可用，但談不上久經考驗。** 本機長期掛著有線鏈路日常使用，DHCP、拔線回落、睡眠喚醒都在這台機器上跑過。這不構成穩定性承諾。最初的 v1.0 帶的缺陷比當時發現的更多，**實際並不穩定**，全靠真實使用一個個露出來：吞吐先被釘在線速約 1%，接著是無鏈路啟動後鏈路卡死。現在明顯好用了，長時間運行仍然最缺測試。它是個能用的驅動程式，還算不上完工。

**其他機型與系統版本未測試。** 理論上任何帶 YT6801（`1f0a:6801`）PCIe 裝置的機器都能用，實際表現取決於平台（PCIe 佈線、BIOS、VTD 組態）。

### 已知使用 YT6801 的機型

| 品牌 | 機型 | 依據 |
|---|---|---|
| 七彩虹 COLORFUL | 將星 X16 Pro（i7-14650HX + RTX 5060） | 拆解報告：網路晶片為裕太微 YT6801 |
| 機械革命 MECHREVO | 蛟龍16Pro（R9-9955HX、潮玩青春版 R9-8940HX） | `lspci` 實測；其中 8940HX 這台就是本驅動程式的開發機 |
| 機械革命 MECHREVO | 極光X（GM6PR0C）、耀世15Pro | 使用者實測；廠商 INF 裡極光X 對應子系統 `139A:1D05` |
| 機械革命 MECHREVO | 無界14X / WUJIE14X(A) | ArchWiki 硬體表記錄乙太網路 `1f0a:6801` 可用 |
| TUXEDO | InfinityBook Pro 14 Gen9（AMD）、InfinityBook Pro 15 Gen10 | TUXEDO 官方 FAQ 明確寫有 Motorcomm YT6801，並發布 `tuxedo-yt6801` 驅動套件 |
| 宏碁 Acer | 未公開具體型號 | 廠商 INF：子系統廠商 `1025`（`1739`、`173C`、`1947`、`194F`） |
| 三星 Samsung | 未公開具體型號 | 廠商 INF：子系統 `C1D9:144D` |
| Ninkear | N10 Pro 迷你主機（Intel N150） | Umbrel OS 上的 `lspci` 報告 |
| 同方 TongFang 模具 | 子系統廠商 `1D05`（`137D`、`137F`、`1380`、`1381`、`1382`、`1383`、`1384`、`1385`、`1388` 等） | 廠商 INF 標註為 `tongfang`；機械革命與 TUXEDO 都基於同方模具 |

同一個型號名不等於同一顆網路晶片，批次與子型號會換料（七彩虹另有筆電使用 Realtek 2.5GbE）。判斷依 PCI ID，不依品牌。

**查自己這台機器：**

- macOS：`ioreg -l -p IODeviceTree | grep -i 1f0a`，命中會印出 `"pci1f0a,6801"`；旁邊的 `pci1d05,7011` 是子系統（ODM）ID。
- Linux：`lspci -nn | grep 1f0a`
- Windows：裝置管理員 → 網路卡的硬體 ID 含 `PCI\VEN_1F0A&DEV_6801`

型號不在表裡但 ID 對得上，理論上就能用，歡迎到 issue 回報結果。

遇到問題請[提交 issue](../../issues)；有建議或指正，歡迎提 PR。

## 常見問題

**macOS 自帶 YT6801 的驅動嗎？**
沒有。蘋果不為 `1f0a:6801` 提供任何驅動。沒有這個專案，網卡在「網路」設定裡根本不會出現，插上網路線也不會有反應，這類機器在 macOS 下通常被直接說成「沒有有線網路」。

**這是 kext 嗎？**
不是。它是 DriverKit 系統擴充（dext），跑在使用者空間。核心裡什麼都不注入，`EFI/OC/Kexts` 也不會被動到。裝一個 app，從 app 裡啟用擴充，和廠商驅動的裝法一樣。

**支援哪些 macOS 版本？**
15.2 及以後的版本，包含 macOS 26 Tahoe。驅動用的 DriverKit 網路介面在 15.2 之前沒有。

**支援 Apple Silicon 嗎？**
不支援。dext 只編了 x86_64，而且沒有哪台 M 系列 Mac 用 YT6801。目標是 Intel、AMD 黑蘋果。

**必須關 SIP 嗎？**
多數黑蘋果本來就關著 SIP 或放寬了 AMFI，啟用直接過。如果被系統攔下來，關掉 SIP 並在 boot-args 裡加 `amfi_get_out_of_my_way=0x1`，一般就能解決。

**介面出來了，但一直是 169.254 位址。**
這是沒拿到 DHCP，不是驅動壞了。先查網路線和交換器埠。如果連線燈是亮的、位址還是不來，收集 `sudo log show --last 5m --predicate 'eventMessage CONTAINS "yt6801:"'` 後提 issue。

**插網路線後 Wi-Fi 會被頂掉嗎？**
不會，兩個服務並存。macOS 按「網路」設定裡的服務順序選出口，拔掉網路線會自動回落到 Wi-Fi。

**實際能跑多快？**
Gigabit 連線下 TCP 下載持續 894 Mbps，約合連線能力的 95%。細節見 [CHANGELOG.md](CHANGELOG.md)。

**驅動簽章了嗎，要公證嗎？**
ad-hoc 簽章。SIP 開啟的白蘋果會拒絕載入。要讓它載入需要 Developer ID 簽章加公證，DriverKit 網路 entitlement 還得蘋果單獨批准。

## 更新日誌

見 [CHANGELOG.md](CHANGELOG.md)（English / 简体中文 / 繁體中文）。目前版本 **v1.0.8**：修掉了驅動在無鏈路狀態下啟動後鏈路卡死、介面停在 `169.254` 位址的毛病；v1.0.7 那版修的是自 v1.0 起就存在的吞吐缺陷（先前實際傳輸速度被釘在千兆線速的約 1%），這版也帶上了。兩處修完，千兆鏈路實測**下載持續 894 Mbps**（約為 TCP 上限的 95%）。

## 安裝

### 黑蘋果（OpenCore）

1. 從 [Releases](../../releases) 下載 `YT6801-Installer-x.x.dmg` 並開啟
2. 啟動 **YT6801 Installer**，選語言、同意協議、點**安裝**
3. 插上網路線即可用，無需重開機

大多數黑蘋果環境（SIP 關閉 / AMFI 放寬）會靜默啟用驅動。如果啟用被攔截，檢查 SIP 是否關閉、boot-args 裡是否有 `amfi_get_out_of_my_way=0x1`。

### 白蘋果（免費方案）

本驅動是 ad-hoc 簽名。正版 Mac 要 Developer ID 簽名加公證（$99/年），免費在白蘋果上執行的辦法，是先把安全政策放寬：

1. 進復原模式，開終端機執行 `csrutil disable`
2. boot-args 加上 `amfi_get_out_of_my_way=0x1 amfi=0x80` 後重開機
3. 然後照上面流程安裝

完全原版（SIP 開啟）的白蘋果無法載入未簽名 dext，這一點沒有免費繞法：得有付費開發者帳號，DriverKit 網路 entitlement 還得單獨向蘋果申請。

也可以走命令列，直接執行安裝程式執行檔（在掛載的 DMG 中，或複製到 /Applications 之後）：

```
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer x.x.app/Contents/MacOS/YT6801Installer" --uninstall
```

驅動程式安裝後，也可以不經過安裝程式，直接用 /Applications 下的宿主 app 啟用或停用擴充功能：

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
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

`scripts/build.sh` 中所有工具鏈路徑（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）都能用環境變數覆寫，CI 就是這麼做的。

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

本專案以 **GNU General Public License v2.0 或更新版本**釋出，詳見 [LICENSE](LICENSE)。

本專案衍生自 Motorcomm 針對同一晶片的 Linux 驅動程式：

```
Copyright (c) 2021 Motor-comm Corporation.
Copyright (c) 2026 MiniYv（macOS DriverKit 移植）
```

本程式係依「現狀」散布，不提供任何明示或默示之擔保，包含但不限於適售性與特定用途適用性之擔保。詳見 GNU 通用公共授權條款。
