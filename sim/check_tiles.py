#!/usr/bin/env python3
"""Render the raw RGB565 tiles DWT_SIM_PREVIEW_SELFTEST dumps.

The selftest exercises thr_preview end to end — card mask -> resample ->
composite — at both sizes the UI asks for, including the 300 px path that is
otherwise only reachable by tapping a card. This turns its dumps into a PNG
so the result can actually be looked at.

    DWT_SIM_PREVIEW_SELFTEST=ibex.thr ./sim/build/dwt_sim
    python sim/check_tiles.py out.png

Requires: Pillow, numpy.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load_rgb565(path: Path) -> Image.Image:
    raw = np.fromfile(path, dtype="<u2")
    n = int(round(len(raw) ** 0.5))
    if n * n != len(raw):
        sys.exit(f"{path}: {len(raw)} pixels is not square")
    a = raw.reshape(n, n)
    r = ((a >> 11) & 0x1F) << 3
    g = ((a >> 5) & 0x3F) << 2
    b = (a & 0x1F) << 3
    return Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="tiles.png")
    ap.add_argument("--dir", default="sim")
    args = ap.parse_args()

    dumps = sorted(Path(args.dir).glob("selftest_*.raw"))
    if not dumps:
        sys.exit(f"no selftest_*.raw in {args.dir} — run the sim with "
                 f"DWT_SIM_PREVIEW_SELFTEST=<pattern.thr> first")

    tiles = []
    for p in dumps:
        img = load_rgb565(p)
        print(f"{p.name}: {img.width}x{img.height}")
        tiles.append(img)

    h = max(t.height for t in tiles)
    strip = Image.new("RGB", (sum(t.width for t in tiles) + 12 * (len(tiles) - 1), h),
                      (28, 28, 28))
    x = 0
    for t in tiles:
        strip.paste(t, (x, (h - t.height) // 2))
        x += t.width + 12
    strip.save(args.out)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
