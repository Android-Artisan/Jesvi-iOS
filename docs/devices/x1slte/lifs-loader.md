# Apple's lifs.kext in our kernel (A, 2026-11-20)

Why: Files shows external drives through userfsd + livefileproviderd + Apple's user-space file systems
(livefiles_exfat/msdos). livefileproviderd needs Apple's **lifs.kext** (IOKit `com_apple_filesystems_lifs` on
IOResources, user client `AppleLIFSUserClient`); without it it exits ("Didn't find matching client") and userfsd waits
forever (docs/devices/x1slte/sdcard-files-parked.md). Our kernel cannot load kexts. Instead of rewriting lifs, load
Apple's own, from the phone's system volume (`/System/Library/Extensions/lifs.kext/lifs`, 280 KB, iOS 16.5 20F66).

## Why it is small
- MH_KEXT_BUNDLE, arm64 (not arm64e: no pointer authentication), segments from vmaddr 0.
- **452 imports, all present in our kernel** (xnu-8796.121.2 = iOS 16.5's XNU; `nm -u lifs` vs `nm -g mach.x1slte`).
- Relocations are all ARM64_RELOC_UNSIGNED, 64-bit, not pc-relative: 664 external (GOT + pointers: symbol + addend)
  and 286 local (pointer += load address). Code reaches imports through __stubs/__got: no instruction patching.
- Entry: `_kmod_info` (D) -> `__start` -> `__realmain` (lifs_kext_start); IOKit classes from __mod_init_func.

## Loader (bsd/dev/redwood_kextload.c)
1. A boot job hands the file to the kernel (sysctl write); the kernel parses it (MH_KEXT_BUNDLE, arm64 only, bounds).
2. Allocate wired kernel memory for all segments; copy them; apply local and external relocations, resolving names
   through a table generated at build time from the import list (tools/kext/lifs-imports.txt -> asm-label table;
   names only, no Apple code in the repo).
3. Text pages: read-only + executable (a pmap helper clears PNX on exactly those pages; pmap_protect never does);
   flush data cache, invalidate instruction cache and TLB.
4. OSMetaClass::preModLoad("__kernel__") -> __mod_init_func constructors -> postModLoad (classes owned by the
   kernel's own OSKext); kmod_info.start(&kmod_info, NULL).
5. The personality from lifs.kext's Info.plist to gIOCatalogue->addDrivers(): IOKit matches it on IOResources (IOBSD).
Behind boot-arg `lifs=1` (default off until it works).

## Then
livefileproviderd stays up -> userfsd answers -> addDisk with a disk node of the card's exFAT partition -> Files.
