# 建置與安裝

## 前置條件

- macOS 15.2+（建置實測於 macOS 26），Intel x86_64
- Xcode 26 與 DriverKit 26 SDK（`xcrun -sdk driverkit26.0 --show-sdk-path`）
- 不需要蘋果開發者帳號：dext 使用 ad-hoc 簽名

## 建置驅動

```
./scripts/build.sh
```

產出 `dist/yt6801.dext`（編譯 → 連結 → 組裝 → 簽名）。所有工具鏈路徑
（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）都支援環境變數覆蓋，CI 就是這麼做的。

宿主側離線驗證（環模擬器、PHY 回環序列）：

```
./scripts/test.sh
```

## 用圖形安裝器安裝

先建置安裝器：

```
cd installer && ./scripts/build.sh    # dist/YT6801-Installer-<版本>.dmg
```

打開 DMG，啟動 **YT6801 Installer**：

1. 選擇語言
2. 同意使用者協議
3. 點 **Install**（已裝驅動時同一介面顯示 **Uninstall**）

安裝器負責 /Applications 宿主 App、系統擴充啟用、解除安裝提權，全程不需要重開機。
宿主 App `/Applications/YT6801Driver.app` 是無介面的背景代理，**不要刪它**——
升級和解除安裝都走它。

## 命令列模式

安裝器：

```
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --uninstall
```

宿主 App（也可獨立使用）：

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
```

## 升級驅動

sysextd 只在 dext `Info.plist` 的 `CFBundleVersion` **嚴格遞增**時才替換已裝的
dext，同版本號會靜默跳過。改版本號 → 重新建置 → 重新安裝，執行中的實例會被
原地替換，不需要重開機。

## 解除安裝

用安裝器的 **Uninstall** 按鈕（需要管理員密碼）。它會等 `systemextensionsctl`
確認擴充已移除之後才動檔案。

## CI

`.github/workflows/release.yml` 在 GitHub 的 macOS runner 上建置驅動和安裝器，
每打一個 `v*` tag 就把 DMG/ZIP 掛到 GitHub Release。

## 驗證執行中的驅動

```
systemextensionsctl list | grep yt6801     # activated enabled
ifconfig                                   # enX，MAC 位址來自 EFUSE
/usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS "yt6801"'
./scripts/nic_test.sh                      # 全量健康/穩定性測試
```
