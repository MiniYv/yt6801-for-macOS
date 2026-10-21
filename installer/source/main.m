/*
 * YT6801 installer — single-file implementation (GUI + CLI dual mode)
 *
 * Design notes:
 *   1. Portable, no install needed: runs by double-click; writes no prefs,
 *      no daemon, no background service.
 *   2. Install = copy itself to /Applications/YT6801Driver.app + activate the
 *      system extension. /Applications is a hard sysextd requirement.
 *   3. Upgrade relies on CFBundleVersion bump + ad-hoc signature +
 *      OSSystemExtensionRequest; sysextd replaces silently, no approval
 *      prompt and no reboot.
 *   4. Uninstall uses the official deactivationRequest, not the root-only
 *      systemextensionsctl path.
 *
 * CLI (for automation; GUI shares the same functions):
 *   --status     print status, exit code 0=installed 1=not installed
 *   --install    silent install
 *   --uninstall  silent uninstall
 *   --activate   submit activation request only (progress logged to file)
 *   --deactivate submit deactivation request only
 */

#import <Cocoa/Cocoa.h>
#import <SystemExtensions/SystemExtensions.h>
#import <Security/Security.h>
#import <sys/stat.h>
#import <unistd.h>

#ifndef YT_BUILTIN_VERSION
#define YT_BUILTIN_VERSION "0.0.0"
#endif

static NSString * const kExtID      = @"io.miniyv.yt6801ethernet";
static NSString * const kHostID     = @"io.miniyv.yt6801host";
static NSString * const kAppPath    = @"/Applications/YT6801Driver.app";
static NSString * const kLogPath    = @"/tmp/yt6801_installer.log";
static NSString * const kExtLogPath = @"/tmp/yt6801_activate.log";
#define LSREG_PATH "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
static NSString * const kInstallerID = @"io.miniyv.yt6801.installer";

static NSString * const kBuiltinVersion = @YT_BUILTIN_VERSION;

/* ------------------------------------------------------------------ */
#pragma mark - UI language

/* Selected on the first screen: 0 = English, 1 = Simplified, 2 = Traditional */
static NSInteger gLang = 1;

enum {
    K_WINDOW_TITLE,
    K_TITLE,
    K_SUBTITLE,
    K_CHECKING,
    K_ST_INSTALLED,
    K_FMT_VERSION,
    K_HINT_UP_TO_DATE,
    K_HINT_DIFF_VERSION,
    K_BTN_REINSTALL,
    K_BTN_UNINSTALL,
    K_ST_TERMINATING,
    K_ST_INACTIVE,
    K_HINT_INACTIVE,
    K_ST_ONDISK,
    K_FMT_ONDISK,
    K_HINT_ONDISK,
    K_ST_NOTINSTALLED,
    K_FMT_BUNDLED,
    K_HINT_FRESH,
    K_BTN_INSTALL,
    K_BTN_CANCEL,
    K_BTN_DETAILS,
    K_BTN_HIDE_DETAILS,
    K_LOG_DETAIL_OPEN,
    K_INSTALLING,
    K_UNINSTALLING,
    K_DONE_INSTALL,
    K_DONE_UNINSTALL,
    K_BTN_DONE,
    K_HINT_INSTALLED_OK,
    K_HINT_UNINSTALLED_OK,
    K_LOG_INSTALLED,
    K_LOG_UNINSTALLED,
    K_FAIL_INSTALL,
    K_FAIL_UNINSTALL,
    K_OK,
    K_CONFIRM_UNINSTALL_TITLE,
    K_CONFIRM_UNINSTALL_INFO,
    K_LICENSE_TITLE,
    K_LICENSE_CHECK,
    K_BTN_AGREE,
    K_FMT_STATE_LINE,
    K_MENU_ABOUT,
    K_MENU_QUIT,
    K_LAUNCH_FAIL,
    K_REPLACING_EXT,
    K_NEED_APPROVAL_SYS,
    K_AFTER_APPROVAL,
    K_REBOOT_REQUIRED_LOG,
    K_COMPLETED_LOG,
    K_REQ_START,
    K_UNLOAD,
    K_LOAD,
    K_CANNOT_CREATE_REQ,
    K_REQ_SUBMITTED,
    K_TIMEOUT_LOG,
    K_OLD_INSTALLER,
    K_STAGE_COPY_FAIL,
    K_HOST_STAGED,
    K_OLD_HOST_REMOVED,
    K_OLD_HOST_BACKED_UP,
    K_HOST_LAND_FAIL,
    K_HOST_IN_PLACE,
    K_SIGN_VERIFY_FAIL,
    K_BUNDLE_ID_MISMATCH,
    K_UNREADABLE,
    K_SIGN_BUNDLE_OK,
    K_REPLACING_HOST,
    K_ACTIVATOR_MISSING,
    K_CANNOT_START_ACT,
    K_REBOOT_NEEDED,
    K_ACTIVATION_DENIED,
    K_ACTIVATION_TIMEOUT,
    K_ACTIVATOR_CRASHED,
    K_STEP1_CHECK,
    K_DEXT_MISSING,
    K_FMT_BUNDLED_VER,
    K_STEP2_DEPLOY,
    K_STEP3_VERIFY,
    K_STEP4_ACTIVATE,
    K_ACTIVATE_NO_ENTRY,
    K_FMT_LOADED_VER,
    K_HOST_HIDDEN,
    K_HOST_HIDE_FAIL,
    K_USTEP1,
    K_NOT_INSTALLED_ERR,
    K_FMT_CURRENT_VER,
    K_UNKNOWN,
    K_USTEP2,
    K_USTEP3,
    K_CANNOT_START_UNIN,
    K_AUTH_TIMEOUT,
    K_CANCELED,
    K_UNINSTALL_FAIL_FMT,
    K_AUTH_DENIED,
    K_UNINSTALL_INCOMPLETE,
    K_SYS_CONFIRMED,
    K_USTEP4,
    K_FMT_REMOVED,
};

/* Order of columns must match the enum above. */
static NSString * kUiStrings[][3] = {
    [K_WINDOW_TITLE] = { @"YT6801 Installer", @"YT6801 安装器", @"YT6801 安裝器" },
    [K_TITLE]        = { @"YT6801 Ethernet Driver", @"YT6801 有线网卡驱动", @"YT6801 有線網卡驅動" },
    [K_SUBTITLE]     = { @"Motorcomm YT6801 Gigabit Ethernet · DriverKit System Extension",
                         @"Motorcomm YT6801 千兆以太网 · DriverKit 系统扩展",
                         @"Motorcomm YT6801 Gigabit 乙太網路 · DriverKit 系統擴充功能" },
    [K_CHECKING]     = { @"Detecting…", @"正在检测…", @"正在偵測…" },
    [K_ST_INSTALLED] = { @"Installed and active", @"已安装并正在使用", @"已安裝並正在使用" },
    [K_FMT_VERSION]  = { @"System version %@ · Bundled version %@",
                         @"系统版本 %@ · 内置版本 %@",
                         @"系統版本 %@ · 內建版本 %@" },
    [K_HINT_UP_TO_DATE] = {
        @"Already up to date. Reinstalling only replaces the files in /Applications; "
        "the system reloads a driver only when the version number increases.",
        @"当前已是最新版本。重装只会覆盖 /Applications 里的文件；"
        @"系统按版本号判断是否重新加载，同版本不会替换正在运行的驱动。",
        @"目前已是最新版本。重新安裝只會覆蓋 /Applications 裡的檔案；"
        @"系統會依版本號判斷是否重新載入，同版本不會替換正在執行中的驅動程式。" },
    [K_HINT_DIFF_VERSION] = {
        @"The bundled version differs from the installed one. Reinstall replaces the "
        "running driver automatically — no restart required.",
        @"安装包里的版本与系统不同，点「重装」会自动替换并让旧版本失效，不需要重启。",
        @"安裝包內的版本與系統不同，點「重裝」會自動替換並讓舊版本失效，不需要重新開機。" },
    [K_BTN_REINSTALL] = { @"Reinstall", @"重装", @"重裝" },
    [K_BTN_UNINSTALL] = { @"Uninstall", @"卸载", @"解除安裝" },
    [K_ST_TERMINATING] = { @"Old version is exiting…", @"旧版本正在退出…", @"舊版本正在退出…" },
    [K_ST_INACTIVE]    = { @"Installed but not activated", @"已安装但未激活", @"已安裝但未啟用" },
    [K_HINT_INACTIVE] = {
        @"Click Reinstall to submit the activation request again. If it stays stuck, "
        "uninstall first and install again.",
        @"点「重装」重新提交激活请求；如果一直卡在这个状态，选「卸载」再装一次。",
        @"點「重裝」重新送出啟用請求；如果一直卡在這個狀態，選「解除安裝」再裝一次。" },
    [K_ST_ONDISK] = { @"Files in place, driver not activated",
                      @"文件已就位，驱动未激活", @"檔案已就位，驅動程式未啟用" },
    [K_FMT_ONDISK] = { @"%1$@ is in /Applications, but missing from the system list",
                       @"/Applications 里有 %1$@，但系统列表里没有",
                       @"/Applications 裡有 %1$@，但系統清單裡沒有" },
    [K_HINT_ONDISK] = {
        @"Click Install to activate again. The files are already in place and will not be copied twice.",
        @"点「安装」重新激活，文件已经在了，不会重复复制。",
        @"點「安裝」重新啟用，檔案已經在了，不會重複複製。" },
    [K_ST_NOTINSTALLED] = { @"Not installed", @"未安装", @"未安裝" },
    [K_FMT_BUNDLED]  = { @"Bundled driver version %@", @"安装包内置版本 %@", @"安裝包內建版本 %@" },
    [K_HINT_FRESH] = {
        @"Installing deploys the driver to /Applications/YT6801Driver.app and registers "
        "the system extension. No restart, no impact on your current network.",
        @"安装会把驱动部署到 /Applications/YT6801Driver.app 并注册系统扩展。"
        @"全程不重启，不影响当前网络。",
        @"安裝會把驅動程式部署到 /Applications/YT6801Driver.app 並註冊系統擴充功能。"
        @"全程不需重新開機，不會影響目前的網路。" },
    [K_BTN_INSTALL]  = { @"Install", @"安装", @"安裝" },
    [K_BTN_CANCEL]   = { @"Cancel", @"取消", @"取消" },
    [K_BTN_DETAILS]  = { @"Details ▾", @"查看详情 ▾", @"查看詳情 ▾" },
    [K_BTN_HIDE_DETAILS] = { @"Hide details ▴", @"收起详情 ▴", @"收起詳情 ▴" },
    [K_LOG_DETAIL_OPEN] = { @"— details expanded —", @"— 详情窗口已展开 —", @"— 詳情視窗已展開 —" },
    [K_INSTALLING]   = { @"Installing…", @"正在安装…", @"正在安裝…" },
    [K_UNINSTALLING] = { @"Uninstalling…", @"正在卸载…", @"正在解除安裝…" },
    [K_DONE_INSTALL]   = { @"Installation complete", @"安装完成", @"安裝完成" },
    [K_DONE_UNINSTALL] = { @"Uninstalled", @"卸载完成", @"解除安裝完成" },
    [K_BTN_DONE]     = { @"Done", @"完成", @"完成" },
    [K_HINT_INSTALLED_OK] = {
        @"Installation complete. Plug in the cable and it works — no restart needed.",
        @"安装完成。插上网线即可使用，不需要重启。",
        @"安裝完成。插上網路線即可使用，不需要重新開機。" },
    [K_HINT_UNINSTALLED_OK] = {
        @"Uninstalled. Feel free to keep the installer around for next time.",
        @"已卸载。安装器可以留着，随时再装。",
        @"已解除安裝。安裝程式可以留著，隨時再裝。" },
    [K_LOG_INSTALLED]   = { @"✓ installed", @"✓ 安装完成", @"✓ 安裝完成" },
    [K_LOG_UNINSTALLED] = { @"✓ uninstalled", @"✓ 卸载完成", @"✓ 解除安裝完成" },
    [K_FAIL_INSTALL]   = { @"Installation failed", @"安装没有完成", @"安裝未完成" },
    [K_FAIL_UNINSTALL] = { @"Uninstall failed", @"卸载没有完成", @"解除安裝未完成" },
    [K_OK]             = { @"OK", @"好", @"好" },
    [K_CONFIRM_UNINSTALL_TITLE] = {
        @"Uninstall the YT6801 driver?", @"要卸载 YT6801 驱动吗？", @"要解除安裝 YT6801 驅動程式嗎？" },
    [K_CONFIRM_UNINSTALL_INFO] = {
        @"This removes the system extension; your wired network will go down. "
        "The installer stays and you can reinstall anytime. No restart needed.",
        @"会从系统里移除这个系统扩展，当前有线网络会断开。"
        @"安装器本身会保留，随时可以再装回来。不需要重启。",
        @"會從系統裡移除此系統擴充功能，目前的有線網路會中斷。"
        @"安裝程式本身會保留，隨時可以再裝回來。不需要重新開機。" },
    [K_LICENSE_TITLE] = { @"License Agreement", @"用户许可协议", @"使用者授權協議" },
    [K_LICENSE_CHECK] = {
        @"I have read and agree to all terms (free of charge · GPL-2.0 open source)",
        @"我已阅读并同意上述全部条款（完全免费 · GPL-2.0 开源许可证）",
        @"我已閱讀並同意上述全部條款（完全免費 · GPL-2.0 開源授權）" },
    [K_BTN_AGREE]    = { @"Agree and Continue", @"同意并继续", @"同意並繼續" },
    [K_FMT_STATE_LINE] = { @"System version %1$@ · %2$@", @"系统版本 %1$@ · %2$@", @"系統版本 %1$@ · %2$@" },
    [K_MENU_ABOUT]   = { @"About YT6801 Installer", @"关于 YT6801 安装器", @"關於 YT6801 安裝器" },
    [K_MENU_QUIT]    = { @"Quit YT6801 Installer", @"退出 YT6801 安装器", @"結束 YT6801 安裝器" },
    [K_LAUNCH_FAIL] = { @"Launch failed: %@", @"启动失败：%@", @"無法啟動：%@" },
    [K_REPLACING_EXT] = { @"Existing extension with same ID: old %@ -> new %@, replacing", @"检测到同 ID 扩展：旧 %@ → 新 %@，选择替换", @"偵測到同 ID 擴充功能：舊 %@ → 新 %@，選擇替換" },
    [K_NEED_APPROVAL_SYS] = { @"NEED_APPROVAL - approve in System Settings > General > Login Items & Extensions", @"NEED_APPROVAL 需要在「系统设置 > 通用 > 登录项与扩展」里批准", @"NEED_APPROVAL 需要在「系統設定 > 一般 > 登入項目與延伸功能」裡核准" },
    [K_AFTER_APPROVAL] = { @"No restart needed after approval; the program continues automatically.", @"提示：批准后无需重启，本程序会自动继续。", @"提示：核准後無需重新開機，本程式會自動繼續。" },
    [K_REBOOT_REQUIRED_LOG] = { @"RESULT=WillCompleteAfterReboot (restart required)", @"RESULT=WillCompleteAfterReboot（这一步才需要重启）", @"RESULT=WillCompleteAfterReboot（這一步才需要重新開機）" },
    [K_COMPLETED_LOG] = { @"RESULT=Completed (no restart) result=%ld", @"RESULT=Completed（无需重启）result=%ld", @"RESULT=Completed（無需重新開機）result=%ld" },
    [K_REQ_START] = { @"==== %@ request started (pid %d) ====", @"==== %@ 请求启动（pid %d）====", @"==== %@ 請求啟動（pid %d）====" },
    [K_UNLOAD] = { @"Uninstall", @"卸载", @"解除安裝" },
    [K_LOAD] = { @"Activate", @"激活", @"啟用" },
    [K_CANNOT_CREATE_REQ] = { @"Cannot create request", @"无法构造请求", @"無法建構請求" },
    [K_REQ_SUBMITTED] = { @"Request submitted, waiting for sysextd callback…", @"请求已提交，等待 sysextd 回调…", @"請求已送出，等待 sysextd 回呼…" },
    [K_TIMEOUT_LOG] = { @"TIMEOUT - no callback within 120 s", @"TIMEOUT 120 秒无回调", @"TIMEOUT 120 秒無回呼" },
    [K_OLD_INSTALLER] = { @"This installer is outdated (no host template). Please re-run from a new DMG.", @"这个安装器是旧版本（没有宿主模板）。请用新的 DMG 重新运行安装。", @"此安裝程式為舊版本（沒有宿主範本）。請用新的 DMG 重新執行安裝。" },
    [K_STAGE_COPY_FAIL] = { @"Failed to stage host copy (rc=%d): %@", @"复制宿主到暂存目录失败（rc=%d）：%@", @"複製宿主到暫存目錄失敗（rc=%d）：%@" },
    [K_HOST_STAGED] = { @"Host staged", @"宿主已复制到暂存目录", @"宿主程式已複製到暫存目錄" },
    [K_OLD_HOST_REMOVED] = { @"Old host could not be moved; removing it instead", @"旧宿主无法移动，改为直接移除", @"舊宿主無法移動，改為直接移除" },
    [K_OLD_HOST_BACKED_UP] = { @"Old host moved aside", @"旧宿主已移到备份位", @"舊宿主已移至備份位置" },
    [K_HOST_LAND_FAIL] = { @"Failed to place new host (/Applications not writable)", @"新宿主落位失败（/Applications 不可写）", @"新宿主放置失敗（/Applications 不可寫入）" },
    [K_HOST_IN_PLACE] = { @"Host deployed to /Applications/YT6801Driver.app", @"宿主已落位 /Applications/YT6801Driver.app", @"宿主程式已就位 /Applications/YT6801Driver.app" },
    [K_SIGN_VERIFY_FAIL] = { @"Signature check failed: %@", @"签名校验未通过：%@", @"簽章驗證未通過：%@" },
    [K_BUNDLE_ID_MISMATCH] = { @"dext bundle ID mismatch: %@", @"dext bundle id 不符：%@", @"dext bundle ID 不符：%@" },
    [K_UNREADABLE] = { @"(unreadable)", @"(读不到)", @"（無法讀取）" },
    [K_SIGN_BUNDLE_OK] = { @"Signature and bundle checks passed", @"签名与 bundle 校验通过", @"簽章與 bundle 驗證通過" },
    [K_REPLACING_HOST] = { @"Old host detected, replacing with the new one", @"检测到旧版宿主，正在替换为新版", @"偵測到舊版宿主，正在替換為新版" },
    [K_ACTIVATOR_MISSING] = { @"Installed activator not found (host version mismatch - click Reinstall)", @"找不到已安装的激活程序（宿主版本不匹配，请点「重装」）", @"找不到已安裝的啟用程式（宿主版本不符，請點「重裝」）" },
    [K_CANNOT_START_ACT] = { @"Cannot start activator: %@", @"无法启动激活程序：%@", @"無法啟動啟用程式：%@" },
    [K_REBOOT_NEEDED] = { @"Restart required (usually an old instance that has not exited)", @"需要重启才能生效（通常代表已有旧实例未退出）", @"需要重新開機才會生效（通常代表已有舊實例未退出）" },
    [K_ACTIVATION_DENIED] = { @"Activation rejected by sysextd", @"激活被 sysextd 拒绝", @"啟用被 sysextd 拒絕" },
    [K_ACTIVATION_TIMEOUT] = { @"Activation timed out (no result in 60 s)", @"激活超时（60 秒无结果）", @"啟用逾時（60 秒無結果）" },
    [K_ACTIVATOR_CRASHED] = { @"Activator exited abnormally", @"激活程序异常退出", @"啟用程式異常結束" },
    [K_STEP1_CHECK] = { @"1. Checking bundled driver files", @"① 检查内置驱动文件", @"① 檢查內建驅動程式檔案" },
    [K_DEXT_MISSING] = { @"Driver file missing from the package - please re-download", @"安装包内缺少驱动文件，请重新下载", @"安裝包內缺少驅動程式檔案，請重新下載" },
    [K_FMT_BUNDLED_VER] = { @"Bundled version %@", @"内置版本 %@", @"內建版本 %@" },
    [K_STEP2_DEPLOY] = { @"2. Deploying to /Applications", @"② 部署到 /Applications", @"② 部署到 /Applications" },
    [K_STEP3_VERIFY] = { @"3. Signature and integrity checks", @"③ 签名与完整性校验", @"③ 簽章與完整性驗證" },
    [K_STEP4_ACTIVATE] = { @"4. Submitting system extension activation", @"④ 提交系统扩展激活请求", @"④ 送出系統擴充功能啟用請求" },
    [K_ACTIVATE_NO_ENTRY] = { @"Activation returned success, but the extension is not in the system list", @"激活返回成功，但扩展列表里仍没有它", @"啟用回報成功，但擴充功能清單裡仍沒有它" },
    [K_FMT_LOADED_VER] = { @"System loaded version %@", @"系统已加载版本 %@", @"系統已載入版本 %@" },
    [K_HOST_HIDDEN] = { @"Host app icon hidden from Launchpad", @"已隐藏 /Applications 里的驱动宿主图标", @"已隱藏 /Applications 裡的驅動宿主圖示" },
    [K_HOST_HIDE_FAIL] = { @"Could not hide host app icon (harmless)", @"宿主图标隐藏失败（不影响使用）", @"宿主圖示隱藏失敗（不影響使用）" },
    [K_USTEP1] = { @"1. Checking current install state", @"① 检查当前安装状态", @"① 檢查目前安裝狀態" },
    [K_NOT_INSTALLED_ERR] = { @"No installed driver found", @"系统里没有已安装的驱动", @"系統裡沒有已安裝的驅動程式" },
    [K_FMT_CURRENT_VER] = { @"Current version %1$@ (%2$@)", @"当前版本 %1$@（%2$@）", @"目前版本 %1$@（%2$@）" },
    [K_UNKNOWN] = { @"unknown", @"未知", @"未知" },
    [K_USTEP2] = { @"2. Deploying host to /Applications (sysextd requirement)", @"② 部署宿主到 /Applications（sysextd 硬性要求）", @"② 部署宿主到 /Applications（sysextd 硬性要求）" },
    [K_USTEP3] = { @"3. Submitting uninstall request (admin password required)", @"③ 提交系统卸载请求（需要管理员密码）", @"③ 送出系統解除安裝請求（需要管理者密碼）" },
    [K_CANNOT_START_UNIN] = { @"Cannot start uninstaller: %@", @"无法启动卸载程序：%@", @"無法啟動解除安裝程式：%@" },
    [K_AUTH_TIMEOUT] = { @"Timed out waiting for admin authorization. Enter the password promptly when prompted.", @"等待管理员授权超时：请在密码框出现后尽快输入。", @"等待管理者授權逾時：請在密碼框出現後盡快輸入。" },
    [K_CANCELED] = { @"Uninstall cancelled.", @"已取消卸载。", @"已取消解除安裝。" },
    [K_UNINSTALL_FAIL_FMT] = { @"Uninstall failed: %@", @"卸载失败：%@", @"解除安裝失敗：%@" },
    [K_AUTH_DENIED] = { @"Admin authorization was not granted", @"管理员授权未通过", @"管理者授權未通過" },
    [K_UNINSTALL_INCOMPLETE] = { @"Uninstall incomplete: the extension is still in the system list. Existing driver files were kept.", @"卸载未完成：系统列表里仍有该扩展，已保留现有驱动文件。", @"解除安裝未完成：系統清單裡仍有該擴充功能，已保留現有驅動程式檔案。" },
    [K_SYS_CONFIRMED] = { @"Uninstall confirmed by the system", @"系统已确认卸载", @"系統已確認解除安裝" },
    [K_USTEP4] = { @"4. Cleaning up", @"④ 清理安装目录", @"④ 清理安裝目錄" },
    [K_FMT_REMOVED] = { @"Removed %@", @"已移除 %@", @"已移除 %@" },
};

static NSString *
S(NSInteger k)
{
    return kUiStrings[k][gLang];
}

/* ------------------------------------------------------------------ */
#pragma mark - Logging

static FILE * gLogFile = NULL;
static void (^gLogSink)(NSString *) = nil;

static NSString *
nowStr(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
    return [NSString stringWithUTF8String:buf];
}

static void
ILog(NSString * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString * msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);

    NSString * line = [NSString stringWithFormat:@"[%@] %@", nowStr(), msg];

    if (gLogFile) {
        fputs([[line stringByAppendingString:@"\n"] UTF8String], gLogFile);
        fflush(gLogFile);
    }
    fputs([[line stringByAppendingString:@"\n"] UTF8String], stderr);

    if (gLogSink) {
        void (^sink)(NSString *) = gLogSink;
        dispatch_async(dispatch_get_main_queue(), ^{ sink(line); });
    }
}

/* ------------------------------------------------------------------ */
#pragma mark - Command execution

/* Run a command, return exit code; out collects stdout+stderr (may be NULL) */
static int
runCmd(NSString * path, NSArray<NSString *> * args, NSString ** out)
{
    NSTask * t = [[NSTask alloc] init];
    t.launchPath = path;
    t.arguments  = args;

    NSPipe * pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    t.standardError  = pipe;

    @try {
        [t launch];
    } @catch (NSException * e) {
        if (out) *out = [NSString stringWithFormat:S(K_LAUNCH_FAIL), e.reason];
        return -1;
    }

    NSData * data = [[pipe fileHandleForReading] readDataToEndOfFile];
    [t waitUntilExit];

    if (out) {
        NSString * s = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        *out = s ?: @"";
    }
    return t.terminationStatus;
}

/* ------------------------------------------------------------------ */
#pragma mark - State detection

typedef struct {
    BOOL      installed;     /* present and activated in the system */
    BOOL      enabled;
    BOOL      terminating;   /* stuck in terminating for upgrade / uninstalling */
    BOOL      needsApproval; /* waiting for user approval */
    __unsafe_unretained NSString * version;
    __unsafe_unretained NSString * stateText;
} ExtState;

/* ARC forbids strong members in a struct, so the strings are held by these
 * two statics and the struct only stores unsafe pointers; skipping this
 * yields deallocated objects and SIGSEGV. */
static NSString * g_stVersion = @"";
static NSString * g_stState   = @"";

static ExtState
queryState(void)
{
    ExtState st;
    memset(&st, 0, sizeof(st));
    g_stVersion = @"";
    g_stState   = @"";
    st.version   = g_stVersion;
    st.stateText = g_stState;

    NSString * out = nil;
    runCmd(@"/usr/bin/systemextensionsctl", @[ @"list" ], &out);
    if (out.length == 0) {
        return st;
    }

    /* The same bundle id can appear on multiple lines: the active line plus
     * leftover lines stuck in [uninstalling] / [terminating] from old
     * versions. Only trust the "activated" line, otherwise a leftover line's
     * old version is taken as current. */
    NSString * activeVersion = nil;
    NSString * activeState   = nil;
    NSString * anyVersion    = nil;
    NSString * anyState      = nil;

    for (NSString * raw in [out componentsSeparatedByString:@"\n"]) {
        if ([raw rangeOfString:kExtID].location == NSNotFound) {
            continue;
        }
        /* Format: *\t*\t-\tio.miniyv.yt6801ethernet (1.4.6/1.4.6)\tyt6801\t[activated enabled] */
        BOOL starred = [raw hasPrefix:@"*"];
        NSString * line = raw;

        /* Version: segment before the slash inside the parentheses */
        NSString * ver = nil;
        NSRange lp = [line rangeOfString:@"("];
        NSRange rp = [line rangeOfString:@")"];
        if (lp.location != NSNotFound && rp.location != NSNotFound && rp.location > lp.location) {
            NSString * inner = [line substringWithRange:NSMakeRange(lp.location + 1,
                                                                    rp.location - lp.location - 1)];
            NSArray * parts = [inner componentsSeparatedByString:@"/"];
            if (parts.count > 0) {
                ver = [parts[0] stringByTrimmingCharactersInSet:
                       [NSCharacterSet whitespaceCharacterSet]];
            }
        }

        /* State: segment inside the square brackets */
        NSRange lb = [line rangeOfString:@"[" options:NSBackwardsSearch];
        NSRange rb = [line rangeOfString:@"]" options:NSBackwardsSearch];
        NSString * state = @"";
        if (lb.location != NSNotFound && rb.location > lb.location) {
            state = [line substringWithRange:NSMakeRange(lb.location + 1, rb.location - lb.location - 1)];
        }

        if (state.length) { anyState = state; }
        if (ver.length)   { anyVersion = ver; }

        if ([state containsString:@"terminating"] || [state containsString:@"uninstalling"]) {
            st.terminating = YES;
        }
        if ([state containsString:@"awaiting approval"]) {
            st.needsApproval = YES;
        }
        if ([state containsString:@"activated"] && [state containsString:@"enabled"]) {
            st.installed = YES;
            st.enabled   = starred;
            if (state.length) { activeState = state; }
            if (ver.length)   { activeVersion = ver; }
        }
    }

    if (activeState.length) {
        g_stState = activeState;
    } else if (anyState.length) {
        g_stState = anyState;
        st.installed = YES;   /* only leftover lines remain, still counts as present */
    }
    if (activeVersion.length) {
        g_stVersion = activeVersion;
    } else if (anyVersion.length) {
        g_stVersion = anyVersion;
    }
    st.version   = g_stVersion;
    st.stateText = g_stState;
    return st;
}

/* Version of the bundled dext (the copy in Resources) */
static NSString *
builtinVersion(void)
{
    return kBuiltinVersion;
}

/* Whether the target location already holds a usable install */
static BOOL
targetInstalledOnDisk(void)
{
    NSString * dext = [kAppPath stringByAppendingPathComponent:
                       @"Contents/Library/SystemExtensions/io.miniyv.yt6801ethernet.dext"];
    return [[NSFileManager defaultManager] fileExistsAtPath:dext];
}

/* ------------------------------------------------------------------ */
#pragma mark - Activation / deactivation (must run in the target app's binary)

@interface ExtRequestRunner : NSObject <OSSystemExtensionRequestDelegate>
@property (nonatomic, assign) BOOL deactivating;
@end

@implementation ExtRequestRunner

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                 actionForReplacingExtension:(OSSystemExtensionInfo *)existing
                               withExtension:(OSSystemExtensionInfo *)ext
{
    ILog(S(K_REPLACING_EXT),
         existing.bundleShortVersion ?: @"?", ext.bundleShortVersion ?: @"?");
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    ILog(S(K_NEED_APPROVAL_SYS));
    ILog(S(K_AFTER_APPROVAL));
}

- (void)request:(OSSystemExtensionRequest *)request
        didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        ILog(S(K_REBOOT_REQUIRED_LOG));
        exit(4);
    }
    ILog(S(K_COMPLETED_LOG), (long)result);
    exit(0);
}

- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error
{
    ILog(@"FAIL %@ (domain=%@ code=%ld)",
         error.localizedDescription, error.domain, (long)error.code);
    if (error.userInfo) {
        ILog(@"userInfo=%@", error.userInfo);
    }
    exit(1);
}

@end

static int
runExtensionRequest(BOOL deactivate, NSString * logPath)
{
    gLogFile = fopen(logPath.UTF8String, "a");
    if (gLogFile) {
        setvbuf(gLogFile, NULL, _IOLBF, 0);
    }

    ILog(S(K_REQ_START),
         deactivate ? S(K_UNLOAD) : S(K_LOAD), (int)getpid());

    ExtRequestRunner * runner = [ExtRequestRunner new];
    runner.deactivating = deactivate;

    OSSystemExtensionRequest * req =
        deactivate
          ? [OSSystemExtensionRequest deactivationRequestForExtension:kExtID
                                                                queue:dispatch_get_main_queue()]
          : [OSSystemExtensionRequest activationRequestForExtension:kExtID
                                                              queue:dispatch_get_main_queue()];
    if (req == nil) {
        ILog(S(K_CANNOT_CREATE_REQ));
        return 2;
    }
    req.delegate = runner;
    [[OSSystemExtensionManager sharedManager] submitRequest:req];
    ILog(S(K_REQ_SUBMITTED));

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120ull * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        ILog(S(K_TIMEOUT_LOG));
        exit(3);
    });

    dispatch_main();
    return 0;
}

/* ------------------------------------------------------------------ */
#pragma mark - Install / uninstall core (shared by GUI and CLI)

static void
appendDetail(NSString * s)
{
    if (gLogSink) {
        void (^sink)(NSString *) = gLogSink;
        dispatch_async(dispatch_get_main_queue(), ^{ sink(s); });
    }
}

/* Actual path of the "host template" bundled in the installer.
 *
 * The template dir name deliberately has no .app suffix (HostTemplate.apppayload):
 * LaunchServices registers any .app nested inside another app as a standalone
 * app, so a duplicate YT6801Driver appears in Applications/Launchpad next to
 * the one in /Applications. Older packages shipped the template as
 * YT6801Driver.app; keep that as fallback. */
static NSString *
hostTemplatePath(void)
{
    NSString * res = [[NSBundle mainBundle] resourcePath];
    NSString * np  = [res stringByAppendingPathComponent:@"HostTemplate.apppayload"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:np]) {
        return np;
    }
    return [res stringByAppendingPathComponent:@"YT6801Driver.app"];
}

/* Copy the embedded headless host template to /Applications.
 *
 * Why not copy the installer itself: the installer is a GUI tool that can be
 * deleted after install, while the driver needs a resident host app for
 * future upgrades/uninstalls. Merging both in one bundle invites users to
 * delete the "installer" along with the host. So copy the template — a
 * headless shell. Returns nil on success. */
static NSString *
deployHostFromTemplate(void)
{
    NSString * tmpl = hostTemplatePath();
    if (![[NSFileManager defaultManager] fileExistsAtPath:tmpl]) {
        /* Older installers used themselves as the host; no template here */
        return S(K_OLD_INSTALLER);
    }

    NSFileManager * fm = [NSFileManager defaultManager];
    NSString * staging = [kAppPath stringByAppendingString:@".new"];
    /* Backup name carries the pid to avoid colliding with a leftover .old
     * from a previous run (a collision makes moveItem fail outright) */
    NSString * backup  = [kAppPath stringByAppendingFormat:@".%d.old", (int)getpid()];

    runCmd(@"/bin/rm", @[ @"-rf", staging ], NULL);
    runCmd(@"/bin/rm", @[ @"-rf", backup ], NULL);

    NSString * out = nil;
    int rc = runCmd(@"/usr/bin/ditto",
                    @[ @"--rsrc", @"--extattr", @"--noqtn", tmpl, staging ], &out);
    if (rc != 0) {
        return [NSString stringWithFormat:S(K_STAGE_COPY_FAIL), rc, out ?: @""];
    }
    appendDetail(S(K_HOST_STAGED));

    /* Move the old host aside instead of deleting it, so a failed install
     * can be restored in place. If mv fails (permissions/cross-volume), fall
     * back to direct deletion — the host dir is only the install source; the
     * running driver copy lives in /Library/SystemExtensions and is unaffected. */
    if ([fm fileExistsAtPath:kAppPath]) {
        if (runCmd(@"/bin/mv", @[ kAppPath, backup ], NULL) != 0) {
            appendDetail(S(K_OLD_HOST_REMOVED));
            runCmd(@"/bin/rm", @[ @"-rf", kAppPath ], NULL);
        } else {
            appendDetail(S(K_OLD_HOST_BACKED_UP));
        }
    }

    if (runCmd(@"/bin/mv", @[ staging, kAppPath ], NULL) != 0) {
        /* Rollback */
        if ([fm fileExistsAtPath:backup]) {
            runCmd(@"/bin/mv", @[ backup, kAppPath ], NULL);
        }
        return S(K_HOST_LAND_FAIL);
    }
    appendDetail(S(K_HOST_IN_PLACE));

    /* The backup is pointless after a successful install; clean it up */
    runCmd(@"/bin/rm", @[ @"-rf", backup ], NULL);

    /* The host is a background program; set the hidden flag again (keep it
     * out of sight in Finder) */
    runCmd(@"/usr/bin/chflags", @[ @"hidden", kAppPath ], NULL);
    return nil;
}

/* Verify signature and bundle integrity */
static NSString *
verifyBundle(void)
{
    NSString * out = nil;
    int rc = runCmd(@"/usr/bin/codesign",
                    @[ @"--verify", @"--deep", @"--strict", kAppPath ], &out);
    if (rc != 0) {
        return [NSString stringWithFormat:S(K_SIGN_VERIFY_FAIL), out ?: @""];
    }

    NSString * plist = [kAppPath stringByAppendingPathComponent:
                        @"Contents/Library/SystemExtensions/io.miniyv.yt6801ethernet.dext/Contents/Info.plist"];
    out = nil;
    runCmd(@"/usr/libexec/PlistBuddy", @[ @"-c", @"Print :CFBundleIdentifier", plist ], &out);
    if (![[out stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
            isEqualToString:kExtID]) {
        return [NSString stringWithFormat:S(K_BUNDLE_ID_MISMATCH), out ?: S(K_UNREADABLE)];
    }
    appendDetail(S(K_SIGN_BUNDLE_OK));
    return nil;
}

/* Run the CLI of the host in /Applications to activate, tailing its log */
static NSString *
activateInstalled(NSString * logPath, double * progress, void (^tick)(double))
{
    [@"" writeToFile:logPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];

    NSString * bin = [kAppPath stringByAppendingPathComponent:@"Contents/MacOS/yt6801host"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:bin]) {
        /* Normally unreachable: performInstall deployed the host earlier.
         * But an old host may sit in /Applications (created by the legacy
         * install.sh, binary named yt6801_activate); instead of a confusing
         * mismatch error, redeploy the host and retry. */
        appendDetail(S(K_REPLACING_HOST));
        NSString * redeploy = deployHostFromTemplate();
        if (redeploy) {
            return redeploy;
        }
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:bin]) {
        return S(K_ACTIVATOR_MISSING);
    }

    NSTask * t = [[NSTask alloc] init];
    t.launchPath = bin;
    t.arguments  = @[ @"--activate", logPath, [NSString stringWithFormat:@"%ld", (long)gLang] ];
    @try {
        [t launch];
    } @catch (NSException * e) {
        return [NSString stringWithFormat:S(K_CANNOT_START_ACT), e.reason];
    }

    /* Wait up to 60 seconds; read the log to track progress */
    for (int i = 0; i < 600; i++) {
        usleep(100 * 1000);

        NSString * log = [NSString stringWithContentsOfFile:logPath
                                                   encoding:NSUTF8StringEncoding
                                                      error:NULL];
        if (log.length > 0) {
            NSArray * lines = [log componentsSeparatedByString:@"\n"];
            NSString * last = @"";
            for (NSString * l in lines) {
                if (l.length > 3) {
                    last = l;
                }
            }
            if (last.length && ![last hasPrefix:@"…"]) {
                appendDetail(last);
            }
            if ([log containsString:@"RESULT=Completed"] ||
                [log containsString:@"RESULT=WillCompleteAfterReboot"]) {
                [t waitUntilExit];
                if ([log containsString:@"RESULT=WillCompleteAfterReboot"]) {
                    return S(K_REBOOT_NEEDED);
                }
                *progress = 0.92;
                if (tick) tick(*progress);
                return nil;
            }
            if ([log containsString:@"FAIL "]) {
                NSString * msg = S(K_ACTIVATION_DENIED);
                for (NSString * l in lines) {
                    if ([l containsString:@"FAIL "]) { msg = l; break; }
                }
                [t waitUntilExit];
                return msg;
            }
            if ([log containsString:@"NEED_APPROVAL"]) {
                *progress = 0.75;
                if (tick) tick(*progress);
            }
        }
        if (![t isRunning]) {
            break;
        }
    }

    if ([t isRunning]) {
        [t terminate];
        return S(K_ACTIVATION_TIMEOUT);
    }
    return S(K_ACTIVATOR_CRASHED);
}

/* ------------------------------------------------------------------ */
#pragma mark - High-level install / uninstall

static NSString *
performInstall(double * progress, void (^tick)(double))
{
    if (tick) tick(0.05);
    appendDetail(S(K_STEP1_CHECK));

    /* The driver currently lives in the "host template":
     * <template>/Contents/Library/SystemExtensions/. Keep the legacy
     * (Contents/Resources/xxx.dext) check for older builds. */
    NSString * hostTmpl = hostTemplatePath();
    NSString * builtinDext = [hostTmpl stringByAppendingPathComponent:
                              [NSString stringWithFormat:
                               @"Contents/Library/SystemExtensions/%@.dext", kExtID]];
    NSString * altDext = [[[NSBundle mainBundle] resourcePath]
                          stringByAppendingPathComponent:
                          [kExtID stringByAppendingString:@".dext"]];

    if (![[NSFileManager defaultManager] fileExistsAtPath:builtinDext] &&
        ![[NSFileManager defaultManager] fileExistsAtPath:altDext]) {
        return S(K_DEXT_MISSING);
    }
    appendDetail([NSString stringWithFormat:S(K_FMT_BUNDLED_VER), builtinVersion()]);

    if (tick) tick(0.15);
    appendDetail(S(K_STEP2_DEPLOY));
    NSString * err = deployHostFromTemplate();
    if (err) {
        return err;
    }

    if (tick) tick(0.35);
    appendDetail(S(K_STEP3_VERIFY));
    err = verifyBundle();
    if (err) {
        return err;
    }

    if (tick) tick(0.50);
    appendDetail(S(K_STEP4_ACTIVATE));
    err = activateInstalled(kExtLogPath, progress, tick);
    if (err) {
        return err;
    }

    if (tick) tick(0.96);
    ExtState st = queryState();
    if (!st.installed) {
        return S(K_ACTIVATE_NO_ENTRY);
    }
    appendDetail([NSString stringWithFormat:S(K_FMT_LOADED_VER), st.version]);

    /* Hide the host from Finder/Launchpad after install.
     * The host app is a hard sysextd requirement (must be in /Applications),
     * but there is no reason to show it in Launchpad — it is not a program
     * users double-click. chflags hidden makes LaunchServices skip it in
     * Launchpad/app lists, while sysextd validates by path and is unaffected.
     * Failure is not fatal, so the return value is ignored. */
    {
        NSTask * h = [[NSTask alloc] init];
        h.launchPath = @"/usr/bin/chflags";
        h.arguments  = @[ @"hidden", kAppPath ];
        @try {
            [h launch];
            [h waitUntilExit];
            appendDetail(S(K_HOST_HIDDEN));
        } @catch (NSException * e) {
            appendDetail(S(K_HOST_HIDE_FAIL));
        }
    }

    if (tick) tick(1.0);
    return nil;
}

static NSString *
performUninstall(double * progress, void (^tick)(double))
{
    NSString * err = nil;

    if (tick) tick(0.10);
    appendDetail(S(K_USTEP1));

    ExtState st = queryState();
    if (!st.installed && !targetInstalledOnDisk()) {
        return S(K_NOT_INSTALLED_ERR);
    }
    appendDetail([NSString stringWithFormat:S(K_FMT_CURRENT_VER),
                  st.version.length ? st.version : S(K_UNKNOWN), st.stateText]);

    if (tick) tick(0.25);
    appendDetail(S(K_USTEP2));
    /* sysextd checks the requesting app's location for both activation and
     * deactivation: "App containing System Extension to be activated must be
     * in /Applications folder". So even an uninstall must be initiated from
     * the copy in /Applications. */
    err = deployHostFromTemplate();
    if (err) {
        return err;
    }

    if (tick) tick(0.32);
    appendDetail(S(K_USTEP3));

    /* OSSystemExtensionManager deactivation requests fail with UserRejected
     * (code=4) within seconds when submitted from a background (LSUIElement)
     * host, because the approval dialog has no UI context. osascript's
     * "with administrator privileges" also fails: systemextensionsctl runs
     * its own AuthorizationCreate afterwards, and the shell osascript spawns
     * has no interactive audit session (error 70). The working path is
     * AuthorizationExecuteWithPrivileges from this GUI process: the password
     * dialog is raised here, and the root child inherits this app's
     * interactive session, so both authorization layers succeed. */
    AuthorizationRef auth = NULL;
    OSStatus rc = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (rc != errAuthorizationSuccess) {
        return [NSString stringWithFormat:S(K_UNINSTALL_FAIL_FMT),
                [NSString stringWithFormat:@"%@ (err %d)", S(K_AUTH_DENIED), (int)rc]];
    }

    char extId[128] = "";
    [kExtID getCString:extId maxLength:sizeof(extId) encoding:NSUTF8StringEncoding];
    char * args[] = { (char *)"uninstall", (char *)"-", extId, NULL };
    FILE * fp = NULL;
    rc = AuthorizationExecuteWithPrivileges(auth, "/usr/bin/systemextensionsctl",
                                            kAuthorizationFlagDefaults, args, &fp);
    if (rc == errAuthorizationCanceled) {
        AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
        return S(K_CANCELED);
    }
    if (rc != errAuthorizationSuccess || fp == NULL) {
        AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
        return [NSString stringWithFormat:S(K_UNINSTALL_FAIL_FMT),
                [NSString stringWithFormat:@"%@ (err %d)", S(K_AUTH_DENIED), (int)rc]];
    }

    /* Drain the child's output; EOF means it has exited. Collect the text
     * so failures can be shown in the detail view. */
    NSMutableString * out = [NSMutableString string];
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        [out appendFormat:@"%.*s", (int)n, buf];
    }
    fclose(fp);
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    if (out.length) {
        appendDetail(out);
    }

    /* Double-check with the system before touching any files. sysextd
     * removes the record asynchronously, so poll for up to 15 s instead
     * of failing on the first read. */
    ExtState after = queryState();
    for (int i = 0; i < 15 && after.installed; i++) {
        sleep(1);
        after = queryState();
    }
    if (after.installed) {
        return S(K_UNINSTALL_INCOMPLETE);
    }
    appendDetail(S(K_SYS_CONFIRMED));

    if (tick) tick(0.75);
    appendDetail(S(K_USTEP4));

    /* Remove the dext from the host, keep the host itself (installer needs it) */
    NSString * seDir = [kAppPath stringByAppendingPathComponent:@"Contents/Library/SystemExtensions"];
    NSFileManager * fm = [NSFileManager defaultManager];
    for (NSString * name in [fm contentsOfDirectoryAtPath:seDir error:NULL]) {
        if ([name hasSuffix:@".dext"]) {
            [fm removeItemAtPath:[seDir stringByAppendingPathComponent:name] error:NULL];
            appendDetail([NSString stringWithFormat:S(K_FMT_REMOVED), name]);
        }
    }

    if (tick) tick(1.0);
    return nil;
}

/* ------------------------------------------------------------------ */
#pragma mark - CLI

static void
printStatus(void)
{
    ExtState st = queryState();
    printf("extension_id: %s\n", kExtID.UTF8String);
    printf("installed: %s\n", st.installed ? "yes" : "no");
    printf("active: %s\n", st.enabled ? "yes" : "no");
    printf("version: %s\n", st.version.length ? st.version.UTF8String : "-");
    printf("state: %s\n", st.stateText.length ? st.stateText.UTF8String : "-");
    printf("builtin_version: %s\n", builtinVersion().UTF8String);
    printf("app_on_disk: %s\n", targetInstalledOnDisk() ? "yes" : "no");
}

static int
cliInstall(void)
{
    __block double p = 0;
    NSString * err = performInstall(&p, ^(double v) {
        printf("progress: %.0f%%\n", v * 100);
        fflush(stdout);
    });
    if (err) {
        printf("ERROR: %s\n", err.UTF8String);
        return 1;
    }
    printf("OK\n");
    return 0;
}

static int
cliUninstall(void)
{
    __block double p = 0;
    NSString * err = performUninstall(&p, ^(double v) {
        printf("progress: %.0f%%\n", v * 100);
        fflush(stdout);
    });
    if (err) {
        printf("ERROR: %s\n", err.UTF8String);
        return 1;
    }
    printf("OK\n");
    return 0;
}


/* ------------------------------------------------------------------ */
#pragma mark - License text

/* Plain-language summary; the binding license (GPL-2.0-or-later) ships
 * with the source as the LICENSE file. */

static NSString *
licenseText(void)
{
    switch (gLang) {
    case 0: return
        @"YT6801 Ethernet Driver · License Agreement\n"
        @"Last updated: October 2026\n\n"
        @"1. Free and open source\n\n"
        @"This driver is completely free of charge. It is released under the\n"
        @"GNU GPL v2.0 (or later) open source license: you may use, study,\n"
        @"modify and redistribute it. If you distribute it — modified or not —\n"
        @"the same license applies and the source stays open. There are no\n"
        @"paid features, ads, subscriptions or telemetry.\n\n"
        @"2. What it installs\n\n"
        @"A DriverKit system extension that drives the Motorcomm YT6801\n"
        @"gigabit Ethernet controller (PCI ID 1f0a:6801).\n\n"
        @"• Deployed to: /Applications/YT6801Driver.app\n"
        @"• Runs in user space: does not modify the kernel or system files\n"
        @"• Remove anytime with the Uninstall button\n\n"
        @"macOS may ask you to approve the extension once in System Settings\n"
        @"> General > Login Items & Extensions > Driver Extensions. This is a\n"
        @"system requirement no installer can bypass. No restart is needed.\n\n"
        @"3. Privacy\n\n"
        @"This software works fully offline. It does not collect, store or\n"
        @"send any data. Network packets are processed locally by the driver.\n\n"
        @"4. No warranty\n\n"
        @"The software is provided \u201cas is\u201d, without warranty of any kind.\n"
        @"Using it on unsupported hardware (Hackintosh) carries risks such as\n"
        @"system instability or network interruption. The author is not liable\n"
        @"for any damage caused by the use of this software.\n\n"
        @"By checking the box below you confirm that you have read, understood\n"
        @"and agreed to all of the above.\n";
    case 1: return
        @"YT6801 有线网卡驱动 · 用户许可协议\n"
        @"最后更新：2026 年 10 月\n\n"
        @"一、免费且开源\n\n"
        @"本驱动完全免费。它以 GNU GPL v2.0（或更新版本）开源许可证发布：\n"
        @"你可以自由使用、学习、修改和再分发本软件。无论是否修改，再分发时\n"
        @"须继续采用同一许可证并保持开源。本软件不含任何付费功能、广告、\n"
        @"订阅或数据统计。\n\n"
        @"二、安装内容\n\n"
        @"一个 DriverKit 系统扩展，用于驱动 Motorcomm YT6801 千兆以太网\n"
        @"控制器（PCI ID 1f0a:6801）。\n\n"
        @"• 安装位置：/Applications/YT6801Driver.app\n"
        @"• 运行在用户态：不修改内核，不改动系统文件\n"
        @"• 随时可用「卸载」按钮移除\n\n"
        @"首次安装时，macOS 可能要求你在「系统设置 > 通用 > 登录项与扩展 >\n"
        @"驱动扩展」中手动批准一次。这是系统强制流程，任何安装器都无法绕过。\n"
        @"批准后无需重启。\n\n"
        @"三、隐私\n\n"
        @"本软件完全在本地工作，不联网、不收集、不存储、不外传任何数据。\n"
        @"网络数据包由驱动在本地处理。\n\n"
        @"四、免责声明\n\n"
        @"本软件按「原样」提供，不附带任何明示或暗示的担保。\n"
        @"在非官方支持的硬件（黑苹果 / Hackintosh）上使用可能存在系统不稳定、\n"
        @"网络中断等风险。作者不对因使用本软件造成的任何损失承担责任。\n\n"
        @"勾选下方方框即表示你已阅读、理解并同意上述全部条款。\n";
    default: return
        @"YT6801 有線網卡驅動程式 · 使用者授權協議\n"
        @"最後更新：2026 年 10 月\n\n"
        @"一、免費且開源\n\n"
        @"本驅動程式完全免費。它以 GNU GPL v2.0（或更新版本）開源授權釋出：\n"
        @"你可以自由使用、學習、修改與再散布本軟體。無論是否修改，再散布時\n"
        @"須繼續採用同一授權並保持開源。本軟體不含任何付費功能、廣告、\n"
        @"訂閱或資料統計。\n\n"
        @"二、安裝內容\n\n"
        @"一個 DriverKit 系統擴充功能，用於驅動 Motorcomm YT6801 Gigabit\n"
        @"乙太網路控制器（PCI ID 1f0a:6801）。\n\n"
        @"• 安裝位置：/Applications/YT6801Driver.app\n"
        @"• 執行於使用者態：不修改核心，不更動系統檔案\n"
        @"• 隨時可用「解除安裝」按鈕移除\n\n"
        @"首次安裝時，macOS 可能會要求你在「系統設定 > 一般 > 登入項目與\n"
        @"延伸功能 > 驅動程式延伸功能」中手動核准一次。這是系統強制流程，\n"
        @"任何安裝程式都無法繞過。核准後無需重新開機。\n\n"
        @"三、隱私\n\n"
        @"本軟體完全在本機運作，不連網、不收集、不儲存、不外傳任何資料。\n"
        @"網路封包由驅動程式在本機處理。\n\n"
        @"四、免責聲明\n\n"
        @"本軟體按「現狀」提供，不附帶任何明示或默示之擔保。\n"
        @"在非官方支援的硬體（黑蘋果 / Hackintosh）上使用，可能存在系統\n"
        @"不穩定、網路中斷等風險。作者不對因使用本軟體造成的任何損失承擔責任。\n\n"
        @"勾選下方方塊即表示你已閱讀、理解並同意上述全部條款。\n";
    }
}

/* ------------------------------------------------------------------ */
#pragma mark - GUI

@interface InstallerController : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow * window;
@property (nonatomic, strong) NSTextField * statusLabel;
@property (nonatomic, strong) NSTextField * versionLabel;
@property (nonatomic, strong) NSTextField * hintLabel;
@property (nonatomic, strong) NSProgressIndicator * progress;
@property (nonatomic, strong) NSTextField * percentLabel;
@property (nonatomic, strong) NSButton * primaryButton;
@property (nonatomic, strong) NSButton * secondaryButton;
@property (nonatomic, strong) NSButton * detailButton;
@property (nonatomic, strong) NSScrollView * detailScroll;
@property (nonatomic, strong) NSTextView * detailView;
@property (nonatomic, strong) NSView * dot;
@property (nonatomic, assign) BOOL busy;
@property (nonatomic, assign) BOOL detailShown;
@property (nonatomic, assign) BOOL licenseAccepted;
/* After install/uninstall: primary button becomes "Done" and exits cleanly (exit 0) */
@property (nonatomic, assign) BOOL finishedInstall;
@end

@implementation InstallerController

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    self.window = [[NSWindow alloc]
                   initWithContentRect:NSMakeRect(0, 0, 640, 430)
                             styleMask:(NSWindowStyleMaskTitled |
                                        NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskMiniaturizable)
                               backing:NSBackingStoreBuffered
                                 defer:NO];
    self.window.backgroundColor = [NSColor windowBackgroundColor];
    [self.window center];
    [self showLanguageStep];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return YES;
}

- (NSTextField *)labelWithFrame:(NSRect)f text:(NSString *)t size:(CGFloat)s bold:(BOOL)b
{
    NSTextField * l = [[NSTextField alloc] initWithFrame:f];
    l.stringValue = t;
    l.bezeled = NO;
    l.drawsBackground = NO;
    l.editable = NO;
    l.selectable = NO;
    l.font = b ? [NSFont systemFontOfSize:s weight:NSFontWeightSemibold]
               : [NSFont systemFontOfSize:s];
    return l;
}

- (NSButton *)buttonWithFrame:(NSRect)f title:(NSString *)t action:(SEL)a
{
    NSButton * b = [[NSButton alloc] initWithFrame:f];
    b.title = t;
    b.bezelStyle = NSBezelStyleRounded;
    b.target = self;
    b.action = a;
    return b;
}

/* ---- Step 1: language picker (all three languages shown together) ---- */
- (void)showLanguageStep
{
    [self.window setTitle:S(K_WINDOW_TITLE)];
    NSView * c = self.window.contentView;
    [c.subviews makeObjectsPerformSelector:@selector(removeFromSuperview)];

    NSTextField * title = [self labelWithFrame:NSMakeRect(40, 330, 560, 30)
                                         text:@"Select a language · 请选择语言 · 請選擇語言"
                                         size:20 bold:YES];
    title.alignment = NSTextAlignmentCenter;
    [c addSubview:title];

    NSTextField * sub = [self labelWithFrame:NSMakeRect(40, 300, 560, 20)
                                        text:@"YT6801 Ethernet Driver Installer · YT6801 有线网卡驱动安装器 · YT6801 有線網卡驅動程式安裝器"
                                        size:12 bold:NO];
    sub.alignment = NSTextAlignmentCenter;
    sub.textColor = [NSColor secondaryLabelColor];
    [c addSubview:sub];

    NSButton * en   = [self buttonWithFrame:NSMakeRect(70, 190, 150, 44) title:@"English"
                                     action:@selector(onLangEn:)];
    NSButton * hans = [self buttonWithFrame:NSMakeRect(245, 190, 150, 44) title:@"简体中文"
                                     action:@selector(onLangHans:)];
    NSButton * hant = [self buttonWithFrame:NSMakeRect(420, 190, 150, 44) title:@"繁體中文"
                                     action:@selector(onLangHant:)];
    for (NSButton * b in @[ en, hans, hant ]) {
        b.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
    }
    [c addSubview:en];
    [c addSubview:hans];
    [c addSubview:hant];
}

- (void)onLangEn:(NSButton *)sender   { gLang = 0; [self showLicenseStep]; }
- (void)onLangHans:(NSButton *)sender { gLang = 1; [self showLicenseStep]; }
- (void)onLangHant:(NSButton *)sender { gLang = 2; [self showLicenseStep]; }

/* ---- Step 2: license agreement, before any install UI exists ---- */
- (void)showLicenseStep
{
    [self.window setContentSize:NSMakeSize(660, 600)];
    [self.window setTitle:S(K_LICENSE_TITLE)];
    NSView * c = self.window.contentView;
    [c.subviews makeObjectsPerformSelector:@selector(removeFromSuperview)];
    CGFloat W = 660;

    NSTextField * t = [self labelWithFrame:NSMakeRect(24, 556, W - 48, 26)
                                      text:S(K_LICENSE_TITLE)
                                      size:18 bold:YES];
    [c addSubview:t];

    NSScrollView * sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(24, 110, W - 48, 436)];
    sv.borderType = NSBezelBorder;
    sv.hasVerticalScroller = YES;
    NSTextView * tv = [[NSTextView alloc] initWithFrame:sv.bounds];
    tv.editable = NO;
    tv.string = licenseText();
    tv.font = [NSFont systemFontOfSize:12];
    tv.textContainerInset = NSMakeSize(10, 10);
    sv.documentView = tv;
    [c addSubview:sv];

    NSButton * check = [[NSButton alloc] initWithFrame:NSMakeRect(24, 74, W - 48, 22)];
    [check setButtonType:NSButtonTypeSwitch];
    check.title = S(K_LICENSE_CHECK);
    check.target = self;
    check.action = @selector(onLicenseCheckChanged:);
    [c addSubview:check];

    NSButton * cancel = [self buttonWithFrame:NSMakeRect(W - 24 - 90 - 12 - 110, 24, 110, 32)
                                        title:S(K_BTN_CANCEL)
                                       action:@selector(onLicenseCancel)];
    [c addSubview:cancel];

    NSButton * agree = [self buttonWithFrame:NSMakeRect(W - 24 - 90, 24, 90, 32)
                                       title:S(K_BTN_AGREE)
                                      action:@selector(onLicenseAgree:)];
    agree.keyEquivalent = @"\r";
    agree.enabled = NO;
    agree.tag = 1002;
    [c addSubview:agree];
}

- (void)onLicenseCheckChanged:(NSButton *)sender
{
    NSView * c = self.window.contentView;
    NSButton * agree = [c viewWithTag:1002];
    agree.enabled = (sender.state == NSControlStateValueOn);
}

- (void)onLicenseCancel
{
    [NSApp terminate:nil];
}

- (void)onLicenseAgree:(NSButton *)sender
{
    self.licenseAccepted = YES;
    [self buildMainWindow];
    [self refreshState];
}

/* ---- Step 3: main window (reachable only after accepting) ---- */
- (void)buildMainWindow
{
    [self.window setContentSize:NSMakeSize(720, 620)];
    [self.window setTitle:S(K_WINDOW_TITLE)];
    NSView * c = self.window.contentView;
    [c.subviews makeObjectsPerformSelector:@selector(removeFromSuperview)];
    CGFloat W = 720;

    NSImageView * icon = [[NSImageView alloc] initWithFrame:NSMakeRect(40, 520, 64, 64)];
    icon.image = [NSImage imageWithSystemSymbolName:@"network"
                           accessibilityDescription:@"NIC"];
    icon.contentTintColor = [NSColor controlAccentColor];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [c addSubview:icon];

    NSTextField * title = [self labelWithFrame:NSMakeRect(120, 556, W - 160, 28)
                                          text:S(K_TITLE)
                                          size:22 bold:YES];
    [c addSubview:title];

    NSTextField * sub = [self labelWithFrame:NSMakeRect(120, 530, W - 160, 20)
                                        text:S(K_SUBTITLE)
                                        size:12 bold:NO];
    sub.textColor = [NSColor secondaryLabelColor];
    [c addSubview:sub];

    NSBox * card = [[NSBox alloc] initWithFrame:NSMakeRect(40, 430, W - 80, 78)];
    card.boxType = NSBoxCustom;
    card.borderWidth = 0;
    card.fillColor = [NSColor controlBackgroundColor];
    card.cornerRadius = 10;
    [c addSubview:card];

    self.dot = [[NSView alloc] initWithFrame:NSMakeRect(22, 46, 10, 10)];
    self.dot.wantsLayer = YES;
    self.dot.layer.cornerRadius = 5;
    self.dot.layer.backgroundColor = [NSColor systemGrayColor].CGColor;
    [card addSubview:self.dot];

    self.statusLabel = [self labelWithFrame:NSMakeRect(44, 40, W - 200, 22)
                                       text:S(K_CHECKING)
                                       size:15 bold:YES];
    [card addSubview:self.statusLabel];

    self.versionLabel = [self labelWithFrame:NSMakeRect(44, 14, W - 200, 18)
                                        text:@""
                                        size:12 bold:NO];
    self.versionLabel.textColor = [NSColor secondaryLabelColor];
    [card addSubview:self.versionLabel];

    self.hintLabel = [self labelWithFrame:NSMakeRect(40, 372, W - 80, 46)
                                     text:@""
                                     size:12 bold:NO];
    self.hintLabel.textColor = [NSColor secondaryLabelColor];
    self.hintLabel.maximumNumberOfLines = 3;
    self.hintLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [c addSubview:self.hintLabel];

    self.progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(40, 336, W - 130, 12)];
    self.progress.style = NSProgressIndicatorStyleBar;
    self.progress.indeterminate = NO;
    self.progress.minValue = 0;
    self.progress.maxValue = 100;
    self.progress.doubleValue = 0;
    self.progress.hidden = YES;
    [c addSubview:self.progress];

    self.percentLabel = [self labelWithFrame:NSMakeRect(W - 84, 332, 44, 16)
                                        text:@""
                                        size:11 bold:NO];
    self.percentLabel.alignment = NSTextAlignmentRight;
    self.percentLabel.textColor = [NSColor secondaryLabelColor];
    self.percentLabel.hidden = YES;
    [c addSubview:self.percentLabel];

    self.detailScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(40, 96, W - 80, 224)];
    self.detailScroll.borderType = NSBezelBorder;
    self.detailScroll.hasVerticalScroller = YES;
    self.detailScroll.autohidesScrollers = YES;
    self.detailScroll.hidden = YES;

    self.detailView = [[NSTextView alloc] initWithFrame:self.detailScroll.bounds];
    self.detailView.editable = NO;
    self.detailView.selectable = YES;
    self.detailView.drawsBackground = YES;
    self.detailView.backgroundColor = [NSColor textBackgroundColor];
    self.detailView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.detailView.textContainerInset = NSMakeSize(8, 8);
    self.detailScroll.documentView = self.detailView;
    [c addSubview:self.detailScroll];

    self.primaryButton = [self buttonWithFrame:NSMakeRect(W - 40 - 140, 40, 140, 34)
                                         title:S(K_BTN_INSTALL)
                                        action:@selector(onPrimary)];
    self.primaryButton.keyEquivalent = @"\r";
    [c addSubview:self.primaryButton];

    self.secondaryButton = [self buttonWithFrame:NSMakeRect(W - 40 - 140 - 12 - 110, 40, 110, 34)
                                           title:S(K_BTN_CANCEL)
                                          action:@selector(onSecondary)];
    [c addSubview:self.secondaryButton];

    self.detailButton = [self buttonWithFrame:NSMakeRect(40, 44, 130, 26)
                                        title:S(K_BTN_DETAILS)
                                       action:@selector(onToggleDetail)];
    self.detailButton.bezelStyle = NSBezelStyleInline;
    [c addSubview:self.detailButton];

    [self.window makeFirstResponder:self.primaryButton];
}

- (void)refreshState
{
    if (self.finishedInstall) {
        /* Already on the "Done" screen; don't revert the buttons */
        return;
    }

    ExtState st = queryState();
    BOOL onDisk = targetInstalledOnDisk();

    if (self.busy) {
        return;
    }

    if (st.installed && st.enabled) {
        BOOL sameVersion = [st.version isEqualToString:builtinVersion()];
        self.statusLabel.stringValue = S(K_ST_INSTALLED);
        self.versionLabel.stringValue =
            [NSString stringWithFormat:S(K_FMT_VERSION),
             st.version.length ? st.version : @"?", builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemGreenColor].CGColor;
        self.hintLabel.stringValue = sameVersion ? S(K_HINT_UP_TO_DATE) : S(K_HINT_DIFF_VERSION);
        self.primaryButton.title = S(K_BTN_REINSTALL);
        self.secondaryButton.title = S(K_BTN_UNINSTALL);
        self.secondaryButton.hidden = NO;
    } else if (st.installed && !st.enabled) {
        self.statusLabel.stringValue = st.terminating ? S(K_ST_TERMINATING) : S(K_ST_INACTIVE);
        self.versionLabel.stringValue =
            [NSString stringWithFormat:S(K_FMT_STATE_LINE),
             st.version ?: @"", st.stateText ?: @""];
        self.dot.layer.backgroundColor = [NSColor systemOrangeColor].CGColor;
        self.hintLabel.stringValue = S(K_HINT_INACTIVE);
        self.primaryButton.title = S(K_BTN_REINSTALL);
        self.secondaryButton.title = S(K_BTN_UNINSTALL);
        self.secondaryButton.hidden = NO;
    } else if (onDisk) {
        /* Driver present on disk but not activated. Reinstall re-submits
         * activation; uninstall works regardless of version match. */
        self.statusLabel.stringValue = S(K_ST_ONDISK);
        self.versionLabel.stringValue =
            [NSString stringWithFormat:S(K_FMT_ONDISK), builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemOrangeColor].CGColor;
        self.hintLabel.stringValue = S(K_HINT_ONDISK);
        self.primaryButton.title = S(K_BTN_REINSTALL);
        self.secondaryButton.title = S(K_BTN_UNINSTALL);
        self.secondaryButton.hidden = NO;
    } else {
        self.statusLabel.stringValue = S(K_ST_NOTINSTALLED);
        self.versionLabel.stringValue =
            [NSString stringWithFormat:S(K_FMT_BUNDLED), builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemGrayColor].CGColor;
        self.hintLabel.stringValue = S(K_HINT_FRESH);
        self.primaryButton.title = S(K_BTN_INSTALL);
        self.secondaryButton.title = S(K_BTN_CANCEL);
        self.secondaryButton.hidden = NO;
    }
}

- (void)onToggleDetail
{
    self.detailShown = !self.detailShown;
    self.detailScroll.hidden = !self.detailShown;
    self.detailButton.title = self.detailShown ? S(K_BTN_HIDE_DETAILS) : S(K_BTN_DETAILS);
    if (self.detailShown) {
        [self appendLog:S(K_LOG_DETAIL_OPEN)];
    }
}

- (void)appendLog:(NSString *)line
{
    if (line.length == 0) {
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAttributedString * s =
            [[NSAttributedString alloc] initWithString:[line stringByAppendingString:@"\n"]
                                            attributes:@{ NSFontAttributeName:
                                                          [NSFont monospacedSystemFontOfSize:11
                                                                                      weight:NSFontWeightRegular],
                                                          NSForegroundColorAttributeName:
                                                              [NSColor labelColor] }];
        [[self.detailView textStorage] appendAttributedString:s];
        [self.detailView scrollRangeToVisible:NSMakeRange(self.detailView.string.length, 0)];
    });
}

- (void)setProgressValue:(double)v
{
    dispatch_async(dispatch_get_main_queue(), ^{
        self.progress.doubleValue = v * 100.0;
        self.percentLabel.stringValue = [NSString stringWithFormat:@"%.0f%%", v * 100.0];
    });
}

- (void)setBusy:(BOOL)busy
{
    /* Must assign _busy directly: assigning self.busy here calls this setter
     * again and recurses until the thread stack overflows. */
    _busy = busy;
    self.primaryButton.enabled = !busy;
    self.secondaryButton.enabled = !busy;
    self.progress.hidden = !busy;
    self.percentLabel.hidden = !busy;
    if (busy) {
        self.progress.doubleValue = 0;
        self.percentLabel.stringValue = @"0%";
    }
}

/* Post-install/uninstall screen: primary button becomes "Done" and exits
 * cleanly (exit 0). Otherwise users see "Reinstall/Uninstall" after a
 * finished install and may trigger it a second time. */
- (void)showCompleted:(NSString *)title
{
    self.finishedInstall = YES;
    self.primaryButton.title = S(K_BTN_DONE);
    self.primaryButton.enabled = YES;
    self.secondaryButton.hidden = YES;
    self.progress.hidden = YES;
    self.percentLabel.hidden = YES;
    self.dot.layer.backgroundColor = [NSColor systemGreenColor].CGColor;
    self.statusLabel.stringValue = title;
    self.hintLabel.hidden = NO;
    [self.window makeFirstResponder:self.primaryButton];
}

/* ---- Primary button ---- */
- (void)onPrimary
{
    /* After completion this button is "Done": exit cleanly */
    if (self.finishedInstall) {
        [NSApp terminate:nil];   // exit 0
        return;
    }
    if (self.busy) {
        return;
    }
    /* The license was accepted before this screen was ever shown. */
    [self runInstall];
}

- (void)onSecondary
{
    if (self.busy) {
        return;
    }
    ExtState st = queryState();
    if (st.installed || targetInstalledOnDisk()) {
        [self confirmUninstall];
    } else {
        [NSApp terminate:nil];
    }
}

- (void)confirmUninstall
{
    NSAlert * a = [[NSAlert alloc] init];
    a.messageText = S(K_CONFIRM_UNINSTALL_TITLE);
    a.informativeText = S(K_CONFIRM_UNINSTALL_INFO);
    [a addButtonWithTitle:S(K_BTN_UNINSTALL)];
    [a addButtonWithTitle:S(K_BTN_CANCEL)];
    a.alertStyle = NSAlertStyleWarning;
    if ([a runModal] != NSAlertFirstButtonReturn) {
        return;
    }
    [self runUninstall];
}

/* ---- Run ---- */
- (void)runInstall
{
    [self setBusy:YES];
    self.statusLabel.stringValue = S(K_INSTALLING);
    self.dot.layer.backgroundColor = [NSColor controlAccentColor].CGColor;

    __weak typeof(self) weakSelf = self;
    gLogSink = ^(NSString * line) {
        [weakSelf appendLog:line];
    };

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        __block double p = 0;
        NSString * err = performInstall(&p, ^(double v) {
            [weakSelf setProgressValue:v];
        });

        dispatch_async(dispatch_get_main_queue(), ^{
            gLogSink = nil;
            [weakSelf setBusy:NO];
            if (err) {
                weakSelf.statusLabel.stringValue = S(K_FAIL_INSTALL);
                weakSelf.versionLabel.stringValue = err;
                weakSelf.dot.layer.backgroundColor = [NSColor systemRedColor].CGColor;
                [weakSelf appendLog:[@"✗ " stringByAppendingString:err]];
                NSAlert * a = [[NSAlert alloc] init];
                a.messageText = S(K_FAIL_INSTALL);
                a.informativeText = err;
                [a addButtonWithTitle:S(K_OK)];
                [a runModal];
            } else {
                [weakSelf refreshState];
                weakSelf.hintLabel.stringValue = S(K_HINT_INSTALLED_OK);
                [weakSelf appendLog:S(K_LOG_INSTALLED)];
                [weakSelf showCompleted:S(K_DONE_INSTALL)];
            }
        });
    });
}

- (void)runUninstall
{
    [self setBusy:YES];
    self.statusLabel.stringValue = S(K_UNINSTALLING);
    self.dot.layer.backgroundColor = [NSColor systemOrangeColor].CGColor;

    __weak typeof(self) weakSelf = self;
    gLogSink = ^(NSString * line) {
        [weakSelf appendLog:line];
    };

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        __block double p = 0;
        NSString * err = performUninstall(&p, ^(double v) {
            [weakSelf setProgressValue:v];
        });

        dispatch_async(dispatch_get_main_queue(), ^{
            gLogSink = nil;
            [weakSelf setBusy:NO];
            if (err) {
                weakSelf.statusLabel.stringValue = S(K_FAIL_UNINSTALL);
                weakSelf.hintLabel.stringValue = err;
                weakSelf.dot.layer.backgroundColor = [NSColor systemRedColor].CGColor;
                [weakSelf appendLog:[@"✗ " stringByAppendingString:err]];
                if (!weakSelf.detailShown) {
                    [weakSelf onToggleDetail];
                }
                [weakSelf refreshState];

                NSAlert * a = [[NSAlert alloc] init];
                a.messageText = S(K_FAIL_UNINSTALL);
                a.informativeText = err;
                [a addButtonWithTitle:S(K_OK)];
                [a runModal];
            } else {
                [weakSelf refreshState];
                weakSelf.hintLabel.stringValue = S(K_HINT_UNINSTALLED_OK);
                [weakSelf appendLog:S(K_LOG_UNINSTALLED)];
                [weakSelf showCompleted:S(K_DONE_UNINSTALL)];
            }
        });
    });
}

@end

/* ------------------------------------------------------------------ */
#pragma mark - main

int
main(int argc, const char * argv[])
{
    @autoreleasepool {
        if (argc > 1) {
            NSString * mode = [NSString stringWithUTF8String:argv[1]];

            if ([mode isEqualToString:@"--activate"]) {
                NSString * p = (argc > 2) ? [NSString stringWithUTF8String:argv[2]] : kExtLogPath;
                return runExtensionRequest(NO, p);
            }
            if ([mode isEqualToString:@"--deactivate"]) {
                NSString * p = (argc > 2) ? [NSString stringWithUTF8String:argv[2]] : kExtLogPath;
                return runExtensionRequest(YES, p);
            }
            if ([mode isEqualToString:@"--status"]) {
                printStatus();
                return queryState().installed ? 0 : 1;
            }
            if ([mode isEqualToString:@"--install"]) {
                return cliInstall();
            }
            if ([mode isEqualToString:@"--uninstall"]) {
                return cliUninstall();
            }

            fprintf(stderr,
                    "YT6801 Installer\n"
                    "  --status      print state\n"
                    "  --install     silent install\n"
                    "  --uninstall   silent uninstall\n"
                    "  --activate    activate only\n"
                    "  --deactivate  deactivate only\n"
                    "(no arguments starts the GUI)\n");
            return 2;
        }

        NSApplication * app = [NSApplication sharedApplication];

        /* Single instance guard: a second copy cannot drive the same system
         * extension state and would race the first one's install/uninstall. */
        for (NSRunningApplication * a in [NSRunningApplication
                runningApplicationsWithBundleIdentifier:kInstallerID]) {
            if (a.processIdentifier != getpid()) {
                NSAlert * dup = [[NSAlert alloc] init];
                dup.alertStyle = NSAlertStyleWarning;
                dup.messageText = @"YT6801 Installer is already running.";
                dup.informativeText =
                    @"Please use the window that is already open. "
                    @"Running two copies at once could corrupt the driver state.";
                [dup addButtonWithTitle:@"OK"];
                [dup runModal];
                return 1;
            }
        }

        InstallerController * ctl = [[InstallerController alloc] init];
        app.delegate = ctl;

        NSMenu * mb = [[NSMenu alloc] init];
        NSMenuItem * appItem = [[NSMenuItem alloc] init];
        [mb addItem:appItem];
        NSMenu * sub = [[NSMenu alloc] init];
        [sub addItemWithTitle:@"About YT6801 Installer"
                       action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [sub addItem:[NSMenuItem separatorItem]];
        [sub addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = sub;
        app.mainMenu = mb;

        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
        (void)ctl;
    }

    /* Launched from a DMG: forget this copy in LaunchServices, or every
     * mount leaves a ghost entry in Launchpad. Only the DMG copy is
     * unregistered — the deployed host in /Applications must stay known to
     * LaunchServices, sysextd locates it through it. */
    {
        NSString * selfPath = [[NSBundle mainBundle] bundlePath];
        if ([selfPath hasPrefix:@"/Volumes/"]) {
            NSTask * u = [[NSTask alloc] init];
            u.launchPath = @LSREG_PATH;
            u.arguments  = @[ @"-u", selfPath ];
            u.standardOutput = [NSPipe pipe];
            u.standardError  = [NSPipe pipe];
            @try {
                [u launch];
                [u waitUntilExit];
            } @catch (NSException * e) {
                /* cosmetic cleanup only, never fatal */
            }
        }
    }
    return 0;
}
