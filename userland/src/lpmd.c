/*
 * lpmd: iOS's Low Power Mode switch, passed to the kernel (B, 2026-11-07).
 *
 * powerd keeps Low Power Mode as the state of the Darwin notification com.apple.system.lowpowermode (1 on,
 * 0 off; NSProcessInfo.lowPowerModeEnabled reads the same). The kernel's clock governor caps the clusters,
 * the GPU and memory while kern.redwood.lowpower is 1 (bsd/dev/redwood_exynos_acpm.c). This daemon sets it
 * at start and whenever the switch moves.
 *
 *   lpmd          stay, and pass the switch on (the job com.redwood.lpmd does this)
 *   lpmd probe    the notification's state now, and the kernel's value
 */
#include <dispatch/dispatch.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#define LPM_NAME        "com.apple.system.lowpowermode"

static int
lpm_state(int token)
{
	uint64_t state = 0;

	return notify_get_state(token, &state) == NOTIFY_STATUS_OK && state != 0;
}

static void
pass_on(int token)
{
	int on = lpm_state(token), was = -1;
	size_t len = sizeof(was);

	sysctlbyname("kern.redwood.lowpower", &was, &len, NULL, 0);
	if (was != on) {
		if (sysctlbyname("kern.redwood.lowpower", NULL, NULL, &on, sizeof(on)) == 0) {
			printf("lpmd: Low Power Mode %s: kernel clocks %s\n", on ? "on" : "off", on ? "capped" : "as normal");
		} else {
			printf("lpmd: Low Power Mode %s, but kern.redwood.lowpower could not be set\n", on ? "on" : "off");
		}
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	int token = 0;

	if (notify_register_check(LPM_NAME, &token) != NOTIFY_STATUS_OK) {
		printf("lpmd: cannot watch %s\n", LPM_NAME);
		return 1;
	}
	if (argc > 1 && strcmp(argv[1], "probe") == 0) {
		int k = -1;
		size_t len = sizeof(k);

		sysctlbyname("kern.redwood.lowpower", &k, &len, NULL, 0);
		printf("lpmd: %s state %d; kern.redwood.lowpower %d\n", LPM_NAME, lpm_state(token), k);
		return 0;
	}
	pass_on(token);
	notify_register_dispatch(LPM_NAME, &token, dispatch_get_main_queue(), ^(int t) {
		pass_on(t);
	});
	dispatch_main();
}
