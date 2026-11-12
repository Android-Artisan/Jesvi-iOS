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
 * Default: top 5 pt (covers the top of the hole), width 81 pt ("iPhone 18 Pro"), user-chosen 2026-11-12.
 * /var/jb/etc/redwood-island.conf overrides it, read when SpringBoard starts (a respring applies it):
 *     y=5 w=81 h=36.667     points; any left out keep Apple's value
 *     off                   Apple's pill
 * Log: /var/mobile/Library/Logs/redwood-island.log
 */
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <stdarg.h>
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

static double island_w = 81.0, island_h = -1, island_y = 5.0;  /* 81: "iPhone 18 Pro" pill (user-chosen) */
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
		} else if (strncmp(p, "y=", 2) == 0) {
			island_y = atof(p + 2);
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
