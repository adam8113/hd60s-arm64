# Bring-up improvements — live progress

Working log for making HD60 S bring-up faster and more reliable. Plain
goals and bench results only.

## Goals

1. **Shorter cold-init for capture** — today’s `data/cold-init-replay.txt` is
   ~10k USB ops (~6–7 s). Find a smaller sequence that still yields
   passthrough + ~258 MB/s isoc capture.
2. **Softer recovery when the source is off** — `./start restart` /
   `bringup` should leave the card ready and apply colour automatically
   once HDMI returns, instead of requiring a manual `./start colour`.

Shipped already (baseline): hardware RGB→YCbCr CSC, TX normalize after
replay/`up`/`bringup` (clear AVMute, clear sticky `0x20`, unstick `0x04`),
`up` / `restart` / `bringup` commands.

## Status

| Item | State |
|---|---|
| Repo hygiene (gitignore dumps, drop stale branch) | done 2026-11-18 |
| Progress doc | this file |
| Auto-colour when source appears after bring-up | coded; needs bench |
| Cold-init shrink (inventory) | started |
| Cold-init shrink (bench bisect) | not started |

## Session log

### 2026-11-18

- Added `*.yuy2` / `*.yuv` to `.gitignore`; removed local `stream.yuy2`.
- Pruned stale `origin/fix/rgb-hardware-csc` (already deleted on GitHub).
- Cold-init inventory (line counts, not a proposed cut yet):
  - ~20k log lines → ~9957 replayed ops (IN+OUT; RET is status).
  - Most OUT traffic is MCU proxy `0x5066` and FE `0x509c`.
  - On `0x5066`, first payload bytes: `0x9d` (FE read), `0x9b` (TX read),
    `0x9a` (TX write), plus bulk `0xd4`/`0xd5`.
  - ~335 TX (`0x9a`) writes. Notable first hits: `0x04=0x3d` early;
    AVMute `0xc1` mid; CSC `0x72`/`0x76` later; sticky `0x20=0x01` near
    the end (cleared by TX normalize).
- Added `phaseColourWhenSource` (60s poll) on `up` / `bringup` after the
  short colour retries fail — applies CSC when HDMI returns.
- Next bench: `./start restart` with console asleep, then wake a game within
  60s and confirm colour applies without a manual `./start colour`. Then
  start cold-init bisect with `./start replay FILE N` on SuperSpeed.

## Bench checklist (when the card is plugged in)

```bash
./start bringup          # expect TX normalize + colour if source is live
./start stream 3         # ~258 MB/s at 1080p60
./start up               # picture-only path
./start restart          # with source off, then turn source on — colour should follow
```

## Ideas (not committed)

- After deferred colour, poll AVI InfoFrame for up to N seconds (e.g. 60s)
  and apply CSC when it appears; print a clear “waiting for HDMI…” line.
- For shrink: keep wake + TX reset/unmute/CSC + FE arm + whatever the
  isoc path needs; drop redundant EDID/DDC churn if capture still works.
- Never ship a cut that blanks the TV or empties the isoc pipe.
