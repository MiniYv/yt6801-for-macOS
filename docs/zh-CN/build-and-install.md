# 构建与安装

## 前置条件

- macOS 15.2+（构建实测于 macOS 26），Intel x86_64
- Xcode 与 DriverKit 24.2 或更新 SDK（`xcrun -sdk driverkit24.2 --show-sdk-path`）
- 不需要苹果开发者账号：dext 使用 ad-hoc 签名

## 构建驱动

```
./scripts/build.sh
```

产出 `dist/yt6801.dext`（编译 → 链接 → 装配 → 签名）。所有工具链路径
（`DKX`、`IIGBIN`、`CLANG`、`STRIP`、`CLT_LIB`）都支持环境变量覆盖，CI 就是这么做的。

宿主侧离线验证（环模拟器、PHY 回环序列）：

```
./scripts/test.sh
```

## 用图形安装器安装

先构建安装器：

```
cd installer && ./scripts/build.sh    # dist/YT6801-Installer-<版本>.dmg
```

打开 DMG，启动 **YT6801 Installer**：

1. 选择语言
2. 同意用户协议
3. 点 **Install**（已装驱动时同一界面显示 **Uninstall**）

安装器负责 /Applications 宿主 App、系统扩展激活、卸载提权，全程不需要重启。
宿主 App `/Applications/YT6801Driver.app` 是无界面的后台代理，**不要删它**——
升级和卸载都走它。

## 命令行模式

安装器：

```
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --status
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --install
"YT6801 Installer.app/Contents/MacOS/YT6801Installer" --uninstall
```

宿主 App（也可独立使用）：

```
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --status
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --activate
/Applications/YT6801Driver.app/Contents/MacOS/yt6801host --deactivate
```

## 升级驱动

sysextd 只在 dext `Info.plist` 的 `CFBundleVersion` **严格递增**时才替换已装的
dext，同版本号会静默跳过。改版本号 → 重新构建 → 重新安装，运行中的实例会被
原地替换，不需要重启。

## 卸载

用安装器的 **Uninstall** 按钮（需要管理员密码）。它会等 `systemextensionsctl`
确认扩展已移除之后才动文件。

## CI

`.github/workflows/release.yml` 在 GitHub 的 macOS runner 上构建驱动和安装器，
每打一个 `v*` tag 就把 DMG/ZIP 挂到 GitHub Release。

## 验证运行中的驱动

```
systemextensionsctl list | grep yt6801     # activated enabled
ifconfig                                   # enX，MAC 地址来自 EFUSE
/usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS "yt6801"'
./scripts/nic_test.sh                      # 全量健康/稳定性测试
```
