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
