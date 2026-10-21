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

static NSString * const kBuiltinVersion = @YT_BUILTIN_VERSION;

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
        if (out) *out = [NSString stringWithFormat:@"启动失败：%@", e.reason];
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
    ILog(@"检测到同 ID 扩展：旧 %@ → 新 %@，选择替换",
         existing.bundleShortVersion ?: @"?", ext.bundleShortVersion ?: @"?");
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    ILog(@"NEED_APPROVAL 需要用户在「系统设置 > 通用 > 登录项与扩展」里批准");
    ILog(@"提示：批准后无需重启，本程序会自动继续。");
}

- (void)request:(OSSystemExtensionRequest *)request
        didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        ILog(@"RESULT=WillCompleteAfterReboot（这一步才需要重启）");
        exit(4);
    }
    ILog(@"RESULT=Completed（无需重启）result=%ld", (long)result);
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

    ILog(@"==== %@ 请求启动（pid %d）====",
         deactivate ? @"卸载" : @"激活", (int)getpid());

    ExtRequestRunner * runner = [ExtRequestRunner new];
    runner.deactivating = deactivate;

    OSSystemExtensionRequest * req =
        deactivate
          ? [OSSystemExtensionRequest deactivationRequestForExtension:kExtID
                                                                queue:dispatch_get_main_queue()]
          : [OSSystemExtensionRequest activationRequestForExtension:kExtID
                                                              queue:dispatch_get_main_queue()];
    if (req == nil) {
        ILog(@"无法构造请求");
        return 2;
    }
    req.delegate = runner;
    [[OSSystemExtensionManager sharedManager] submitRequest:req];
    ILog(@"请求已提交，等待 sysextd 回调…");

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120ull * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        ILog(@"TIMEOUT 120 秒无回调");
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
        return @"这个安装器是旧版本（没有宿主模板）。请用新的 DMG 重新运行安装。";
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
        return [NSString stringWithFormat:@"复制宿主到暂存目录失败（rc=%d）：%@", rc, out ?: @""];
    }
    appendDetail(@"宿主已复制到暂存目录");

    /* Move the old host aside instead of deleting it, so a failed install
     * can be restored in place. If mv fails (permissions/cross-volume), fall
     * back to direct deletion — the host dir is only the install source; the
     * running driver copy lives in /Library/SystemExtensions and is unaffected. */
    if ([fm fileExistsAtPath:kAppPath]) {
        if (runCmd(@"/bin/mv", @[ kAppPath, backup ], NULL) != 0) {
            appendDetail(@"旧宿主无法移动，改为直接移除");
            runCmd(@"/bin/rm", @[ @"-rf", kAppPath ], NULL);
        } else {
            appendDetail(@"旧宿主已移到备份位");
        }
    }

    if (runCmd(@"/bin/mv", @[ staging, kAppPath ], NULL) != 0) {
        /* Rollback */
        if ([fm fileExistsAtPath:backup]) {
            runCmd(@"/bin/mv", @[ backup, kAppPath ], NULL);
        }
        return @"新宿主落位失败（/Applications 不可写）";
    }
    appendDetail(@"宿主已落位 /Applications/YT6801Driver.app");

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
        return [NSString stringWithFormat:@"签名校验未通过：%@", out ?: @""];
    }

    NSString * plist = [kAppPath stringByAppendingPathComponent:
                        @"Contents/Library/SystemExtensions/io.miniyv.yt6801ethernet.dext/Contents/Info.plist"];
    out = nil;
    runCmd(@"/usr/libexec/PlistBuddy", @[ @"-c", @"Print :CFBundleIdentifier", plist ], &out);
    if (![[out stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
            isEqualToString:kExtID]) {
        return [NSString stringWithFormat:@"dext bundle id 不符：%@", out ?: @"(读不到)"];
    }
    appendDetail(@"签名与 bundle 校验通过");
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
        appendDetail(@"检测到旧版宿主，正在替换为新版");
        NSString * redeploy = deployHostFromTemplate();
        if (redeploy) {
            return redeploy;
        }
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:bin]) {
        return @"找不到已安装的激活程序（宿主版本不匹配，请点「重装」）";
    }

    NSTask * t = [[NSTask alloc] init];
    t.launchPath = bin;
    t.arguments  = @[ @"--activate", logPath ];
    @try {
        [t launch];
    } @catch (NSException * e) {
        return [NSString stringWithFormat:@"无法启动激活程序：%@", e.reason];
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
                    return @"需要重启才能生效（通常代表已有旧实例未退出）";
                }
                *progress = 0.92;
                if (tick) tick(*progress);
                return nil;
            }
            if ([log containsString:@"FAIL "]) {
                NSString * msg = @"激活被 sysextd 拒绝";
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
        return @"激活超时（60 秒无结果）";
    }
    return @"激活程序异常退出";
}

/* ------------------------------------------------------------------ */
#pragma mark - High-level install / uninstall

static NSString *
performInstall(double * progress, void (^tick)(double))
{
    if (tick) tick(0.05);
    appendDetail(@"① 检查内置驱动文件");

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
        return @"安装包内缺少驱动文件，请重新下载";
    }
    appendDetail([NSString stringWithFormat:@"内置版本 %@", builtinVersion()]);

    if (tick) tick(0.15);
    appendDetail(@"② 部署到 /Applications");
    NSString * err = deployHostFromTemplate();
    if (err) {
        return err;
    }

    if (tick) tick(0.35);
    appendDetail(@"③ 签名与完整性校验");
    err = verifyBundle();
    if (err) {
        return err;
    }

    if (tick) tick(0.50);
    appendDetail(@"④ 提交系统扩展激活请求");
    err = activateInstalled(kExtLogPath, progress, tick);
    if (err) {
        return err;
    }

    if (tick) tick(0.96);
    ExtState st = queryState();
    if (!st.installed) {
        return @"激活返回成功，但扩展列表里仍没有它";
    }
    appendDetail([NSString stringWithFormat:@"系统已加载版本 %@", st.version]);

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
            appendDetail(@"已隐藏 /Applications 里的驱动宿主图标");
        } @catch (NSException * e) {
            appendDetail(@"宿主图标隐藏失败（不影响使用）");
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
    appendDetail(@"① 检查当前安装状态");

    ExtState st = queryState();
    if (!st.installed && !targetInstalledOnDisk()) {
        return @"系统里没有已安装的驱动";
    }
    appendDetail([NSString stringWithFormat:@"当前版本 %@（%@）",
                  st.version.length ? st.version : @"未知", st.stateText]);

    if (tick) tick(0.25);
    appendDetail(@"② 部署宿主到 /Applications（sysextd 硬性要求）");
    /* sysextd checks the requesting app's location for both activation and
     * deactivation: "App containing System Extension to be activated must be
     * in /Applications folder". So even an uninstall must be initiated from
     * the copy in /Applications. */
    err = deployHostFromTemplate();
    if (err) {
        return err;
    }

    if (tick) tick(0.32);
    appendDetail(@"③ 向 sysextd 提交卸载请求");

    [@"" writeToFile:kExtLogPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    NSString * bin = [kAppPath stringByAppendingPathComponent:@"Contents/MacOS/yt6801host"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:bin]) {
        /* deployHostFromTemplate above already succeeded; still missing means
         * the template inside the installer package is incomplete */
        return @"宿主程序缺失（安装包不完整），请重新下载安装器";
    }

    NSTask * t = [[NSTask alloc] init];
    t.launchPath = bin;
    t.arguments  = @[ @"--deactivate", kExtLogPath ];
    @try {
        [t launch];
    } @catch (NSException * e) {
        return [NSString stringWithFormat:@"无法启动卸载程序：%@", e.reason];
    }

    BOOL completed = NO;
    for (int i = 0; i < 300; i++) {
        usleep(100 * 1000);
        NSString * log = [NSString stringWithContentsOfFile:kExtLogPath
                                                   encoding:NSUTF8StringEncoding
                                                      error:NULL];
        if (log.length) {
            if ([log containsString:@"RESULT=Completed"]) {
                completed = YES;
                [t waitUntilExit];
                break;
            }
            if ([log containsString:@"FAIL "]) {
                [t waitUntilExit];
                return @"系统拒绝了卸载：需要你在授权弹窗里点「允许」。";
            }
        }
        if (![t isRunning]) {
            break;
        }
    }
    if ([t isRunning]) {
        [t terminate];
        return @"等待系统授权超时：卸载需要你在弹窗中点「允许」。\n"
                "如果没有弹窗出现，请改用「重装」，或手动在"
                @"「系统设置 > 通用 > 登录项与扩展」里移除。";
    }

    /* Child process exit != uninstall success. Without an explicit Completed
     * in the log, treat it as failure and never touch the dext inside the
     * host — otherwise a still-active driver file could be half-deleted. */
    if (!completed) {
        NSString * log = [NSString stringWithContentsOfFile:kExtLogPath
                                                   encoding:NSUTF8StringEncoding
                                                      error:NULL];
        if (![log containsString:@"RESULT=Completed"]) {
            return @"卸载未完成：系统没有确认卸载成功，已保留现有驱动文件。";
        }
    }

    if (tick) tick(0.75);
    appendDetail(@"④ 清理安装目录");

    /* Remove the dext from the host, keep the host itself (installer needs it) */
    NSString * seDir = [kAppPath stringByAppendingPathComponent:@"Contents/Library/SystemExtensions"];
    NSFileManager * fm = [NSFileManager defaultManager];
    for (NSString * name in [fm contentsOfDirectoryAtPath:seDir error:NULL]) {
        if ([name hasSuffix:@".dext"]) {
            [fm removeItemAtPath:[seDir stringByAppendingPathComponent:name] error:NULL];
            appendDetail([NSString stringWithFormat:@"已移除 %@", name]);
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

static NSString *
licenseText(void)
{
    return @"YT6801 有线网卡驱动 · 用户许可协议\n"
           @"最后更新：2026 年 10 月\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           @"一、完全免费\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"本软件及其全部源代码完全免费，遵循 MIT 许可证发布。\n"
           @"任何人都可以自由使用、修改、再分发，包括商业用途，无需支付任何费用，"
           @"也无需署名（但保留原始版权声明是礼貌）。\n\n"
           @"本软件不含任何付费功能、订阅、内购、广告或功能限制。\n"
           @"如果你是通过付费渠道获得本软件的，说明你被中间商赚了差价。\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           @"二、MIT 许可证全文\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"Copyright (c) 2026 YT6801 Driver Contributors\n\n"
           @"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
           @"of this software and associated documentation files (the \"Software\"), to deal\n"
           @"in the Software without restriction, including without limitation the rights\n"
           @"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
           @"copies of the Software, and to permit persons to whom the Software is\n"
           @"furnished to do so, subject to the following conditions:\n\n"
           @"The above copyright notice and this permission notice shall be included in all\n"
           @"copies or substantial portions of the Software.\n\n"
           @"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
           @"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
           @"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
           @"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
           @"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
           @"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
           @"SOFTWARE.\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           @"三、系统扩展说明\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"本软件包含一个 DriverKit 系统扩展（DEXT），用于驱动 Motorcomm YT6801\n"
           @"千兆以太网控制器（PCI ID 1f0a:6801）。\n\n"
           @"• 安装位置：/Applications/YT6801Driver.app\n"
           @"• 驱动类型：DriverKit 用户态驱动，不注入内核，不修改系统文件\n"
           @"• 卸载方式：本安装器的「卸载」按钮\n\n"
           @"首次安装时，macOS 可能要求你在「系统设置 > 通用 > 登录项与扩展 >\n"
           @"驱动扩展」中手动批准一次。这是系统强制流程，任何第三方安装器都无法绕过。\n"
           @"批准后无需重启。\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           @"四、隐私\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"本软件不联网、不收集任何数据、不含遥测与统计上报。\n"
           @"驱动在本地处理网络数据包，不会读取、复制或外传你的任何内容。\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
           @"五、免责声明\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"本软件按「原样」提供，不附带任何明示或暗示的担保。\n"
           @"在非官方硬件（黑苹果 / Hackintosh）环境下使用本软件可能存在未知风险，\n"
           @"包括但不限于：系统不稳定、网络中断、需要手动恢复。\n\n"
           @"作者不对因使用本软件造成的任何直接或间接损失承担责任。\n"
           @"请在充分了解风险的前提下自行决定是否安装。\n\n"
           @"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
           @"勾选下方方框即表示你已阅读、理解并同意上述全部条款。\n";
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
    [self buildWindow];
    [self refreshState];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return YES;
}

- (void)buildWindow
{
    NSRect frame = NSMakeRect(0, 0, 720, 620);
    self.window = [[NSWindow alloc]
                   initWithContentRect:frame
                             styleMask:(NSWindowStyleMaskTitled |
                                        NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskMiniaturizable)
                               backing:NSBackingStoreBuffered
                                 defer:NO];
    self.window.title = @"YT6801 安装器";
    self.window.backgroundColor = [NSColor windowBackgroundColor];
    [self.window center];

    NSView * c = self.window.contentView;
    CGFloat W = 720;

    /* Icon */
    NSImageView * icon = [[NSImageView alloc] initWithFrame:NSMakeRect(40, 520, 64, 64)];
    icon.image = [NSImage imageWithSystemSymbolName:@"network"
                           accessibilityDescription:@"网卡"];
    icon.contentTintColor = [NSColor controlAccentColor];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [c addSubview:icon];

    /* Title */
    NSTextField * title = [self labelWithFrame:NSMakeRect(120, 556, W - 160, 28)
                                          text:@"YT6801 有线网卡驱动"
                                          size:22 bold:YES];
    [c addSubview:title];

    NSTextField * sub = [self labelWithFrame:NSMakeRect(120, 530, W - 160, 20)
                                        text:@"Motorcomm YT6801 千兆以太网 · DriverKit 系统扩展"
                                        size:12 bold:NO];
    sub.textColor = [NSColor secondaryLabelColor];
    [c addSubview:sub];

    /* Status card */
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
                                       text:@"正在检测…"
                                       size:15 bold:YES];
    [card addSubview:self.statusLabel];

    self.versionLabel = [self labelWithFrame:NSMakeRect(44, 14, W - 200, 18)
                                        text:@""
                                        size:12 bold:NO];
    self.versionLabel.textColor = [NSColor secondaryLabelColor];
    [card addSubview:self.versionLabel];

    /* Hint */
    self.hintLabel = [self labelWithFrame:NSMakeRect(40, 372, W - 80, 46)
                                     text:@""
                                     size:12 bold:NO];
    self.hintLabel.textColor = [NSColor secondaryLabelColor];
    self.hintLabel.maximumNumberOfLines = 3;
    self.hintLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [c addSubview:self.hintLabel];

    /* Progress bar */
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

    /* Detail log */
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

    /* Buttons */
    self.primaryButton = [self buttonWithFrame:NSMakeRect(W - 40 - 140, 40, 140, 34)
                                         title:@"安装"
                                        action:@selector(onPrimary)];
    self.primaryButton.keyEquivalent = @"\r";
    [c addSubview:self.primaryButton];

    self.secondaryButton = [self buttonWithFrame:NSMakeRect(W - 40 - 140 - 12 - 110, 40, 110, 34)
                                           title:@"取消"
                                          action:@selector(onSecondary)];
    [c addSubview:self.secondaryButton];

    self.detailButton = [self buttonWithFrame:NSMakeRect(40, 44, 130, 26)
                                        title:@"查看详情 ▾"
                                       action:@selector(onToggleDetail)];
    self.detailButton.bezelStyle = NSBezelStyleInline;
    [c addSubview:self.detailButton];

    [self.window makeKeyAndOrderFront:nil];
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
        self.statusLabel.stringValue = @"已安装并正在使用";
        self.versionLabel.stringValue =
            [NSString stringWithFormat:@"系统版本 %@ · 内置版本 %@",
             st.version.length ? st.version : @"未知", builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemGreenColor].CGColor;
        self.hintLabel.stringValue = sameVersion
            ? @"当前已是最新版本。重装只会覆盖 /Applications 里的文件；"
              @"系统按版本号判断是否重新加载，同版本不会替换正在运行的驱动。"
            : @"安装包里的版本与系统不同，点「重装」会自动替换并让旧版本失效，不需要重启。";
        self.primaryButton.title = @"重装";
        self.secondaryButton.title = @"卸载";
    } else if (st.installed && !st.enabled) {
        self.statusLabel.stringValue = st.terminating ? @"旧版本正在退出…" : @"已安装但未激活";
        self.versionLabel.stringValue =
            [NSString stringWithFormat:@"系统版本 %@ · %@", st.version ?: @"", st.stateText ?: @""];
        self.dot.layer.backgroundColor = [NSColor systemOrangeColor].CGColor;
        self.hintLabel.stringValue =
            @"点「重装」重新提交激活请求；如果一直卡在这个状态，选「卸载」再装一次。";
        self.primaryButton.title = @"重装";
        self.secondaryButton.title = @"卸载";
    } else if (onDisk) {
        self.statusLabel.stringValue = @"文件已就位，驱动未激活";
        self.versionLabel.stringValue =
            [NSString stringWithFormat:@"/Applications 里有 %@，但系统列表里没有", builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemOrangeColor].CGColor;
        self.hintLabel.stringValue = @"点「安装」重新激活，文件已经在了，不会重复复制。";
        self.primaryButton.title = @"安装";
        self.secondaryButton.title = @"取消";
    } else {
        self.statusLabel.stringValue = @"未安装";
        self.versionLabel.stringValue =
            [NSString stringWithFormat:@"安装包内置版本 %@", builtinVersion()];
        self.dot.layer.backgroundColor = [NSColor systemGrayColor].CGColor;
        self.hintLabel.stringValue =
            @"安装会把驱动部署到 /Applications/YT6801Driver.app 并注册系统扩展。"
            @"全程不重启，不影响当前网络。";
        self.primaryButton.title = @"安装";
        self.secondaryButton.title = @"取消";
    }
}

- (void)onToggleDetail
{
    self.detailShown = !self.detailShown;
    self.detailScroll.hidden = !self.detailShown;
    self.detailButton.title = self.detailShown ? @"收起详情 ▴" : @"查看详情 ▾";
    if (self.detailShown) {
        [self appendLog:@"— 详情窗口已展开 —"];
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
    self.primaryButton.title = @"完成";
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
    /* Both install and reinstall require accepting the license first;
     * licenseAccepted is valid for the current session only. */
    if (!self.licenseAccepted) {
        [self showLicenseSheet];
        return;
    }
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
    a.messageText = @"要卸载 YT6801 驱动吗？";
    a.informativeText = @"会从系统里移除这个系统扩展，当前有线网络会断开。"
                        @"安装器本身会保留，随时可以再装回来。不需要重启。";
    [a addButtonWithTitle:@"卸载"];
    [a addButtonWithTitle:@"取消"];
    a.alertStyle = NSAlertStyleWarning;
    if ([a runModal] != NSAlertFirstButtonReturn) {
        return;
    }
    [self runUninstall];
}

/* ---- License sheet ---- */
- (void)showLicenseSheet
{
    NSWindow * sheet = [[NSWindow alloc]
                        initWithContentRect:NSMakeRect(0, 0, 640, 520)
                                  styleMask:NSWindowStyleMaskTitled
                                    backing:NSBackingStoreBuffered
                                      defer:NO];

    NSView * c = sheet.contentView;

    NSTextField * t = [self labelWithFrame:NSMakeRect(24, 478, 592, 24)
                                      text:@"用户许可协议"
                                      size:16 bold:YES];
    [c addSubview:t];

    NSScrollView * sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(24, 108, 592, 358)];
    sv.borderType = NSBezelBorder;
    sv.hasVerticalScroller = YES;
    NSTextView * tv = [[NSTextView alloc] initWithFrame:sv.bounds];
    tv.editable = NO;
    tv.string = licenseText();
    tv.font = [NSFont systemFontOfSize:12];
    tv.textContainerInset = NSMakeSize(10, 10);
    sv.documentView = tv;
    [c addSubview:sv];

    NSButton * check = [[NSButton alloc] initWithFrame:NSMakeRect(24, 74, 480, 22)];
    [check setButtonType:NSButtonTypeSwitch];
    check.title = @"我已阅读并同意上述全部条款（完全免费 · MIT 许可证）";
    check.target = self;
    check.action = @selector(onCheckChanged:);
    [c addSubview:check];

    NSButton * cancel = [self buttonWithFrame:NSMakeRect(440, 24, 90, 32)
                                        title:@"取消"
                                       action:@selector(onSheetCancel)];
    cancel.target = self;
    [c addSubview:cancel];

    NSButton * agree = [self buttonWithFrame:NSMakeRect(538, 24, 78, 32)
                                       title:@"同意"
                                      action:@selector(onSheetAgree:)];
    agree.target = self;
    agree.keyEquivalent = @"\r";
    agree.enabled = NO;
    agree.tag = 1001;
    [c addSubview:agree];

    [self.window beginSheet:sheet completionHandler:nil];
}

- (void)onCheckChanged:(NSButton *)sender
{
    NSWindow * sheet = self.window.attachedSheet;
    NSButton * agree = [sheet.contentView viewWithTag:1001];
    agree.enabled = (sender.state == NSControlStateValueOn);
}

- (void)onSheetCancel
{
    [self.window endSheet:self.window.attachedSheet];
}

- (void)onSheetAgree:(NSButton *)sender
{
    self.licenseAccepted = YES;
    [self.window endSheet:self.window.attachedSheet];
    [self runInstall];
}

/* ---- Run ---- */
- (void)runInstall
{
    [self setBusy:YES];
    self.statusLabel.stringValue = @"正在安装…";
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
                weakSelf.statusLabel.stringValue = @"安装失败";
                weakSelf.versionLabel.stringValue = err;
                weakSelf.dot.layer.backgroundColor = [NSColor systemRedColor].CGColor;
                [weakSelf appendLog:[@"✗ " stringByAppendingString:err]];
                NSAlert * a = [[NSAlert alloc] init];
                a.messageText = @"安装没有完成";
                a.informativeText = err;
                [a addButtonWithTitle:@"好"];
                [a runModal];
            } else {
                [weakSelf refreshState];
                weakSelf.hintLabel.stringValue = @"安装完成。插上网线即可使用，不需要重启。";
                [weakSelf appendLog:@"✓ 安装完成"];
                [weakSelf showCompleted:@"安装完成"];
            }
        });
    });
}

- (void)runUninstall
{
    [self setBusy:YES];
    self.statusLabel.stringValue = @"正在卸载…";
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
                weakSelf.statusLabel.stringValue = @"卸载未完成";
                weakSelf.hintLabel.stringValue = err;
                weakSelf.dot.layer.backgroundColor = [NSColor systemRedColor].CGColor;
                [weakSelf appendLog:[@"✗ " stringByAppendingString:err]];
                if (!weakSelf.detailShown) {
                    [weakSelf onToggleDetail];
                }
                [weakSelf refreshState];

                NSAlert * a = [[NSAlert alloc] init];
                a.messageText = @"卸载没有完成";
                a.informativeText = [err stringByAppendingString:
                    @"\n\n驱动文件保持原样，可以正常使用。"
                    @"「重装」会用安装包内的版本覆盖现有驱动，"
                    @"要是只想换版本，用重装就够了，不需要卸载。"];
                [a addButtonWithTitle:@"知道了"];
                [a runModal];
            } else {
                [weakSelf refreshState];
                weakSelf.hintLabel.stringValue = @"已卸载。安装器可以留着，随时再装。";
                [weakSelf appendLog:@"✓ 卸载完成"];
                [weakSelf showCompleted:@"卸载完成"];
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
                    "YT6801 安装器\n"
                    "  --status      查看状态\n"
                    "  --install     静默安装\n"
                    "  --uninstall   静默卸载\n"
                    "  --activate    仅激活\n"
                    "  --deactivate  仅卸载扩展\n"
                    "（不带参数则启动图形界面）\n");
            return 2;
        }

        NSApplication * app = [NSApplication sharedApplication];
        InstallerController * ctl = [[InstallerController alloc] init];
        app.delegate = ctl;

        NSMenu * mb = [[NSMenu alloc] init];
        NSMenuItem * appItem = [[NSMenuItem alloc] init];
        [mb addItem:appItem];
        NSMenu * sub = [[NSMenu alloc] init];
        [sub addItemWithTitle:@"关于 YT6801 安装器" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
        [sub addItem:[NSMenuItem separatorItem]];
        [sub addItemWithTitle:@"退出" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = sub;
        app.mainMenu = mb;

        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
        (void)ctl;
    }
    return 0;
}
