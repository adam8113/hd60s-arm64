#!/usr/bin/env python3
"""Index cold-init-replay.txt for tint-bisect stop points.

Matches start's phaseReplay counting: RET lines ignored, standard bReq
0x06/0x00/0x0b/0x31 skipped and not counted, everything else is one attempted op.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

SKIP_BREQ = {0x06, 0x00, 0x0B, 0x31}

# Named stops: stop_after is inclusive attempted-op index in the current replay file.
# Verified by tools/test_tint_bisect_map.py against data/cold-init-replay.txt.
RECOMMENDED_STOPS = (
    ("host_setup", 9, "Opening host-setup ops that stall as vendor requests."),
    ("wake", 15, "Through 0xC2 wake and gpio read back."),
    ("before_hdmi_tx", 704, "Just before the first IT66121 (0x5098) writes."),
    ("hdmi_tx1", 707, "After the first three HDMI transmitter writes."),
    ("before_matrix", 978, "Just before the first p0 0x93..0xAA colour-matrix writes."),
    ("dense_matrix", 1025, "After the first dense YCbCr-style matrix block."),
    ("cold_matrix", 1457, "After later cold-style matrix values settle."),
    ("full", 9966, "Entire cold-init replay (same as OBS --init init half)."),
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_replay_path() -> Path:
    return repo_root() / "data" / "cold-init-replay.txt"


def parse_ops(path: Path) -> list[dict]:
    ops: list[dict] = []
    text = path.read_text()
    for line_no, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("RET"):
            continue
        parts = line.split("|")
        if len(parts) < 5:
            continue
        direction, breq_s, wv_s, wi_s, len_s = parts[:5]
        try:
            breq = int(breq_s, 16)
            wvalue = int(wv_s, 16)
            windex = int(wi_s, 16)
            length = int(len_s, 10)
        except ValueError:
            continue
        if breq in SKIP_BREQ:
            continue
        payload: list[int] = []
        if length > 0 and len(parts) >= 6 and parts[5].strip():
            payload = [int(x, 16) for x in parts[5].split()]
        ops.append(
            {
                "op": len(ops) + 1,
                "line": line_no,
                "dir": direction,
                "breq": breq,
                "wvalue": wvalue,
                "windex": windex,
                "length": length,
                "data": payload,
            }
        )
    return ops


def describe_op(op: dict) -> str:
    data = op["data"]
    payload = ""
    if data:
        payload = " data=" + " ".join(f"{b:02x}" for b in data[:8])
        if len(data) > 8:
            payload += "…"
    tag = ""
    if op["breq"] == 0xC2:
        tag = " [wake/reset]"
    elif op["wvalue"] == 0x5098:
        tag = " [hdmi-tx]"
    elif op["wvalue"] == 0x509C and data and 0x93 <= data[0] <= 0xAA:
        tag = " [colour-matrix]"
    elif op["wvalue"] == 0x509C and data and data[0] == 0x00:
        tag = " [video-page]"
    elif op["wvalue"] == 0x5066:
        tag = " [mcu]"
    return (
        f"op {op['op']} line {op['line']} {op['dir']} "
        f"bReq=0x{op['breq']:02x} wV=0x{op['wvalue']:04x} "
        f"wI=0x{op['windex']:04x} len={op['length']}{payload}{tag}"
    )


def stages_covering(stop_after: int) -> list[tuple[str, int, str]]:
    return [s for s in RECOMMENDED_STOPS if s[1] <= stop_after]


def resolve_stop(token: str, total: int) -> int:
    if token.isdigit():
        n = int(token)
        if n < 1:
            raise ValueError("stop-after must be >= 1")
        return min(n, total)
    for name, n, _ in RECOMMENDED_STOPS:
        if name == token:
            return min(n, total)
    known = ", ".join(s[0] for s in RECOMMENDED_STOPS)
    raise ValueError(f"unknown stop {token!r}; use a number or one of: {known}")


def print_plan(ops: list[dict], stop_after: int) -> None:
    total = len(ops)
    stop_after = min(stop_after, total)
    print(f"replay file ops: {total}")
    print(f"stop after op:   {stop_after}")
    print("recommended stops:")
    for name, n, note in RECOMMENDED_STOPS:
        mark = "<" if n == stop_after else ("*" if n <= stop_after else " ")
        print(f"  [{mark}] {name:14} {n:5d}  {note}")
    print("boundary ops:")
    for op in ops[max(0, stop_after - 3) : stop_after]:
        print("  " + describe_op(op))
    crossed = stages_covering(stop_after)
    if crossed:
        print("stages reached by this stop:")
        for name, n, note in crossed:
            print(f"  - {name} (@{n}): {note}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--file",
        type=Path,
        default=None,
        help="seq capture path (default: data/cold-init-replay.txt)",
    )
    p.add_argument(
        "--list-stops",
        action="store_true",
        help="print recommended stop names and exit",
    )
    p.add_argument(
        "--stop-after",
        default=None,
        help="number or recommended stop name; print plan for that cut",
    )
    p.add_argument(
        "--describe-op",
        type=int,
        default=None,
        help="print one attempted op by index",
    )
    args = p.parse_args(argv)
    path = args.file or default_replay_path()
    if not path.is_file():
        print(f"cannot open {path}", file=sys.stderr)
        return 2
    ops = parse_ops(path)
    if args.list_stops:
        for name, n, note in RECOMMENDED_STOPS:
            print(f"{name}\t{n}\t{note}")
        print(f"total\t{len(ops)}")
        return 0
    if args.describe_op is not None:
        if args.describe_op < 1 or args.describe_op > len(ops):
            print("op out of range", file=sys.stderr)
            return 2
        print(describe_op(ops[args.describe_op - 1]))
        return 0
    if args.stop_after is None:
        print_plan(ops, len(ops))
        return 0
    try:
        stop = resolve_stop(args.stop_after, len(ops))
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2
    print_plan(ops, stop)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
