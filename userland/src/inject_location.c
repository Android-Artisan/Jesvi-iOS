/*
 * libredwood.dylib inside locationd: the compass (and device motion) without Apple's motion coprocessor.
 *
 * locationd picks its compass from a table of capabilities per iPhone model, CLPlatformInfo::queryMotionCaps().
 * For the iPhone X we pass for, bits 4 and 5 of that word say "device motion comes fused from the SPU"
 * (the always-on processor), so CLCompass7 is built: it waits for the SPU's DeviceMotion HID stream (page
 * 0xFF0C) that this phone has no source for, and the Compass app sits on north. Without those bits locationd
 * builds CLCompass6 instead ("Legacy compass" in its log), which computes the heading itself from the raw
 * magnetometer and accelerometer that src/inject_sensors.c already delivers.
 *
 * The word is computed once, behind dispatch_once, and kept in locationd's __DATA. locationd calls
 * dispatch_once through its GOT, so the GOT slot is pointed at a wrapper that clears the two bits right
 * after the first computation. Data only: patching locationd's code would make the kernel take CS_VALID away
 * (cs_invalid_page), and without it SecTaskLoadEntitlements fails and CloudKit traps the daemon at start.
 *
 * Addresses are those of /usr/libexec/locationd of iOS 16.5 (20F66); each is checked before it is used.
 * Boot-arg loc_swfusion=0 leaves locationd alone.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define MOTION_CAPS_FN          0x1003b5734UL   /* CLPlatformInfo::motionCaps() */
#define MOTION_CAPS_TOKEN       0x101cdf358UL   /* its dispatch_once token */
#define MOTION_CAPS_WORD        0x101cdf360UL   /* the cached capability word */
#define GOT_DISPATCH_ONCE       0x101b3f7f0UL   /* __DATA,__got slot the dispatch_once stub loads */
#define CAPS_SPU_FUSION         0xb8ULL         /* SPU fusion: 3 6-axis, 7 3-axis, 4 9-axis, 5 compass (CLCompass7) */

static void *caps_token;
static volatile uint64_t *caps_word;
static void (*real_dispatch_once)(dispatch_once_t *, dispatch_block_t);

static void
location_dispatch_once(dispatch_once_t *predicate, dispatch_block_t block)
{
	real_dispatch_once(predicate, block);
	if ((void *)predicate == caps_token && (*caps_word & CAPS_SPU_FUSION) != 0) {
		uint64_t old = *caps_word;

		*caps_word = old & ~CAPS_SPU_FUSION;
		fprintf(stderr, "redwood: locationd: motion caps %#llx -> %#llx: software compass and device motion\n",
		    old, *caps_word);
	}
}

void
redwood_location_fixes(void)
{
	static const uint32_t caps_fn_code[4] = { 0xa9bf7bfd, 0x910003fd, 0xd000c948, 0xf941ad08 };
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	intptr_t slide = 0;
	bool found = false;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "loc_swfusion=0") != NULL) {
		fprintf(stderr, "redwood: locationd: loc_swfusion=0, left alone\n");
		return;
	}
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t length = name != NULL ? strlen(name) : 0;

		if (length >= 10 && strcmp(name + length - 10, "/locationd") == 0) {
			slide = _dyld_get_image_vmaddr_slide(i);
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "redwood: locationd: own image not found\n");
		return;
	}
	if (memcmp((const void *)(MOTION_CAPS_FN + slide), caps_fn_code, sizeof(caps_fn_code)) != 0) {
		fprintf(stderr, "redwood: locationd: not the 20F66 locationd, left alone\n");
		return;
	}
	void **got = (void **)(GOT_DISPATCH_ONCE + slide);
	void *once = dlsym(RTLD_DEFAULT, "dispatch_once");

	if (once == NULL || *got != once) {
		fprintf(stderr, "redwood: locationd: GOT slot holds %p, not dispatch_once %p; left alone\n", *got, once);
		return;
	}
	caps_token = (void *)(MOTION_CAPS_TOKEN + slide);
	caps_word = (volatile uint64_t *)(MOTION_CAPS_WORD + slide);
	real_dispatch_once = (void (*)(dispatch_once_t *, dispatch_block_t))once;

	vm_address_t page = (vm_address_t)got & ~(vm_address_t)(vm_page_size - 1);

	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS &&
	    vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		fprintf(stderr, "redwood: locationd: GOT not writable\n");
		return;
	}
	*got = (void *)location_dispatch_once;
}

/*
 * The same table again, inside apps: CoreMotion.framework (dyld shared cache) carries its own
 * CLPlatformInfo::motionCaps() (0x1919a4580), and with the SPU bits set an app's CMMotionManager reads fused
 * device motion straight from the SPU's HID stream in its own process -- the Compass app's dial among them.
 * Its word (0x1e20a7290) and dispatch_once token (0x1e20a7288) live in the cache's __DATA_DIRTY. Before
 * CoreMotion computes them, they are filled in: the iPhone X word that locationd computes, without the SPU
 * fusion bits, and the token marked done. CoreMotion then asks locationd, whose own fusion now runs.
 */
#define CM_MOTION_CAPS_FN       0x1919a4580UL
#define CM_MOTION_CAPS_TOKEN    0x1e20a7288UL
#define CM_MOTION_CAPS_WORD     0x1e20a7290UL
#define IPHONE_X_MOTION_CAPS    0x1216014dfa30cfeULL    /* what both copies compute on this system */
#define DYLD_CACHE_BASE         0x180000000UL

extern const void *_dyld_get_shared_cache_range(size_t *length);

void
redwood_coremotion_fixes(void)
{
	static const uint32_t caps_fn_code[4] = { 0xa9bf7bfd, 0x910003fd, 0xf0283808, 0xf9414508 };
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (cache == NULL) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "loc_swfusion=0") != NULL) {
		return;
	}
	uintptr_t slide = (uintptr_t)cache - DYLD_CACHE_BASE;
	volatile intptr_t *token = (volatile intptr_t *)(CM_MOTION_CAPS_TOKEN + slide);
	volatile uint64_t *word = (volatile uint64_t *)(CM_MOTION_CAPS_WORD + slide);

	if (memcmp((const void *)(CM_MOTION_CAPS_FN + slide), caps_fn_code, sizeof(caps_fn_code)) != 0) {
		fprintf(stderr, "redwood: CoreMotion: not the 20F66 cache, left alone\n");
		return;
	}
	if (*token != 0) {
		fprintf(stderr, "redwood: CoreMotion: motion caps already computed (%#llx), left alone\n", *word);
		return;
	}
	*word = IPHONE_X_MOTION_CAPS & ~CAPS_SPU_FUSION;
	__atomic_store_n(token, ~(intptr_t)0, __ATOMIC_RELEASE);
	fprintf(stderr, "redwood: CoreMotion (%s): motion caps %#llx, device motion from locationd\n", getprogname(), *word);
}
