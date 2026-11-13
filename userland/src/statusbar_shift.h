/*
 * The status bar's left side a little nearer the screen edge (user, 2026-11-13: "the carrier name or the time ...
 * can go a bit more to the left, just a tiny bit"). With island=1 iOS lays the status bar out with
 * _UIStatusBarVisualProvider_DynamicSplit (the 14 Pro's); its -edgeInsets' leading value is the gap at the left.
 * That gap is made smaller by `lead` points (default 3). The status bar is drawn in SpringBoard (home screen) and
 * in every app, so this is included by libredwood-sb (inject_sb.c) and libredwood (inject.c).
 *   /var/jb/etc/redwood-statusbar.conf: "lead=N" (points; 0 = Apple's). Read at process start: respring / reopen apps.
 */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sbs_insets { double top, leading, bottom, trailing; };   /* NSDirectionalEdgeInsets */
static double sbs_lead = 16.0;   /* (user 2026-11-13 22:10: a little more padding both sides; was 20) */
static struct sbs_insets (*sbs_orig)(id, SEL);

static void
sbs_log(const char *fmt, double a, double b)
{
	FILE *f = fopen("/var/mobile/Library/Logs/redwood-statusbar.log", "a");

	if (f != NULL) {
		fprintf(f, "statusbar: pid %d ", getpid());
		fprintf(f, fmt, a, b);
		fputc('\n', f);
		fclose(f);
	}
}

static struct sbs_insets
sbs_edge_insets(id self, SEL cmd)
{
	struct sbs_insets v = sbs_orig(self, cmd);

	/* (allowed below 0: the time is centred in the left area, so the area's start must go past the edge for the
	 * time to move by more than half of what is taken off -- user 2026-11-13: "push it further left") */
	{
		static int told;
		if (told++ < 2) {
			sbs_log("edgeInsets leading %.1f -> %.1f", v.leading, v.leading - sbs_lead);
		}
	}
	v.leading -= sbs_lead;
	return v;
}

/*
 * The cutout the status bar lays itself out around (user 2026-11-13: "still has padding to the left"). The time is
 * centred in the area between the left edge and the island; with the S20's small punch-hole pill (21 pt) that area
 * is far wider than a 14 Pro's, so the time sat near the middle of it. The cutout width the status bar uses is held
 * at least `cut` points (default 125, Apple's pill; "cut=0" in the conf: as iOS computes it), which puts the time
 * where an iPhone has it; `lead` then moves it further left. Only the status bar's layout: the island is untouched.
 */
struct sbs_rect { double x, y, w, h; };
static double sbs_cut = 125.0;
static bool (*sbs_orig_avoid)(id, SEL, struct sbs_rect, double *, double *);

static bool
sbs_avoid(id self, SEL cmd, struct sbs_rect frame, double *offset, double *width)
{
	bool changed = sbs_orig_avoid(self, cmd, frame, offset, width);

	static int told;

	if (width != NULL && *width > 0 && *width < sbs_cut) {
		if (told < 3 && getpid() > 0) {
			FILE *f = fopen("/var/mobile/Library/Logs/redwood-statusbar.log", "a");   /* (apps' sandbox: fails, fine) */
			if (f != NULL) {
				fprintf(f, "statusbar: pid %d cutout %.1f -> %.1f (lead %.1f)\n", getpid(), *width, sbs_cut, sbs_lead);
				fclose(f);
			}
			told++;
		}
		*width = sbs_cut;
		changed = true;
	}
	return changed;
}

/*
 * At boot the status bar creates its cutout-width constraint straight from the island's size and only goes through
 * _needsUpdateOfConstraintsForAvoidanceFrame: when the island later changes (2026-11-13: the log stayed empty after a
 * boot and the time sat at Apple's place). So the constraint is also widened when it is handed over.
 */
static void (*sbs_orig_set_cut)(id, SEL, id);

static void
sbs_widen(id constraint, const char *why)
{
	SEL get = sel_registerName("constant"), set = sel_registerName("setConstant:");
	double c;

	if (constraint == nil || sbs_cut <= 0 || !class_respondsToSelector(object_getClass(constraint), get)) {
		return;
	}
	c = ((double (*)(id, SEL))objc_msgSend)(constraint, get);
	{
		static int told;
		if (told++ < 4) {
			sbs_log("cutout constraint constant %.1f (cut %.1f)", c, sbs_cut);
		}
	}
	if (c > 0 && c < sbs_cut) {
		((void (*)(id, SEL, double))objc_msgSend)(constraint, set, sbs_cut);
		FILE *f = fopen("/var/mobile/Library/Logs/redwood-statusbar.log", "a");
		if (f != NULL) {
			fprintf(f, "statusbar: pid %d %s %.1f -> %.1f\n", getpid(), why, c, sbs_cut);
			fclose(f);
		}
	}
}

static void
sbs_set_cut(id self, SEL cmd, id constraint)
{
	sbs_orig_set_cut(self, cmd, constraint);
	sbs_widen(constraint, "cutout constraint");
}

static id (*sbs_orig_setup)(id, SEL, id);

static id
sbs_setup(id self, SEL cmd, id container)
{
	id r = sbs_orig_setup(self, cmd, container);
	id c = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("cutoutWidthConstraint"));

	sbs_widen(c, "setup");
	return r;
}

/*
 * The layout this phone really uses (2026-11-13, found by logging: DynamicSplit's methods were never called): the
 * screen is the iPhone X's (1125 px), so UIKit picks _UIStatusBarVisualProvider_Split1125, the notch layout, even with
 * the island on. It centres the time in the 83 pt left of a 209 pt notch, nudged by +leadingCenteringOffset
 * (-0.667). That nudge is moved left by lead/2 (the same feel `lead` had: lead=20 -> about 10 pt left).
 */
static double (*sbs_orig_lco)(id, SEL);

static double
sbs_lco(id self, SEL cmd)
{
	double v = sbs_orig_lco(self, cmd), r = v - sbs_lead / 2 - sbs_notch_shift;
	static int told;

	if (told++ < 2) {
		sbs_log("Split1125 leadingCenteringOffset %.2f -> %.2f", v, r);
	}
	return r;
}

/*
 * UIKit derives the right side from the left: trailingCenteringEdgeInset = leadingCenteringEdgeInset +
 * +trailingCenteringOffset (UIKitCore 0x189ab0cd0). Moving the time left therefore pushed the battery side right by
 * the same amount (user 2026-11-13: "the battery ... too much to the right"). It is given back here, plus `trail`
 * points more inward (conf trail=, default 0). Added on Split1125's own metaclass (the method is inherited).
 */
static double sbs_trail = 2;     /* (same: the right side 2 pt further in) */
static double (*sbs_orig_tco)(id, SEL);

static double
sbs_tco(id self, SEL cmd)
{
	double v = sbs_orig_tco(self, cmd), r = v + sbs_lead / 2 + sbs_trail;    /* (no notch shift: it is derived from the left, already corrected) */
	static int told;

	if (told++ < 2) {
		sbs_log("Split1125 trailingCenteringOffset %.2f -> %.2f", v, r);
	}
	return r;
}

/*
 * The side areas are (screen width - notchSize.width) / 2 wide (UIKitCore 0x189ab1044): with the iPhone X's 209 pt
 * notch the left one is ~83 pt, so a longer carrier text ("... | Jio" on the lock screen) scrolled although the S20's
 * hole is small (user 2026-11-13). +notchSize's width is reported as `notch` (conf notch=, default 125; 0 = Apple's).
 * The centring points move inward by (209 - notch) / 4 with it (leadingCenteringEdgeInset = notchRect.x / 2 + ...),
 * so lco/tco take that back: the time and the battery stay where lead/trail put them.
 */
struct sbs_size { double w, h; };
static double sbs_notch = 125;
static struct sbs_size (*sbs_orig_notch)(id, SEL);
static double sbs_notch_shift;          /* (orig - notch) / 4, set once the original is known */

static struct sbs_size
sbs_notch_size(id self, SEL cmd)
{
	struct sbs_size v = sbs_orig_notch(self, cmd);

	if (sbs_notch > 0 && sbs_notch < v.w) {
		sbs_notch_shift = (v.w - sbs_notch) / 4;
		v.w = sbs_notch;
	}
	return v;
}

static void
sbs_hook(void *unused)
{
	{
		Class sc = objc_getClass("_UIStatusBarVisualProvider_Split1125");
		Method nm = sc ? class_getClassMethod(sc, sel_registerName("notchSize")) : NULL;
		if (nm != NULL && sbs_orig_notch == NULL && sbs_notch > 0) {
			sbs_orig_notch = (struct sbs_size (*)(id, SEL))method_getImplementation(nm);
			struct sbs_size o = sbs_orig_notch((id)sc, sel_registerName("notchSize"));
			if (sbs_notch < o.w) {
				sbs_notch_shift = (o.w - sbs_notch) / 4;
			}
			if (!class_addMethod(object_getClass((id)sc), sel_registerName("notchSize"), (IMP)sbs_notch_size,
			    method_getTypeEncoding(nm))) {
				method_setImplementation(nm, (IMP)sbs_notch_size);
			}
		}
	}
	{
		Class sc = objc_getClass("_UIStatusBarVisualProvider_Split1125");
		SEL ts = sel_registerName("trailingCenteringOffset");
		Method tm = sc ? class_getClassMethod(sc, ts) : NULL;
		if (tm != NULL && sbs_orig_tco == NULL && (sbs_lead > 0 || sbs_trail != 0)) {
			sbs_orig_tco = (double (*)(id, SEL))method_getImplementation(tm);
			if (!class_addMethod(object_getClass((id)sc), ts, (IMP)sbs_tco, method_getTypeEncoding(tm))) {
				method_setImplementation(class_getClassMethod(sc, ts), (IMP)sbs_tco);   /* (it was Split1125's own) */
			}
		}
	}
	{
		Class sc = objc_getClass("_UIStatusBarVisualProvider_Split1125");
		Method lm = sc ? class_getClassMethod(sc, sel_registerName("leadingCenteringOffset")) : NULL;
		if (lm != NULL && sbs_orig_lco == NULL && sbs_lead > 0) {
			sbs_orig_lco = (double (*)(id, SEL))method_setImplementation(lm, (IMP)sbs_lco);
		}
	}
	{
		Class dc = objc_getClass("_UIStatusBarVisualProvider_DynamicSplit");
		Method um = dc ? class_getInstanceMethod(dc, sel_registerName("setupInContainerView:")) : NULL;
		if (um != NULL && sbs_orig_setup == NULL && sbs_cut > 0) {
			sbs_orig_setup = (id (*)(id, SEL, id))method_setImplementation(um, (IMP)sbs_setup);
		}
	}
	{
		Class dc = objc_getClass("_UIStatusBarVisualProvider_DynamicSplit");
		Method sm = dc ? class_getInstanceMethod(dc, sel_registerName("setCutoutWidthConstraint:")) : NULL;
		if (sm != NULL && sbs_orig_set_cut == NULL && sbs_cut > 0) {
			sbs_orig_set_cut = (void (*)(id, SEL, id))method_setImplementation(sm, (IMP)sbs_set_cut);
		}
	}
	{
		Class dc = objc_getClass("_UIStatusBarVisualProvider_DynamicSplit");
		Method am = dc ? class_getInstanceMethod(dc,
		    sel_registerName("_needsUpdateOfConstraintsForAvoidanceFrame:cutoutOffset:cutoutWidth:")) : NULL;
		if (am != NULL && sbs_orig_avoid == NULL && sbs_cut > 0) {
			sbs_orig_avoid = (bool (*)(id, SEL, struct sbs_rect, double *, double *))method_setImplementation(am,
			    (IMP)sbs_avoid);
		}
	}
	Class c = objc_getClass("_UIStatusBarVisualProvider_DynamicSplit");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("edgeInsets")) : NULL;

	(void)unused;
	if (m != NULL && sbs_orig == NULL) {
		sbs_orig = (struct sbs_insets (*)(id, SEL))method_setImplementation(m, (IMP)sbs_edge_insets);
		sbs_log("hooks: edgeInsets %.0f, avoidance/setup/constraint %.0f", 1, sbs_orig_avoid && sbs_orig_setup && sbs_orig_set_cut);
	} else if (m == NULL) {
		static int told;
		if (told++ < 1) {
			sbs_log("hooks: DynamicSplit not there yet (%.0f %.0f)", unused != NULL, 0);
		}
	}
}

static void
statusbar_shift_start(void)
{
	FILE *f = fopen("/var/jb/etc/redwood-statusbar.conf", "r");
	char b[128];

	if (f != NULL) {
		size_t n = fread(b, 1, sizeof(b) - 1, f);
		fclose(f);
		b[n] = 0;
		char *p = strstr(b, "lead=");
		if (p != NULL) {
			sbs_lead = atof(p + 5);
		}
		p = strstr(b, "notch=");
		if (p != NULL) {
			sbs_notch = atof(p + 6);
		}
		p = strstr(b, "trail=");
		if (p != NULL) {
			sbs_trail = atof(p + 6);
		}
		p = strstr(b, "cut=");
		if (p != NULL) {
			sbs_cut = atof(p + 4);
		}
	}
	if (sbs_lead <= 0 && sbs_cut <= 0) {
		return;
	}
	/*
	 * At once, from the library's constructor: UIKitCore is mapped (a dependency of every UI process) and its classes
	 * are registered before any constructor runs. (2026-11-13: hooked from the main queue only, the status bar could
	 * be laid out first; the cutout is only re-measured when the island changes, so after a boot the time stayed at
	 * Apple's place until a Live Activity came and went.) The main queue again as a fallback.
	 */
	sbs_hook(NULL);
	dispatch_async_f(dispatch_get_main_queue(), NULL, sbs_hook);
}
