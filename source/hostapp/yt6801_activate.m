/*
 * yt6801_activate.m — minimal System Extension activator.
 *
 * DriverKit drivers are system extensions and cannot be loaded from the
 * command line; the only supported entry point is a host app calling
 * OSSystemExtensionRequest. Works without a developer account when:
 *   1. systemextensionsctl developer on  — allows ad-hoc signed extensions
 *   2. SIP Kext Signing / NVRAM Protections disabled (Custom config)
 *   3. AMFI bypassed via boot-arg amfi=0x80
 * No dk=0x8001 and no reboot required.
 *
 * Usage: yt6801_activate [log path]
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

@interface Activator : NSObject <OSSystemExtensionRequestDelegate>
@end

@implementation Activator

- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                 actionForReplacingExtension:(OSSystemExtensionInfo *)existing
                               withExtension:(OSSystemExtensionInfo *)ext
{
    LOG(@"Existing extension with same ID: old %@ -> new %@, replacing",
        existing.bundleShortVersion ?: @"?", ext.bundleShortVersion ?: @"?");
    return OSSystemExtensionReplacementActionReplace;
}

- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request
{
    LOG(@"NEED_APPROVAL -- sysextd requires user approval in System Settings");
    LOG(@"Open System Settings > General > Login Items & Extensions > Driver Extensions and allow YT6801");
    LOG(@"Waiting up to 15 minutes; activation resumes automatically after approval. No reboot needed.");
}

- (void)request:(OSSystemExtensionRequest *)request
        didFinishWithResult:(OSSystemExtensionRequestResult)result
{
    if (result == OSSystemExtensionRequestWillCompleteAfterReboot) {
        LOG(@"RESULT = WillCompleteAfterReboot (reboot required)");
        LOG(@"Note: only this result requires a reboot.");
        if (gLog) fclose(gLog);
        exit(4);
    }
    LOG(@"RESULT = Completed (extension activated and loaded, no reboot), result=%ld", (long)result);
    if (gLog) fclose(gLog);
    exit(0);
}

- (void)request:(OSSystemExtensionRequest *)request
        didFailWithError:(NSError *)error
{
    LOG(@"FAIL -- %@ (domain=%@, code=%ld)",
        error.localizedDescription, error.domain, (long)error.code);
    LOG(@"userInfo = %@", error.userInfo);
    if (gLog) fclose(gLog);
    exit(1);
}

@end

int
main(int argc, const char * argv[])
{
    @autoreleasepool {
        NSString * logPath = (argc > 1)
            ? [NSString stringWithUTF8String:argv[1]]
            : @"/tmp/yt6801_activate.log";
        gLog = fopen(logPath.UTF8String, "a");
        if (gLog) setvbuf(gLog, NULL, _IOLBF, 0);

        LOG(@"======== yt6801 activator started (pid %d) ========", (int)getpid());
        LOG(@"bundle id = %@, log = %@", kExtensionID, logPath);

        Activator * act = [Activator new];

        OSSystemExtensionRequest * req =
            [OSSystemExtensionRequest activationRequestForExtension:kExtensionID
                                                              queue:dispatch_get_main_queue()];
        if (req == nil) {
            LOG(@"Failed to create activation request");
            return 2;
        }
        req.delegate = act;
        [[OSSystemExtensionManager sharedManager] submitRequest:req];
        LOG(@"Request submitted, waiting for sysextd callbacks...");

        /* Heartbeat: show the process is alive and still waiting */
        dispatch_source_t hb = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                      dispatch_get_main_queue());
        dispatch_source_set_timer(hb, dispatch_time(DISPATCH_TIME_NOW, 10ull * NSEC_PER_SEC),
                                  30ull * NSEC_PER_SEC, 1ull * NSEC_PER_SEC);
        dispatch_source_set_event_handler(hb, ^{
            LOG(@"...still waiting for callbacks (process alive)");
        });
        dispatch_resume(hb);

        /* Hard timeout */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 900ull * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            LOG(@"TIMEOUT -- no sysextd callbacks within 15 minutes");
            if (gLog) fclose(gLog);
            exit(3);
        });

        /* Run the main queue, otherwise delegate callbacks never fire */
        dispatch_main();
    }
}
