# Native FairPlay reconstruction prototype

Active source branch: `feat/s20-native-fairplay`, separate worktree
`work/hackios-fairplay-native`. Open `fairplay-native.code-workspace` for a
focused VS Code workspace. All source tools are tracked in `tools/fairplay-native`;
firmware and generated images remain outside Git in ignored private storage.

## Current continuation

The edited native admission gate is now in this worktree's XNU source and
exported to `xnu/redwood.patch`. It is default off; only `fairplay_native=1`
admits the two native modules. The optional loader implementation is in
`xnuloader/loader.c` and `xnuloader/native_sidecar.h`. Neither change has been
deployed. The shared x1slte checkout was not edited.

Stock and custom declared base sizes match: OSObject16, IOService136,
IOUserClient216 bytes. Bounded Unicorn runs of both stock and relocated static
initializers return the same seven module class records, with only the
OSMetaClass constructor modeled. Other external calls stop the emulator.
This does not execute real class registration or driver start/licensing.

The first 47,710,208-byte prelink layout exceeds the existing 16 MiB kernel read
window and would overlap the recovery ramdisk slot. It must not be deployed.
The sidecar prototype instead produces a 15,220,736-byte kernel and 1,732,312-byte
separate payload (8 pieces). Its coalesced VM ranges reserve the address gaps
without storing the gaps in the kernel file. The loader proposal reads the
sidecar only from recovery+24 MiB into allocated source memory, validates the
kernel UUID, bounds, destination ranges, overlap and checksums in two passes,
then copies before kernel entry. Invalid input triggers no copy. Checksums
detect transfer damage, not malicious substitution.

Fifteen host tests pass, including actual C validation against synthetic bad
payloads. The actual private eight-piece payload also passes the C validator,
byte-for-byte matching the Python reference. The gate object and loader object
compile in isolation. Archive relink attempts have not produced an accepted
kernel: the latest output failed the gate-marker check, so the tool rejected it.
Do not use the rejected binaries. Investigate object/archive admission or replay
the exact canonical link filelist. Even a successful archived build would not
be a current release source build or ready to flash.

Next: complete isolated relink validation, regenerate native imports for that
exact binary, and finish payload/recovery packaging checks. B currently holds
the phone. Any later test must use a current canonical build, preserve existing
phone features, and start with a layout-only boot (`fairplay_native=0`) before
native execution is considered. No successful download/install is established.

2026-11-19, codex-fairplayed. Native installation is unfinished. No kernel image
was built or flashed, and no account, service or phone state changed this phase.

Original 20F66 iPhone10,3 kernelcache fetched from the same Apple CDN IPSW used
by sysimage/fetch-from-ipsw.py. Firmware and generated bundles stay in workspace
out/fairplayed-audit/private, outside tracked source.

## Findings

Matching components: FairPlayIOKit 71.1.0 (1,378,052 executable bytes) and
IOTextEncryptionFamily 1.0.0 (5,732 executable bytes). The latter references the
text-crypter hook setter. Declared dependencies include kernel KPIs and FairPlay,
not a separate hardware driver; runtime hardware requirements remain unknown.

inspect_kernel.py preserves Apple's XML ID/IDREF values. The ipsw JSON listing
lost some referenced values and is not authoritative for provider classes or
dependency versions. Its extractor rejects this legacy non-fileset kernel.

Per-kext symbol/dynamic-symbol/split-relocation load commands are zero. However,
the outer _PrelinkLinkKASLROffsets retains 387,543 pointer sites relative to
__PRELINK_TEXT, as processed by OSKext.cpp::__slideOldKaslrOffsets. Of these,
8,278 belong to the two components:

| Component | Internal/cross-component pointers | Kernel symbol pointers |
| --- | ---: | ---: |
| FairPlayIOKit | 7,140 | 469 |
| IOTextEncryptionFamily | 67 | 602 |

All recorded external pointers map to symbols present in out/mach.x1slte,
including 34 small symbol addends which must be preserved. Matching names do not
prove runtime ABI compatibility. Base vtable checks match 364 named slots:
IOService 167, IOUserClient 185, OSMetaClass 12. Thirteen unknown slots remain.

stage_native.py reconstructs both modules with a common page-aligned virtual
slide, rebinds recorded pointers, rebuilds segment/section offsets, and checks
pointer readback and unchanged bytes outside linkage/header fields. It refuses
unresolved/ambiguous imports. Private staged-v1 contains 1,794,088 bytes of
fixed-address analysis Mach-Os. Its manifest says
OFFLINE_ANALYSIS_ONLY_DO_NOT_DEPLOY. These are neither boot images nor dynamically
loadable kexts. Original data addresses overlap the custom kernel; copying the
originals directly is invalid.

audit_flow.py requires capstone. From 6,190 pointer-derived roots it visits
313,832 instruction addresses, finding no outgoing direct branches or ADRP pages
outside the copied components. Roots may identify data as code: six invalid
decode locations remain, along with 5,919 BR and 1,125 BLR instructions whose
computed destinations this scan does not establish. This is incomplete evidence.

## Reproduce from workspace root

```
P=work/hackios-xnu-kernel/proposals/fairplayed-audit
K=out/fairplayed-audit/private/20F66__iPhone10,3/kernelcache.release.iPhone10,3_6
python3 "$P/inspect_kernel.py" "$K" work/hackios-xnu-kernel/out/mach.x1slte \
  --output out/fairplayed-audit/private/compatibility.json
python3 "$P/audit_flow.py" "$K" out/fairplayed-audit/private/compatibility.json \
  --output out/fairplayed-audit/private/flow-summary.json
python3 "$P/stage_native.py" "$K" work/hackios-xnu-kernel/out/mach.x1slte \
  --output out/fairplayed-audit/private/staged-v2
python3 -m unittest discover -s "$P" -p 'test_*.py' -v
```

The output directory must be new. Eight synthetic tests passed: XML references,
malformed input, signed branches, file bounds, pointer addends, relative layout,
and rejecting unresolved/ambiguous imports. Real module reconstruction passed
pointer readback and untouched-byte checks. This validates the host tooling,
not native execution or licensing.

## Next exact gates

1. Audit six invalid locations in private flow-summary.json and computed BR/BLR
   targets; verify no unhandled runtime address construction. The scan's roots
   include possible data, so invalid decode is not itself proof of a bad module.
2. Validate C++ class sizes and configuration ABI, not only named vtable slots.
3. Implement prelink admission, initialization and VM permissions using the
   actual current canonical kernel. Keep native loading behind its own boot arg.
   Do not flash these analysis files or alter live source based on symbol names.
4. Coordinate with codex-store-download on H2 early return; that active session's
   work is separate. No GET/sign-in trial until a viable change exists.
5. Validate owned-app native GET, full transfer, installation and launch before
   claiming App Store works. No success stub or null crypter can replace decryption.

Previous live probe: cryptid0 succeeds, cryptid1/2 errno45 ENOTSUP, establishing
no registered text_crypter_create at test time. No FairPlayIOKit node in ioreg.
This does not identify the cause of stock H2 early return.
