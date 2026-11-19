/* Bounded, two-pass native-module sidecar validation. No kernel APIs execute.
 * Checksums detect damaged/mismatched transfer; they are not authentication.
 * Caller owns the destination VM reservation and performs physical translation.
 */
#ifndef REDWOOD_NATIVE_SIDECAR_H
#define REDWOOD_NATIVE_SIDECAR_H
#include <stdint.h>
#include <stddef.h>

typedef void (*rwfp_copy_fn)(void *, uint64_t, const uint8_t *, uint64_t);

static uint32_t rwfp_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rwfp_u64(const uint8_t *p) {
    return (uint64_t)rwfp_u32(p) | (uint64_t)rwfp_u32(p + 4) << 32;
}
static int rwfp_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int rwfp_name(const uint8_t *p, const char *s) {
    size_t i = 0;
    for (; i < 16 && s[i]; i++) if (p[i] != (uint8_t)s[i]) return 0;
    return i == 16 ? s[i] == 0 : p[i] == 0;
}
static uint32_t rwfp_checksum(const uint8_t *p, uint64_t n) {
    uint32_t h = 2166136261u;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static int rwfp_group(const uint8_t *name) {
    return rwfp_name(name, "__PRELINK_TEXT") ? 1 : rwfp_name(name, "__PLK_TEXT_EXEC") ? 2 :
           rwfp_name(name, "__PLK_DATA_CONST") ? 4 : rwfp_name(name, "__PRELINK_DATA") ? 8 : 0;
}

/* Success returns number of copied pieces (1..16); rejection returns zero.
 * Every entry is checked before the first copy callback, including checksums.
 */
static unsigned rwfp_apply(const uint8_t *kernel, uint64_t kernel_size,
                          const uint8_t *blob, uint64_t blob_size,
                          rwfp_copy_fn copy, void *context) {
    uint64_t group_start[4], group_size[4], target[16], length[16], offset[16];
    unsigned groups = 0, uuids = 0, group_mask = 0;
    const uint8_t *identity = NULL;
    if (!kernel || !blob || !copy || kernel_size < 32 || blob_size < 32 || blob_size > 0x400000u) return 0;
    if (rwfp_u32(kernel) != 0xfeedfacfu || rwfp_u32(kernel + 4) != 0x0100000cu) return 0;
    uint32_t commands = rwfp_u32(kernel + 16), command_bytes = rwfp_u32(kernel + 20);
    if (!commands || commands > 64 || command_bytes > 65536 || command_bytes > kernel_size - 32) return 0;
    uint64_t pos = 32, end = 32 + command_bytes;
    for (uint32_t i = 0; i < commands; i++) {
        if (pos > end || end - pos < 8) return 0;
        uint32_t kind = rwfp_u32(kernel + pos), size = rwfp_u32(kernel + pos + 4);
        if (size < 8 || size % 8 || size > end - pos) return 0;
        if (kind == 0x1bu) {
            if (size != 24 || uuids++) return 0;
            identity = kernel + pos + 8;
        }
        if (kind == 0x19u) {
            if (size < 72) return 0;
            uint64_t address = rwfp_u64(kernel + pos + 24), span = rwfp_u64(kernel + pos + 32);
            if (span > UINT64_MAX - address) return 0;
            if (rwfp_group(kernel + pos + 8)) {
                unsigned bit = (unsigned)rwfp_group(kernel + pos + 8);
                if (group_mask & bit) return 0;
                group_mask |= bit;
                if (!span || groups == 4 || rwfp_u64(kernel + pos + 48) != 0) return 0;
                for (unsigned j = 0; j < groups; j++)
                    if (address < group_start[j] + group_size[j] && group_start[j] < address + span) return 0;
                group_start[groups] = address;
                group_size[groups++] = span;
            }
        }
        pos += size;
    }
    if (pos != end || groups != 4 || uuids != 1) return 0;
    /* Native reservations may not overlap any ordinary kernel segment. */
    pos = 32;
    for (uint32_t i = 0; i < commands; i++) {
        uint32_t kind = rwfp_u32(kernel + pos), size = rwfp_u32(kernel + pos + 4);
        if (kind == 0x19u && !rwfp_group(kernel + pos + 8)) {
            uint64_t address = rwfp_u64(kernel + pos + 24), span = rwfp_u64(kernel + pos + 32);
            if (span) for (unsigned j = 0; j < groups; j++)
                if (address < group_start[j] + group_size[j] && group_start[j] < address + span) return 0;
        }
        pos += size;
    }
    if (!rwfp_equal(blob, (const uint8_t *)"RFP1", 4) || rwfp_u32(blob + 4) != 1 ||
        rwfp_u32(blob + 12) != blob_size || !rwfp_equal(blob + 16, identity, 16)) return 0;
    uint32_t count = rwfp_u32(blob + 8);
    if (!count || count > 16 || 32u + count * 32u > blob_size) return 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = blob + 32 + i * 32;
        target[i] = rwfp_u64(entry);
        length[i] = rwfp_u64(entry + 8);
        offset[i] = rwfp_u64(entry + 16);
        if (rwfp_u32(entry + 28) || !length[i] || length[i] > UINT64_MAX - target[i] ||
            offset[i] < 32u + count * 32u || offset[i] > blob_size || length[i] > blob_size - offset[i]) return 0;
        int contained = 0;
        for (unsigned j = 0; j < groups; j++)
            if (target[i] >= group_start[j] && target[i] - group_start[j] <= group_size[j] &&
                length[i] <= group_size[j] - (target[i] - group_start[j])) contained = 1;
        if (!contained) return 0;
        for (unsigned j = 0; j < i; j++) {
            if (target[i] < target[j] + length[j] && target[j] < target[i] + length[i]) return 0;
            if (offset[i] < offset[j] + length[j] && offset[j] < offset[i] + length[i]) return 0;
        }
        if (rwfp_checksum(blob + offset[i], length[i]) != rwfp_u32(entry + 24)) return 0;
    }
    for (unsigned i = 0; i < count; i++) copy(context, target[i], blob + offset[i], length[i]);
    return count;
}
#endif
