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
    LOG(@"已存在同 ID 扩展：旧 %@ → 新 %@，选择替换",
        existing.bundleShortVersion ?: @"?", ext.bundleShortVersion ?: @"?");
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    if (self.unloading) {
        LOG(@"NEED_APPROVAL —— 卸载需要管理员授权，请在弹出的框里点「允许」");
    } else {
        LOG(@"NEED_APPROVAL —— sysextd 要求用户批准，请在系统设置里允许 YT6801");
    }
}

- (void)request:(OSSystemExtensionRequest *)request
        didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        LOG(@"RESULT=WillCompleteAfterReboot（需要重启才生效）");
        finish(4);
    }
    LOG(@"RESULT=Completed（无需重启）result=%ld", (long)result);
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
        printf("检测失败：%s\n", e.reason.UTF8String ?: "");
        return;
    }
    NSData * d = [[pipe fileHandleForReading] readDataToEndOfFile];
    NSString * out = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
    for (NSString * line in [out componentsSeparatedByString:@"\n"]) {
        if ([line rangeOfString:kExtensionID].location != NSNotFound) {
            printf("%s\n", line.UTF8String);
        }
    }
}

int
main(int argc, const char * argv[])
{
    @autoreleasepool {
        NSString * mode = (argc > 1) ? [NSString stringWithUTF8String:argv[1]] : @"";
        NSString * logPath = (argc > 2)
            ? [NSString stringWithUTF8String:argv[2]]
            : @"/tmp/yt6801_host.log";

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

        LOG(@"======== 宿主 %@（pid %d）========",
            unloading ? @"卸载" : @"激活", (int)getpid());
        LOG(@"bundle = %@", [[NSBundle mainBundle] bundlePath]);

        HostActivator * act = [HostActivator new];
        act.unloading = unloading;

        OSSystemExtensionRequest * req = unloading
            ? [OSSystemExtensionRequest deactivationRequestForExtension:kExtensionID
                                                                  queue:dispatch_get_main_queue()]
            : [OSSystemExtensionRequest activationRequestForExtension:kExtensionID
                                                                queue:dispatch_get_main_queue()];
        if (req == nil) {
            LOG(@"无法构造请求");
            finish(2);
        }
        req.delegate = act;
        [[OSSystemExtensionManager sharedManager] submitRequest:req];
        LOG(@"请求已提交，等待 sysextd 回调…");

        /* Hard timeout: deactivation waits for user approval (5 min);
         * 2 min is enough for activation */
        unsigned long long limit = unloading ? 300ull : 120ull;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            LOG(@"TIMEOUT —— %llu 秒内没有收到 sysextd 回调", limit);
            finish(3);
        });

        /* The main queue must run for delegate callbacks to arrive */
        dispatch_main();
    }
}
