/*
 * The Phone app's calls go out over VoLTE (A, 2026-11-14). There is no CommCenter: the telephony call provider has
 * nothing behind it, so a dial from the Phone app (and Contacts, Recents) is handed to RedwoodPhone instead, the
 * CallKit provider of redwood-ims (tools/cellular/RedwoodPhone): -[TUDialRequest provider] answers RedwoodPhone's
 * provider wherever it would answer the telephony one. callservicesd then sends RedwoodPhone a CXStartCallAction.
 * In MobilePhone and InCallService (dials made from the in-call screen), from libredwood.dylib's constructor.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>

#define PHONE_PROVIDER_ID "com.redwood.phone"

static IMP phone_orig_provider;

static id
phone_our_provider(void)
{
	static id ours;
	if (ours) return ours;
	Class cc = objc_getClass("TUCallCenter");
	id center = cc ? ((id (*)(id, SEL))objc_msgSend)((id)cc, sel_registerName("sharedInstance")) : nil;
	id mgr = center ? ((id (*)(id, SEL))objc_msgSend)(center, sel_registerName("providerManager")) : nil;
	if (!mgr) return nil;
	id idstr = ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), PHONE_PROVIDER_ID);
	const char *sels[] = { "providerWithIdentifier:", "providerForIdentifier:" };
	for (unsigned i = 0; i < 2 && !ours; i++) {
		SEL s = sel_registerName(sels[i]);
		if (class_respondsToSelector(object_getClass(mgr), s)) ours = ((id (*)(id, SEL, id))objc_msgSend)(mgr, s, idstr);
	}
	return ours;
}

static id
phone_provider(id self, SEL _cmd)
{
	id p = ((id (*)(id, SEL))phone_orig_provider)(self, _cmd);
	SEL tel = sel_registerName("isTelephonyProvider");
	if (p == nil || (class_respondsToSelector(object_getClass(p), tel) && ((BOOL (*)(id, SEL))objc_msgSend)(p, tel))) {
		id ours = phone_our_provider();
		if (ours) return ours;
	}
	return p;
}

static void
phone_dial_hook(void)
{
	static int tries;
	Class c = objc_getClass("TUDialRequest");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("provider")) : NULL;
	if (!m) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_dial_hook(); });
		return;
	}
	if (!phone_orig_provider) phone_orig_provider = method_setImplementation(m, (IMP)phone_provider);
}

/*
 * RedwoodPhone's calls are the phone's own calls: in InCallService and SpringBoard its provider answers YES to
 * isSystemProvider / isTelephonyProvider, so iOS gives them its full call screen (as a third-party CallKit app's
 * calls they were kept in the Dynamic Island, the user, 2026-11-14 04:22). Not in MobilePhone: there the telephony
 * test above is what sends a dial to RedwoodPhone.
 */
static IMP phone_orig_system, phone_orig_tel;

static int
phone_is_ours(id provider)
{
	SEL ident = sel_registerName("identifier");
	if (!class_respondsToSelector(object_getClass(provider), ident)) return 0;
	id s = ((id (*)(id, SEL))objc_msgSend)(provider, ident);
	const char *c = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : NULL;
	return c && !strcmp(c, PHONE_PROVIDER_ID);
}
static BOOL phone_system(id self, SEL _cmd) { return phone_is_ours(self) ? YES : ((BOOL (*)(id, SEL))phone_orig_system)(self, _cmd); }
static BOOL phone_tel(id self, SEL _cmd) { return phone_is_ours(self) ? YES : ((BOOL (*)(id, SEL))phone_orig_tel)(self, _cmd); }

static void
phone_system_hook(void)
{
	static int tries;
	Class c = objc_getClass("TUCallProvider");
	Method a = c ? class_getInstanceMethod(c, sel_registerName("isSystemProvider")) : NULL;
	Method b = c ? class_getInstanceMethod(c, sel_registerName("isTelephonyProvider")) : NULL;
	if (!a || !b) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_system_hook(); });
		return;
	}
	if (!phone_orig_system) phone_orig_system = method_setImplementation(a, (IMP)phone_system);
	if (!phone_orig_tel) phone_orig_tel = method_setImplementation(b, (IMP)phone_tel);
}

/*
 * The call screen's Speaker button (A, 2026-11-14). The audio plug-in has no receiver port that iOS knows, so the
 * button has nothing to switch between; in InCallService a tap on it (TURouteController pickRoute:) toggles our route
 * instead: notify com.redwood.call.speaker / .earpiece, which redwood-ims follows (kern.redwood.snd_route), and the
 * button shows it (TURoute isSpeaker). Every call starts on the earpiece (redwood-ims posts com.redwood.call.started).
 */
#include <notify.h>
static int phone_speaker;
static IMP phone_orig_pick, phone_orig_pick_id, phone_orig_is_speaker;

static void
phone_toggle_speaker(void)
{
	phone_speaker = !phone_speaker;
	notify_post(phone_speaker ? "com.redwood.call.speaker" : "com.redwood.call.earpiece");
}
static void phone_pick(id self, SEL _cmd, id route) { phone_toggle_speaker(); ((void (*)(id, SEL, id))phone_orig_pick)(self, _cmd, route); }
static void phone_pick_id(id self, SEL _cmd, id ident) { phone_toggle_speaker(); ((void (*)(id, SEL, id))phone_orig_pick_id)(self, _cmd, ident); }
static BOOL phone_is_speaker(id self, SEL _cmd) { return ((BOOL (*)(id, SEL))phone_orig_is_speaker)(self, _cmd) ? (BOOL)phone_speaker : NO; }

static void
phone_speaker_hook(void)
{
	static int tries;
	Class rc = objc_getClass("TURouteController"), r = objc_getClass("TURoute");
	Method a = rc ? class_getInstanceMethod(rc, sel_registerName("pickRoute:")) : NULL;
	Method b = rc ? class_getInstanceMethod(rc, sel_registerName("pickRouteWithUniqueIdentifier:")) : NULL;
	Method c = r ? class_getInstanceMethod(r, sel_registerName("isSpeaker")) : NULL;
	if (!a || !b || !c) {
		if (tries++ < 20) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ phone_speaker_hook(); });
		return;
	}
	if (!phone_orig_pick) phone_orig_pick = method_setImplementation(a, (IMP)phone_pick);
	if (!phone_orig_pick_id) phone_orig_pick_id = method_setImplementation(b, (IMP)phone_pick_id);
	if (!phone_orig_is_speaker) phone_orig_is_speaker = method_setImplementation(c, (IMP)phone_is_speaker);
	int t;
	notify_register_dispatch("com.redwood.call.started", &t, dispatch_get_main_queue(), ^(int tok) { (void)tok; phone_speaker = 0; });
}
