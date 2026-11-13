/*
 * The status bar's left side a little nearer the screen edge (user, 2026-11-13: "the carrier name or the time ...
 * can go a bit more to the left, just a tiny bit"). With island=1 iOS lays the status bar out with
 * _UIStatusBarVisualProvider_DynamicSplit (the 14 Pro's); its -edgeInsets' leading value is the gap at the left.
 * That gap is made smaller by `lead` points (default 3). The status bar is drawn in SpringBoard (home screen) and
 * in every app, so this is included by libredwood-sb (inject_sb.c) and libredwood (inject.c).
 *   /var/jb/etc/redwood-statusbar.conf: "lead=N" (points; 0 = Apple's). Read at process start: respring / reopen apps.
 */
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sbs_insets { double top, leading, bottom, trailing; };   /* NSDirectionalEdgeInsets */
static double sbs_lead = 20.0;   /* (user-verified 2026-11-13 with cut=125; was 3) */
static struct sbs_insets (*sbs_orig)(id, SEL);

static struct sbs_insets
sbs_edge_insets(id self, SEL cmd)
{
	struct sbs_insets v = sbs_orig(self, cmd);

	/* (allowed below 0: the time is centred in the left area, so the area's start must go past the edge for the
	 * time to move by more than half of what is taken off -- user 2026-11-13: "push it further left") */
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

static void
sbs_hook(void *unused)
{
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
	}
}

static void
statusbar_shift_start(void)
{
	FILE *f = fopen("/var/jb/etc/redwood-statusbar.conf", "r");
	char b[64];

	if (f != NULL) {
		size_t n = fread(b, 1, sizeof(b) - 1, f);
		fclose(f);
		b[n] = 0;
		char *p = strstr(b, "lead=");
		if (p != NULL) {
			sbs_lead = atof(p + 5);
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
