# RGB sources: green passthrough and capture (issue #1)

Bench session 2026-11-10 on an HD60 S (`0fd9:0074`) with a PS5 on HDMI in and a
Toshiba TV on HDMI out. The card was on a USB 2.0 link through a dock, so
control transfers worked but isochronous capture did not. Everything below was
checked on the TV picture, not in OBS.

## Summary

- An RGB source goes green on the passthrough TV and in OBS because the cold
  init leaves the video front end's colour matrix at identity. The HDMI
  transmitter and the capture path both expect YCbCr from the front end.
- Writing `p0 0x92 = 0x40` plus the vendor's BT.709 RGB to YCbCr matrix into
  `p0 0x93..0xAA` turned a green TV picture normal. Seen once, from a green
  start.
- `./start colour` now does this automatically from the source's AVI InfoFrame,
  and `hd60s-obs --init` runs it after the replay.
- **Open:** after that one good result, every following run gave a black
  picture with the signal still present (not "No signal"), whatever the colour
  settings. Not explained yet. See the last section.

## The I2C proxy is not just the MCU

Target `0x5066` is the MCU's I2C master proxy, and the first payload byte is the
I2C address:

| first byte | device |
|---|---|
| `9a REG VAL` | write, IT66121 HDMI transmitter (I2C 0x9A) |
| `9b 01 REG` then IN | read, IT66121 |
| `9d 01 REG` then IN | read, video front end (I2C 0x9C) |

So `mcuRead`/`mcuWrite` in `start.cpp` are really IT66121 accesses. The chip ID
at TX regs `0x00..0x03` reads `54 49 12 16` (ITE, 0x1612). The new read-only
`./start txdump` prints TX bank 0.

## What the cold init does to the transmitter

From `data/cold-init-replay.txt`, op numbers counting every OUT/IN line:

| op | TX write | meaning |
|---|---|---|
| 2345 | `04 = 1d` | reset |
| 2351 | `70 = 40` | input from the front end is YCbCr 4:2:2 |
| 2379..2383 | `73..88`, `72 = 03` | CSC on, YCbCr to RGB |
| 2390..2436 | bank 1 `58..65`, `cd = 03` | AVI InfoFrame to the TV: RGB, BT.709, 16:9, VIC 16 |
| 2347 / 2504 | `c1 = 01` | AVMute on |
| 2789 | `c1 = 00` | AVMute off, last write to `c1` |

The transmitter always assumes YCbCr from the front end, whatever the source sends.

## Source colour space: AVI InfoFrame on front end page 2

Page 2 holds received InfoFrames. With the PS5:

- `0x5a..` SPD: "SCEI", then "PS5" at `0x62`
- `0x48..0x4b` AVI PB1..PB4: `80 28 04 10`
  - `0x48` bit 7 is set whenever a frame was received (`80`, `82`, `d2` seen,
    `00` with no signal). Bits 6:5 are the colour space: 0 RGB, 1 YCbCr 4:2:2,
    2 YCbCr 4:4:4.
  - `0x49 = 28`: no colorimetry, 16:9
  - `0x4a = 04`: limited range
  - `0x4b = 10`: VIC 16, 1080p60

The vendor capture references show `0x48 = 82` (RGB) and `0x48 = d2`
(YCbCr 4:4:4), so the vendor sessions included both kinds of source.

## Front end colour matrix

`p0 0x93..0xAA` are twelve 16-bit values, high byte first, 1.0 = 0x1000,
negatives in two's complement over 15 bits. They are laid out by input R, G, B,
each giving Cr, Y, Cb, then three offsets.

Cold init leaves (identity with range scaling, correct for YCbCr in):

    0dc0 0000 0000 | 0000 0df6 0000 | 0000 0000 0dc0 | 2000 0000 2000
    p0 0x92 = 0x62

The vendor's RGB values (`kTiming` in `start.cpp`) decode as BT.709 full to
limited:

    R: Cr  0.439  Y 0.185  Cb -0.100
    G: Cr -0.398  Y 0.624  Cb -0.338
    B: Cr -0.040  Y 0.063  Cb  0.439
    offsets 2000 0000 2000
    p0 0x92 = 0x40

Writing the matrix alone gave **dark green**: chroma was at zero instead of 128.
Adding `0x92 = 0x40` gave a **normal** picture. `0x92` differs from the cold
init in bits 0x20 and 0x02, and at least one of them is the YCbCr input /
chroma offset mode. Which bit does what has not been isolated.

The PS5 sends limited RGB and the matrix is the full-range one, so blacks may be
slightly lifted and whites slightly dim. That was not visible on the TV and has
not been measured.

## Bench log

| # | state | action | TV |
|---|---|---|---|
| 1 | dormant card | none | PS5 normal (hardware bypass) |
| 2 | PS5 asleep | full replay | black |
| 3 | PS5 awake, HDCP setting changed | full replay | normal (one-off, cause unclear) |
| 4 | after power cycle | replay 978 ops | No signal |
| 5 | after power cycle | replay 1457 ops | No signal |
| 6 | HDCP off, power cycle | full replay | **green** (issue reproduced) |
| 7 | after 6 | matrix only | dark green |
| 8 | after 7 | `0x92 = 40` | **normal** |
| 9 | warm card | full replay + `colour` | black |
| 10 | power cycle | full replay + `colour` | black |
| 11 | after 10 | `colour ycbcr` | black |

Between 9 and 11 the front end matrix and `0x92` read back as intended, the PS5
stayed locked at 1080p (H total 2200, V total 1125), the InfoFrame read RGB, and
the TX looked healthy (`c1 = 80`, so AVMute off; `61 = 00`; `04 = 00`;
`70 = 40`; `72 = 03`).

## Open: black picture after the cold init

The black has a signal present and doesn't depend on the matrix settings. It
was seen with HDCP in an unknown state (2) and, after the fix, with the HDCP
setting believed off (9 to 11). Candidates, untested:

1. The PS5 is back to HDCP, or only drops it for a while after the setting is
   toggled. HDCP content through the front end to TX path would give black,
   while the dormant hardware bypass passes it fine. Check the PS5 setting,
   then repeat run 6 (power cycle, replay only). Green means HDCP; black means
   timing.
2. Timing. In run 8 the matrix went in a minute or so after the replay, with
   the TV already showing green. In runs 9 and 10 `colour` ran straight after.
   Try a delay, or wait until the front end reports a stable lock first.
3. A cold init on a card that is already initialised (run 9, and the double
   replay earlier) always gave black. Separate from the colour fix, but it
   matters for OBS restarting the source without a replug.

Front end registers that changed between an earlier session and run 11
(`p1 0x08..0x11`, `p1 0x34` 90 to 40, `p2 0x0d`, `p2 0x0e`, `p2 0x40`) may be
link or HDCP status and are worth correlating with good and bad runs.

## Not verified

- OBS capture. The card was on USB 2.0, so no stream. The capture path should
  get correct YCbCr now, but that is a reasonable inference, not a result.
- The plugin's "Input colour: RGB" option from PR #3 is a software correction for
  the old green. With the hardware fix in place it would correct twice. Anyone
  who set RGB should go back to YCbCr. The option and the README text still need
  revisiting once capture is checked.
- A Switch. ley10n's source. Expected to behave like the PS5 (RGB, no HDCP on
  the home screen).
