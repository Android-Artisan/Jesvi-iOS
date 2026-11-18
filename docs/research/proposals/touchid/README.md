# Native Touch ID on the S20 (plan, 2026-11-18)

Goal: Apple's own Touch ID (Settings → Touch ID & Passcode enrolment screen, lock-screen unlock, app authentication)
driven by the S20's real fingerprint stack, with real matching.

## Sources

- **redfin SweetMesa** (DenialOfGravity/ios-redfin `redfin-int` 7f0764b, MIT, credit in NOTICE): a kernel persona of
  the Touch ID (Mesa) side of the Secure Enclave for Apple's unmodified `biometrickitd` + `libBKDM1`, and a bridge
  user client to a userland sensor daemon. `redfin/SweetMesa.cpp` here is their patches 1192, 1198-1207, 1210-1213
  applied in order (2755 lines); `redfin/sweetmesa_bridge.h` is the bridge ABI; `redfin/sweetfpcd.c` their daemon.
- **Our fingerprint stack** (`feat/s20-fingerprint`, FINGERPRINT-HANDOVER.md): sensor driver + TEEGRIS TA channel.
  Enrolment works (17 touches, 100 %), template save/load works, **matching returns TA 0x20**. The user reports that a
  match succeeds only with the finger placed exactly as at registration: consistent with a template built from too
  few or near-identical captures, or with the identify call missing a step (to verify, step 4).

## Architecture on the S20

```
Settings / SpringBoard / LocalAuthentication
        │  (unmodified)
biometrickitd + libBKDM1 (iPhone 8's, relinked)       ← MobileGestalt says: Touch ID device
        │  AppleBiometricServicesUserClient, 'BM' commands
kernel: RedwoodMesa (SweetMesa port)  ── 4 hooks ──→ MiniAppleKeyStore (software passcode)
        │  bridge user client (WAIT / REPLY / EVENT / STATE)
userland: redwood-fpd  ──→  our fingerprint driver + TEEGRIS TA (enrol-capture, identify, template store)
```

Keystore hooks (`sweet_pc_bio_*` in redfin): allowed only after a passcode unlock since boot, within 48 h, fewer than
5 failed matches; a verified match unlocks; failures counted; passcode always required after reboot.

## Steps

1. **Kernel:** `RedwoodMesa.cpp` from SweetMesa.cpp (`redwood_` names, `kern.redwood.mesa*` sysctls), 4 hooks in
   MiniAppleKeyStore, `touchid=0` boot argument (default off). Build only.
2. **iOS side:** iPhone 8 (iPhone10,1) iOS 16.5 `libBKDM1` from Apple's CDN via the existing partial-download path,
   relinked; MobileGestalt Touch ID keys; `com.apple.biometrickitd` Mesa job. Image build only.
3. **redwood-fpd:** bridge daemon: enrol requests → our per-touch enrol capture (Apple's UI asks for ~16 touches in two
   phases), identify → our TA identify, events back as enrol progress / match records.
4. **Matching (0x20):** with the native UI producing a proper multi-touch template, re-test identify; then trace the TA
   identify sequence against Samsung's stock flow (FINGERPRINT-HANDOVER.md, tools/fingerprint/reverse).
5. Phone test (one batch, behind `touchid=1`): enrol in Settings, lock-screen unlock, app prompt, reboot (passcode
   first), 5 wrong fingers → passcode.

Matches are only ever delivered when our TA says the finger matched: no "any touch" path.
