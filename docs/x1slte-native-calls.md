# S20 native calls (VoLTE on iOS's own Phone)

Status 2026-11-15 (A, from B's `feat/native-calls`, merged into `x1slte` `ad93a151`): **native only**, on at boot.
Incoming, outgoing, hold, swap (two calls), DTMF, speaker, mute verified on Jio (S20 +91 70104 93945 <-> +91 90801 57822).

## How it fits together

| Piece | Where | What it does |
| --- | --- | --- |
| `redwood-cellulard` | boot image (`com.redwood.cellular`) | modem, LTE data, IMS PDN; starts `/var/root/redwood-ims` and starts it again when it leaves (at most every 30 s) |
| `redwood-ims` | `/var/root/redwood-ims` (`tools/cellular/redwood-ims.c` + `ims-call.h`, `ims-media.h`) | SIP registration (ISIM AKA, IPsec), SMS over IMS, **the call engine** (up to 4 slots, 2 live), RTP/AMR voice |
| `redwood-telephonyd` | `/var/jb/redwood/bin` (`tools/cellular/telephonyd`), job in the boot image | the **telephony CallKit provider under CommCenter's identity**: iOS lists it as `com.apple.coretelephony telephony=1`, so calls are carrier calls in Phone, Recents, the lock screen and the island |
| `libredwood-calls` | `/var/jb` (`userland/build-calls.sh install`) | InCallService hook: Speaker button |
| `callservicesd` | `/var/jb/redwood/callservicesd`, job in the boot image | Apple's CallKit daemon (the system image leaves it out; its job points there) |

The engine and the provider talk through files in `/var/tmp/redwood-cell/call-ui/`:

- `calls` (engine, rewritten on every change and at least every 2 s): `ims PID TIME registered 0|1`, then one
  `call ID in|out STATE HOLD NUMBER TAG REASON` per call (ended calls stay 40 s).
- `cmd.SEQ` (provider): `dial TAG NUMBER`, `answer ID`, `end ID`, `hold ID 0|1`, `dtmf ID DIGITS`, `mute 0|1`,
  `route speaker|earpiece`, `cancel TAG`. Taken in SEQ order; a `dial` older than 15 s is dropped.
- `alive` (provider, touched every 4 s): without it the engine answers incoming calls `486 Busy`.

Logs: `/var/tmp/redwood-ims.log` (numbers not logged), `/var/mobile/Library/Logs/redwood-telephonyd.log`,
`/var/tmp/redwood-cellular.log` (health line every minute). Last incoming INVITE: `/var/root/ims-invite-last.txt` (root only).

## Flags and files

| What | Values |
| --- | --- |
| Call stack | native only (the user, 2026-11-15). RedwoodPhone (the old third-party-style provider) is no longer in the boot image; its source stays in `tools/cellular/RedwoodPhone`. The old one-call engine is kept on the phone as `/var/root/redwood-ims.legacy` (not used). |
| `/var/root/cell-noimsclient` | exists: cellulard never starts redwood-ims |
| Settings > Cellular | `RedwoodCellular.dylib` (`userland/src/cellular_settings.m`, `tools/cellular/build-ui.sh`, `install-ui.py install --session NAME`). VoLTE / Calls / Text Messages come from the engine's live `calls` header (fresh within 15 s) and the provider's `alive`. |

## Deploy without a reboot

`tools/cellular/telephonyd/deploy-native-calls.sh` (refuses during a call): new redwood-ims, libredwood-calls,
telephonyd. `bash tools/cellular/build-ims.sh` builds the engine (opencore-amr static; plain Xcode clang, **not** the
xnu-deps wrapper). Never restart redwood-ims or cellulard while a call is up: check that `call-ui/calls` has no line
in ringing/dialing/alerting/connecting/active (an `ended` line is not a live call).

## Engine behaviour worth knowing

- Outgoing: `Supported: 100rel,replaces,timer`; reliable 18x are PRACKed. The 32 s give-up applies only when the
  network sends nothing at all; after `100 Trying` an outgoing call waits up to 90 s like a ringing one.
- Session timers both ways (we refresh with UPDATE when the other side allows it, else re-INVITE). Glare: 491 + retry.
- Incoming AMR payload type is whatever the offer says (Jio uses 100 or 112; a fixed 100 made PT 112 calls silent).
- Messages are built with `MSG_CAT` (never past the buffer) and not sent when they would not fit.

## Open problems

1. **Outgoing calls: ringing/answer sometimes arrive late, together** (6-20 s after `100 Trying`, then 183+180+200
   within 200 ms). Not the phone: cellulard had nothing pending during the gap. Clean call 2026-11-15 07:52
   (timestamped cellulard): INVITE +0, 100 +0.6 s, 183 +1.9 s, **voice bearer UP 33 ms after the 183**, 180 +2.1 s,
   200 when answered, ACK at once. The network builds the voice bearer in answer to the 183, so we never wait on
   it; the slow calls are the 183 itself coming late from the far side. If it comes back: compare
   `grep "voice bearer" /var/tmp/redwood-cellular.log` with the 183 time in redwood-ims.log; only if the bearer
   comes up well before a late 183 is precondition support (or the IPC 02/06 call list) worth trying.
2. Missed-call notifications: callservicesd posts them for its own telephony provider; check now that telephonyd
   is that provider (see `proposals/notifications/README.md` for the rest of the notification analysis).
3. Personal Hotspot: needs CommCenter (only the parked stand-in `tools/cellular/redwood-commcenter.m` exists; its
   kill switch `/var/root/cell-nocommcenter` is set because zero answers blanked the status bar), a BCM4375 AP mode
   and NAT between Wi-Fi and the cellular utun. Not started.
4. No conference (merge), emergency registration, USSD, video, Wi-Fi calling.
