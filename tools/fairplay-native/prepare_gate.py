"""Prepare a reviewable boot-argument admission patch without editing XNU."""
import argparse
import difflib
from pathlib import Path

INCLUDE = '#include <libkern/crypto/sha2.h>\n'
ANCHOR = '\t/* Validate that the Kext Collection is prelinked to the loaded KC */\n'
GATE = '''\t/* Redwood native FairPlay prototype: omit both modules unless explicitly
\t * enabled. Filtering happens before OSKext creates objects or constructors
\t * run. This argument does not alter purchase/licensing decisions.
\t */
\tif (type == KCKindPrimary) {
\t\tint nativeFairPlay = 0;
\t\tif (!PE_parse_boot_argn("fairplay_native", &nativeFairPlay, sizeof(nativeFairPlay))) {
\t\t\tPE_parse_boot_argn("fplay_native", &nativeFairPlay, sizeof(nativeFairPlay));
\t\t}
\t\tOSArray *nativeArray = OSDynamicCast(OSArray,
\t\t    infoDict->getObject(kPrelinkInfoDictionaryKey));
\t\tunsigned int omitted = 0, nativeSeen = 0, nativeHeaders = 0;
\t\tkernel_segment_command_t *nativeText = getsegbynamefromheader(mh, kPrelinkTextSegment);
\t\tif (nativeArray) {
\t\t\tfor (unsigned int i = 0; i < nativeArray->getCount(); i++) {
\t\t\t\tOSDictionary *entry = OSDynamicCast(OSDictionary, nativeArray->getObject(i));
\t\t\t\tOSString *identifier = entry ? OSDynamicCast(OSString, entry->getObject("CFBundleIdentifier")) : NULL;
\t\t\t\tif (!identifier || (!identifier->isEqualTo("com.apple.driver.FairPlayIOKit") &&
\t\t\t\t    !identifier->isEqualTo("com.apple.IOTextEncryptionFamily"))) {
\t\t\t\t\tcontinue;
\t\t\t\t}
\t\t\t\tnativeSeen++;
\t\t\t\tOSNumber *location = OSDynamicCast(OSNumber, entry->getObject("_PrelinkExecutableLoadAddr"));
\t\t\t\tuint64_t address = location ? location->unsigned64BitValue() : 0;
\t\t\t\tif (nativeText && address >= nativeText->vmaddr && address - nativeText->vmaddr <= nativeText->vmsize &&
\t\t\t\t    sizeof(kernel_mach_header_t) <= nativeText->vmsize - (address - nativeText->vmaddr)) {
\t\t\t\t\tconst kernel_mach_header_t *header = (const kernel_mach_header_t *)(uintptr_t)address;
\t\t\t\t\tif (header->magic == MH_MAGIC_64 && header->cputype == CPU_TYPE_ARM64 && header->filetype == MH_KEXT_BUNDLE) {
\t\t\t\t\t\tnativeHeaders++;
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t\tbool nativeAllowed = nativeFairPlay == 1 && nativeSeen == 2 && nativeHeaders == 2;
\t\tif (nativeArray && !nativeAllowed) {
\t\t\tfor (unsigned int i = nativeArray->getCount(); i > 0; i--) {
\t\t\t\tOSDictionary *entry = OSDynamicCast(OSDictionary, nativeArray->getObject(i - 1));
\t\t\t\tOSString *identifier = entry ? OSDynamicCast(OSString,
\t\t\t\t    entry->getObject("CFBundleIdentifier")) : NULL;
\t\t\t\tif (identifier &&
\t\t\t\t    (identifier->isEqualTo("com.apple.driver.FairPlayIOKit") ||
\t\t\t\t    identifier->isEqualTo("com.apple.IOTextEncryptionFamily"))) {
\t\t\t\t\tnativeArray->removeObject(i - 1);
\t\t\t\t\tomitted++;
\t\t\t\t}
\t\t\t}
\t\t}
\t\tIOLog("redwood_native_fairplay_gate_v1: requested=%d enabled=%d omitted=%u headers=%u/%u\\n",
\t\t    nativeFairPlay == 1, nativeAllowed, omitted, nativeHeaders, nativeSeen);
\t}

'''


def prepare(original):
    if original.count(INCLUDE) != 1 or original.count(ANCHOR) != 1:
        raise ValueError('Expected unique bootstrap include and admission anchors')
    return original.replace(INCLUDE, INCLUDE + '#include <pexpert/pexpert.h>\n').replace(ANCHOR, GATE + ANCHOR)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    before = args.source.read_text()
    after = prepare(before)
    patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                       fromfile='a/libsa/bootstrap.cpp', tofile='b/libsa/bootstrap.cpp'))
    args.output.write_text(patch)
    print('Prepared default-off admission patch; canonical XNU source unchanged.')
