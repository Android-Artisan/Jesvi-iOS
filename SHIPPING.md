# x1slte (iOS 16.5 on the Galaxy S20, SM-G980F) — shipping log

Local branch `x1slte` only (not pushed). One entry per **verified** change: what it does, the commit(s), the boot
args that control it (default in **bold**), and how it was tested on the phone. Newest first. When cutting a release,
this file is the changelog; "Open" lists what a release must mention as known issues.

## Open (known issues)
- **Restarts at the lock screen after a flash / cold start** (2-3 times, then stable). Not the WDT_CLUSTER0 watchdog
  (off: `wdt.look` WTCON 0x18000). Being captured with `tools/serial-watch.sh` (USB serial log across restarts).
- Mic: the kernel's diagnostic peak counter reads full scale on every capture although recorded files do not clip (only
  with `snd_debug=1` now).

## 2026-11-07

### Audio: debug off by default, self-checks, quieter speaker and vibration — `26dfb5b`
- `snd_debug=` **0**: routine log lines (amps on/off, 60 s stats, mic start/stop), mic per-sample peaks and the
  adapter's per-write peak scan + report only with `snd_debug=1`. Errors are always logged.
- Self-checks: amplifier fault -> reset and set up again (3 per boot; before: off until restart); every quiet minute
  both amps must answer on I2C, else recovery (3 per boot); mic WDMA4 stuck 200 ms -> capture path restarted; playback
  DMA stalls counted. `rwsys kern.redwood.poke snd.health` reports it all (memory only, safe while playing).
- Speaker maximum 70 % of before: `snd_below=` **109** (1/8 dB under the amps' full level; was 84).
- Vibration maximum 60 % of before: `haptic_max_pct=` **34** (was 56).
- Debug command table 32 -> 48 entries (it was full).
- Tested: tone, 4 s record with vibration, `snd.health` clean, log shows `109/8 dB` and `at 34 %`.

### Microphone: faster start, no idle CPU; amp I2C fix — `ac6c313`
- Mic worker sleeps while nothing records (was waking every 10 ms), polls every 4 ms while recording, 20 ms settle:
  capture starts in ~40 ms (was 100-120 ms).
- GPIO: mic power / haptic reset pins share a bank with the speaker amps' bit-banged I2C; a neighbour's
  read-modify-write could set the I2C lines' output latch high. Lines are now driven low explicitly every time.
- Tested: record, record + vibration, record + system sounds (6 s, 0 dropped frames), both amps answer after.

### Microphone works in iOS — `2dc3b49` (and `d206416`..`976b8d7`)
- `/dev/redwood-mic` (48 kHz, 16-bit mono) fed from the three DMIC converters (VTS S_LIF -> UAIF6 -> NSRC4 -> WDMA4);
  iOS sees "iPhone Microphone"; Voice Memos records. `mic_enable` (default on with `snd=1`).
- Watchdog WDT_CLUSTER0 switched off at boot (`wdt_off=` **1**): it restarted iOS at the lock screen (`36fc39a`).

### Speaker sound works — `150a7cd`, `4ffbc65`, `4ed2fa0`, `976b8d7`
- ABOX RDMA3 -> ASRC -> UAIF2 -> two CS35L41 amps, Samsung's register set (`snd=1 snd_samsung=1`). Amp start
  retried at boot and recovered on the playback worker.

## Earlier (2026-11-06, see git log)
Charging (watchdog kick, PD/AFC 9 V, limits), live battery %, USB serial + USB networking + SSH, flashlight, Wi-Fi
signal and power save, Samsung DVFS tables, iOS-only storage (system on `super`, 108 GB `/var`), UFS TurboWrite
(`ufs_tw=1`, `6dbc8c5`), rotation (`shub=6`), vibration (`haptic=5`), Location Services, Bluetooth audio fixes.
