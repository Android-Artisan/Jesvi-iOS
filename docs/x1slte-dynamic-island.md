# S20 Dynamic Island (`island=1`)

iOS 16.5's own Dynamic Island on the S20's top-centre camera hole: SpringBoard's SystemAperture, the
"Pill" status bar, Live Activities layout. Nothing is drawn by us; iOS is told the screen is a 14 Pro's.
User-verified 2026-11-12 (pill over the hole, home-bar swipe, touch normal).

## The flag

| Boot argument | Result |
| --- | --- |
| `island=1` (**default**, in `x1slte.cmdline`) | Dynamic Island |
| no `island=` / `island=0` | iPhone X notch layout, as before |

Switch it by editing `x1slte.cmdline`, then `./x1slte.sh payload "$(cat x1slte.cmdline)"`,
`./x1slte.sh pack`, `tools/x1slte-auto.sh --recovery`. One reboot; nothing else to do.

## What it changes

All in the loader's device tree (`xnuloader/loader.c`), only with `island=1`:

| Device tree | iPhone X (off) | With `island=1` | Read by |
| --- | --- | --- | --- |
| `/product/artwork-device-subtype` | 2436 | **2556** (iPhone 14 Pro) | MobileGestalt `ArtworkTraits.ArtworkDeviceSubType` → SpringBoard turns on SystemAperture |
| `/buttons/home-button-type` | (absent) | **2** (no home button) | MobileGestalt `HomeButtonType` → BaseBoard `-[BSPlatform homeButtonType]` |

Why the second one: BaseBoard takes MobileGestalt's `HomeButtonType` (1 or 2) and otherwise calls a phone
buttonless **only if its subtype is 2436**. The iPhone X's tree has no `/buttons` node, so with the subtype
alone at 2556 iOS acts like a home-button iPhone ("press home to unlock", swipe-up opens Control Center,
a black bar at the top). A real 14 Pro's tree has `home-button-type` 2.

MobileGestalt caches its answers in
`/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`
and does not rebuild it when the device tree changes. `init` (`userland/src/init.c`, `island_cache_sync`)
keeps the last boot's flag in `/var/db/redwood-island` and removes that cache when the flag changed, so
MobileGestalt rebuilds it from the new tree. (This needs a root image built with that `init`; with an
older one, delete the cache by hand before the reboot.)

## Pill position and size (in libredwood-sb)

iOS 16.5 hard-codes the pill in SpringBoardUIServices (14 Pro numbers): size 125.33 x 36.67 pt and top gap
11.33 pt (`-[SBUISystemApertureLayoutMetrics _minimumScreenEdgeInset]`). `userland/src/island_sb.h`,
part of libredwood-sb (SpringBoard's library on the boot image), replaces those two methods from the main
queue once SpringBoard runs, only with `island=1`.

| Setting | Default | Apple |
| --- | --- | --- |
| top gap `y` | **5 pt** (pill covers the top of the S20 hole; user-checked 2026-11-12) | 11.33 |
| width `w` | Apple's | 125.33 (~81 = "iPhone 18 Pro" -35%) |
| height `h` | Apple's | 36.67 |

Override in `/var/jb/etc/redwood-island.conf` (e.g. `y=5 w=81`; `off` = Apple's pill), then respring.
Log: `/var/mobile/Library/Logs/redwood-island.log`. 1 pt on the S20 panel ~ 0.18 mm.

Not as a /var/jb tweak: any library from /var/jb loaded into SpringBoard fails the kernel's
`map_with_linking_np` code-signature check and costs SpringBoard its CS_VALID; if that lands before
SpringBoardUI's +load, SpringBoardUI throws "This process should not be linking or loading
SpringBoardUI.framework" and SpringBoard crash-loops (timing-dependent). (userland/src/island_tweak.m
and island-install.sh are that withdrawn version.)

## Live Activities

The Clock timer shows in the island (user-verified 2026-11-12, after a cold boot). The first one after a boot
can take a while to appear (the Live Activity services are still starting), so give it a moment before
calling it broken. The stopwatch shows too (user-verified).
Live log on the phone: `ssh s20 /var/jb/usr/bin/oslog` (Procursus `oslog`, installed 2026-11-12).

## Checking it

```
ssh s20 /redwood/bin/mg_test HomeButtonType oPeik/9e8lQWMszEjbPzng   # 2, ArtworkDeviceSubType 2556
ssh s20 /usr/sbin/ioreg -p IODeviceTree -l -w0 | grep -E "home-button-type|artwork-device-subtype"
```

## Notes

- Not an overlay like Dynamic Peninsula; same mechanism as DynamicCow / Nugget (ArtworkDeviceSubType 2556),
  plus the home-button answer, which on real iPhones comes from their own device tree.
- The screen iOS renders stays 1125x2436 (iPhone X); the pill is placed by points from the top centre and
  covers the S20's hole. Its size is Apple's (bigger than the hole, as on a 14 Pro).
- How it was found: `ipsw` 3.1.745 on the 20F66 iPhone10,3 shared cache (no `.symbols` file in it:
  a copy of the main cache with the expected UUID stands in). BaseBoard `sub_18dd64384`
  (`cmp w0, #0x984`); libMobileGestalt `sub_19c53a248` (`IODeviceTree:/buttons`, `home-button-type`).
