#!/usr/bin/env python3
"""Grab a screenshot from the running UI sim.

Requests a self-snapshot (sim/shot.req), waits for shot.done, composites the
three ARGB8888 layer dumps (screen, layer_top, layer_sys) and writes a PNG.

    python sim/shot.py out.png [--dir sim] [--timeout 5]
"""

import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image


def load_layer(path: Path):
    raw = path.read_bytes()
    w, h, stride = struct.unpack("<III", raw[:12])
    px = np.frombuffer(raw[12:], dtype=np.uint8)
    px = px[: stride * h].reshape(h, stride)[:, : w * 4].reshape(h, w, 4)
    # LVGL ARGB8888 memory order is B,G,R,A.
    return Image.fromarray(px[:, :, [2, 1, 0, 3]], "RGBA")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--dir", default="sim")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    d = Path(args.dir)
    done = d / "shot.done"
    done.unlink(missing_ok=True)
    (d / "shot.req").touch()

    deadline = time.time() + args.timeout
    while not done.exists():
        if time.time() > deadline:
            sys.exit("sim did not answer - is it running?")
        time.sleep(0.05)
    done.unlink()

    out = None
    for i in range(3):
        p = d / f"shot_layer{i}.raw"
        if not p.exists():
            continue
        layer = load_layer(p)
        out = layer if out is None else Image.alpha_composite(out, layer)
    if out is None:
        sys.exit("no layer dumps found")
    out.convert("RGB").save(args.out)
    print(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
