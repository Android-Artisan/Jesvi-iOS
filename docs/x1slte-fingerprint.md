# S20 ultrasonic fingerprint research

## Status: no fingerprint unlock yet

Branch `feat/s20-fingerprint` starts at local main/master `8d584cc` (14 November
2026), including the newer A76/GPU thermal steps, Wi-Fi scan coalescing and quieter
logs. Main/master are not advanced with this experiment. The user approved the first
probe boot and explicitly authorized taking control of the shared phone lock.
Fingerprint enrollment has not been performed. The current passcode stays
under the existing software-keybag implementation.

The user wants the actual under-display fingerprint sensor to authenticate native
iOS unlock, with passcode fallback. Merely detecting a finger, fabricating a
BiometricKit match, dismissing SpringBoard, or accepting arbitrary IPC is not that.
No successful-match injection or biometric unlock selector is added here.

Prepared artifacts in ignored `out/fingerprint/`:

- `fingerprint-status`: read-only iOS LocalAuthentication/IOKit diagnostic.
- `wiring.json`: selected properties from the phone's locally owned DTB/DTBO.
- `kernel-build.log`, `mach.x1slte-fingerprint-probe`, `kernel-slot.bin`: the
  experimental one-shot compatibility-probe kernel and its padded flash slot.
- Synthetic host tests for the policy, DTB inventory and manual-probe guard.

## Confirmed device wiring

The actual saved `dtbo.1.dtb` has a `qcom,qbt2000` node, matching the reference
Exynos990 x1slte revision-21 device tree. The base DTB resolves its external
symbols as follows (overlay phandles such as `0xffffffff` are not physical addresses):

| Item | Configuration evidence |
| --- | --- |
| Sensor/model | Qualcomm QBT2000 / G98X |
| Firmware IPC interrupt | GPA1 pin 3 |
| Finger detect / WUHB | GPA1 pin 1 |
| GPIO controller | `/pinctrl@15850000/gpa1` |
| Power supply | `VDD_BTP_1P8` |
| SPI bus | SPI4 / PERIC0 USI04; GPP2 pins 0–2, function 2 |
| SPI chip select | GPP2 pin 3, function 2 in the active pinctrl state |
| SPI inactive | GPP2 pins 0–3, input function, pull-up |

This identifies board wiring, not working communication or a successful live
sensor probe. No GPIO, regulator, sensor reset or SPI configuration was changed.

Samsung's source at local reference commit `69515fbb7` has
`CONFIG_SENSORS_QBT2000=y`, `CONFIG_TEEGRIS_VERSION=4`, and secure fingerprint mode.
Its Kconfig explicitly assigns SPI communication to the trusted application in
secure mode. The normal-world driver exposes power/clock control, finger-down/up
and firmware notification plumbing. The nonsecure qbtspi path is not a demonstrated
production matching API. Do not substitute the touchscreen for fingerprint input.

Local Android init backups reference `tzdaemon`, `tzts_daemon`, the fingerprint
HAL service, and qbt2000 device nodes. A vendor TEE object with a fingerprint-like
UUID namespace is present, but its filename does not establish a callable protocol
or successful loading. The proprietary fingerprint HAL/client libraries have not
been located in the captured firmware subset. No firmware or biometric material
is copied into git.

## Live native-iOS baseline

Read-only checks found no AppleMesa or ApplePearlSEPDriver service, no exposed
qbt2000/tzdev device nodes, and no running biometrickitd in the sampled process
list. AppleKeyStore advertises the existing software passcode backend. After the
user re-enabled their passcode, native state was protected/locked (MKB 1, AKS 0x5).

`tools/fingerprint/status.m` ran on the phone without evaluating an authentication
policy or starting enrollment/matching. Results:

| Query | Result |
| --- | --- |
| LocalAuthentication biometrics available | false |
| Reported biometry type | 0 |
| Biometrics capability error | -1000 |
| Device-owner capability error | -1000 |
| AppleKeyStore / software passcode backend | present / enabled |
| AppleMesa / ApplePearlSEPDriver | absent / absent |

Do not reinterpret the device-owner capability error as “no passcode”: raw native
MKB independently confirms the protected bag. The probe is a command-line
caller, not proof of every app's entitlement/policy behavior. It logs only
capability booleans, numeric errors and state, never credentials or templates.

The extracted iOS 16.5 BiometricKit cache contains BKDeviceTouchID, BKDevicePearl,
BKEnrollTouchIDOperation, BKMatchOperation and XPC match/enrollment callbacks. The
public app-facing goal is LocalAuthentication with fingerprint semantics, not
claiming fingerprint hardware is Face ID. Existing native keybag/credential binding
must work too; drawing a Touch ID prompt or enabling biometrickitd alone is insufficient.

## Prototype policy, deliberately not connected to unlock

`tools/fingerprint/policy.h` is a host-testable decision core only. It is not
linked into the kernel, SpringBoard or any daemon. It has no credential storage,
matcher, IPC service or unlock API. `FP_VERIFIED_MATCH` is a caller contract: a
future trusted backend must verify secure-world proof before making that call.
Passing that enum does not cryptographically verify anything by itself.

The prototype requires a genuine passcode verification after initialization,
backend failure/restart, enrollment change or invalidated keybag generation. It
binds each request to a fresh coordinator-generated 128-bit challenge, keybag and
enrollment generations, and a five-second lifetime. Presence events only wait;
cancelled, timed-out, cross-generation or replayed completions are denied.
A failed match requires passcode immediately (the user's requested conservative
policy); successful biometrics do not reset the passcode-verification timestamp.
An eight-hour passcode validity ceiling is a prototype restriction, not a claim
to reproduce every Apple security timer.

Apple's documented policy requires passcode after restart and allows at most five
unsuccessful biometric matches; a lower fallback threshold can be used. Native
policy integration and its first-unlock, timeout and lockout behavior still need
to be validated when a real matching backend exists.

## Prepared first hardware test: TEE compatibility only

The kernel debug port gains `fp.tee-version 0|1` on the Exynos/GICv2 build:

- `0` reports the planned call, or its cached result; it never initiates a call.
- `1` makes exactly one explicit compatibility call per boot. Further requests
  return its cached result, including errors. No retries or polling thread.

The allowlisted ABI is Samsung's ARM fast SMC32 trusted-OS CHECK_VERSION:
function `0xb200000d`, reference-client version field `0x41357` (4.19.87), driver
ABI `0x30000` (3.0.0), with unused argument registers zero. The client version is
an observed ABI field, not an assertion that XNU implements Linux. Calls use the
existing root-only serialized debug port. No generic arbitrary-SMC interface is
added. The diagnostic does not run at boot, register shared pages, load a trustlet,
control the sensor, verify a fingerprint or change a keybag. Compatibility
negotiation can affect volatile secure-world state; unknown firmware can still
reject or fail the call.

Cross-compile `tools/fingerprint/probe.c` with the iOS SDK, sign ad hoc and copy
it to `/var/tmp/redwood-fingerprint-probe`. After an approved test boot, run on
the phone as root; inspect first, then explicitly call once:

```sh
/var/tmp/redwood-fingerprint-probe --inspect
/var/tmp/redwood-fingerprint-probe --call-once
```

Before deployment, acquire the shared phone lock, obtain user reboot approval,
and back up/read back the currently installed 16-MiB recovery kernel slot. Flash
only the new slot at recovery offset `0x2000000`; preserve current boot arguments,
loader, root volume and userdata. Retain the exact previous slot for rollback.
A successful version response proves only this SMC exchange; it does not prove
trustlet loading, enrollment, matching, signed proof verification or iOS unlock.

## Hardware result: version probe passed

The current 16-MiB recovery kernel slot was read from offset 0x2000000 into
`out/fingerprint/kernel-before-84931a3b.bin` before any write. The new slot was
written and verified by the loader's readback. Boot arguments, loader, root and
userdata were left in place. Deployment hashes are in ignored `deployment.json`.

After the approved reboot, native MKB reported protected/locked before first
unlock (1 / AKS 0x1). The inspect command said NOT RUN. The explicit version call
returned `x0=0, x2=0`, and a subsequent inspect returned the same cached result.
There was no second SMC call. The user confirmed passcode unlock both immediately
after boot and again after re-locking. This establishes the compatibility
exchange, not any enrollment, match, signed-token or native-biometric capability.
`/var/tmp` is cleared on this port's boot; copy the diagnostic client after boot.

## AUX and service channels: hardware test passed

`redwood_fp_channels.h` and fixed helpers in the debug port implement the first
shared-memory steps observed in Samsung's initialization order: per-CPU AUX
registration (SMC 0xb2000000), followed by one service channel (0xb2000001,
mode 1, one page). The manually armed `fp.tee-channels 1` command first requires
a successful explicit version probe, a supported CPU count and all configured
CPUs online. Inspect mode 0 allocates nothing and invokes nothing.

The reference Linux page configuration defaults to 4 KiB with a 32-bit PFN ABI.
For this prototype, each shared page has a dedicated, zeroed, wired 16-KiB XNU
backing page, and only its first 4 KiB is shared. PFNs are physical >> 12, checked
for alignment/range; no kernel/user data from adjacent allocations is included.
All nine backing pages are allocated before the first registration on an 8-core
phone. Each AUX call is bound to its CPU, and the original binding is restored.
Cache clean/invalidate and barriers surround sharing and the bounded service
response read. Registered/uncertain pages stay wired until reboot, including on
failure; total backing on this phone is at most 144 KiB. No periodic work.

At the first firmware error, the sequence stops without retrying or registering
later channels. Repeated commands only report cached status. The prototype does
not install IRQ handlers, send SYSCONF, start firmware scheduling/TA loading,
power the fingerprint sensor, enroll, match or change keybag authentication.
Zero/absent service masks must not be treated as working authentication.

The channel sequence passes host guard/partial-failure tests with ASan/UBSan, and
the Exynos kernel cross-build passes. The user approved a second reboot and test. A fresh backup matched the previously
tested version-only slot, and the loader verified the channel-kernel write. It
is now the running kernel. The explicit test returned:

`retained AUX=8 service=1 result=0 cpu_mask=0x1 user_mask=0x1`.

A subsequent inspect returned the cached result, and native MKB remained protected
and locked before first unlock (1 / AKS 0x1). This confirms the first shared-memory
transport handshake, not trustlet loading or fingerprint authentication. The
on-phone commands were:

```sh
/var/tmp/redwood-fingerprint-probe --inspect
/var/tmp/redwood-fingerprint-probe --call-once
/var/tmp/redwood-fingerprint-probe --inspect-channels
/var/tmp/redwood-fingerprint-probe --connect-channels
```

The fixed-command C client refuses a kernel without the expected diagnostic
prefix and never accepts arbitrary SMC addresses or arguments.

## Next prepared test: IRQ and SYSCONF (not flashed)

The saved base DT `/teegris` node contains GIC SPI 222/223, level-high, corresponding
to absolute INTID 254/255. The next manually armed `fp.tee-sysconf 1` prototype
requires a successful complete channel handshake. It claims these otherwise
unused IRQs, with conflict checks, and sends `{event=254, panic=255, flags=0}` in
CPU0's dedicated AUX buffer to SMC 0xb2000009. It reads only the first four response
words: secure OS version, CPU count, big-core mask and flags. CPU count and mask
shape are validated; a zero SMC result alone is insufficient.

Handlers mask on the first event and increment counters. No firmware scheduling,
thread pool, log drain or matching is started in the ISR, and panic events
invalidate the diagnostic's result. This avoids an interrupt storm while the full
scheduler is absent. IRQs and backing buffers are retained safely until reboot
once firmware may reference them; this is not production IRQ handling. If an IRQ
is already claimed or fires before configuration, the exchange is refused. No
hardware call or IRQ attachment occurs at boot or in inspect mode.

This kernel cross-builds, and host tests cover gating, partial/error conditions,
invalid response shape and panic rejection. It is staged as
`out/fingerprint/kernel-sysconf-slot.bin`, not flashed. After an approved reboot,
repeat the version/channel sequence, then use:

```sh
/var/tmp/redwood-fingerprint-probe --inspect-sysconf
/var/tmp/redwood-fingerprint-probe --exchange-sysconf
```

## IRQ and SYSCONF: hardware test passed

The user approved a third reboot. A fresh backup of the running channel kernel was taken and verified
(`out/fingerprint/kernel-before-sysconf-b630a525.bin`); the loader verified the IRQ/SYSCONF slot's write. After boot
native MKB was protected and locked before first unlock (1 / AKS 0x1). Version and channel prerequisites were repeated
(`x0=0 x2=0`; `retained AUX=8 service=1 result=0 cpu_mask=0x1 user_mask=0x1`), then the exchange:

`fp.tee-sysconf: result=0 valid=1 os=0x4010000 cpus=8 big_mask=0xc0 flags=0x1 event_irqs=0 panic_irqs=0`

The secure OS reports version 0x4010000 (TEEGRIS 4.1), 8 CPUs and big cores 6-7 (mask 0xc0, the M5 pair), flags 0x1.
No event or panic interrupt arrived: with no normal-world scheduler yet, the secure world has nothing queued that
would raise one. A later inspect returned the cached result. This is the last step of Samsung's tzdev initialization
before scheduling; it does not load a trusted application, power the sensor, enroll or match.

## Vendor-client reference located

A public Exynos990 vendor reference was located at
`Exynoobs/proprietary_vendor_samsung_exynos990-common`, commit
`715080389b102e4d175a9244a84910ee549d3d67` (lineage-19.1). Selected ARM64 ELF client
libraries and tzdaemon were downloaded only into ignored
`out/fingerprint/vendor-reference/` for local protocol analysis. They are not
executed on the phone, linked into the port, or committed. They are not verified
to match the phone's exact stock firmware version.

Exported symbols include BAuth enrollment, identification, HAT, password and
session operations, plus TEEC and Samsung TEECS calls. The identification wrappers
use TEEC InvokeCommand IDs 5 (init), 6 (do) and 7 (final). The common transfer path
uses a registered partial in/out shared-memory parameter. The TEEC client expects
`/dev/tzdev`, `/dev/tziwsock` and a loader daemon path. These are observed protocol
clues, not a ready-to-use matching implementation; no guessed packets were sent.
Different TA/loading modes and shared-memory layouts must be resolved before
invoking enrollment or identification on actual firmware.

## Remaining implementation and traps

1. Version compatibility returned zero on hardware. Next validate the prepared
   shared-memory handshake; stop on firmware errors rather than guessing SMC IDs.
2. Implement a narrow TEEGRIS transport: per-core AUX buffers, shared-memory
   registration, event/panic IRQs, scheduling, inter-world sockets and userspace
   loading/storage services as required by the actual firmware. Samsung's
   reference init order registers several such services; one SMC is not tzdev.
3. Resolve the signed fingerprint trusted application's real client protocol and
   matching result verification. Locate legitimate matching HAL/client binaries
   from this device's own stock firmware if the captured subset lacks them.
   Do not assume Android ELF/HIDL services can execute directly under Darwin.
4. Implement protected enrollment and template lifecycle. Old Android enrollment
   data may not survive the current userdata layout; never promise reuse or read
   template/TEE storage to bypass enrollment. Passcode consent occurs on-device.
5. Connect verified matching to a keybag-bound native authentication operation
   and the actual iOS BiometricKit/LocalAuthentication provider. Do not store the
   user's passcode for replay or expose a “match=true” notification as authority.
6. Validate wrong/right finger, after-boot passcode, cancellation, replay, lockout,
   app policy queries, passcode changes/removal, and repeated locks. Keep backend
   capability unavailable until these prerequisites exist.

Further traps:

- XNU uses 16-KiB pages; Samsung's shared-memory ABI derives page-frame numbers
  and AUX buffer sizes from its Linux page configuration. Resolve the actual
  secure page size/PFN width before sharing memory. Do not blindly pass XNU PFNs
  or expose adjacent allocations in a larger host page.
- Per-core AUX channels require CPU affinity/lifetime handling; a migrating XNU
  thread cannot assume the same channel remains selected across calls.
- Preserve secure storage and EFS. Firmware presence does not grant permission
  to overwrite secure enrollment/keymaster/gatekeeper state.
- Verify signed/protected result provenance and bind fresh challenges to one
  unlock request. A sensor IRQ, native UI callback or root CLI argument is not
  a fingerprint match proof.
- This remains software-keybag authentication on the iOS side, not Apple SEP
  equivalence, Apple Pay, attestation or full keychain access-control support.
- User credentials are entered only on the device. Never log them, embed them
  in commands/test vectors, or enable verbose native keybagd logging.

## Reproduce checks

```sh
python3 tools/fingerprint/test_inventory.py
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_policy.c -o /tmp/fp-policy && /tmp/fp-policy
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_tee_probe.c -o /tmp/fp-tee-probe && /tmp/fp-tee-probe
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_channels.c -o /tmp/fp-channels && /tmp/fp-channels
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/fingerprint/test_sysconf.c -o /tmp/fp-sysconf && /tmp/fp-sysconf
python3 tools/passcode/test_build_integration.py
```

Read the phone's own DTBs without copying them into the repository:

```sh
python3 tools/fingerprint/inventory.py /path/to/boot.dtb /path/to/dtbo.dtb
```

The status probe cross-compiles with the iPhoneOS16.5 SDK, ARC/blocks, and
Foundation, LocalAuthentication and IOKit frameworks. No special authentication
entitlement or match permission is requested. Kernel build: Exynos GICv2
DEVELOPMENT. Tests validate code/policy/guard behavior; they do not validate a
physical fingerprint match. The IRQ/SYSCONF kernel is flashed and its exchange passed (above). No physical fingerprint match has been attempted.

## Primary references

- [Samsung Exynos990 kernel reference](https://github.com/ExtremeXT/android_kernel_samsung_exynos990/tree/69515fbb7/drivers/fingerprint)
  and its `drivers/misc/tzdev` implementation, Kconfig, board DT and clock tables.
- [Apple biometric/passcode requirements](https://support.apple.com/en-mt/guide/security/sec9479035f1/web).
- [Android fingerprint HAL architecture](https://android.googlesource.com/platform/docs/source.android.com/+/0fda4ae8f8f65df3360a3170059b5ea11c99f7ec/src/devices/tech/security/authentication/fingerprint-hal.jd).
- [Android authentication tokens and trusted matching](https://source.android.com/docs/security/features/authentication).

- [Public Exynos990 vendor-client reference](https://github.com/Exynoobs/proprietary_vendor_samsung_exynos990-common/tree/715080389b102e4d175a9244a84910ee549d3d67).
