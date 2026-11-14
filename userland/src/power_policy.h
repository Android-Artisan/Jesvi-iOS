/* Shared pure policy: die temperatures, not battery or case temperatures. */
#ifndef REDWOOD_POWER_POLICY_H
#define REDWOOD_POWER_POLICY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* TMU order: M5, A76, A55, GPU. Earlier intervention on the M5s;
 * each zone has its own hysteresis, and recovery requires all to be cool. */
static const int power_hot_c[4] = {76, 80, 82, 80};
static const int power_cool_c[4] = {68, 72, 74, 72};
#define POWER_COOL_S 30
#define POWER_STALE_S 30

struct power_policy {
    int capped, cooling;
    uint64_t cool_since;
};

static int
power_update(struct power_policy *p, int valid, const int temp[4], uint64_t now)
{
    int hot = 0, cool = 1;
    for (unsigned z = 0; valid && z < 4; z++) {
        hot |= temp[z] >= power_hot_c[z];
        cool &= temp[z] <= power_cool_c[z];
    }
    if (!valid) {
        /* Missing telemetry must not release a cap, or allow full clocks indefinitely. */
        p->capped = 1;
        p->cooling = 0;
    } else if (hot) {
        p->capped = 1;
        p->cooling = 0;
    } else if (cool && p->capped) {
        if (!p->cooling) {
            p->cool_since = now;
            p->cooling = 1;
        } else if (now - p->cool_since >= POWER_COOL_S) {
            p->capped = 0;
            p->cooling = 0;
        }
    } else {
        p->cooling = 0;
    }
    return p->capped;
}

/* Scan bounded log bytes, including beyond embedded NULs. All four zones are required.
 * absolute_end identifies the sample across snapshots/ring wrap; unrelated log traffic
 * cannot make an old temperature report fresh. */
static int
power_parse(const char *text, size_t len, uint64_t total, uint64_t *absolute_end, int temp[4])
{
    static const char prefix[] = "; temperatures big ";
    int found = 0;
    for (size_t i = 0; i + sizeof(prefix) - 1 < len; i++) {
        if (memcmp(text + i, prefix, sizeof(prefix) - 1) != 0) continue;
        const char *end = memchr(text + i, '\n', len - i);
        if (!end) continue; /* incomplete record */
        size_t n = (size_t)(end - (text + i));
        if (n >= 160) continue;
        char line[160];
        memcpy(line, text + i, n);
        line[n] = 0;
        int t[4], used = 0;
        if (sscanf(line, "; temperatures big %d mid %d little %d g3d %d%n",
            &t[0], &t[1], &t[2], &t[3], &used) != 4 || used != (int)n) continue;
        for (unsigned z = 0; z < 4; z++) {
            temp[z] = t[z] >= 0 && t[z] <= 125 ? t[z] : -1;
        }
        /* A newer sensor failure supersedes any older good record. */
        *absolute_end = total - len + (uint64_t)(end - text) + 1;
        found = 1;
    }
    return found;
}
#endif
