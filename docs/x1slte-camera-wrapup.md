# Camera wrap-up — 2026-11-16, approved integration

The user confirms the native Camera lens control works and the rear primary
close-focus fix is “10x better”. User subsequently approved: “its all good.. push the changes.. make them
permanent and available in all branches and builds.” Permanent deployment,
branch integration and push are authorized; results are recorded below.

## Current behavior

- Native Camera dial: 0.5x is full ultra-wide, 0.9x preferred default, 1x is
  physical primary; 1–5x digitally crops primary. These are two active rear
  lenses. GW2 is identified (0972/a300) but has no production capture path.
- 2LA ultra-wide: native4032x3024 continuous capture, exposure and gain controls,
  guarded DMA, clean stop/rollback. Front remains on the pinned original worker.
- Primary: correct board voltages, live-state-preserving PMIC enable/shutdown,
  black64 default rather than old undervoltage correction tables. Kernel source
  defaults setup bus half-period to1us with existing failure fallback to2us.
  Running kernel still has2us default with1us selected live; source change awaits
  final kernel deployment.
- Rear zoom keeps the capture graph fixed, including video; switching physical
  sensors incurs a cold-start pause. Seamless transitions are not implemented.
- Native primary full-resolution mode is an explicit opt-in via
  redwood-primary-full; no unverified switch to full mode for fresh installs.
  Current live phone has that opt-in from earlier testing. Binned preview/still
  upsampling must not be described as native12MP capture.

## Close-focus correction now installed for user test

Kernel redwood_camera_s20_focus() accepts0..1023 and shifts right1 for the
AK737x9-bit motor; focus_now reports the motor position shifted left1. The
prepared primary AF cap511 used motor units in a userland-scaled interface,
losing half the available travel. Primary now searches0..1022, with the existing
stepwise motor writes, complete tap sweeps, fine refinement and lock handling.
The test harness models motor quantization as well as competing peaks and both
endpoints. Zoomed primary focus/measurement windows map back into RAW crop
coordinates. Front range, sampling and processing retain the original behavior.

Installed test library SHA256:
69621b4a2ac87f0d6ad5d39d8fc30e30b4694ea84989fdbf5dab6c03169f2b35

Phone rollback: /var/root/libredwood-media-before-close-af.dylib
Host rollback: build/camera-final/rollback/libredwood-media-dev.dylib
Restore that file to /var/redwood/libredwood-media-dev.dylib and restart Camera
and mediaserverd. No reboot or image change was needed for this test.

## Prepared clean-install integration (not deployed)

- userland/build.sh builds the dual-profile media backend for S20, preserving
  original front and independent ultra-wide processing. Other devices retain
  their existing build path.
- build-image-hooks.sh builds native Camera dial and70% foreground brightness
  hooks. prepare-root.py packages media in /redwood/lib, ensures Camera loads
  the tweak loader at every boot and seeds absent lens/brightness hooks plus the
  ultra-wide enable flag. Native keybag, telephony and AOD rules are preserved.
- Cache directory ownership is declared mobile501:501 in var.skel, before init
  seeds controls. Existing data files/settings are preserved by init's
  copy-if-absent semantics. No captured calibration or private scene is shipped.
- Tracked prebuilt artifacts are prepared for PREBUILT builds, including the
  tweak loader. Missing required artifacts fail assembly instead of silently
  dropping Camera features. Built-in media ignores stale development overrides.
- Root packaging happens after var.skel and seed.list assembly. Both kernel
  overlays and root payload must be included in a release image; flashing an
  older image will of course restore that image's older camera code.

Validation: actual generated AF controller with address/undefined sanitizers;
primary stats comparison with front/UW identity; profile ABI tests; zoom ROI
checks; clean-data seed fixture and idempotent root assembly; passcode and AOD
integration checks; shell syntax; signed arm64 media/hooks cross-build. User
confirms rear primary close focus now works. Final optical/regression acceptance
and actual clean-install boot verification remain pending.

## After explicit approval

1. Record user acceptance. Commit reviewed source/artifacts; preserve pre-existing
   changes. Recheck latest accepted main/master before merge.
2. Merge into local main/master and requested branches with normal merges. Three
   branches have unique/cherry-picked histories: camera-quality, fingerprint,
   release/devops. Preserve their unrelated changes; inspect every conflict.
   Never reset worktrees or overwrite local changes. Keep historical snapshot
   and legacy branches as archives unless specifically requested otherwise.
3. Prepare a new deployment from the live root/loader baseline, include latest
   kernel and camera root additions, verify APFS contents and payload sizes.
   Growing root may need repack/sign; do not assume the old root-slot size fits.
   Preserve passcode restore chain, logos, AOD, telephony, system and user data.
4. Back up/read-back verify, deploy with existing session camera-reboot approval,
   then verify locked keybag, native dial, front/main/UW, tap near/far and repeat
   lock. Request user's optical confirmation. Remove temporary primary-test
   default only as agreed (preferred normal default0.9x).
5. Push accepted branches to the writable Jesvi-iOS private remote. Upstream
   oosexgang denies this account write access. Update SHIPPING/handoff with
   actual commit IDs, image hashes and test boundaries; release phone lock.
