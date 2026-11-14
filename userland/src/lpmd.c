/* iOS Low Power Mode plus automatic heat control, without rebooting.
 * New kernels: automatic heat control uses kern.redwood.efficiency; older kernels:
 * kern.redwood.lowpower = Settings LPM OR automatic heat cap. Never posts a fake
 * Settings notification. See docs/x1slte-efficiency.md for thresholds and rollback.
 */
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "power_policy.h"

#define LPM_NAME "com.apple.system.lowpowermode"
#define LOG_MAX (6656u * 1024u)
#define POLL_S 5

static uint64_t
monotonic_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static int
lpm_state(int token)
{
    uint64_t state = 0;
    return notify_get_state(token, &state) == NOTIFY_STATUS_OK ? state != 0 : -1;
}

static int
get_int(const char *name)
{
    int value = -1;
    size_t len = sizeof(value);
    return sysctlbyname(name, &value, &len, NULL, 0) == 0 ? value : -1;
}

static int
set_int(const char *name, int value)
{
    if (get_int(name) == value) return 0;
    if (sysctlbyname(name, NULL, NULL, &value, sizeof(value)) != 0) {
        perror(name);
        return -1;
    }
    printf("lpmd: %s=%d\n", name, value);
    fflush(stdout);
    return 0;
}

/* loader_log_snapshot and total are separate sysctls. Retry if a writer races
 * the snapshot; never assign freshness using mismatched offsets. */
static int
sample(char *buf, uint64_t *position, int temp[4])
{
    for (unsigned tries = 0; tries < 3; tries++) {
        uint64_t before = 0, after = 0;
        size_t size = sizeof(before), len = LOG_MAX;
        if (sysctlbyname("kern.redwood_log_total", &before, &size, NULL, 0) != 0 ||
            sysctlbyname("kern.redwood_log", buf, &len, NULL, 0) != 0) return 0;
        size = sizeof(after);
        if (sysctlbyname("kern.redwood_log_total", &after, &size, NULL, 0) != 0) return 0;
        if (before != after || after < len) continue;
        /* The loader retains a fixed 1 MiB boot head plus a rolling tail.
         * Once wrapped, exclude that head: its old reports have no tail offsets. */
        size_t head = after > len && len > (1u << 20) ? (1u << 20) : 0;
        return power_parse(buf + head, len - head, after, position, temp);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    int token = 0;
    if (notify_register_check(LPM_NAME, &token) != NOTIFY_STATUS_OK) return 1;
    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        printf("lpmd: Settings=%d kernel-lowpower=%d efficiency=%d\n",
            lpm_state(token), get_int("kern.redwood.lowpower"), get_int("kern.redwood.efficiency"));
        notify_cancel(token);
        return 0;
    }
    /* Explicit rollback: Settings mirroring only, no automatic thermal control. */
    int automatic = !(argc > 1 && strcmp(argv[1], "--settings-only") == 0);
    char *buf = automatic ? malloc(LOG_MAX) : NULL;
    if (automatic && !buf) return 1;
    struct power_policy policy = {0};
    uint64_t start = monotonic_s(), last_seen = start, last_position = 0;
    int temp[4] = {-1, -1, -1, -1}, last_cap = -1, have_sample = 0;
    int has_efficiency = get_int("kern.redwood.efficiency") >= 0;
    /* Preserve an existing cap across daemon restarts until a full cool dwell. */
    if (automatic) {
        policy.capped = get_int(has_efficiency ? "kern.redwood.efficiency" : "kern.redwood.lowpower") > 0;
    }
    printf("lpmd: automatic=%d; engage M5/A76/A55/GPU=76/80/82/80 C; "
        "recover <=68/72/74/72 C for %d s; backend=%s\n",
        automatic, POWER_COOL_S, has_efficiency ? "efficiency" : "lowpower");
    fflush(stdout);
    for (;;) {
        uint64_t now = monotonic_s(), position = 0;
        int t[4] = {-1, -1, -1, -1};
        if (automatic && sample(buf, &position, t) && (!have_sample || position != last_position)) {
            last_position = position;
            last_seen = now;
            memcpy(temp, t, sizeof(temp));
            have_sample = 1;
        }
        int valid = have_sample && now - last_seen < POWER_STALE_S;
        for (unsigned z = 0; z < 4; z++) valid &= temp[z] >= 0;
        /* Give the 10-second kernel reporter time to start; once running, missing
         * or invalid telemetry immediately prevents release. */
        if (automatic && (have_sample || now - start >= POWER_STALE_S)) {
            power_update(&policy, valid, temp, now);
        }
        if (automatic && policy.capped != last_cap) {
            printf("lpmd: automatic cap %s; M5/A76/A55/GPU=%d/%d/%d/%d C telemetry=%s\n",
                policy.capped ? "on" : "off", temp[0], temp[1], temp[2], temp[3], valid ? "fresh" : "unavailable");
            fflush(stdout);
            last_cap = policy.capped;
        }
        int manual = lpm_state(token);
        /* Notification failure cannot silently cancel a user's existing LPM. */
        if (manual < 0) manual = get_int("kern.redwood.lowpower") != 0;
        if (has_efficiency && automatic) set_int("kern.redwood.efficiency", policy.capped);
        set_int("kern.redwood.lowpower", manual || (automatic && !has_efficiency && policy.capped));
        sleep(POLL_S);
    }
}
