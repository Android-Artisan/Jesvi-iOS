/*
 * The fingerprint sensor's spot on screen, while Touch ID waits for a finger (part of libredwood-sb, in SpringBoard).
 *
 * The S20's sensor sits under the display, but iOS's Touch ID screens were made for a home button and show nothing
 * where the finger should go: presses then land beside the sensor ("PARTIAL" from the matcher). redwood-fpd sets the
 * Darwin notification com.redwood.touchid.sensor's state to 1 while a capture waits for a finger (enrolment, the lock
 * screen, app prompts) and to 0 otherwise; here a window above every other one shows a ring and a fingerprint glyph
 * on the sensor, and never takes a touch.
 *
 * Where: bsd/dev/redwood_teegris.c captures inside x 27500-38000, y 46500-55500 of the touch panel's 0-65535 range
 * (Samsung's qcom,position: centre 32768, 50790), so the ring is centred at 50 % / 77.5 % of the screen and about
 * 16 % of its width across.
 */
#import <UIKit/UIKit.h>
#include <notify.h>
#import <objc/runtime.h>
#import <objc/message.h>

#define RING_NOTIFY "com.redwood.touchid.sensor"

static UIWindow *ringWindow;

/* SpringBoard shows only "secure" windows while the device is locked (the ring flashed up only after an unlock) */
static BOOL yes(id self, SEL _cmd) { return YES; }

static Class
secureWindowClass(void)
{
	static Class c;
	if (c == Nil) {
		c = objc_allocateClassPair(UIWindow.class, "RedwoodRingWindow", 0);
		class_addMethod(c, sel_registerName("_isSecure"), (IMP)yes, "B@:");
		class_addMethod(c, sel_registerName("_shouldCreateContextAsSecure"), (IMP)yes, "B@:");
		objc_registerClassPair(c);
	}
	return c;
}

static UIWindow *
makeRingWindow(void)
{
	CGRect screen = UIScreen.mainScreen.bounds;
	UIWindow *w = nil;

	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
		if ([scene isKindOfClass:UIWindowScene.class]) {
			w = [[secureWindowClass() alloc] initWithWindowScene:(UIWindowScene *)scene];
			break;
		}
	}
	if (w == nil) {
		w = [[secureWindowClass() alloc] initWithFrame:screen];   /* SpringBoard may have no window scene of its own */
	}
	w.frame = screen;
	w.windowLevel = 10000000;          /* above the lock screen and every SpringBoard window */
	w.userInteractionEnabled = NO;
	w.backgroundColor = UIColor.clearColor;
	w.rootViewController = [UIViewController new];
	w.rootViewController.view.userInteractionEnabled = NO;
	w.rootViewController.view.backgroundColor = UIColor.clearColor;

	CGFloat d = screen.size.width * 0.16;
	CGPoint c = CGPointMake(screen.size.width * 0.5, screen.size.height * (50790.0 / 65535.0));
	UIView *spot = [[UIView alloc] initWithFrame:CGRectMake(c.x - d / 2, c.y - d / 2, d, d)];
	spot.userInteractionEnabled = NO;
	spot.layer.cornerRadius = d / 2;
	spot.layer.borderWidth = 1.5;
	spot.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
	spot.backgroundColor = [UIColor colorWithWhite:1 alpha:0.06];   /* faint: blends with any wallpaper */
	UIImageView *glyph = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"touchid"]];
	glyph.tintColor = [UIColor colorWithWhite:1 alpha:0.4];
	glyph.contentMode = UIViewContentModeScaleAspectFit;
	glyph.frame = CGRectInset(spot.bounds, d * 0.2, d * 0.2);
	[spot addSubview:glyph];
	[w.rootViewController.view addSubview:spot];
	return w;
}

/*
 * A match runs also while Settings' Touch ID pane is open, on the passcode keypad and for a moment after an unlock:
 * the ring belongs only to the lock screen itself (locked, keypad not shown). An enrolment (state 1) always shows it.
 */
static BOOL
msgBool(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return o && [o respondsToSelector:s] && ((BOOL (*)(id, SEL))objc_msgSend)(o, s);
}

static id
msgId(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return o && [o respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(o, s) : nil;
}

static BOOL
fingerWanted(void)
{
	id lsm = msgId((id)objc_getClass("SBLockScreenManager"), "sharedInstance");
	if (!msgBool(lsm, "isUILocked")) {
		return NO;
	}
	id cs = msgId(lsm, "coverSheetViewController");
	return !msgBool(cs, "isPasscodeLockVisible") && !msgBool(cs, "isShowingPasscode") && !msgBool(lsm, "isPasscodeEntryVisible");
}

static uint64_t ringState;
static NSTimer *ringTimer;

static void
ringUpdate(void)
{
	BOOL on = ringState == 1 || (ringState == 2 && fingerWanted());
	if (on && ringWindow == nil) {
		ringWindow = makeRingWindow();
		ringWindow.alpha = 0;
	}
	if (ringWindow == nil) {
		return;
	}
	if (on) {
		ringWindow.hidden = NO;
	}
	CGFloat want = on ? 1 : 0;
	if (ringWindow.alpha != want) {
		[UIView animateWithDuration:0.25 animations:^{ ringWindow.alpha = want; } completion:^(BOOL done) {
			if (done && ringWindow.alpha == 0) ringWindow.hidden = YES;
		}];
	}
}

__attribute__((constructor)) static void
fpRingInit(void)
{
	static int token;

	if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.springboard"]) {
		return;
	}

	notify_register_dispatch(RING_NOTIFY, &token, dispatch_get_main_queue(), ^(int t) {
		uint64_t state = 0;
		notify_get_state(t, &state);
		ringState = state;
		ringUpdate();
		if (state && ringTimer == nil) {
			ringTimer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *tm) { ringUpdate(); }];
		} else if (!state) {
			[ringTimer invalidate];
			ringTimer = nil;
		}
	});
}
