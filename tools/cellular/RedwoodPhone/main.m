/*
 * RedwoodPhone (A, 2026-11-14): the S20's VoLTE calls on iOS's own call screen. There is no CommCenter; redwood-ims
 * (root) does SIP and the voice. This is the CallKit provider for it: a registered app (CallKit refuses a plain process:
 * "a CXXPCCallSource couldn't be created"), run by launchd as mobile, always on, with no UI of its own.
 * It talks to redwood-ims through /var/tmp/redwood-cell/call-ui/ (see redwood-ims.c "Calls, step 4"):
 *   state  "ringing ID NUMBER" -> reportNewIncomingCall (the native incoming call screen, lock screen included)
 *          "ended ID REASON"   -> reportCallWithUUID:endedAtDate:reason:
 *   cmd.*  answer / end / mute, from the call screen's buttons
 *   alive  touched every 5 s: redwood-ims only rings the screen while this app runs (else it answers 486 as before)
 * Log: /var/mobile/Library/Logs/redwood-phone.log (numbers are not logged).
 */
#import <Foundation/Foundation.h>
#import <CallKit/CallKit.h>
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

static NSString *const Dir = @"/var/tmp/redwood-cell/call-ui";

static void rp_log(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void rp_log(NSString *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    FILE *f = fopen("/var/mobile/Library/Logs/redwood-phone.log", "a");
    if (f) { fprintf(f, "%s %s\n", [[[NSDate date] description] UTF8String], s.UTF8String); fclose(f); }
}

static void rp_cmd(NSString *line)
{
    NSString *fn = [Dir stringByAppendingFormat:@"/cmd.%.0f.%u", [[NSDate date] timeIntervalSince1970] * 1000, arc4random() % 100000];
    NSString *tmp = [fn stringByAppendingString:@".tmp"];
    [[line stringByAppendingString:@"\n"] writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil];
    rename(tmp.fileSystemRepresentation, [fn stringByReplacingOccurrencesOfString:@".tmp" withString:@""].fileSystemRepresentation);
    rp_log(@"-> %@", line);
}

@interface RP : NSObject <CXProviderDelegate>
@property (strong) CXProvider *provider;
@property (strong) NSMutableDictionary<NSNumber *, NSUUID *> *calls;   /* redwood-ims call id -> CallKit UUID */
@property (strong) NSMutableSet<NSNumber *> *done;                       /* ids already reported ended */
@property (copy) NSString *lastState;
@property (strong) NSMutableDictionary<NSString *, NSUUID *> *dialing;   /* tag -> UUID until redwood-ims names the call */
@property (strong) NSMutableSet<NSUUID *> *outgoing;
@end

@implementation RP
- (instancetype)init
{
    if ((self = [super init])) {
        CXProviderConfiguration *c = [[CXProviderConfiguration alloc] init];
        c.supportsVideo = NO;
        c.maximumCallGroups = 1;
        c.maximumCallsPerCallGroup = 1;
        c.includesCallsInRecents = YES;
        c.supportedHandleTypes = [NSSet setWithObject:@(CXHandleTypePhoneNumber)];
        _provider = [[CXProvider alloc] initWithConfiguration:c];
        [_provider setDelegate:self queue:nil];
        _calls = [NSMutableDictionary new];
        _done = [NSMutableSet new];
        _dialing = [NSMutableDictionary new];
        _outgoing = [NSMutableSet new];
        [[NSFileManager defaultManager] createDirectoryAtPath:Dir withIntermediateDirectories:YES attributes:nil error:nil];
    }
    return self;
}

- (NSNumber *)idFor:(NSUUID *)u
{
    for (NSNumber *k in self.calls) if ([self.calls[k] isEqual:u]) return k;
    return nil;
}

- (void)tick
{
    static NSDate *alive;
    if (!alive || -[alive timeIntervalSinceNow] > 5) {
        alive = [NSDate date];
        NSString *a = [Dir stringByAppendingString:@"/alive"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:a]) [@"" writeToFile:a atomically:NO encoding:NSUTF8StringEncoding error:nil];
        [[NSFileManager defaultManager] setAttributes:@{NSFileModificationDate: [NSDate date]} ofItemAtPath:a error:nil];
    }
    NSString *st = [NSString stringWithContentsOfFile:[Dir stringByAppendingString:@"/state"] encoding:NSUTF8StringEncoding error:nil];
    st = [st stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!st.length || [st isEqualToString:self.lastState]) return;
    self.lastState = st;
    NSArray<NSString *> *w = [st componentsSeparatedByString:@" "];
    if (w.count < 2) return;
    NSNumber *cid = @([w[1] integerValue]);
    if ([w[0] isEqualToString:@"ringing"] && w.count >= 3 && !self.calls[cid] && ![self.done containsObject:cid]) {
        NSUUID *u = [NSUUID UUID];
        self.calls[cid] = u;
        CXCallUpdate *up = [CXCallUpdate new];
        NSString *num = w[2];
        up.remoteHandle = [[CXHandle alloc] initWithType:CXHandleTypePhoneNumber value:[num isEqualToString:@"unknown"] ? @"" : num];
        up.hasVideo = NO; up.supportsHolding = NO; up.supportsGrouping = NO; up.supportsUngrouping = NO; up.supportsDTMF = NO;
        rp_log(@"call %@ ringing", cid);
        [self.provider reportNewIncomingCallWithUUID:u update:up completion:^(NSError *e) {
            if (e) { rp_log(@"call %@: the call screen refused it: %@", cid, e); rp_cmd([NSString stringWithFormat:@"end %@", cid]); }
        }];
    } else if ([w[0] isEqualToString:@"ended"] && cid.integerValue != 0) {
        NSUUID *u = self.calls[cid];
        if (u && ![self.done containsObject:cid]) {
            [self.done addObject:cid];
            NSString *why = w.count > 2 ? [[w subarrayWithRange:NSMakeRange(2, w.count - 2)] componentsJoinedByString:@" "] : @"";
            CXCallEndedReason r = CXCallEndedReasonRemoteEnded;
            if ([why containsString:@"not answered"]) r = CXCallEndedReasonUnanswered;
            else if ([why containsString:@"restarted"] || [why containsString:@"no ACK"]) r = CXCallEndedReasonFailed;
            rp_log(@"call %@ ended (%@)", cid, why);
            [self.provider reportCallWithUUID:u endedAtDate:[NSDate date] reason:r];
        }
    } else if ([w[0] isEqualToString:@"dialing"] && w.count >= 3) {
        NSUUID *u = self.dialing[w[2]];
        if (u) { self.calls[cid] = u; [self.dialing removeObjectForKey:w[2]]; [self.outgoing addObject:u];
            [self.provider reportOutgoingCallWithUUID:u startedConnectingAtDate:[NSDate date]]; rp_log(@"call %@ dialing", cid); }
    } else if ([w[0] isEqualToString:@"ended"] && [w[1] isEqualToString:@"0"] && w.count >= 4) {
        NSUUID *u = self.dialing[w[3]];                     /* refused before it had a number (busy with another call) */
        if (u) { [self.dialing removeObjectForKey:w[3]]; [self.provider reportCallWithUUID:u endedAtDate:[NSDate date] reason:CXCallEndedReasonFailed]; }
    } else if ([w[0] isEqualToString:@"alerting"]) {
        rp_log(@"call %@ ringing at the other end", cid);
    } else if ([w[0] isEqualToString:@"up"]) {
        NSUUID *u = self.calls[cid];
        if (u && [self.outgoing containsObject:u]) [self.provider reportOutgoingCallWithUUID:u connectedAtDate:[NSDate date]];
        rp_log(@"call %@ up", cid);
    }
}

- (void)providerDidReset:(CXProvider *)p { rp_log(@"provider reset"); }
- (void)providerDidBegin:(CXProvider *)p { rp_log(@"provider ready"); }
- (void)provider:(CXProvider *)p performAnswerCallAction:(CXAnswerCallAction *)a
{
    NSNumber *cid = [self idFor:a.callUUID];
    if (cid) rp_cmd([NSString stringWithFormat:@"answer %@", cid]);
    [a fulfill];
}
- (void)provider:(CXProvider *)p performEndCallAction:(CXEndCallAction *)a
{
    NSNumber *cid = [self idFor:a.callUUID];
    if (cid && ![self.done containsObject:cid]) { [self.done addObject:cid]; rp_cmd([NSString stringWithFormat:@"end %@", cid]); }
    [a fulfill];
}
- (void)provider:(CXProvider *)p performSetMutedCallAction:(CXSetMutedCallAction *)a
{
    NSNumber *cid = [self idFor:a.callUUID];
    if (cid) rp_cmd([NSString stringWithFormat:@"mute %@ %d", cid, a.muted ? 1 : 0]);
    [a fulfill];
}
/* a call from the Phone app (its dial goes to us: see the dial hook in libredwood, tools/cellular/RedwoodPhone/README) */
- (void)provider:(CXProvider *)p performStartCallAction:(CXStartCallAction *)a
{
    NSString *num = a.handle.value ?: @"";
    NSString *tag = [[a.callUUID UUIDString] substringToIndex:8];
    self.dialing[tag] = a.callUUID;
    rp_log(@"dialling (from the Phone app)");
    rp_cmd([NSString stringWithFormat:@"dial %@ %@", tag, [[num componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] componentsJoinedByString:@""]]);
    CXCallUpdate *up = [CXCallUpdate new];
    up.remoteHandle = a.handle; up.hasVideo = NO; up.supportsHolding = NO; up.supportsDTMF = NO;
    [a fulfill];
    [p reportCallWithUUID:a.callUUID updated:up];
}
- (void)provider:(CXProvider *)p performSetHeldCallAction:(CXSetHeldCallAction *)a { [a fail]; }
- (void)provider:(CXProvider *)p didActivateAudioSession:(AVAudioSession *)s { rp_log(@"audio session on"); }
- (void)provider:(CXProvider *)p didDeactivateAudioSession:(AVAudioSession *)s { rp_log(@"audio session off"); }
@end

/* When a call is answered iOS also launches the provider's app through FrontBoard (UIKitApplication:com.redwood.phone):
 * that instance must not be a second provider (it reported the same call again, 2026-11-14 04:03): it only runs UIKit,
 * shows nothing and stays out of the way; the launchd instance keeps the calls. */
@interface RPApp : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation RPApp
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o
{
    rp_log(@"launched by iOS (pid %d): UI instance only", getpid());
    return YES;
}
/* brought to the front when a call is answered: nothing to show (a blank screen covered the call screen, the user
 * 2026-11-14 04:08): step back at once, so iOS's own call screen (InCallService) stays in front */
- (void)applicationDidBecomeActive:(UIApplication *)a
{
    rp_log(@"in front: stepping back to the call screen");
    dispatch_async(dispatch_get_main_queue(), ^{
        SEL s = NSSelectorFromString(@"suspend");
        if ([a respondsToSelector:s]) ((void (*)(id, SEL))[a methodForSelector:s])(a, s);
    });
}
@end

int main(int argc, char **argv)
{
    const char *svc = getenv("XPC_SERVICE_NAME");
    if (svc && !strncmp(svc, "UIKitApplication", 16)) {
        @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([RPApp class])); }
    }
    @autoreleasepool {
        rp_log(@"RedwoodPhone starting (pid %d)", getpid());
        RP *rp = [RP new];
        [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *t) { [rp tick]; }];
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
