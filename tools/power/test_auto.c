#include "../../userland/src/power_policy.h"
#include <assert.h>
int main(void) {
    int cool[4] = {68, 72, 74, 72}, t[4] = {75, 79, 81, 79};
    struct power_policy p = {0};
    assert(!power_update(&p, 1, t, 0));
    for (unsigned z = 0; z < 4; z++) {
        memset(&p, 0, sizeof(p));
        memcpy(t, cool, sizeof(t));
        t[z] = power_hot_c[z];
        assert(power_update(&p, 1, t, 5)); /* independent triggers */
        assert(power_update(&p, 1, cool, 10));
        assert(power_update(&p, 1, cool, 39));
        assert(!power_update(&p, 1, cool, 40));
    }
    assert(power_update(&p, 0, cool, 50));
    assert(power_update(&p, 1, cool, 55));
    memcpy(t, cool, sizeof(t)); t[0]++;
    assert(power_update(&p, 1, t, 70)); /* warm M5 interrupts recovery */
    assert(power_update(&p, 1, cool, 75));
    assert(power_update(&p, 0, cool, 90));
    assert(power_update(&p, 1, cool, 95));
    assert(!power_update(&p, 1, cool, 125));
    const char log[] = "old\0dvfs: every 10 s: PLL_G3D 455 MHz; temperatures big 77 mid 78 little 72 g3d 70\n";
    uint64_t pos = 0;
    assert(power_parse(log, sizeof(log)-1, 1000, &pos, t));
    assert(t[0] == 77 && t[1] == 78 && t[2] == 72 && t[3] == 70 && pos == 1000);
    const char invalid[] = "; temperatures big 70 mid 65 little 67 g3d 60\n; temperatures big -1 mid 65 little 67 g3d 60\n";
    assert(power_parse(invalid, sizeof(invalid)-1, 2000, &pos, t) && t[0] == -1);
    const char partial[] = "; temperatures big 80 mid 80 little 80 g3d 80";
    assert(!power_parse(partial, sizeof(partial)-1, 3000, &pos, t));
    const char wrap[] = "; temperatures big 70 mid 65 little 67 g3d 60\nother traffic\n";
    assert(power_parse(wrap, sizeof(wrap)-1, 4000, &pos, t));
    uint64_t same = pos;
    const char wrap2[] = "; temperatures big 70 mid 65 little 67 g3d 60\nother traffic\nnew\n";
    assert(power_parse(wrap2, sizeof(wrap2)-1, 4004, &pos, t) && pos == same);
    puts("PASS: per-cluster triggers, hysteresis, recovery dwell, missing telemetry and parser freshness");
}
