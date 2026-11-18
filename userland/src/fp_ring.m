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

#define RING_NOTIFY "com.redwood.touchid.sensor"

static UIWindow *ringWindow;

static UIWindow *
makeRingWindow(void)
{
	CGRect screen = UIScreen.mainScreen.bounds;
	UIWindow *w = nil;

	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
		if ([scene isKindOfClass:UIWindowScene.class]) {
			w = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
			break;
		}
	}
	if (w == nil) {
		w = [[UIWindow alloc] initWithFrame:screen];   /* SpringBoard may have no window scene of its own */
	}
	w.frame = screen;
	w.windowLevel = UIWindowLevelStatusBar + 1000;
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
	spot.layer.borderWidth = 2.5;
	spot.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.9].CGColor;
	spot.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
	UIImageView *glyph = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"touchid"]];
	glyph.tintColor = UIColor.whiteColor;
	glyph.contentMode = UIViewContentModeScaleAspectFit;
	glyph.frame = CGRectInset(spot.bounds, d * 0.2, d * 0.2);
	[spot addSubview:glyph];
	[w.rootViewController.view addSubview:spot];
	return w;
}

static void
ringShow(BOOL on)
{
	if (on && ringWindow == nil) {
		ringWindow = makeRingWindow();
	}
	if (ringWindow == nil) {
		return;
	}
	ringWindow.hidden = !on;
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
		ringShow(state != 0);
	});
}
