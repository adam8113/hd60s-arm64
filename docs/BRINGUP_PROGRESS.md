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
| Cold-init shrink (inventory) | done — see below |
| Cold-init shrink (bench bisect) | next: try cutoff ~op 3000 |

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
- Auto-colour wait coded (see above). Still needs an asleep→wake bench.
- Cold-init phase map (replay op index 1..10000):
  - **1–500**: MCU bulk + empty (FX3/MCU bring-up).
  - **500–1500**: heavy FE `0x509c` (tables / EDID-ish).
  - **1500–3000**: TX read/write heavy (reset, AVMute, CSC, sticky `0x20`).
  - **3000–10000**: almost pure repeating FE read + MCU empty + `0x509c` —
    looks like an **idle poll loop captured into the log**, not unique init.
    ~70% of the replay may be droppable if capture still works after a
    cutoff near op 3000.
- All **335 TX (`0x9a`) writes** fall in replay ops **77–2996**. Ops
  3000–10000 have **zero** TX writes — FE/`0x509c` + MCU empty polling.
- Bench attempt `./start replay … 3000`: TX normalize still saw `0x20=0x01`
  and cleared it (so sticky write is inside the first 3k). No InfoFrame /
  no isoc data — but the console had no signal for a full bringup either
  (60s wait timed out). **Cutoff not proven yet; needs a live source.**
- Auto-colour wait exercised: printed 5s ticks for 60s, then deferred
  cleanly. Ready to re-test when a game is on.

## Bench checklist (when the card is plugged in)

```bash
./start bringup          # expect TX normalize + colour if source is live
./start stream 3         # ~258 MB/s at 1080p60
./start up               # picture-only path
./start restart          # with source off, then turn source on within 60s

# cold-init shrink probes (live HDMI required)
./start replay data/cold-init-replay.txt 3000
./start colour
./start stream 3
# if empty, try 4000 / 5000 / 7000 / full
```

## Ideas (not committed)

- If 3000 works with a live source: ship a trimmed replay (or default
  `bringup` cutoff) and keep the full file for recovery.
- If 3000 fails: bisect between 3000 and 10000 — something in the FE
  poll/table region is still required for isoc.
- Never ship a cut that blanks the TV or empties the isoc pipe.
