"""Prepare the bounded sidecar loader patch without editing canonical source."""
import argparse
import difflib
from pathlib import Path


def replace_one(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Loader source anchor changed or is ambiguous')
    return text.replace(old, new)


FIELDS = '''\tuint32_t kernel_size;
\tconst uint8_t *native_sidecar;
\tchar fpart[32];
\tuint64_t foff, fsize;
'''
PARSE = '''\t\t} else if (str_eq(e->name, "fpart")) {
\t\t\tif (e->size < 2 || e->size > sizeof(pi->fpart) || text[e->size - 1] != 0) return 0;
\t\t\tif (!str_eq(text, "recovery")) return 0;
\t\t\tmemcpy(pi->fpart, text, e->size);
\t\t} else if (str_eq(e->name, "foff") || str_eq(e->name, "fsize")) {
\t\t\tif (e->size < 2 || e->size > 17 || text[e->size - 1] != 0) return 0;
\t\t\tfor (unsigned k = 0; k + 1 < e->size; k++)
\t\t\t\tif (!((text[k] >= '0' && text[k] <= '9') || (text[k] >= 'a' && text[k] <= 'f'))) return 0;
\t\t\tif (str_eq(e->name, "foff")) pi->foff = parse_number(text, 16);
\t\t\telse pi->fsize = parse_number(text, 16);
'''
FUNCTIONS = '''/* Optional native FairPlay bytes occupy recovery+24 MiB, outside the
 * boot image, before the existing kernel slot. No other partition is accepted.
 * Reserve source memory before choose_window so XNU cannot own the buffer.
 */
static uint64_t native_sidecar_base;
static int
load_native_sidecar(struct payload_info *pi)
{
\tif (!pi->fpart[0]) return pi->fsize == 0 && pi->foff == 0;
\tif (!str_eq(pi->fpart, "recovery") || pi->foff != 0x01800000ULL ||
\t    pi->fsize < 32 || pi->fsize > 0x00400000ULL) return 0;
\tif (!native_sidecar_base && !allocate_low_dma(0x00400000ULL, 0x10000, &native_sidecar_base)) return 0;
\tuint64_t read_size = (pi->fsize + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
\tif (!read_partition_into(pi->fpart, pi->foff, (void *)(uintptr_t)native_sidecar_base, read_size)) return 0;
\tpi->native_sidecar = (const uint8_t *)(uintptr_t)native_sidecar_base;
\treturn 1;
}

static void
copy_native_sidecar_piece(void *context, uint64_t address, const uint8_t *source, uint64_t length)
{
\tuint64_t slide = *(uint64_t *)context;
\tmemcpy((void *)(uintptr_t)(address + slide), source, length);
}

'''
APPLY = '''\tif (pi.native_sidecar) {
\t\tunsigned copied = rwfp_apply(kernel, pi.kernel_size, pi.native_sidecar, pi.fsize,
\t\t    copy_native_sidecar_piece, &slide);
\t\tif (!copied) {
\t\t\tprint("native sidecar validation failed; kernel not started\\n");
\t\t\treturn 0;
\t\t}
\t\tprint("native sidecar: %d validated pieces copied, execution governed by fairplay_native\\n", (uint64_t)copied);
\t} else {
\t\tlc = (const struct load_command *)(mh + 1);
\t\tfor (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
\t\t\tconst struct segment_command_64 *sg = (const void *)lc;
\t\t\tif (lc->cmd == LC_SEGMENT_64 && sg->vmsize && rwfp_group((const uint8_t *)sg->segname)) {
\t\t\t\tprint("native sidecar required for populated prelink VM; kernel not started\\n");
\t\t\t\treturn 0;
\t\t\t}
\t\t}
\t}
'''


def prepare(original):
    result = replace_one(original, '#include "s20_efi_memory.h"\n', '#include "s20_efi_memory.h"\n#include "native_sidecar.h"\n')
    result = replace_one(result, '\tconst uint8_t *kernel;\n', '\tconst uint8_t *kernel;\n' + FIELDS)
    result = replace_one(result, '\t\t\tpi->kernel = p + sizeof(*e);\n', '\t\t\tpi->kernel = p + sizeof(*e);\n\t\t\tpi->kernel_size = e->size;\n')
    result = replace_one(result, '\t\t} else if (str_eq(e->name, "rdpart")) {\n', PARSE + '\t\t} else if (str_eq(e->name, "rdpart")) {\n')
    result = replace_one(result, 'static int starts_with(const char *s, const char *prefix);\n', FUNCTIONS + 'static int starts_with(const char *s, const char *prefix);\n')
    result = replace_one(result, '\t\tpi.kernel = (const uint8_t *)(uintptr_t)download_base;\n', '\t\tpi.kernel = (const uint8_t *)(uintptr_t)download_base;\n\t\tpi.kernel_size = DOWNLOAD_MAX_SIZE;\n')
    result = replace_one(result, '\tconst uint8_t *kernel = pi.kernel;\n', '\tif (!load_native_sidecar(&pi)) {\n\t\tprint("native sidecar storage preparation failed\\n");\n\t\treturn 0;\n\t}\n\tconst uint8_t *kernel = pi.kernel;\n')
    result = replace_one(result, '\tprint("kernel loaded at pa %x..%x\\n", kern_lo, kern_hi);\n', APPLY + '\tprint("kernel loaded at pa %x..%x\\n", kern_lo, kern_hi);\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepared-source', type=Path, required=True)
    args = parser.parse_args()
    before = args.source.read_text()
    after = prepare(before)
    patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                       fromfile='a/xnuloader/loader.c', tofile='b/xnuloader/loader.c'))
    header = (Path(__file__).parent / 'native_sidecar.h').read_text()
    patch += ''.join(difflib.unified_diff([], header.splitlines(True),
                                        fromfile='/dev/null', tofile='b/xnuloader/native_sidecar.h'))
    args.output.write_text(patch)
    args.prepared_source.write_text(after)
    print('Prepared loader patch/source; canonical loader unchanged.')
