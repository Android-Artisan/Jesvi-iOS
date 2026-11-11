/* MIT. Exercise the loader's actual range selector, without UEFI or firmware. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../../xnuloader/s20_memory.h"

static int valid(uint64_t lo, uint64_t hi, uint64_t at, uint64_t size, uint64_t align)
{
    if (at < lo || at < 0x80000000ULL || at > hi || size > hi - at ||
        at > 0x100000000ULL || size > 0x100000000ULL - at || (at & (align - 1))) return 0;
    for (unsigned i = 0; i < sizeof(s20_cp_regions) / sizeof(s20_cp_regions[0]); i++) {
        if (at < s20_cp_regions[i].base + s20_cp_regions[i].size &&
            s20_cp_regions[i].base < at + size) return 0;
    }
    return 1;
}

/* Brute-force page search provides an independent oracle for bounded windows. */
static uint64_t brute(uint64_t lo, uint64_t hi, uint64_t size, uint64_t align)
{
    uint64_t best = 0;
    for (uint64_t at = lo; at < hi; at += 4096) {
        if (valid(lo, hi, at, size, align)) best = at;
    }
    return best;
}

int main(void)
{
    /* Historical 192-MiB pool fits exactly at d4000000 without headroom. */
    assert(s20_low_dma_candidate(0xd0000000, 0xe0000000, 0xc000000, 0x10000) == 0xd4000000);
    assert(s20_low_dma_candidate(0xf0000000, 0xf0800000, 4096, 4096) == 0);
    assert(s20_low_dma_candidate(0xf6200000, 0xf6201000, 4096, 4096) == 0);
    assert(s20_low_dma_candidate(0xd0000000, 0xd4000000, 4096, 4096) == 0);
    assert(s20_low_dma_candidate(0, UINT64_MAX, 4096, 4096) == 0xfffff000ULL);
    assert(s20_low_dma_candidate(0x90000000, 0x80000000, 4096, 4096) == 0);
    assert(s20_low_dma_candidate(0, UINT64_MAX, UINT64_MAX, 4096) == 0);
    assert(s20_low_dma_candidate(0, UINT64_MAX, 4096, 12288) == 0);
    assert(s20_low_dma_candidate(0, UINT64_MAX, 4095, 4096) == 0);
    assert(s20_memory_candidate(0x100000000ULL, 0x200000000ULL, 4096, 4096) == 0x1fffff000ULL);
    assert(s20_memory_candidate(0x1000, 0x8000000, 4096, 4096) == 0x7fff000);
    const uint64_t boundaries[] = {0x80000000, 0xd0000000, 0xd4000000,
        0xf0000000, 0xf0800000, 0xf6200000, 0xf6201000, 0x100000000ULL};
    unsigned comparisons = 0;
    for (unsigned b = 0; b < sizeof(boundaries) / sizeof(boundaries[0]); b++) {
        for (unsigned left = 1; left <= 16; left++) {
            for (unsigned right = 1; right <= 16; right++) {
                uint64_t lo = boundaries[b] - left * 4096, hi = boundaries[b] + right * 4096;
                for (unsigned pages = 1; pages <= 16; pages++) {
                    for (uint64_t align = 4096; align <= 65536; align *= 2) {
                        uint64_t result = s20_low_dma_candidate(lo, hi, pages * 4096, align);
                        assert(result == brute(lo, hi, pages * 4096, align));
                        comparisons++;
                    }
                }
            }
        }
    }
    printf("loader allocator: %u boundary comparisons passed\n", comparisons);
    return 0;
}
