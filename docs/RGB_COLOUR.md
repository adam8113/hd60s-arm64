# RGB sources: green passthrough and capture (issue #1)

Bench on an HD60 S (`0fd9:0074`) with a PS5 on HDMI in (HDCP off). Early runs
were on USB 2.0 so only the TV picture was checked; later runs on SuperSpeed
also checked capture.

## Summary

- An RGB source goes green on the passthrough TV and in OBS because the cold
  init leaves the video front end's colour matrix at identity. The HDMI
  transmitter and the capture path both expect YCbCr from the front end.
- Writing `p0 0x92 = 0x40` plus the vendor's BT.709 RGB to YCbCr matrix into
  `p0 0x93..0xAA` turns a green TV picture normal.
- `./start colour` does this from the source's AVI InfoFrame (waits for a
  stable InfoFrame in auto mode). `./start up` is wake + colour. `./start
  restart` is USB re-enumerate + up. `hd60s-obs --init` runs cold-init replay
  then colour (capture needs the replay; wake+colour alone streams 0 bytes).
- Leave OBS **Input colour** on **YCbCr**. The RGB option is legacy software
  undo for inits that skipped hardware CSC; using both corrects twice.
- Capture needs SuperSpeed. About 258 MB/s at 1080p60 after replay + colour.
  USB 2.0 is fine for control / passthrough work only.
- Full cold-init replay used to leave the TV black with a lock. That was TX
  `0x20 = 0x01`; `./start replay` clears it. See below.

## Soft bring-up

**Picture only (no capture):**

    ./start up         # wake (if needed) + colour from AVI InfoFrame
    ./start restart    # USB re-enumerate + wake + bringup, when wedged

**Capture / OBS:**

    ./start bringup    # replay + colour (same as hd60s-obs --init)

Op 35 of the cold init writes IT66121 `0x04 = 0x3d` (TX reset). The TV goes
"No signal" mid-replay; finishing the replay + colour brings capture up, and
the `0x20` clear stops the TV staying black afterwards.

Re-enumerate without wake restores hardware bypass so you can check menus
without unplugging HDMI. `./start reset` re-enumerates then wakes. If `0xC2`
times out, power-cycle the USB cable once.

## The I2C proxy is not just the MCU

Target `0x5066` is the MCU's I2C master proxy, and the first payload byte is the
I2C address:

| first byte | device |
|---|---|
| `9a REG VAL` | write, IT66121 HDMI transmitter (I2C 0x9A) |
| `9b 01 REG` then IN | read, IT66121 |
| `9d 01 REG` then IN | read, video front end (I2C 0x9C) |

So `mcuRead`/`mcuWrite` in `start.cpp` are really IT66121 accesses. The chip ID
at TX regs `0x00..0x03` reads `54 49 12 16` (ITE, 0x1612). The read-only
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

The vendor's RGB values (`kCscRgb` in `start.cpp`, also used by `./start format`)
decode as BT.709 full to limited:

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
slightly lifted and whites slightly dim. Not visible here and not measured.

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
`70 = 40`; `72 = 03`). That black was the `0x20` issue below, not the matrix.

## Black after cold init: TX `0x20`

Cold init writes IT66121 `0x20 = 0x01` around replay op 2667. With that set the
TX locks but the picture stays black. Clearing `0x20` to `0x00` restores it.
Capture rate is the same either way.

| stop after | notes | TV |
|---|---|---|
| 2460 | | No signal |
| 2464 (`0x61 = 00`) | AVMute still on (`c1 = 01`) | black |
| 2464 + unmute | | normal |
| 2667 (`0x20 = 01`) + unmute | | normal |
| 2668 (`0x21 = 01`) + unmute | | black |
| full replay | `0x20` left at `01` | black |
| full replay + `0x20 = 00` | what `./start replay` does now | **normal** |

`sinkedid` does not fix it.

## Not verified

- OBS plugin UI end-to-end with Input colour YCbCr (raw SuperSpeed stream after
  replay + colour is fine).
- A Switch. Same RGB path as the PS5; waiting on confirmation in #1.
