/*
 * libredwood-calls.dylib (A, 2026-11-14): the call hooks of the Phone app and the call screen, kept outside the boot
 * image so they can change without a reboot. Loaded by libredwood.dylib (src/inject.c) in MobilePhone and InCallService
 * from /var/jb/redwood/lib. Build: userland/build-calls.sh; install: copy, then restart MobilePhone / InCallService.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "phone_dial.h"

__attribute__((constructor))
static void
calls_init(void)
{
	const char *name = getprogname();
	if (!name) return;
	{   /* which processes have the hooks: /var/tmp/redwood-calls-hooks.log */
		FILE *f = fopen("/var/tmp/redwood-calls-hooks.log", "a");
		if (f) { fprintf(f, "%ld %s[%d] loaded\n", (long)time(NULL), name, getpid()); fclose(f); }
	}
	if (!strcmp(name, "MobilePhone")) {
		phone_dial_hook();
	} else if (!strcmp(name, "callservicesd")) {
		/* the source of every client's call information: RedwoodPhone's calls as phone calls here (live switchable:
		 * notify state com.redwood.call.spoof, bit 7 + bits 0/1; default off until proven) */
		phone_spoof_default = 0;
		phone_system_hook();
	} else if (!strcmp(name, "InCallService")) {
		phone_spoof_default = 0;
		phone_system_hook();
		phone_speaker_hook();
	}
}
