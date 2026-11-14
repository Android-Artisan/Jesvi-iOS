# Software passcode work (experimental)

Reported symptom: Settings accepts a new passcode and its confirmation, but never
turns passcode on. The running phone still reports AKS state flags 0x6 (no
passcode, first-unlock complete). No passcode or keybag data was read, logged,
replaced or removed during diagnosis.

Two concrete blockers were found:

1. MiniAppleKeyStore hard-codes a no-passcode state. Lock/unlock return success
   without checking a secret; create/save/load/change-secret were unsupported.
2. `userland/mkjobs.py` excludes keybagd. MKBKeyBagChangeSystemSecret calls the
   MobileKeyBag shared service, so the native setup service is absent too.

## Implemented foundation

`SoftwareKeybag.h` implements a software-only verifier: random 128-bit salt,
PBKDF2-HMAC-SHA256 with 100,000 rounds, constant-time comparison, bounded inputs,
wrong-code rejection, lock/first-unlock state, old-code verification on change
or removal, and exponentially increasing retry delays after five failures
(1 second up to 60 seconds). Temporary password/KDF buffers are explicitly wiped.

The kernel adapter implements the observed iOS 16.5 libaks ABI for temporary bag
creation, opaque save/load, unload, system-bag selection, lock, unlock, state,
UUID and change-secret. A saved protected bag loads locked before first unlock.
Flag 4 changes produce a separate candidate bag so the original verifier is not
changed before native MobileKeyBag can persist/install its replacement. Root is
required for bag export, replacement and passcode changes. Queries and checking
a code do not automatically succeed. Unsupported SE-derived operations fail.

The code is gated by **`softpasscode=1`**, off by default. It is enabled on the
current test phone alongside the existing cooling changes. The SpringBoard
bring-up unlock helper refuses dismissal when a passcode is set or keybag state
is unknown; `autounlock=0` remains active.

Native keybagd now runs through a dedicated job with a restrictive umask. The
`no-effaceable-storage` device-tree capability selects Apple's software storage
path. Native change-secret epilogue verifies the installed secret rather than
returning unconditional success. `softkeybag-init` restores the saved software
bag locked before handing control to launchd; invalid or insecure existing files
stop startup rather than silently reverting to no passcode. Production keybagd
verbose logging is disabled because native log messages can contain credentials.

On-device isolated bag tests passed. Native empty-to-empty system transactions
passed before the user enabled a real code. The user then confirmed Settings
keeps the passcode enabled; native state is protected/unlocked (AKS 0x4, MKB 0),
and the native saved keybag is private (0600).

**Remaining failure:** the user can still swipe through the lock screen without
a code. SpringBoard's authentication controller caches “no passcode” despite the
protected system bag. This is not a working end-to-end passcode yet.

A prepared kernel now emits Apple's IOGeneralInterest keybag events after system
state changes. Local iOS 16.5 `MKBEventsRegister` (0x1ae395488) registers this
interest, and `aksNotificationCB` (0x1ae37e8b8) maps `0xe3ffd00d` to state-change
and `0xe3ffd00f` to first-unlock. `SBFMobileKeyBag` refreshes its cached passcode
state on these events. Notifications run outside the bag lock and require no
poller. This patch builds but awaits an approved device reboot and lock-screen
validation; its diagnosis is supported by the missing events, not yet confirmed
by the phone. A user-client backup notification port is a separate interface.

## Limits

This is software authentication for this SEP-less development port, not a Secure
Enclave emulator or equivalent hardware security. It does not add encrypted
storage/class keys, SEP-backed keychain access control, Face ID/Touch ID, UIK
certificates or Apple attestation. Existing key wrapping remains reversible.
Root/kernel access can modify the verifier; exported records permit offline
passcode guessing. Retry counters are volatile and are reset by reload/reboot;
no hardware anti-hammering, rollback prevention or cryptographic erase is claimed.
Existing real SEP keybags are rejected, not converted or made accessible.

The KDF runs only on credential operations, not in a monitor or polling thread.
Read-only state calls do not run the KDF. The experimental global state lock
serializes bag operations; latency must be measured on-device before enabling it.

## Evidence from the locally extracted iOS 16.5 cache

AppleKeyStore exports and call sites establish these selectors:

| Selector | Operation | Input shape |
| --- | --- | --- |
| 2 | create bag | 6 scalars; password pointer/length at 2/3 |
| 3 | save bag | handle scalar, opaque structure output |
| 4 | unload bag | handle scalar |
| 5 | set system bag | handle, system ID, optional password pointer/length |
| 6 | load bag | opaque structure input, handle scalar output |
| 7 | state | handle, flags scalar output |
| 8 / 0x20 | lock | handle |
| 9 / 0x0c | unlock | handle, user password pointer, length |
| 0x0f | change secret | 11 scalars, old pointer/length at 3/4, new at 5/6 |

Addresses (unslid): aks_create_bag 0x1f22d38e0, aks_load_bag 0x1f22d3b94,
aks_save_bag 0x1f22d3de8, aks_change_secret_se 0x1f22d3f1c,
aks_unlock_device 0x1f22d7788. MKBKeyBagChangeSystemSecret at 0x1ae386ca0
calls sharedService/changeSystemSecretFromOldPasscode:ToNew:withOpaqueData:.
These are development ABI observations, not a published stable interface.

## Tests and next step

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tools/passcode/test_software_keybag.c -o /tmp/test-software-keybag
/tmp/test-software-keybag
python3 tools/passcode/test_native_adapter.py
```

The host test compares the actual KDF against CommonCrypto's independent PBKDF2
implementation and verifies wrong-code rejection, change/remove, binary secrets, saved reload
requiring authentication, rate limits and malformed records. The actual kernel adapter also passes host tests under address/undefined-behavior
sanitizers with narrow IOKit mocks: privilege and input checks, transaction cloning,
serialization/reload and system installation. These mocks do not open a real
keystore. The kernel builds for Exynos GICv2 DEVELOPMENT. The SpringBoard library and iOS probe cross-compile.

`tools/passcode/keybag-probe.c` is read-only by default. Its isolated tests use
non-system temporary bags and were validated on-device. Native transaction
probes refuse to run when a passcode is present; do not use a real passcode in
command arguments, logs or test tools. The user enters it only on the phone.

`tools/passcode/prepare-root.py` adds the native keybagd job, guarded SpringBoard
library and boot restore helper to a prepared root. `SOFTWARE_PASSCODE=1` opts
userland builds into this integration. Kernel artifact:
`out/mach.x1slte-softpasscode`. Private deployment images and backups remain in
ignored `out/passcode-deploy-20261114`; never commit them.

Validation still required: approved boot with a protected saved bag; code prompt
on lock; incorrect code rejected; correct code accepted; subsequent lock/unlock
and passcode change/removal state updates. A reboot needs user confirmation.
