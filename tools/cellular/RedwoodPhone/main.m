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
#import <Intents/Intents.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <notify.h>

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

/* the same, without the line in the log (it has the number) */
static void rp_cmd_quiet(NSString *line)
{
    NSString *fn = [Dir stringByAppendingFormat:@"/cmd.%.0f.%u", [[NSDate date] timeIntervalSince1970] * 1000, arc4random() % 100000];
    NSString *tmp = [fn stringByAppendingString:@".tmp"];
    [[line stringByAppendingString:@"\n"] writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil];
    rename(tmp.fileSystemRepresentation, fn.fileSystemRepresentation);
    rp_log(@"-> dial");
}

@interface RP : NSObject <CXProviderDelegate, CXCallObserverDelegate>
@property (strong) CXProvider *provider;
@property (strong) NSMutableDictionary<NSNumber *, NSUUID *> *calls;   /* redwood-ims call id -> CallKit UUID */
@property (strong) NSMutableSet<NSNumber *> *done;                       /* ids already reported ended */
@property (copy) NSString *lastState;
@property (strong) NSMutableDictionary<NSString *, NSUUID *> *dialing;   /* tag -> UUID until redwood-ims names the call */
@property (strong) NSMutableSet<NSUUID *> *outgoing;
@property (strong) AVAudioEngine *engine;
@property (strong) CXCallObserver *observer;
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
        /* safety net: a call iOS has ended must not stay up at the network (05:21: an End that never reached the provider
         * left the call connected in the background) */
        _observer = [CXCallObserver new];
        [_observer setDelegate:self queue:nil];
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
    } else if ([w[0] isEqualToString:@"dialing"] && w.count >= 3 && !self.dialing[w[2]]) {
        rp_cmd([NSString stringWithFormat:@"end %@", cid]);   /* (a dial the user has already ended) */
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

/* CallKit dropped us (callservicesd restarted, or refused the call): no call may stay ringing at the other end */
- (void)providerDidReset:(CXProvider *)p
{
    rp_log(@"provider reset");
    for (NSNumber *cid in self.calls.allKeys) {
        if (![self.done containsObject:cid]) { [self.done addObject:cid]; rp_cmd([NSString stringWithFormat:@"end %@", cid]); }
    }
}
- (void)callObserver:(CXCallObserver *)o callChanged:(CXCall *)c
{
    if (!c.hasEnded) return;
    NSNumber *cid = [self idFor:c.UUID];
    if (cid && ![self.done containsObject:cid]) {
        [self.done addObject:cid];
        rp_log(@"call %@ ended by iOS: hanging up", cid);
        rp_cmd([NSString stringWithFormat:@"end %@", cid]);
    }
}
- (void)providerDidBegin:(CXProvider *)p { rp_log(@"provider ready"); }
static void rp_session_for_call(void)
{
    AVAudioSession *as = [AVAudioSession sharedInstance];
    [as setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeVoiceChat options:0 error:nil];
}
- (void)provider:(CXProvider *)p performAnswerCallAction:(CXAnswerCallAction *)a
{
    rp_session_for_call();
    NSNumber *cid = [self idFor:a.callUUID];
    if (cid) rp_cmd([NSString stringWithFormat:@"answer %@", cid]);
    [a fulfill];
}
- (void)provider:(CXProvider *)p performEndCallAction:(CXEndCallAction *)a
{
    NSNumber *cid = [self idFor:a.callUUID];
    if (cid && ![self.done containsObject:cid]) { [self.done addObject:cid]; rp_cmd([NSString stringWithFormat:@"end %@", cid]); }
    else if (!cid) {
        /* still being dialled (redwood-ims has not named it yet): cancel by its tag (05:34: a hang-up during "calling"
         * was lost and the call went out anyway) */
        for (NSString *tag in [self.dialing allKeysForObject:a.callUUID]) { rp_cmd([NSString stringWithFormat:@"cancel %@", tag]); [self.dialing removeObjectForKey:tag]; }
    }
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
    rp_session_for_call();
    NSString *num = a.handle.value ?: @"";
    NSString *tag = [[a.callUUID UUIDString] substringToIndex:8];
    if (![[NSFileManager defaultManager] fileExistsAtPath:@"/var/tmp/redwood-cell/ims-registered"]) {
        /* VoLTE not registered (yet): fail now instead of a dial that goes out much later */
        rp_log(@"dial refused: VoLTE is not registered");
        [a fail];
        return;
    }
    self.dialing[tag] = a.callUUID;
    rp_log(@"dialling (from the Phone app)");
    rp_cmd_quiet([NSString stringWithFormat:@"dial %@ %@", tag, [[num componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] componentsJoinedByString:@""]]);
    CXCallUpdate *up = [CXCallUpdate new];
    up.remoteHandle = a.handle; up.hasVideo = NO; up.supportsHolding = NO; up.supportsDTMF = NO;
    [a fulfill];
    [p reportCallWithUUID:a.callUUID updated:up];
}
- (void)provider:(CXProvider *)p performSetHeldCallAction:(CXSetHeldCallAction *)a { [a fail]; }
/* The call's audio session: silence played through it, so that iOS's route (Receiver = earpiece by default, Speaker
 * from the call screen's button) runs the audio plug-in's device of that name, which tells the kernel the route.
 * The voice itself goes past iOS (redwood-ims -> /dev/redwood-audio). */
- (void)provider:(CXProvider *)p didActivateAudioSession:(AVAudioSession *)s
{
    rp_log(@"audio session on");
    if (!self.engine) {
        self.engine = [AVAudioEngine new];
        AVAudioFormat *f = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:1];
        AVAudioSourceNode *src = [[AVAudioSourceNode alloc] initWithFormat:f renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n, AudioBufferList *out) {
            for (UInt32 i = 0; i < out->mNumberBuffers; i++) memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
            *silence = YES;
            return noErr;
        }];
        [self.engine attachNode:src];
        [self.engine connect:src to:self.engine.mainMixerNode format:f];
    }
    /* (off: iOS has no receiver port from the audio plug-in yet, so the silence ran the Speaker device, which set the
     * speaker route over the earpiece redwood-ims had chosen. The call stays on the earpiece; route from the plug-in later.) */
    if ([[NSFileManager defaultManager] fileExistsAtPath:@"/var/tmp/redwood-cell/call-ui/route-player"]) {
        NSError *e = nil;
        if (![self.engine startAndReturnError:&e]) rp_log(@"route player: %@", e);
    }
    [[NSNotificationCenter defaultCenter] addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        rp_log(@"route now %@", [AVAudioSession sharedInstance].currentRoute.outputs.firstObject.portType);
    }];
}
- (void)provider:(CXProvider *)p didDeactivateAudioSession:(AVAudioSession *)s { rp_log(@"audio session off"); [self.engine stop]; }
@end

/* When a call is answered iOS also launches the provider's app through FrontBoard (UIKitApplication:com.redwood.phone):
 * that instance must not be a second provider (it reported the same call again, 2026-11-14 04:03): it only runs UIKit,
 * shows nothing and stays out of the way; the launchd instance keeps the calls. */
@interface RPCallVC : UIViewController <CXCallObserverDelegate>
@property (strong) UILabel *who, *status;
@property (strong) UIButton *mute, *speaker, *keypad, *end;
@property (strong) CXCallObserver *obs;
@property (strong) CXCallController *cc;
@property (strong) NSDate *connectedAt;
@property BOOL muted, onSpeaker;
@property (copy) NSString *lastState;
@end
static NSDate *rp_last_event;   /* a call started, was answered or changed state */

/* RedwoodPhone's own call screen (A, 2026-11-14): what a tap on the call's banner / Dynamic Island opens (iOS opens the
 * provider's app for a CallKit call). Number, status / time, mute, speaker, end; reads redwood-ims's state. */
@implementation RPCallVC
static UIButton *rp_round(NSString *sym, NSString *title, UIColor *bg)
{
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *c = [UIButtonConfiguration filledButtonConfiguration];
    c.image = [UIImage systemImageNamed:sym withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:28]];
    c.imagePlacement = NSDirectionalRectEdgeTop; c.imagePadding = 8;
    c.title = title; c.baseBackgroundColor = bg; c.baseForegroundColor = UIColor.whiteColor;
    c.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    c.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *a) { NSMutableDictionary *m = a.mutableCopy; m[NSFontAttributeName] = [UIFont systemFontOfSize:13]; return m; };
    b.configuration = c;
    b.translatesAutoresizingMaskIntoConstraints = NO;
    [b.widthAnchor constraintEqualToConstant:84].active = YES; [b.heightAnchor constraintEqualToConstant:104].active = YES;
    return b;
}
- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.08 green:0.09 blue:0.11 alpha:1];
    self.who = [UILabel new]; self.who.font = [UIFont systemFontOfSize:34 weight:UIFontWeightRegular];
    self.who.textColor = UIColor.whiteColor; self.who.textAlignment = NSTextAlignmentCenter; self.who.adjustsFontSizeToFitWidth = YES;
    self.status = [UILabel new]; self.status.font = [UIFont systemFontOfSize:18]; self.status.textColor = [UIColor colorWithWhite:0.75 alpha:1];
    self.status.textAlignment = NSTextAlignmentCenter;
    UIColor *g = [UIColor colorWithWhite:1 alpha:0.18];
    self.mute = rp_round(@"mic.slash.fill", @"mute", g);
    self.keypad = rp_round(@"circle.grid.3x3.fill", @"keypad", g); self.keypad.enabled = NO;
    self.speaker = rp_round(@"speaker.wave.3.fill", @"speaker", g);
    self.end = rp_round(@"phone.down.fill", @"", UIColor.systemRedColor);
    [self.mute addTarget:self action:@selector(toggleMute) forControlEvents:UIControlEventTouchUpInside];
    [self.speaker addTarget:self action:@selector(toggleSpeaker) forControlEvents:UIControlEventTouchUpInside];
    [self.end addTarget:self action:@selector(hangUp) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[self.mute, self.keypad, self.speaker]];
    row.spacing = 26; row.axis = UILayoutConstraintAxisHorizontal;
    UIStackView *top = [[UIStackView alloc] initWithArrangedSubviews:@[self.who, self.status]];
    top.axis = UILayoutConstraintAxisVertical; top.spacing = 8;
    for (UIView *v in @[top, row, self.end]) { v.translatesAutoresizingMaskIntoConstraints = NO; [self.view addSubview:v]; }
    UILayoutGuide *sa = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [top.topAnchor constraintEqualToAnchor:sa.topAnchor constant:70], [top.leadingAnchor constraintEqualToAnchor:sa.leadingAnchor constant:20],
        [top.trailingAnchor constraintEqualToAnchor:sa.trailingAnchor constant:-20],
        [row.centerXAnchor constraintEqualToAnchor:sa.centerXAnchor], [row.bottomAnchor constraintEqualToAnchor:self.end.topAnchor constant:-60],
        [self.end.centerXAnchor constraintEqualToAnchor:sa.centerXAnchor], [self.end.bottomAnchor constraintEqualToAnchor:sa.bottomAnchor constant:-40],
    ]];
    self.cc = [CXCallController new];
    self.obs = self.cc.callObserver; [self.obs setDelegate:self queue:nil];
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) { [self refresh]; }];
    [self refresh];
}
- (CXCall *)current { for (CXCall *c in self.obs.calls) if (!c.hasEnded) return c; return nil; }
- (void)refresh
{
    NSString *st = [NSString stringWithContentsOfFile:[Dir stringByAppendingString:@"/state"] encoding:NSUTF8StringEncoding error:nil];
    NSArray *w = [[st stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] componentsSeparatedByString:@" "];
    NSString *num = [NSString stringWithContentsOfFile:[Dir stringByAppendingString:@"/number"] encoding:NSUTF8StringEncoding error:nil];
    if (st && ![st isEqualToString:self.lastState]) { self.lastState = st; rp_last_event = [NSDate date]; }
    CXCall *c = [self current];
    self.who.text = num.length ? num : @"Call";
    if (!c) { self.status.text = @"Call ended"; self.connectedAt = nil; return; }
    if (c.hasConnected) {
        if (!self.connectedAt) self.connectedAt = [NSDate date];
        int s = (int)-[self.connectedAt timeIntervalSinceNow];
        self.status.text = [NSString stringWithFormat:@"%02d:%02d", s / 60, s % 60];
    } else self.status.text = [w.firstObject isEqualToString:@"alerting"] ? @"ringing…" : (c.isOutgoing ? @"calling…" : @"incoming…");
}
- (void)callObserver:(CXCallObserver *)o callChanged:(CXCall *)c { rp_last_event = [NSDate date]; [self refresh]; }
- (void)toggleMute
{
    CXCall *c = [self current]; if (!c) return;
    self.muted = !self.muted;
    [self.cc requestTransaction:[[CXTransaction alloc] initWithAction:[[CXSetMutedCallAction alloc] initWithCallUUID:c.UUID muted:self.muted]] completion:^(NSError *e) {}];
    UIButtonConfiguration *k = self.mute.configuration; k.baseBackgroundColor = self.muted ? UIColor.whiteColor : [UIColor colorWithWhite:1 alpha:0.18];
    k.baseForegroundColor = self.muted ? UIColor.blackColor : UIColor.whiteColor; self.mute.configuration = k;
}
- (void)toggleSpeaker
{
    self.onSpeaker = !self.onSpeaker;
    notify_post(self.onSpeaker ? "com.redwood.call.speaker" : "com.redwood.call.earpiece");
    UIButtonConfiguration *k = self.speaker.configuration; k.baseBackgroundColor = self.onSpeaker ? UIColor.whiteColor : [UIColor colorWithWhite:1 alpha:0.18];
    k.baseForegroundColor = self.onSpeaker ? UIColor.blackColor : UIColor.whiteColor; self.speaker.configuration = k;
}
- (void)hangUp
{
    CXCall *c = [self current]; if (!c) return;
    [self.cc requestTransaction:[[CXTransaction alloc] initWithAction:[[CXEndCallAction alloc] initWithCallUUID:c.UUID]] completion:^(NSError *e) {}];
}
@end

@interface RPApp : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation RPApp
/* iOS also brings us to the front itself when a call is dialled or answered; its own full call screen is the one to
 * keep then (05:40: native screen, then ours over it). Ours is for a tap on the banner / Dynamic Island later on. */
- (void)applicationDidBecomeActive:(UIApplication *)a
{
    if (rp_last_event && -[rp_last_event timeIntervalSinceNow] < 4) {
        rp_log(@"in front with a call event just now: back to iOS's call screen");
        dispatch_async(dispatch_get_main_queue(), ^{
            SEL s = NSSelectorFromString(@"suspend");
            if ([a respondsToSelector:s]) ((void (*)(id, SEL))[a methodForSelector:s])(a, s);
        });
    } else rp_log(@"in front: our call screen");
}
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o
{
    rp_log(@"launched by iOS (pid %d): the call screen", getpid());
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [RPCallVC new];
    [self.window makeKeyAndVisible];
    rp_last_event = [NSDate date];
    NSDictionary *uad = o[UIApplicationLaunchOptionsUserActivityDictionaryKey];
    NSUserActivity *ua = uad[@"UIApplicationLaunchOptionsUserActivityKey"];
    if ([ua isKindOfClass:[NSUserActivity class]]) { rp_log(@"dial request at launch (%@)", ua.activityType); rp_start_call(rp_intent_number(ua)); }
    return YES;
}
/* a dial from the Phone app / Recents / Contacts: iOS opens the provider's app with an INStartCallIntent; the app
 * asks CallKit to start the call (CXStartCallAction), which callservicesd hands to our provider in the launchd
 * instance (same bundle), which writes "dial" for redwood-ims */
static NSString *rp_intent_number(NSUserActivity *ua)
{
    INIntent *in = ua.interaction.intent;
    NSArray *people = nil;
    if ([in respondsToSelector:@selector(contacts)]) people = [(id)in contacts];
    INPerson *p = people.firstObject;
    return p.personHandle.value;
}
static void rp_start_call(NSString *num)
{
    if (!num.length) { rp_log(@"dial: no number in the request"); return; }
    static CXCallController *cc;
    if (!cc) cc = [CXCallController new];
    CXHandle *h = [[CXHandle alloc] initWithType:CXHandleTypePhoneNumber value:num];
    CXStartCallAction *st = [[CXStartCallAction alloc] initWithCallUUID:[NSUUID UUID] handle:h];
    [cc requestTransaction:[[CXTransaction alloc] initWithAction:st] completion:^(NSError *e) {
        rp_log(@"dial: start-call request %@", e ? e.localizedDescription : @"accepted");
    }];
}
- (BOOL)application:(UIApplication *)a continueUserActivity:(NSUserActivity *)ua restorationHandler:(void (^)(NSArray<id<UIUserActivityRestoring>> *))r
{
    rp_log(@"dial request from iOS (%@)", ua.activityType);
    rp_start_call(rp_intent_number(ua));
    return YES;
}
- (BOOL)application:(UIApplication *)a openURL:(NSURL *)url options:(NSDictionary *)o
{
    NSString *num = url.resourceSpecifier ?: url.host;
    rp_log(@"dial request by URL");
    rp_start_call([num stringByRemovingPercentEncoding]);
    return YES;
}

@end

int main(int argc, char **argv)
{
    const char *svc = getenv("XPC_SERVICE_NAME");
    if (svc && !strncmp(svc, "UIKitApplication", 16)) {
        dlopen("/System/Library/PrivateFrameworks/TelephonyUtilities.framework/TelephonyUtilities", RTLD_NOW);
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
