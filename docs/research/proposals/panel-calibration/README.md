# Hubble1 factory brightness data

The SM-G980F panel flash identifies as Macronix `c2 28 14`. Samsung's
`s6e3hab_hubble_panel_poc.h` locates normal/120 HS dimming at `0xf0000` and
`0xf3000`. Each partition has 7392 bytes of calibration data, a big-endian
16-bit byte sum at offset `0x26b4`, and marker 1 at `0x26b6`.

Verified on this phone: normal `411d`, 120 HS `d472`. Full extraction SHA-256:

* Normal: `c306c71684a6bc2776cee965cf3a9a80d87944b4debea596271062695ffb10bb`
* 120 HS: `a376fb6df6031503ea29a6c83a19f56642d488885cbd1e47663699df3314d4de`

`read.py --output-dir /tmp/panel-calibration` extracts and validates both
partitions using the SSH alias `s20`. It refuses to overwrite existing dumps.
The firmware reader only implements JEDEC ID and flash READ, restores CMGP/USI
clock and pin state, and serializes its work with display commands. It does not
implement flash erase/program/write-enable commands. An initial GPIO SPI
prototype returned all zeros and was superseded by the hardware SPI controller.

Data offsets used by the factory gamma/AOR path:

| Offset | Shape | Meaning |
| --- | --- | --- |
| 0 | 34 bytes | HBM gamma (unused by this path) |
| 34 | 74 × 34 | Normal-range gamma, 2..420 nominal nits |
| 2550 | 90 × 2 | Big-endian AOR (first 74 rows used) |

Gamma packing and luminance points come from Samsung's `s6e3hab.c` and
`s6e3hab_hubble1_a3_s0_panel_dimming.h`. Interpolation handles the three 10-bit
channels and six packed nibbles independently, rather than interpolating packed
bytes. Factory data is read per phone at runtime, not embedded in source.

Host regression test:

```sh
clang -Wall -Wextra -Werror -std=c99 proposals/panel-calibration/test_gamma.c -o /tmp/test-gamma
/tmp/test-gamma
```

`fb.factory` reports state, `fb.factory 1` loads/verifies/enables factory gamma
and AOR, and `fb.factory 0` schedules restoration of the original gamma and
approximate AOR curve. Boot argument `fb_cal=1` opts into loading after the
initial panel report. Device visual validation is pending; this is not a claim
of a measured photometric calibration. VINT/ELVSS/IRC compensation and HBM above
420 nits are outside the initial gamma/AOR path.
