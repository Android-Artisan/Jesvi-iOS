/*
 * calls-mode.h (A, 2026-11-15): which call stack runs. Shared by redwood-ims (root), redwood-telephonyd and
 * RedwoodPhone (both mobile).
 *
 *   /var/jb/etc/redwood-calls.conf: "calls=N" (no file: 1, native -- the user, 2026-11-15: "we are going full native")
 *     0  legacy: the old one-call engine (/var/root/redwood-ims.legacy, else .before-native) + RedwoodPhone
 *     1  native: redwood-ims (ims-call.h) + redwood-telephonyd, the telephony provider under CommCenter's identity
 *     2  native, falling back to legacy for the rest of this boot when the native provider cannot register
 *
 * The fallback flag is call-ui/native-failed (mobile may write there), holding this boot's kern.bootsessionuuid: a
 * flag from an earlier boot does not count (the phone's clock resets at boot, so file times cannot tell). Changing the
 * mode: edit the conf, then end redwood-ims (cellulard starts it again) and `launchctl kickstart -k` both providers --
 * or reboot.
 */
#ifndef CALLS_MODE_H
#define CALLS_MODE_H
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define CALLS_CONF   "/var/jb/etc/redwood-calls.conf"
#define CALLS_FAILED "/var/tmp/redwood-cell/call-ui/native-failed"

static int calls_mode(void)
{
    int m = 1;
    char b[128];
    FILE *f = fopen(CALLS_CONF, "r");
    if (f) {
        size_t n = fread(b, 1, sizeof(b) - 1, f);
        b[n] = 0;
        fclose(f);
        char *p = strstr(b, "calls=");
        if (p && p[6] >= '0' && p[6] <= '2') m = p[6] - '0';
    }
    return m;
}

static void calls_boot_id(char *out, size_t n)
{
    size_t len = n;
    if (sysctlbyname("kern.bootsessionuuid", out, &len, NULL, 0) || len == 0) snprintf(out, n, "unknown");
    out[n - 1] = 0;
}

/* the native provider failed in this boot (mode 2's fallback is in force) */
static int calls_native_failed(void)
{
    char want[64], got[64] = { 0 };
    calls_boot_id(want, sizeof(want));
    FILE *f = fopen(CALLS_FAILED, "r");
    if (!f) return 0;
    if (!fgets(got, sizeof(got), f)) got[0] = 0;
    fclose(f);
    got[strcspn(got, " \n")] = 0;
    return !strcmp(got, want);
}

__attribute__((unused)) static void calls_mark_native_failed(const char *why)
{
    char id[64];
    calls_boot_id(id, sizeof(id));
    int fd = open(CALLS_FAILED, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf(fd, "%s %s\n", id, why);
        close(fd);
    }
}

/* should the native stack run now? */
__attribute__((unused)) static int calls_use_native(void)
{
    int m = calls_mode();
    return m == 1 || (m == 2 && !calls_native_failed());
}
#endif
