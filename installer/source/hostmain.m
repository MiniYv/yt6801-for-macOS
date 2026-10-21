/*
 * hostmain.m — YT6801 headless host
 *
 * Single responsibility: act as the "carrier app" for the System Extension
 * and perform activation/deactivation on the user's behalf. It is a
 * background program (LSUIElement=1) and shows no window when launched.
 *
 * Why it exists:
 *   sysextd only accepts activation/deactivation requests from an app in
 *   /Applications; the CLI has no install subcommand and a bare dext has no
 *   entry point. So the driver must live inside an app, and that app only
 *   needs to exist — no UI required.
 *
 * Usage:
 *   yt6801host --activate   [log path]   activate/upgrade to the bundled dext
 *   yt6801host --deactivate [log path]   deactivate (prompts for admin approval)
 *   yt6801host --status                  print current extension status
 *   yt6801host                           no-op (default on double-click)
 */

#import <Foundation/Foundation.h>
#import <SystemExtensions/SystemExtensions.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static NSString * const kExtensionID = @"io.miniyv.yt6801ethernet";

/* UI language passed by the installer: 0 = en, 1 = zh-Hans, 2 = zh-Hant */
static int gLang = 1;

enum {
    H_REPLACING, H_APPROVAL_UNLOAD, H_APPROVAL_LOAD, H_REBOOT,
    H_COMPLETED, H_BANNER, H_NO_REQ, H_SUBMITTED, H_TIMEOUT, H_DETECT_FAIL,
    H_NOT_INSTALLED,
};

/* Column order must match the enum above. */
static NSString * kHostStrings[][3] = {
    [H_REPLACING] = { @"Existing extension with same ID: old %@ -> new %@, replacing",
                      @"检测到同 ID 扩展：旧 %@ → 新 %@，选择替换",
                      @"偵測到同 ID 擴充功能：舊 %@ → 新 %@，選擇替換" },
    [H_APPROVAL_UNLOAD] = { @"NEED_APPROVAL - uninstall requires admin approval, click Allow in the dialog",
                      @"NEED_APPROVAL —— 卸载需要管理员授权，请在弹出的框里点「允许」",
                      @"NEED_APPROVAL —— 解除安裝需要管理者授權，請在彈出的框裡點「允許」" },
    [H_APPROVAL_LOAD] = { @"NEED_APPROVAL - sysextd requires user approval, allow YT6801 in System Settings",
                      @"NEED_APPROVAL —— sysextd 要求用户批准，请在系统设置里允许 YT6801",
                      @"NEED_APPROVAL —— sysextd 要求使用者核准，請在系統設定裡允許 YT6801" },
    [H_REBOOT] = { @"RESULT=WillCompleteAfterReboot (restart required)",
                      @"RESULT=WillCompleteAfterReboot（需要重启才生效）",
                      @"RESULT=WillCompleteAfterReboot（需要重新開機才生效）" },
    [H_COMPLETED] = { @"RESULT=Completed (no restart) result=%ld",
                      @"RESULT=Completed（无需重启）result=%ld",
                      @"RESULT=Completed（無需重新開機）result=%ld" },
    [H_BANNER] = { @"======== host %@ (pid %d) ========",
                      @"======== 宿主 %@（pid %d）========",
                      @"======== 宿主 %@（pid %d）========" },
    [H_NO_REQ] = { @"Cannot create request", @"无法构造请求", @"無法建構請求" },
    [H_SUBMITTED] = { @"Request submitted, waiting for sysextd callback…",
                      @"请求已提交，等待 sysextd 回调…",
                      @"請求已送出，等待 sysextd 回呼…" },
    [H_TIMEOUT] = { @"TIMEOUT - no sysextd callback within %llu s",
                      @"TIMEOUT —— %llu 秒内没有收到 sysextd 回调",
                      @"TIMEOUT —— %llu 秒內沒有收到 sysextd 回呼" },
    [H_DETECT_FAIL] = { @"Detection failed: %s", @"检测失败：%s", @"偵測失敗：%s" },
    [H_NOT_INSTALLED] = { @"extension_id: %@\ninstalled: no\nactive: no\nstate: not installed",
                  @"extension_id: %@\ninstalled: 否\nactive: 否\n状态：未安装",
                  @"extension_id: %@\ninstalled: 否\nactive: 否\n狀態：未安裝" },
};

static NSString *
SL(int k)
{
    return kHostStrings[k][gLang];
}

static FILE * gLog = NULL;

static const char *
now_str(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static void
LOG(NSString * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString * msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);

    NSString * line = [NSString stringWithFormat:@"[%s] %@\n", now_str(), msg];
    if (gLog) {
        fputs(line.UTF8String, gLog);
        fflush(gLog);
    }
    fputs(line.UTF8String, stderr);
    fflush(stderr);
}

static void
finish(int code)
{
    if (gLog) {
        fclose(gLog);
        gLog = NULL;
    }
    exit(code);
}

@interface HostActivator : NSObject <OSSystemExtensionRequestDelegate>
@property (nonatomic, assign) BOOL unloading;
@end

@implementation HostActivator

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                 actionForReplacingExtension:(OSSystemExtensionInfo *)existing
                               withExtension:(OSSystemExtensionInfo *)ext
{
    LOG(SL(H_REPLACING),
        existing.bundleShortVersion ?: @"?", ext.bundleShortVersion ?: @"?");
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    if (self.unloading) {
        LOG(SL(H_APPROVAL_UNLOAD));
    } else {
        LOG(SL(H_APPROVAL_LOAD));
    }
}

- (void)request:(OSSystemExtensionRequest *)request
        didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        LOG(SL(H_REBOOT));
        finish(4);
    }
    LOG(SL(H_COMPLETED), (long)result);
    finish(0);
}

- (void)request:(OSSystemExtensionRequest *)request
        didFailWithError:(NSError *)error
{
    LOG(@"FAIL —— %@ (domain=%@, code=%ld)",
        error.localizedDescription, error.domain, (long)error.code);
    finish(1);
}

@end

/* Print status via systemextensionsctl (read-only, no privileges needed) */
static void
printStatus(void)
{
    NSTask * t = [[NSTask alloc] init];
    t.launchPath = @"/usr/bin/systemextensionsctl";
    t.arguments = @[ @"list" ];
    NSPipe * pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    t.standardError = [NSPipe pipe];
    @try {
        [t launch];
        [t waitUntilExit];
    } @catch (NSException * e) {
        printf("%s\n", [SL(H_DETECT_FAIL) UTF8String]);
        return;
    }
    NSData * d = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString * out = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
    BOOL found = NO;
    NSString * state = nil;
    for (NSString * line in [out componentsSeparatedByString:@"\n"]) {
        if ([line rangeOfString:kExtensionID].location != NSNotFound) {
            found = YES;
            NSRange lb = [line rangeOfString:@"["];
            NSRange rb = [line rangeOfString:@"]"];
            if (lb.location != NSNotFound && rb.location != NSNotFound && rb.location > lb.location) {
                state = [line substringWithRange:NSMakeRange(lb.location + 1,
                                                             rb.location - lb.location - 1)];
            }
            printf("%s\n", line.UTF8String);
        }
    }
    if (!found) {
        printf("%s\n", [[NSString stringWithFormat:SL(H_NOT_INSTALLED), kExtensionID] UTF8String]);
        return;
    }
    printf("state: %s\n", (state ?: @"unknown").UTF8String);
}

int
main(int argc, const char * argv[])
{
    @autoreleasepool {
        NSString * mode = (argc > 1) ? [NSString stringWithUTF8String:argv[1]] : @"";
        NSString * logPath = (argc > 2)
            ? [NSString stringWithUTF8String:argv[2]]
            : @"/tmp/yt6801_host.log";
        if (argc > 3) {
            gLang = atoi(argv[3]);
        }

        if ([mode isEqualToString:@"--status"]) {
            printStatus();
            return 0;
        }

        BOOL unloading = [mode isEqualToString:@"--deactivate"];
        if (!unloading && ![mode isEqualToString:@"--activate"]) {
            /* Default on double-click: do nothing, exit quietly */
            return 0;
        }

        gLog = fopen(logPath.UTF8String, "a");
        if (gLog) setvbuf(gLog, NULL, _IOLBF, 0);

        LOG(SL(H_BANNER),
            unloading ? @"Uninstall" : @"Activate", (int)getpid());
        LOG(@"bundle = %@", [[NSBundle mainBundle] bundlePath]);

        HostActivator * act = [HostActivator new];
        act.unloading = unloading;

        OSSystemExtensionRequest * req = unloading
            ? [OSSystemExtensionRequest deactivationRequestForExtension:kExtensionID
                                                                  queue:dispatch_get_main_queue()]
            : [OSSystemExtensionRequest activationRequestForExtension:kExtensionID
                                                                queue:dispatch_get_main_queue()];
        if (req == nil) {
            LOG(SL(H_NO_REQ));
            finish(2);
        }
        req.delegate = act;
        [[OSSystemExtensionManager sharedManager] submitRequest:req];
        LOG(SL(H_SUBMITTED));

        /* Hard timeout: deactivation waits for user approval (5 min);
         * 2 min is enough for activation */
        unsigned long long limit = unloading ? 300ull : 120ull;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            LOG(SL(H_TIMEOUT), limit);
            finish(3);
        });

        /* The main queue must run for delegate callbacks to arrive */
        dispatch_main();
    }
}
