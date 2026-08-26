#!/usr/bin/env python3
"""Prepare a pattern microSD card for the dune-weaver touch panel.

Writes the layout src/board/sdcard.h documents:

    <out>/patterns.json          manifest: JSON array of paths relative to
                                 /patterns (the table's /sand_patterns format)
    <out>/previews/<key>.bin     raw RGB565 (little-endian) 300x300 tiles,
                                 key = pattern basename lowercased (".thr" kept)

Preview tiles are byte-compatible with the firmware's own render cache and
match its look (same dish geometry + colors, supersampled here for quality).

Usage:
    python tools/make_pattern_sd.py --out /Volumes/PATTERNS
    python tools/make_pattern_sd.py --out sim/simfs/sdcard          # UI sim
    python tools/make_pattern_sd.py --from-table http://192.168.68.123 \
        --out /Volumes/PATTERNS   # manifest from that table's catalog

With --from-table, the manifest is the TABLE's catalog; preview pixels still
come from the local library (matched by basename), and entries missing
locally are fetched from the table (slow: boards serve ~45 KB/s).

Requires: Pillow, numpy (both in the miniforge base env).
"""

import argparse
import json
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw
import numpy as np

DEFAULT_PATTERNS = Path("/Volumes/SSD/projects/dune-weaver-pi/patterns")

# Firmware look (src/render/thr_preview.c render_tile).
COLOR_BG = "#171310"
COLOR_DISH = "#1b1712"
COLOR_RING = "#3e362c"
COLOR_SAND = "#d8b578"
SUPERSAMPLE = 4


def preview_key(rel: str) -> str:
    """Basename, lowercased, '.thr' kept — dune-weaver-mobile semantics."""
    base = rel.rsplit("/", 1)[-1]
    return base.lower()


def parse_thr(text: str):
    """Yield (theta, rho) pairs; skip blanks/comments; clamp rho to [0,1]."""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            theta = float(parts[0])
            rho = float(parts[1])
        except ValueError:
            continue
        yield theta, min(1.0, max(0.0, rho))


def render_tile(text: str, size: int) -> bytes:
    """Render one .thr to raw little-endian RGB565, firmware geometry."""
    big = size * SUPERSAMPLE
    margin = big * 12 // 512
    cx = big / 2.0
    radius = cx - margin

    img = Image.new("RGB", (big, big), COLOR_BG)
    draw = ImageDraw.Draw(img)
    draw.ellipse([cx - radius, cx - radius, cx + radius, cx + radius], fill=COLOR_RING)
    r2 = radius - 2.0 * SUPERSAMPLE
    draw.ellipse([cx - r2, cx - r2, cx + r2, cx + r2], fill=COLOR_DISH)

    pts = [
        (cx + rho * radius * np.cos(theta), cx + rho * radius * np.sin(theta))
        for theta, rho in parse_thr(text)
    ]
    if len(pts) >= 2:
        draw.line(pts, fill=COLOR_SAND, width=2 * SUPERSAMPLE, joint="curve")
    elif len(pts) == 1:
        x, y = pts[0]
        w = SUPERSAMPLE
        draw.ellipse([x - w, y - w, x + w, y + w], fill=COLOR_SAND)

    img = img.resize((size, size), Image.LANCZOS)
    rgb = np.asarray(img, dtype=np.uint16)
    r5 = (rgb[:, :, 0] >> 3) << 11
    g6 = (rgb[:, :, 1] >> 2) << 5
    b5 = rgb[:, :, 2] >> 3
    return (r5 | g6 | b5).astype("<u2").tobytes()


def local_index(patterns_dir: Path) -> dict:
    """basename-lowercase -> local Path, for cross-folder matching."""
    index = {}
    for p in sorted(patterns_dir.rglob("*.thr")):
        index.setdefault(p.name.lower(), p)
    return index


def fetch(url: str, timeout: int = 60) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="SD card root (or staging dir)")
    ap.add_argument("--patterns", type=Path, default=DEFAULT_PATTERNS,
                    help="local .thr library for preview pixels")
    ap.add_argument("--from-table", metavar="URL",
                    help="take the manifest from this table's /sand_patterns")
    ap.add_argument("--size", type=int, default=300,
                    help="tile size in px (must match the firmware, default 300)")
    ap.add_argument("--force", action="store_true", help="re-render existing tiles")
    args = ap.parse_args()

    out = Path(args.out)
    previews = out / "previews"
    previews.mkdir(parents=True, exist_ok=True)

    if args.from_table:
        base = args.from_table.rstrip("/")
        manifest = json.loads(fetch(base + "/sand_patterns", timeout=30))
        manifest = [m.lstrip("/") for m in manifest if isinstance(m, str)]
        print(f"manifest: {len(manifest)} patterns from {base}")
    else:
        if not args.patterns.is_dir():
            sys.exit(f"patterns dir not found: {args.patterns}")
        manifest = sorted(
            str(p.relative_to(args.patterns)) for p in args.patterns.rglob("*.thr")
        )
        print(f"manifest: {len(manifest)} patterns from {args.patterns}")

    (out / "patterns.json").write_text(json.dumps(manifest, indent=0))

    index = local_index(args.patterns) if args.patterns.is_dir() else {}
    done = skipped = missing = 0
    for i, rel in enumerate(manifest):
        key = preview_key(rel)
        dst = previews / (key + ".bin")
        if dst.exists() and not args.force:
            skipped += 1
            continue
        src = index.get(key)
        if src is not None:
            text = src.read_text(errors="replace")
        elif args.from_table:
            try:
                text = fetch(f"{args.from_table.rstrip('/')}/sd/patterns/{rel}").decode(
                    errors="replace")
            except OSError as e:
                print(f"  SKIP {rel}: not local, fetch failed ({e})")
                missing += 1
                continue
        else:
            missing += 1
            continue
        dst.write_bytes(render_tile(text, args.size))
        done += 1
        if done % 50 == 0:
            print(f"  rendered {done} ({i + 1}/{len(manifest)})")

    print(f"done: {done} rendered, {skipped} already present, {missing} without a source")
    print(f"card root: {out}  ({len(manifest)} manifest entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
