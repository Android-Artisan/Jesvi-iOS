# S20 GPS implementation status

2026-11-16, feat/s20-gps, based on accepted master735b41e0 (camera completion).
**No satellite fix yet. No GPS service or Core Location provider is shipped.**
The existing camera, authentication, root image and kernel are unchanged.
The previous feat/s20-fingerprint branch is preserved separately.

## Current phone state / immediate next action

Phone lock: codex-gps. Running kernel initially verified as2026-11-16 07:42:36 IST,
matching the accepted permanent GW2 camera deployment.

A16-byte stock ROM memory RPC worked. A larger768-byte request triggered
`host_com_rx.cpp` firmware exceptions and stopped sensor sample counters. iOS and
SSH remained running. Subsequent requests did not acknowledge; all device probing
was stopped. **Sensor-hub recovery is pending a reboot of the existing installed
image, explicitly requested from the user. No reboot approval has arrived yet.**
Do not assume a read-only firmware request is harmless at arbitrary sizes.
Do not issue repeated requests into this failed state or reset the hub separately:
the normal firmware/SSP startup belongs to the existing kernel thread.

The host-side reader now refuses requests above the only proven16-byte size,
stops on exception RPCs, and preserves partial private output. That fixed binary
was cross-built but has NOT been deployed or qualified on the phone.

## Confirmed boundaries

Stock S20 vendor binaries `lhd` and `gpsd` have been partially decompiled; detailed
local research and private Ghidra artifacts are in ignored
`proposals/gps-research-20261116/`. Do not add firmware/decompiled binaries to git.

- Hub scan returned000577771f07f6bf; firmware version23030400.
- Advertised scan bit29 (GPS) is absent. Firmware contains a descriptor29 GPS
  initializer at0x82d9ae, start/stop callback table0x82dba0, location callback
  0x82d694 and shared buffer0x90f7a8, but registration reachability is unproved.
  No literal pointer or direct BL/B.W call to that initializer was found.
  Treat it as potentially dormant code, not an available sensor API.
- Stock lhd encodes RPC1C as navigation-channel Start(bool), RPC1E as host-to-hub
  bytes and RPC1F as hub-to-host bytes.1E/1F bodies contain a LE16 byte count.
  A transport acknowledgement is not evidence of acquisition or a fix.
- RPC7 MemoryRead body is bool,u32 address,u16 length; RPC8 response is
  u32 address,u16 length,data. The small successful result contained a ROM vector
  table starting with SP0x00913a30. Large-read compatibility is explicitly failed.
- Inner MEIF startsA5 93, endsA5 68, doubles literalA5, and uses an additive LE16
  checksum. This framing is distinct from outer BBD packets/CRC and from the
  gpsd/lhd daemon connection. Stock message0x8183 configures MicroNav, but its
  flag meanings and complete required startup sequence remain unresolved.
- Actual iOS16.5 locationd includes an accessory/NMEA provider. Its private ABI
  and genuine fix delivery have not been implemented or verified. Compass fusion
  support does not establish GPS support.

## Source/tools added

`tools/gps/gnss_protocol.h` implements bounded pure MEIF decoding/encoding and the
RPC1E body boundary. It performs no device I/O or positioning. Its tests use the
literal passthrough-control packet embedded in stock gpsd as an independent input.

`tools/gps/gnss-read.c` captures fixed firmware windows using the existing
`/dev/redwood-gnss` SPI-owner interface. It never writes firmware memory or resets
hardware. A reply may precede the kernel's acknowledgement record, so it waits
for both. It refuses oversize reads before I/O and aborts on a firmware exception.
Output creation is exclusive, no-follow and0600. Firmware files remain private.
The program is an experimental bring-up tool, not a boot service.

Host checks:

```sh
clang -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/gps/test_gnss_read.c -o out/gps/test-gnss-read
out/gps/test-gnss-read
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  tools/gps/test_protocol.c -o out/gps/test-protocol
out/gps/test-protocol
```

Both passed; protocol cases cover the stock frame, split escaping, bad checksum,
size/overflow recovery and nested navigation length. Reader checks cover pre-I/O
size refusal and the actual captured reply fields. The reader cross-compiles for
arm64 iOS16.5. Existing passcode build-integration test also passed; no build/profile
changes were required.

## Next implementation steps

1. With user approval, reboot the unchanged installed image; verify increasing
   motion sample counters before further experiments. Do not flash a new image.
2. Recover the actual B1 ROM codec/controller state and the allowed response
   size using small, bounded requests. Large bulk-read attempts are disallowed.
3. Resolve stock GNSS-channel/pass-through initialization and RF/clock config.
   A native session must stop/clean up through the same SPI-owner thread and
   abort on exceptions, dropped records or protocol-state mismatch.
4. Prove real outdoor acquisition, fresh timestamps, validity and accuracy.
5. Add a demand-driven native service and real Core Location provider path;
   retain authorization, stale-fix rejection, fallback and paired start/stop.
6. Validate cold/warm starts, repeat sessions, screen-off/wake, airplane mode,
   power consumption, camera/motion regressions and passcode repeat locking.

Do not replace steps4–6 with simulation or fabricated coordinates. Ubuntu Touch/
Halium references keep Android vendor interfaces; they do not supply a demonstrated
native XNU implementation for this chip. The cached stock vendor image is593920
bytes short of its declared ext4 size and must be re-extracted from an intact
archive before being used for an Android runtime lab.
