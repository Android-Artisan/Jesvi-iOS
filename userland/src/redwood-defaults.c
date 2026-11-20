/*
 * redwood-defaults: this port's defaults for a fresh data volume, applied ONCE (2026-11-20).
 *
 * A clean install starts the data volume empty, so preferences that make the phone work as shipped were missing:
 * Location Services off (Compass, Maps, GPS dead, and no switch in Settings to find), keyboard haptics off, Live
 * Photo on. Run at boot (com.redwood.defaults); the marker /var/root/.redwood-defaults-v1 makes it a once-only step,
 * so whatever the user changes later is never overwritten. Bump the marker's version to apply a new default.
 * Keys found by diffing the preference files around the Settings switches, and in iOS 16.5's CameraUI:
 *   root   com.apple.locationd           LocationServicesEnabledIn7.0 = true   (locationd runs as root and reads
 *                                                                               root's domain, not mobile's)
 *   mobile com.apple.keyboard.preferences KeyboardVisceral = 2                  (Keyboard Feedback: Haptic)
 *   mobile com.apple.camera               CAMUserPreferencePreserveLivePhoto = true, ..DesiredIrisMode = 0
 *                                                                               (Live Photo off, and kept off)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MARKER "/var/root/.redwood-defaults-v1"

/* libproc (in libSystem; no header in the iOS SDK) */
int proc_listallpids(void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);

static int
set(CFStringRef user, const char *domain, const char *key, CFPropertyListRef value)
{
	CFStringRef d = CFStringCreateWithCString(NULL, domain, kCFStringEncodingUTF8);
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFPreferencesSetValue(k, value, d, user, kCFPreferencesAnyHost);
	Boolean ok = CFPreferencesSynchronize(d, user, kCFPreferencesAnyHost);
	printf("redwood-defaults: %s %s = set%s\n", domain, key, ok ? "" : " (NOT written)");
	CFRelease(k);
	CFRelease(d);
	return ok ? 0 : 1;
}

static int
set_int(CFStringRef user, const char *domain, const char *key, int v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
	int r = set(user, domain, key, n);
	CFRelease(n);
	return r;
}

/* locationd reads its switch at start: restart it (launchd starts it again) */
static void
restart_locationd(void)
{
	pid_t pids[2048];
	int n = proc_listallpids(pids, sizeof(pids));

	for (int i = 0; i < n; i++) {
		char path[4096];
		if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && strcmp(path, "/usr/libexec/locationd") == 0) {
			kill(pids[i], SIGKILL);
			printf("redwood-defaults: locationd restarted\n");
		}
	}
}

int
main(void)
{
	struct stat st;
	int r = 0;

	if (stat(MARKER, &st) == 0) {
		return 0;               /* applied on this data volume already: the user's choices stand */
	}
	/* cfprefsd must be up to write mobile's preferences: wait for it (up to a minute after boot) */
	for (int i = 0; i < 30; i++) {
		CFPropertyListRef probe = CFPreferencesCopyValue(CFSTR("probe"), CFSTR("com.redwood.defaults"), CFSTR("mobile"),
		    kCFPreferencesAnyHost);
		if (probe) CFRelease(probe);
		if (access("/var/mobile/Library/Preferences", W_OK) == 0) break;
		sleep(2);
	}
	r |= set(CFSTR("root"), "com.apple.locationd", "LocationServicesEnabledIn7.0", kCFBooleanTrue);
	r |= set(CFSTR("mobile"), "com.apple.locationd", "LocationServicesEnabledIn7.0", kCFBooleanTrue);
	r |= set_int(CFSTR("mobile"), "com.apple.keyboard.preferences", "KeyboardVisceral", 2);
	r |= set(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferencePreserveLivePhoto", kCFBooleanTrue);
	r |= set_int(CFSTR("mobile"), "com.apple.camera", "CAMUserPreferenceDesiredIrisMode", 0);
	restart_locationd();
	if (r == 0) {
		int fd = open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) close(fd);
		printf("redwood-defaults: applied (marker %s)\n", MARKER);
	} else {
		printf("redwood-defaults: some not written; tries again at the next boot\n");
	}
	return r;
}
