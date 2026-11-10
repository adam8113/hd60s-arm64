# Tint bisect diagnostic

Opt in only. Ordinary `replay`, `hd60s-obs --init`, and OBS capture are unchanged.

## What it is for

Physical HDMI passthrough goes green once cold init runs, even with OBS closed. This mode replays the first N ops of `data/cold-init-replay.txt`, names important ops, and logs every USB failure with IDs so you can bracket which stretch of startup introduces the tint.

It does not write guessed colour matrix values. It is not a fix.

## Safety

Partial replay leaves the card mid sequence. Between every trial:

1. Quit OBS and anything else using the card.
2. Unplug the HD60 S USB cable, wait about two seconds, plug it back in.
3. Confirm the TV baseline as normal, green, or no-signal.
4. Run one tint-bisect command.
5. Score the TV again as normal, green, or no-signal.
6. Unplug and replug before the next trial or before opening OBS.

If the run reports unexpected USB failures, treat the bracket as incomplete.

## Build (Apple Silicon Mac)

From this branch:

```bash
make start
make test-tint-bisect
```

`make test-tint-bisect` needs only Python 3 and the replay file. It does not touch USB.

## Plan without USB

```bash
./start tint-bisect --list-stops
./start tint-bisect --stop-after wake --plan-only
python3 tools/tint_bisect_map.py --stop-after before_matrix
```

## Live run

Example coarse ladder (replug between each):

```bash
./start tint-bisect --stop-after host_setup --i-will-replug
./start tint-bisect --stop-after wake --i-will-replug
./start tint-bisect --stop-after before_hdmi_tx --i-will-replug
./start tint-bisect --stop-after hdmi_tx1 --i-will-replug
./start tint-bisect --stop-after before_matrix --i-will-replug
./start tint-bisect --stop-after dense_matrix --i-will-replug
./start tint-bisect --stop-after cold_matrix --i-will-replug
./start tint-bisect --stop-after full --i-will-replug
```

You can stop early once you have a normal then green pair. Numeric `--stop-after N` is allowed for a tighter bracket.

## What to keep

For each trial: stop name or N, TV score (normal / green / no-signal), the summary block, and any `FAIL` / `UNEXPECTED` lines. No need for a full verbose dump.
