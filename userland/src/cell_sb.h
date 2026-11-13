/*
 * The status bar's cellular icon from the S20's own modem (x1slte, 2026-11-13). Included by inject_sb.c.
 *
 * There is no CommCenter here (Apple's baseband daemon has no Apple baseband to talk to), so SpringBoard's
 * STTelephonyStateProvider (SystemStatusServer) answers "no cellular" and the status bar shows none. Our
 * modem service (work/hackios-cellular tools/cellular/live-modem.c --stay, Samsung IPC over the S5123's
 * shared memory) writes what the modem itself reports to /var/tmp/redwood-cell/state:
 *     bars N / max 4 / dbm -99 / operator Jio / plmn 40585x / reg 3 / rat LTE / updated <time>
 * While that file is fresh (updated within two minutes), the provider answers from it: cellular
 * telephony present, radio active, SIM present in slot 1, and subscription info for slot 1 with the
 * modem's bars, operator, registered status and LTE. Every three seconds the file is looked at, and on a
 * change the provider's observers (the status bar's data provider among them) are told.
 * Without the file everything answers as before. Boot-arg cellbar=0: not even hooked.
 * Also hides the satellite (Stewie) glyph: cell_hide_satellite() below, boot-arg cellsat=0 keeps it.
 * The values of the registration/data-type enums are found by name through SystemStatusServer's own
 * STTelephonyRegistrationStatusDebugName() / STTelephonyDataConnectionTypeDebugName(), not assumed.
 */
#include <sys/stat.h>
#include <time.h>

static struct {
	int valid, bars, max, reg;
	char op[32], plmn[8], rat[8];
	long ios_reg, ios_data;         /* iOS enum values given by the modem service (override the found ones) */
	int data_up;                    /* 1: the modem's data connection carries traffic (LTE glyph); -1 not said */
	long updated;
} cell_state;
static unsigned long cell_reg_registered = ~0UL, cell_data_lte = ~0UL;
static id cell_provider;                        /* the STTelephonyStateProvider SpringBoard made */
static BOOL (*cell_orig_has_telephony)(id, SEL);
static BOOL (*cell_orig_radio_active)(id, SEL);
static BOOL (*cell_orig_radio_dead)(id, SEL);
static BOOL (*cell_orig_sim_present)(id, SEL, long);
static id (*cell_orig_subscription)(id, SEL, long);

static int
cell_read(void)
{
	FILE *f = fopen("/var/tmp/redwood-cell/state", "r");
	char key[32], value[64];
	typeof(cell_state) s;

	memset(&s, 0, sizeof(s));
	s.max = 4;
	s.ios_reg = s.ios_data = -1;
	s.data_up = -1;
	if (f == NULL) {
		return 0;
	}
	while (fscanf(f, "%31s %63s", key, value) == 2) {
		if (!strcmp(key, "bars")) s.bars = atoi(value);
		else if (!strcmp(key, "max")) s.max = atoi(value);
		else if (!strcmp(key, "reg")) s.reg = atoi(value);
		else if (!strcmp(key, "operator")) snprintf(s.op, sizeof(s.op), "%s", value);
		else if (!strcmp(key, "plmn")) snprintf(s.plmn, sizeof(s.plmn), "%s", value);
		else if (!strcmp(key, "rat")) snprintf(s.rat, sizeof(s.rat), "%s", value);
		else if (!strcmp(key, "updated")) s.updated = atol(value);
		else if (!strcmp(key, "ios_reg")) s.ios_reg = atol(value);
		else if (!strcmp(key, "ios_data")) s.ios_data = atol(value);
		else if (!strcmp(key, "data_up")) s.data_up = atoi(value);
	}
	fclose(f);
	s.valid = s.updated != 0 && time(NULL) - s.updated < 120;
	if (s.bars < 0) s.bars = 0;
	if (s.bars > s.max) s.bars = s.max;
	/* a heartbeat alone (only "updated" moved) is not a change */
	typeof(cell_state) a = s, b = cell_state;
	a.updated = b.updated = 0;
	int changed = memcmp(&a, &b, sizeof(a)) != 0;
	cell_state = s;
	return changed;
}

/* the enum value whose debug name contains `word` (e.g. "Registered", "LTE") */
static unsigned long
cell_enum_named(const char *function, const char *word, const char *not_word)
{
	id (*name_of)(unsigned long) = (id (*)(unsigned long))dlsym(RTLD_DEFAULT, function);

	if (name_of == NULL) {
		say("redwood: cellular: %s not found\n", function);
		return ~0UL;
	}
	for (unsigned long v = 0; v < 32; v++) {
		id name = name_of(v);
		const char *c = name ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : NULL;

		if (c != NULL && v < 16) {
			say("redwood: cellular: %s(%lu) = %s\n", function, v, c);
		}
		if (c != NULL && strcasestr(c, word) != NULL && (not_word == NULL || strcasestr(c, not_word) == NULL)) {
			return v;
		}
	}
	return ~0UL;
}

static BOOL
cell_has_telephony(id self, SEL sel)
{
	cell_provider = self;
	return cell_state.valid ? YES : cell_orig_has_telephony(self, sel);
}

static BOOL
cell_radio_active(id self, SEL sel)
{
	cell_provider = self;
	return cell_state.valid ? YES : cell_orig_radio_active(self, sel);
}

static BOOL
cell_radio_dead(id self, SEL sel)
{
	return cell_state.valid ? NO : cell_orig_radio_dead(self, sel);
}

static BOOL
cell_sim_present(id self, SEL sel, long slot)
{
	cell_provider = self;
	return cell_state.valid && slot == 1 ? YES : cell_orig_sim_present(self, sel, slot);
}

static void
cell_set(id o, const char *setter, id value)
{
	SEL s = sel_registerName(setter);

	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), s)) {
		((void (*)(id, SEL, id))objc_msgSend)(o, s, value);
	}
}

static void
cell_set_n(id o, const char *setter, unsigned long value)
{
	SEL s = sel_registerName(setter);

	if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), s)) {
		((void (*)(id, SEL, unsigned long))objc_msgSend)(o, s, value);
	}
}

static id
cell_nsstring(const char *c)
{
	return ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), c);
}

static id
cell_subscription(id self, SEL sel, long slot)
{
	Class mutable_class = objc_getClass("STMutableTelephonySubscriptionInfo");
	id info;

	cell_provider = self;
	if (!cell_state.valid || slot != 1 || mutable_class == Nil) {
		return cell_orig_subscription(self, sel, slot);
	}
	info = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(mutable_class, sel_registerName("alloc")),
	    sel_registerName("init"));
	if (info == nil) {
		return cell_orig_subscription(self, sel, slot);
	}
	cell_set(info, "setIdentifier:", cell_nsstring("redwood-s5123-slot1"));
	cell_set(info, "setSIMLabel:", cell_nsstring("Primary"));
	cell_set(info, "setShortSIMLabel:", cell_nsstring("P"));
	cell_set(info, "setSIMStatus:", cell_nsstring("kCTSIMSupportSIMStatusReady"));
	unsigned long reg = cell_state.ios_reg >= 0 ? (unsigned long)cell_state.ios_reg : cell_reg_registered;
	unsigned long data = cell_state.ios_data >= 0 ? (unsigned long)cell_state.ios_data : cell_data_lte;

	if (reg != ~0UL) {
		cell_set_n(info, "setRegistrationStatus:", reg);
		cell_set_n(info, "setCellularRegistrationStatus:", reg);
	}
	if (data != ~0UL) {
		cell_set_n(info, "setDataConnectionType:", data);
	}
	/* the status bar draws the data glyph (LTE) only for the SIM providing the data connection */
	if (cell_state.data_up >= 0) {
		cell_set_n(info, "setPreferredForDataConnections:", 1);
		cell_set_n(info, "setProvidingDataConnection:", cell_state.data_up ? 1 : 0);
	}
	cell_set_n(info, "setSignalStrengthBars:", (unsigned long)cell_state.bars);
	cell_set_n(info, "setMaxSignalStrengthBars:", (unsigned long)cell_state.max);
	cell_set(info, "setOperatorName:", cell_nsstring(cell_state.op[0] ? cell_state.op : cell_state.plmn));
	cell_set(info, "setLastKnownNetworkCountryCode:", cell_nsstring("in"));
	return info;          /* (autoreleased by the caller's pool is not needed: +1 kept, a few objects a minute) */
}

/* the provider's observers are told, as CoreTelephony's callbacks would */
static void
cell_notify(void)
{
	id provider = cell_provider;
	Ivar iv;
	id table;

	if (provider == nil || (iv = class_getInstanceVariable(object_getClass(provider), "_observers")) == NULL ||
	    (table = object_getIvar(provider, iv)) == nil) {
		return;
	}
	id all = ((id (*)(id, SEL))objc_msgSend)(table, sel_registerName("allObjects"));
	long n = ((long (*)(id, SEL))objc_msgSend)(all, sel_registerName("count"));
	SEL subscription = sel_registerName("subscriptionInfoDidChangeForStateProvider:slot:");
	SEL state = sel_registerName("stateDidChangeForStateProvider:enabled:inactiveEnabled:");

	for (long i = 0; i < n; i++) {
		id o = ((id (*)(id, SEL, long))objc_msgSend)(all, sel_registerName("objectAtIndex:"), i);

		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), state)) {
			((void (*)(id, SEL, id, BOOL, BOOL))objc_msgSend)(o, state, provider, YES, NO);
		}
		if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), subscription)) {
			((void (*)(id, SEL, id, long))objc_msgSend)(o, subscription, provider, 1);
		}
	}
	say("redwood: cellular: %s, %d/%d bars, %s; %ld observers told\n", cell_state.valid ? "modem up" : "no modem",
	    cell_state.bars, cell_state.max, cell_state.op, n);
}

/*
 * Control Center's cellular data button (ConnectivityModule, loaded when Control Center first opens): iOS hides it
 * because MobileGestalt says cellular-data false (no Apple baseband). Its own checks are answered here instead:
 * -_isCellularCapable YES, -_isCellularDataEnabled from the modem service's data_on. The module's cellular methods are
 * listed in the console once (to wire the tap next). Boot-arg cellcc=0: left alone.
 */
static BOOL cell_cc_yes(id self, SEL _cmd) { (void)self; (void)_cmd; return YES; }
static BOOL cell_cc_no(id self, SEL _cmd) { (void)self; (void)_cmd; return NO; }
static BOOL cell_cc_data_on(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	FILE *f = fopen("/var/tmp/redwood-cell/state", "r");
	char key[32], value[64];
	int on = 0;

	if (f != NULL) {
		while (fscanf(f, "%31s %63s", key, value) == 2) {
			if (!strcmp(key, "data_on")) on = atoi(value);
		}
		fclose(f);
	}
	return on ? YES : NO;
}
/* the button itself: the module leaves it out (MobileGestalt: no cellular data), so it is added after the module's
 * own setup, made by the module's own factory; a tap sends data/nodata to the modem service's pipe */
static void (*cell_cc_orig_portrait)(id, SEL), (*cell_cc_orig_landscape)(id, SEL);
static void (*cell_cc_orig_tapped)(id, SEL, id);
static BOOL cell_responds(id o, const char *sel)
{
	return o != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), sel_registerName(sel));
}
static id cell_get(id o, const char *sel) { return ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)); }
static void cell_set1(id o, const char *sel, id v) { ((void (*)(id, SEL, id))objc_msgSend)(o, sel_registerName(sel), v); }
static id
cell_with_inserted(id arr, id obj)
{   /* a mutable copy of arr with obj after the first element (after airplane mode, as on an iPhone) */
	id m = arr != nil ? cell_get(arr, "mutableCopy") :
	    ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSMutableArray"), sel_registerName("array"));
	if (((BOOL (*)(id, SEL, id))objc_msgSend)(m, sel_registerName("containsObject:"), obj)) return nil;
	unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(m, sel_registerName("count"));
	((void (*)(id, SEL, id, unsigned long))objc_msgSend)(m, sel_registerName("insertObject:atIndex:"), obj, n ? 1 : 0);
	return m;
}
static void
cell_cc_add(id self, const char *list)
{
	Class c = objc_getClass("CCUIConnectivityCellularDataViewController");
	id btn;

	if (c == Nil || !cell_responds(self, "_makeButtonWithClass:")) return;
	btn = cell_get(self, "cellularDataButton");
	if (btn == nil) {
		btn = ((id (*)(id, SEL, Class))objc_msgSend)(self, sel_registerName("_makeButtonWithClass:"), c);
		if (btn == nil) return;
		cell_set1(self, "setCellularDataButton:", btn);
	}
	char get[96], set[96];
	snprintf(get, sizeof(get), "%sButtonViewControllers", list);
	snprintf(set, sizeof(set), "set%c%sButtonViewControllers:", list[0] - 32, list + 1);
	id m = cell_with_inserted(cell_get(self, get), btn);
	if (m == nil) return;
	cell_set1(self, set, m);
	id a = cell_with_inserted(cell_get(self, "buttonViewControllers"), btn);
	if (a != nil) cell_set1(self, "setButtonViewControllers:", a);
	say("redwood: cellular: Control Center: cellular data button added (%s)\n", list);
}
static void cell_cc_portrait(id self, SEL _cmd) { cell_cc_orig_portrait(self, _cmd); cell_cc_add(self, "portrait"); }
static void cell_cc_landscape(id self, SEL _cmd) { cell_cc_orig_landscape(self, _cmd); cell_cc_add(self, "landscape"); }
static void
cell_cc_tapped(id self, SEL _cmd, id sender)
{
	(void)_cmd;
	(void)sender;
	BOOL on = cell_cc_data_on(self, NULL);
	int fd = open("/var/tmp/redwood-cell/cmd", O_WRONLY | O_NONBLOCK);
	if (fd >= 0) {
		const char *c = on ? "nodata\n" : "data\n";
		(void)write(fd, c, strlen(c));
		close(fd);
	}
	say("redwood: cellular: Control Center: mobile data %s\n", on ? "off" : "on");
	for (int k = 1; k <= 3; k++)
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)k * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			if (cell_responds(self, "_updateState"))
				((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("_updateState"));
		});
}

static void
cell_cc_scan(void)
{
	static int done, off = -1;
	unsigned n = 0, hooked = 0;

	if (done) return;
	if (off < 0) {
		char args[1024];
		size_t size = sizeof(args) - 1;

		off = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (args[size < sizeof(args) ? size : sizeof(args) - 1] = 0, strstr(args, "cellcc=0") != NULL);
	}
	if (off || objc_getClass("CCUIConnectivityCellularDataViewController") == Nil) return;   /* module not loaded yet */
	Class *all = objc_copyClassList(&n);
	for (unsigned i = 0; i < n; i++) {
		const char *cn = class_getName(all[i]);
		if (strncmp(cn, "CCUIConnectivity", 16)) continue;
		unsigned mc = 0;
		Method *ms = class_copyMethodList(all[i], &mc);
		for (unsigned j = 0; j < mc; j++) {
			const char *sn = sel_getName(method_getName(ms[j]));
			const char *t = method_getTypeEncoding(ms[j]);
			if (strcasestr(sn, "ellular") != NULL || strcasestr(sn, "utton") != NULL || strcasestr(sn, "tap") != NULL)
				say("redwood: cellular: CC %s -%s %s\n", cn, sn, t ? t : "");
			if (t != NULL && t[0] == 'B' && method_getNumberOfArguments(ms[j]) == 2) {
				if (!strcmp(sn, "_isCellularCapable") || !strcmp(sn, "isCellularCapable")) {
					method_setImplementation(ms[j], (IMP)cell_cc_yes);
					hooked++;
				} else if (!strcmp(sn, "_isCellularDataEnabled") || !strcmp(sn, "isCellularDataEnabled")) {
					method_setImplementation(ms[j], (IMP)cell_cc_data_on);
					hooked++;
				}
			}
		}
		free(ms);
	}
	free(all);
	{
		Class mv = objc_getClass("CCUIConnectivityModuleViewController"), cv = objc_getClass("CCUIConnectivityCellularDataViewController");
		Method m1 = mv ? class_getInstanceMethod(mv, sel_registerName("_setupPortraitButtons")) : NULL;
		Method m2 = mv ? class_getInstanceMethod(mv, sel_registerName("_setupLandscapeButtons")) : NULL;
		Method m3 = cv ? class_getInstanceMethod(cv, sel_registerName("buttonTapped:")) : NULL;
		Method m4 = cv ? class_getInstanceMethod(cv, sel_registerName("_toggleState")) : NULL;
		Method m5 = cv ? class_getInstanceMethod(cv, sel_registerName("_isCellularDataRestricted")) : NULL;
		if (m1) { cell_cc_orig_portrait = (void (*)(id, SEL))method_setImplementation(m1, (IMP)cell_cc_portrait); hooked++; }
		if (m2) { cell_cc_orig_landscape = (void (*)(id, SEL))method_setImplementation(m2, (IMP)cell_cc_landscape); hooked++; }
		if (m3) { cell_cc_orig_tapped = (void (*)(id, SEL, id))method_setImplementation(m3, (IMP)cell_cc_tapped); hooked++; }
		if (m4) { method_setImplementation(m4, (IMP)cell_cc_data_on); hooked++; }
		if (m5) { method_setImplementation(m5, (IMP)cell_cc_no); hooked++; }
	}
	done = 1;
	say("redwood: cellular: Control Center module seen, %u check(s) answered\n", hooked);
}

/*
 * Inspection and live answers without a new image (root writes these files; each is consumed once):
 *   /var/tmp/redwood-cell/sb-dump   "CLASSPART [METHODPART]": classes whose name contains CLASSPART, their methods
 *                                   (containing METHODPART), and the loaded images whose path contains CLASSPART,
 *                                   to the console;
 *   /var/tmp/redwood-cell/sb-hook   lines "CLASS SELECTOR YES|NO|DATA": that BOOL getter answers yes, no, or data_on.
 */
static void
cell_sb_tools(void)
{
	char a[128] = "", b[128] = "";
	FILE *f = fopen("/var/tmp/redwood-cell/sb-dump", "r");

	if (f != NULL) {
		int got = fscanf(f, "%127s %127s", a, b);
		fclose(f);
		unlink("/var/tmp/redwood-cell/sb-dump");
		if (got >= 1) {
			uint32_t images = _dyld_image_count();
			for (uint32_t i = 0; i < images; i++) {
				const char *path = _dyld_get_image_name(i);
				if (path != NULL && strcasestr(path, a) != NULL) say("redwood: sb-dump: image %s\n", path);
			}
			unsigned n = 0, shown = 0;
			Class *all = objc_copyClassList(&n);
			for (unsigned i = 0; i < n && shown < 400; i++) {
				const char *cn = class_getName(all[i]);
				if (strcasestr(cn, a) == NULL) continue;
				unsigned mc = 0;
				Method *ms = class_copyMethodList(all[i], &mc);
				say("redwood: sb-dump: class %s (%u methods)\n", cn, mc);
				{
					unsigned cc = 0;
					Method *cm = class_copyMethodList(object_getClass((id)all[i]), &cc);
					for (unsigned j = 0; j < cc; j++) say("redwood: sb-dump:   +%s\n", sel_getName(method_getName(cm[j])));
					free(cm);
				}
				for (unsigned j = 0; j < mc && shown < 400; j++) {
					const char *sn = sel_getName(method_getName(ms[j]));
					if (got < 2 || strcasestr(sn, b) != NULL) {
						say("redwood: sb-dump:   -%s %s\n", sn, method_getTypeEncoding(ms[j]) ? method_getTypeEncoding(ms[j]) : "");
						shown++;
					}
				}
				free(ms);
			}
			free(all);
			say("redwood: sb-dump: done (%s %s)\n", a, b);
		}
	}
	f = fopen("/var/tmp/redwood-cell/sb-hook", "r");
	if (f != NULL) {
		char c[16];
		while (fscanf(f, "%127s %127s %15s", a, b, c) == 3) {
			Class k = objc_getClass(a);
			Method m = k != Nil ? class_getInstanceMethod(k, sel_registerName(b)) : NULL;
			if (m == NULL || method_getTypeEncoding(m) == NULL || method_getTypeEncoding(m)[0] != 'B') {
				say("redwood: sb-hook: %s %s not a BOOL getter here\n", a, b);
				continue;
			}
			method_setImplementation(m, !strcmp(c, "DATA") ? (IMP)cell_cc_data_on : !strcmp(c, "YES") ? (IMP)cell_cc_yes : (IMP)cell_cc_no);
			say("redwood: sb-hook: %s %s -> %s\n", a, b, c);
		}
		fclose(f);
		unlink("/var/tmp/redwood-cell/sb-hook");
	}
}

static void
cell_tick(void *context)
{
	static id told;

	(void)context;
	if (cell_read() || (cell_provider != nil && cell_provider != told)) {
		cell_notify();
		told = cell_provider;
	}
	cell_cc_scan();
	cell_sb_tools();
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, cell_tick);
}

/*
 * The satellite (Stewie) glyph. With no CommCenter, SystemStatus decides there is no cellular service and
 * offers emergency SOS via satellite, and the status bar and Control Center draw the satellite. This phone
 * has no satellite radio: STStewieStatusDomainData answers inactive / not connected and
 * STTelephonyStatusDomainData not using Stewie for SOS. Boot-arg cellsat=0: left alone.
 */
static BOOL
cell_no(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return NO;
}

static void
cell_hide_satellite(void)
{
	struct { const char *cls, *sel; } off[] = {
		{ "STStewieStatusDomainData", "isStewieActive" },
		{ "STStewieStatusDomainData", "isStewieConnected" },
		{ "STTelephonyStatusDomainData", "isUsingStewieForSOS" },
	};
	unsigned done = 0;

	for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
		Class c = objc_getClass(off[i].cls);
		Method m = c != Nil ? class_getInstanceMethod(c, sel_registerName(off[i].sel)) : NULL;

		if (m != NULL && method_getTypeEncoding(m) != NULL && method_getTypeEncoding(m)[0] == 'B') {
			method_setImplementation(m, (IMP)cell_no);
			done++;
		}
	}
	say("redwood: cellular: satellite glyph off (%u of 3 answers)\n", done);
}

static void
cell_hook(void *context)
{
	Class provider = objc_getClass("STTelephonyStateProvider");
	struct { const char *sel; IMP ours; void **orig; } hooks[] = {
		{ "hasCellularTelephony", (IMP)cell_has_telephony, (void **)&cell_orig_has_telephony },
		{ "isCellularRadioCapabilityActive", (IMP)cell_radio_active, (void **)&cell_orig_radio_active },
		{ "isRadioModuleDead", (IMP)cell_radio_dead, (void **)&cell_orig_radio_dead },
		{ "isSIMPresentForSlot:", (IMP)cell_sim_present, (void **)&cell_orig_sim_present },
		{ "subscriptionInfoForSlot:", (IMP)cell_subscription, (void **)&cell_orig_subscription },
	};

	(void)context;
	{
		char args[1024];
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (args[size < sizeof(args) ? size : sizeof(args) - 1] = 0,
		    strstr(args, "cellsat=0") == NULL)) {
			cell_hide_satellite();
		}
	}
	if (provider == Nil || objc_getClass("STMutableTelephonySubscriptionInfo") == Nil) {
		/* SystemStatus not loaded yet: try again soon (hooking late lets SpringBoard cache "no cellular") */
		static int tries;
		if (++tries < 120) {
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), NULL, cell_hook);
		} else {
			say("redwood: cellular: no STTelephonyStateProvider here; status bar left alone\n");
		}
		return;
	}
	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		if (class_getInstanceMethod(provider, sel_registerName(hooks[i].sel)) == NULL) {
			say("redwood: cellular: -[STTelephonyStateProvider %s] missing; status bar left alone\n", hooks[i].sel);
			return;
		}
	}
	cell_reg_registered = cell_enum_named("STTelephonyRegistrationStatusDebugName", "registered", "not");
	cell_data_lte = cell_enum_named("STTelephonyDataConnectionTypeDebugName", "lteConnection", "Plus");
	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		Method m = class_getInstanceMethod(provider, sel_registerName(hooks[i].sel));

		*hooks[i].orig = (void *)method_setImplementation(m, hooks[i].ours);
	}
	say("redwood: cellular: status bar follows /var/tmp/redwood-cell/state (registered=%ld, LTE=%ld)\n",
	    (long)cell_reg_registered, (long)cell_data_lte);
	cell_tick(NULL);
}

static void
cell_start(void)
{
	char args[1024];
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		args[size < sizeof(args) ? size : sizeof(args) - 1] = 0;
		if (strstr(args, "cellbar=0") != NULL) {
			return;
		}
	}
	dispatch_async_f(dispatch_get_main_queue(), NULL, cell_hook);   /* as early as SpringBoard's main queue runs */
}
