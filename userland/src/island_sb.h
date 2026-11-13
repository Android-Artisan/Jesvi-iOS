/*
 * The Dynamic Island's pill placed for the S20's camera hole (island=1; docs/x1slte-dynamic-island.md).
 * Included by src/inject_sb.c: it has to live in libredwood-sb. As a /var/jb tweak the same code cost
 * SpringBoard its CS_VALID (map_with_linking_np: "not code signed"), and SpringBoardUI's +load then
 * threw "This process should not be linking or loading SpringBoardUI.framework": a crash loop.
 *
 * iOS 16.5 hard-codes the pill in SpringBoardUIServices (20F66, iPhone10,3 cache, unslid):
 *   IMP 0x1a33f7598  the collapsed pill's size, {125.0 or 125.333, 36.667} pt
 *   IMP 0x1a33f7954  -[SBUISystemApertureLayoutMetrics _minimumScreenEdgeInset], 11.333 pt
 * Both are found by address among the classes of SpringBoardUIServices and replaced, from the main
 * queue once SpringBoard runs, and only when SpringBoardUIServices is already loaded on its own.
 *
 * Default: top 5 pt, 75 x 28 pt (preset "18pro-small"), user-chosen 2026-11-13 (was 81 x 36.67).
 * /var/jb/etc/redwood-island.conf overrides it, read when SpringBoard starts (a respring applies it):
 *     y=5 w=81 h=36.667     points; any left out keep Apple's value
 *     lh=32                 height while a Live Activity shows (kept when the conf leaves it out)
 *     off                   Apple's pill
 * Log: /var/mobile/Library/Logs/redwood-island.log
 */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#define ISLAND_CONF     "/var/jb/etc/redwood-island.conf"
#define ISLAND_LOG      "/var/mobile/Library/Logs/redwood-island.log"
#define ISLAND_SBUIS    "/System/Library/PrivateFrameworks/SpringBoardUIServices.framework/SpringBoardUIServices"
#define ISLAND_ANCHOR   0x1dd7b9568ULL  /* _OBJC_CLASS_$_SBUISystemApertureLayoutMetrics: gives the slide */
#define ISLAND_SIZE     0x1a33f7598ULL
#define ISLAND_TOP      0x1a33f7954ULL

struct island_size { double width, height; };   /* CGSize */

static double island_w = 75.0, island_h = 28.0, island_y = 5.0;  /* the "18pro-small" pill (user-chosen 2026-11-13) */
/* lh: the island's height while a Live Activity shows (compact / minimal views), user 2026-11-13: "when a live
 * activity is there it should increase its height". Default 32 pt; conf lh=; <= 0: the same rules as the pill. */
static double island_lh = 32.0;
static struct island_size (*island_orig_size)(id, SEL);
static double (*island_orig_top)(id, SEL);

static void
island_say(const char *fmt, ...)
{
	FILE *f = fopen(ISLAND_LOG, "a");
	va_list ap;

	if (f == NULL) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static struct island_size
island_size_imp(id self, SEL cmd)
{
	struct island_size s = island_orig_size(self, cmd);

	if (island_w > 0) {
		s.width = island_w;
	}
	if (island_h > 0) {
		s.height = island_h;
	}
	return s;
}

static double
island_top_imp(id self, SEL cmd)
{
	return island_y >= 0 ? island_y : island_orig_top(self, cmd);
}

/*
 * The rest of the island's geometry (2026-11-13). The pill size alone left iOS's other numbers at the 14 Pro's:
 * the minimal and compact views, the corner radius and the app-to-island ("minimise") animation still used
 * 125 x 36.67, and with a small or round pill an app no longer shrank into the island. Apple's values, read on
 * the phone from +[SBUISystemApertureLayoutMetrics sharedInstanceForEmbeddedDisplay]:
 *   sensorRegionSize, _inertSize, minimumCompactSize   125 x 36.667
 *   minimumMinimalSize, compactConcentricAreaSize       36.667 x 36.667
 *   maximumHorizontalMinimalViewSize                    45 x 36.667
 *   maximumLeadingTrailingViewSize                      88.333 x 36.667, maximumCompactSize 301.667 x 36.667
 *   minimumContinuousCornerRadius                       18.333 (half the height)
 *   interSensorRegionInWindowSpace                      {213.333, 11.333, 9, 36.667}
 *   minimumScreenEdgeInsets                             11.333 all round; _minimumScreenEdgeInset 11.333
 * Mapped by value, so whichever getter returns them: height 36.667 -> h, width 125/125.333 -> w, the square
 * 36.667 -> h, the minimal 45 -> 45 * h / 36.667, radius 18.333 -> h / 2, top 11.333 -> y. The expanded and
 * micro-notice sizes (84, 66.667, 352.333 ...) are left alone.
 */
#define ISLAND_APPLE_H  36.667
#define ISLAND_IS(v, c) ((v) > (c) - 0.05 && (v) < (c) + 0.05)
struct island_rect { double x, y, w, h; };
struct island_insets { double top, leading, bottom, trailing; };
#define ISLAND_MAX_HOOKS 32
static struct { SEL sel; IMP orig; } island_hooks[ISLAND_MAX_HOOKS];
static int island_nhooks;

static IMP
island_orig(SEL cmd)
{
	for (int i = 0; i < island_nhooks; i++) {
		if (island_hooks[i].sel == cmd) {
			return island_hooks[i].orig;
		}
	}
	return NULL;
}

static double
island_eff_w(void)
{
	return island_w > 0 ? island_w : 125.0;
}

static double
island_eff_h(void)
{
	return island_h > 0 ? island_h : ISLAND_APPLE_H;
}

/* live: a getter of the compact / minimal (Live Activity) layout: its height is lh, its widths stay Apple's (a
 * compact activity is at least 125 pt wide, a proper pill around the small resting one) */
static struct island_size
island_map_size(struct island_size s, bool live)
{
	if (live && island_lh > 0 && ISLAND_IS(s.height, ISLAND_APPLE_H)) {
		if (ISLAND_IS(s.width, ISLAND_APPLE_H)) {
			s.width = island_lh;
		} else if (ISLAND_IS(s.width, 45.0)) {
			s.width = 45.0 * island_lh / ISLAND_APPLE_H;
		}
		s.height = island_lh;
		return s;
	}
	if (ISLAND_IS(s.height, ISLAND_APPLE_H)) {
		if (ISLAND_IS(s.width, ISLAND_APPLE_H)) {
			s.width = island_eff_h();
		} else if (ISLAND_IS(s.width, 125.0) || ISLAND_IS(s.width, 125.333)) {
			s.width = island_eff_w();
		} else if (ISLAND_IS(s.width, 45.0)) {
			s.width = 45.0 * island_eff_h() / ISLAND_APPLE_H;
		}
		s.height = island_eff_h();
	}
	return s;
}

static struct island_size
island_any_size_imp(id self, SEL cmd)
{
	IMP o = island_orig(cmd);
	struct island_size s = o ? ((struct island_size (*)(id, SEL))o)(self, cmd) : (struct island_size){ 0, 0 };

	const char *name = sel_getName(cmd);
	bool live = strstr(name, "ompact") != NULL || strstr(name, "Minimal") != NULL || strstr(name, "LeadingTrailing") != NULL;

	return island_map_size(s, live);
}

static double
island_any_double_imp(id self, SEL cmd)
{
	IMP o = island_orig(cmd);
	double v = o ? ((double (*)(id, SEL))o)(self, cmd) : 0;

	if (ISLAND_IS(v, ISLAND_APPLE_H / 2)) {
		return island_eff_h() / 2;
	}
	if (ISLAND_IS(v, 11.333) && island_y >= 0) {
		return island_y;
	}
	return v;
}

static struct island_insets
island_any_insets_imp(id self, SEL cmd)
{
	IMP o = island_orig(cmd);
	struct island_insets v = o ? ((struct island_insets (*)(id, SEL))o)(self, cmd) : (struct island_insets){ 0, 0, 0, 0 };

	if (ISLAND_IS(v.top, 11.333) && island_y >= 0) {
		v.top = island_y;
	}
	return v;
}

static struct island_rect
island_any_rect_imp(id self, SEL cmd)
{
	IMP o = island_orig(cmd);
	struct island_rect r = o ? ((struct island_rect (*)(id, SEL))o)(self, cmd) : (struct island_rect){ 0, 0, 0, 0 };

	if (ISLAND_IS(r.h, ISLAND_APPLE_H)) {
		/* the gap between the two sensors, inside the pill: scaled about the screen's centre */
		Class c = objc_getClass("SBUISystemApertureLayoutMetrics");
		SEL nw = sel_registerName("_nativeScreenWidth");
		double sw = (c && class_getClassMethod(c, nw)) ? ((double (*)(id, SEL))objc_msgSend)((id)c, nw) : 375.0;
		double k = island_eff_w() / 125.0, mid = sw / 2;

		r.x = mid + (r.x - mid) * k;
		r.w = r.w * k;
		r.h = island_eff_h();
		if (ISLAND_IS(r.y, 11.333) && island_y >= 0) {
			r.y = island_y;
		}
	}
	return r;
}

static void
island_hook_geometry(void)
{
	Class c = objc_getClass("SBUISystemApertureLayoutMetrics");
	unsigned n = 0;
	Method *ms = c ? class_copyMethodList(c, &n) : NULL;
	int hooked = 0;

	for (unsigned i = 0; i < n && island_nhooks < ISLAND_MAX_HOOKS; i++) {
		SEL sel = method_getName(ms[i]);
		const char *name = sel_getName(sel), *t = method_getTypeEncoding(ms[i]);
		IMP mine = NULL;

		if (strchr(name, ':') != NULL || t == NULL) {
			continue;
		}
		/* (sensorRegionSize and _minimumScreenEdgeInset: already replaced by address above) */
		if (strcmp(name, "sensorRegionSize") == 0 || strcmp(name, "_minimumScreenEdgeInset") == 0) {
			continue;
		}
		if (strncmp(t, "{CGSize=dd}", 11) == 0) {
			mine = (IMP)island_any_size_imp;
		} else if (strncmp(t, "{CGRect=", 8) == 0) {
			mine = (IMP)island_any_rect_imp;
		} else if (strncmp(t, "{NSDirectionalEdgeInsets=dddd}", 30) == 0) {
			mine = (IMP)island_any_insets_imp;
		} else if (t[0] == 'd' && strstr(name, "Radius") != NULL) {
			mine = (IMP)island_any_double_imp;
		}
		if (mine == NULL) {
			continue;
		}
		island_hooks[island_nhooks].sel = sel;
		island_hooks[island_nhooks].orig = method_setImplementation(ms[i], mine);
		island_nhooks++;
		hooked++;
	}
	free(ms);
	island_say("island: %d more geometry getters follow the pill (w=%g h=%g)", hooked, island_eff_w(), island_eff_h());
}

/* 0: "off" (or island=1 not given) */
static int
island_read_conf(void)
{
	char buf[256];
	FILE *f = fopen(ISLAND_CONF, "r");
	size_t n;

	if (f == NULL) {
		return 1;
	}
	island_y = -1;
	island_w = -1;
	island_h = -1;   /* (a conf names what it changes: what it leaves out is Apple's) */
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (char *p = strtok(buf, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
		if (strcmp(p, "off") == 0) {
			return 0;
		} else if (strncmp(p, "w=", 2) == 0) {
			island_w = atof(p + 2);
		} else if (strncmp(p, "h=", 2) == 0) {
			island_h = atof(p + 2);
		} else if (strncasecmp(p, "y=", 2) == 0) {
			island_y = atof(p + 2);
		} else if (strncmp(p, "lh=", 3) == 0) {
			island_lh = atof(p + 3);
		}
	}
	return 1;
}

static IMP
island_hook_by_address(uintptr_t addr, IMP replacement, const char *what)
{
	unsigned nclasses = 0;
	const char **names = objc_copyClassNamesForImage(ISLAND_SBUIS, &nclasses);
	IMP found = NULL;

	for (unsigned i = 0; names && i < nclasses && !found; i++) {
		Class cls = objc_getClass(names[i]);

		for (int meta = 0; cls && meta < 2 && !found; meta++) {
			Class c = meta ? object_getClass((id)cls) : cls;
			unsigned nm = 0;
			Method *ms = class_copyMethodList(c, &nm);

			for (unsigned j = 0; j < nm; j++) {
				if ((uintptr_t)method_getImplementation(ms[j]) == addr) {
					island_say("island: %s = %c[%s %s], hooked", what, meta ? '+' : '-', names[i],
					    sel_getName(method_getName(ms[j])));
					found = method_setImplementation(ms[j], replacement);
					break;
				}
			}
			free(ms);
		}
	}
	free(names);
	if (!found) {
		island_say("island: %s: no method at %#lx (not the 20F66 cache?), left alone", what, (unsigned long)addr);
	}
	return found;
}

static void
island_hook(void *unused)
{
	uintptr_t anchor = (uintptr_t)objc_lookUpClass("SBUISystemApertureLayoutMetrics"), slide;

	(void)unused;
	if (anchor == 0) {
		island_say("island: SpringBoardUIServices not loaded, left alone");
		return;
	}
	slide = anchor - (uintptr_t)ISLAND_ANCHOR;
	island_say("island: w=%g h=%g y=%g (negative = Apple's), slide %#lx", island_w, island_h, island_y,
	    (unsigned long)slide);
	if (island_w > 0 || island_h > 0) {
		island_orig_size = (struct island_size (*)(id, SEL))island_hook_by_address(ISLAND_SIZE + slide,
		    (IMP)island_size_imp, "pill size");
	}
	if (island_y >= 0) {
		island_orig_top = (double (*)(id, SEL))island_hook_by_address(ISLAND_TOP + slide, (IMP)island_top_imp,
		    "pill top");
	}
	if (island_w > 0 || island_h > 0 || island_y >= 0) {
		island_hook_geometry();
	}
}

/* From the constructor: only with island=1, and the hooks themselves from the main queue. */
static void
island_start(void)
{
	char args[1024];
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return;
	}
	args[size < sizeof(args) ? size : sizeof(args) - 1] = 0;
	if (strstr(args, "island=1") == NULL || !island_read_conf()) {
		return;
	}
	dispatch_async_f(dispatch_get_main_queue(), NULL, island_hook);
}
