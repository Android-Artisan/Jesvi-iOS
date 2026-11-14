# Fingerprint handover — 14 November 2026

## Current outcome

- Enrollment works. The latest fresh registration completed with **17 accepted
  touches, 18 quality retries, 100%**.
- Template persistence/load works when the correct storage path is supplied.
  A real post-reboot load of the original file succeeded. The fresh file was
  explicitly read from disk and reloaded through the structured client, not the
  kernel cache; the TA accepted finger ID `1:`.
- **Matching still fails**: both old and fresh templates return TA error `0x20`.
  The fresh match test had no image-quality errors. No verified match or native
  fingerprint unlock has been demonstrated.
- Native software passcode is intact. Both approved kernel installations were
  read-back verified; each boot started with a locked keybag (`AKS 0x1`), and the
  user confirmed unlock/relock/unlock on both kernels.

The user is handing the phone to another kernel agent. Capture is complete; no
additional flash or touches are needed for the next offline analysis step.
Consult `tools/phone-lock.sh status` before touching the phone. Do not reboot it
as part of this handover without the user's approval.

## Branch, installed image, prepared image

Repository: `/Users/jesvi/ios-port-research/work/hackios-cellular`

Branch: `feat/s20-fingerprint`. Main/master/release worktrees were not switched or
modified. Current branch HEAD contains the automatic storage-path fix and host
view/diagnostic tools. Do not integrate only the last patch into a branch missing
the earlier TEEGRIS/fingerprint infrastructure.

The phone currently runs **`7e8181fe`, exchange-v2**, with the path restored
manually during this session. It does not yet contain the automatic kernel fix.
The installed artifact is `out/fingerprint/mach.x1slte-exchange` (15,195,608 B),
SHA256 `1286c7889ccd348cd0dddc0e3aa9025efa73b5a42eccbeb7d07c04d15ce25432`.
Its status reports `fingerprint wire: exchange-v2 (research only), active 0`.

Prepared next kernel, **not installed**:
`out/fingerprint/mach.x1slte-storepath` (15,195,608 B), SHA256
`6b3a617f0e4664d9313a2717d7e3136df33de5c9936aabfca51632d3e41fb0d1`.
Manifest: `out/fingerprint/storepath-build.json`. This adds automatic path
restoration and nonblocking research-file opens. Source mirrors are identical;
GICv2 build and relevant host tests pass. The exact request packing was validated
on exchange-v2 using the structured client. This artifact has not had a boot test.

Kernel-slot backups (16 MiB each):

- `out/fingerprint/slot-backup-200455.bin`: before the first audit kernel.
- `out/fingerprint/slot-backup-204306.bin`: audit-v1 before exchange-v2.

These are kernel-slot backups, not full recovery images. Preserve loader, boot
arguments, root/system images, current keybags and data. Preserve `softpasscode=1`,
`autounlock=0` and all existing native keybag/relock guards.

## Confirmed restore bug

`BAuth_Enroll_Init` (`libbauthtzcommon`, Ghidra `0x1047a4`) copies **store_path**
to request `+0x118`. Our code had called it a name and supplied `"fingerprint"`.
That exact zero-padded **100-byte path** is authenticated in the template's outer
HMAC. Same-boot tests reused that context and the cached template, hiding the
missing restore step. A cold load with an empty path produced outer-HMAC error28
and final error31. Restoring `"fingerprint"` made the existing file load without
changing it or bypassing integrity.

The latest kernel source uses `FP_STORE_PATH` in both Enroll Init and Identify
Init (`+0x6455d`). **Do not rename the legacy path when branding the project.**
Python `wire.identify_init()` defaults to the same `LEGACY_STORE_PATH`.

On the installed older kernel, the workaround is CONTROL `0x30` with the exact
100-byte path before Identify Init. The existing private request is
`out/fingerprint/private/legacy-path-request.bin`; use a new local reply filename
with `exchange.py`. Do not send `fp exchange` to a kernel without exchange-v2:
the older generic parser does not reject unknown words; the host tool guards it.

Factory-calibrated initialization used:

1. `tools/fingerprint/tee-up.sh 1:2` starts TEE/driver TAs and powers the sensor.
2. Sequentially: `fp cbgeefs`, `fp 10:1`, `fp 1:2`; wait for each to finish.
3. Restore the legacy path on the installed kernel, then `fp idinit`.
4. Verify zero errors and finger list `1:` before capture. Never run auto-CBGE on
   error68; that was an incorrect earlier interpretation.

The fresh enrollment was requested by the user. It used `fp einit 1`, then
`fp run 3 1 150 4`. The subsequent full wire Identify Init used the file fetched
from disk with the explicit legacy path. The immediate test was
`fp run 6 1 30 0`; no match. No further live test is running.

## Private evidence captured before the next flash

Never commit, paste or publish the following payloads.

On the phone, all mode0600:

- `/var/root/fp-template.bin`: current fresh encrypted template, 376,182 B.
- `/var/root/fp-template.before-wire-audit.bin`: preserved original template,
  376,182 B. Its bytes differ from the fresh file.
- `/var/root/fp-calib/cbge_0.dat`, `cbge_8.dat`: factory sec_efs calibration,
  368,096 B and 368,024 B. Mac originals are in private `device/fp-calib/`.

On the Mac, `out/fingerprint/private/` is mode0700, files mode0600:

- `original-template-before-fresh.bin`, `fresh-template-2101.bin`.
- `fresh-idinit-request.bin` / reply: actual forced-file-load request and result.
- `identify-2055-trace.log`, `fresh-identify-2104-trace.log`.
- `qdbl-2105.bin` (4,984 B), its full wire reply and sanitized JSON summary.
- `handover-tee-status.txt`, `handover-message-buffer.bin`,
  `handover-evidence.json`.

The current saved file was compared against the forced-reload input: exact
byte equality. The original and fresh files were compared: they differ.
The message buffer and idle TA status have been captured. No secure key or
passcode was exported or requested.

## Next: decode the actual comparison path, offline first

Samsung's `getAdditionalLog` (`libbauthserver`, `0x14bcc8`) invokes CONTROL `0x4c`.
The TA `qfp_get_qdbl_info` drains a ring up to 0x8000 bytes; the research request
provided that capacity. This works despite disabled level-2 text logging.

Run:

```sh
python3 tools/fingerprint/qdbl.py out/fingerprint/private/qdbl-2105.bin
```

The capture contains these record types/counts:
`4:8`, `5:43`, `6:21`, `7:8`, `10:1`, `11:8`, `13:5`.
All eight type-7 records: opaque metrics `b3=8,b4=1,b5=255`.
All eight type-11 outcomes: `matched_flag=0, metric_cb=2, aed=0`.
**Those metric meanings are not yet decoded. Do not call metric_cb=2 a known
failure reason.** Next trace the writers of those metrics and the actual matcher
inputs before asking for further presses or assuming HAT/SID is the cause.

Useful TA functions (Ghidra image base +0x100000):

- `0x1537ec`: QFP_VerifyInject; `0x1210f0`, case6: matching state machine.
- `0x15d124`: QDBL headers/reset; `0x15e9f0`: record sizes/export.
- `0x15d644`: metric setter; `0x15e944`: metric getter.
- `0x1253c0`: append record to export ring.
- `0x15341c`: DatabaseInject (join outlined branches before trusting decompile).
- `0x11f750`: decode_each_templ_ver3 (also uses outlined branches).
- `0x117c98`: secure KDF wrapper; `0x1179b8`: Gatekeeper auth-key service.
- Client `0x151a7c`: identify; `0x14abbc`: common_prepare;
  `0x106038`: BAuth_Control_OP; `0x106780`: genuine HAT command13 packing.

Ghidra projects: `/Users/jesvi/ios-port-research/tools/re/proj/{finge,bauth,tzc}`.
Existing/new decompiles: `tools/re/out/`, `tools/re/audit-out/` at workspace root.
Use `tools/fingerprint/reverse/DecompileOutlined.java` with `-readOnly` for the
split functions; it changes the analysis model temporarily, not signed firmware.

## Do not repeat the earlier assumptions

- Finger ID1 is not the secure user ID. HAT/SID metadata is still unimplemented;
  its absence has not been proven to cause no-match. An arbitrary unsigned HAT
  is not a fix. Genuine HATs require the phone's Gatekeeper; never export its key
  or overwrite its state as an experiment.
- Client CONTROL0x16 is intercepted locally. A TA-side handler exists too, but
  that does not make direct transmission part of stock's flow. The host builder
  rejects client-local operations (including0x31).
- The tested first-call PREPARE returned zero encoded metadata bytes. Do not
  invent or silently initialize metadata to conceal an integrity failure.
- A `SENSOR MATCH candidate` is not verified native authentication. No HAT MAC,
  request challenge, freshness, keybag binding or iOS unlock provider exists yet.
- Old matching parsed the wrong wire field; raw r[6] is not this wrapper's match
  index, and op0x4d can occur on a no-match path. See the protocol audit.
- Capture timing remains400ms and the original calibrated sensor window remains
  x27500..38000/y46500..55500. No timing change was made during this handoff.

Once comparison works, the remaining work is legitimate auth/session binding,
verified on-device passcode fallback, native iOS integration and automatic
calibration/template restoration. Do not advertise fingerprint unlock before
real same-finger success and different-finger rejection are verified.

## Validation and ownership

Kernel cross-build passes. Host tests: eight structured-exchange tests, three
view/cursor tests, two QDBL tests, sanitized C request/reply tests, existing policy
tests and passcode build integration pass. On the phone, empty PREPARE transport
passed; bad file modes, hardlinked input and symlink input were rejected. Kernel
concurrent-command protection is implemented; host cross-checkout locking is
tested. FIFO rejection with nonblocking opens is a staged source change, not a
claimed live test.

The updated view uses a private multiplexed SSH connection, not rwlogd polling or
daemon restarts. It ignores old log history and labels registration vs matching.
It was used for fresh enrollment and matching; no other agent's processes were
stopped. Phone/source lock release is recorded in the shared team log; confirm
current ownership before the next live action.
