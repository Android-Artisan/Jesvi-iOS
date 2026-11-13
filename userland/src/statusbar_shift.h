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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sbs_insets { double top, leading, bottom, trailing; };   /* NSDirectionalEdgeInsets */
static double sbs_lead = 3.0;
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

static void
sbs_hook(void *unused)
{
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
	}
	if (sbs_lead <= 0) {
		return;
	}
	/* (UIKitCore is loaded by the time the main queue runs, in SpringBoard and in apps) */
	dispatch_async_f(dispatch_get_main_queue(), NULL, sbs_hook);
}
