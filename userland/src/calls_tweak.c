/*
 * libredwood-calls.dylib (A, 2026-11-14): the call hooks of the Phone app and the call screen, kept outside the boot
 * image so they can change without a reboot. Loaded by libredwood.dylib (src/inject.c) in MobilePhone and InCallService
 * from /var/jb/redwood/lib. Build: userland/build-calls.sh; install: copy, then restart MobilePhone / InCallService.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "phone_dial.h"

__attribute__((constructor))
static void
calls_init(void)
{
	const char *name = getprogname();
	if (!name) return;
	if (!strcmp(name, "MobilePhone")) {
		phone_dial_hook();
	} else if (!strcmp(name, "InCallService")) {
		phone_spoof_default = 0;
		phone_system_hook();
		phone_speaker_hook();
	}
}
