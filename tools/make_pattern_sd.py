#!/usr/bin/env python3
"""Prepare a pattern microSD card for the dune-weaver touch panel.

Writes the layout docs/PORTING_NOTES.md 7a documents:

    <out>/patterns.json          manifest: JSON array of paths relative to
                                 /patterns (the table's /sand_patterns format)
    <out>/previews/<key>.bin     4-bit ALPHA MASK, 300x300, 45,000 bytes,
                                 key = pattern basename lowercased (".thr" kept)

ONE folder, ONE size. A tile is the pattern's stroke coverage and nothing
else — no colour, no dish, no ring. The panel owns the look: it builds the
dish/ring/background from its own theme tokens and composites the sand colour
through this mask (src/render/thr_preview.c). That means:

  * 45,000 B/tile instead of 180,000 as RGB565, on a bus where the read is
    the whole cost — and smaller even than a 160 px RGB565 tile (51,200 B).
  * The panel resamples ONE 8-bit channel to reach a widget's size instead of
    three packed RGB565 channels, so the Browse grid's 300 -> 160 costs ~3 ms
    rather than ~20.
  * Tiles are theme-independent: night/day retheming needs no card re-prep.

Nibble order is low-first: byte i holds pixel 2i in its low nibble and pixel
2i+1 in its high nibble. 0 = bare dish, 15 = full sand. Row-major, no header,
no padding — the length IS the format check, so the size must be exact.

Usage:
    python tools/make_pattern_sd.py --out /Volumes/PATTERNS
    python tools/make_pattern_sd.py --out sim/simfs/sdcard          # UI sim
    python tools/make_pattern_sd.py --from-table http://192.168.68.123 \
        --out /Volumes/PATTERNS   # manifest from that table's catalog
    python tools/make_pattern_sd.py --out /Volumes/PATTERNS \
        --patterns /Volumes/PATTERNS/patterns \
        --from-bundle /Volumes/PATTERNS/patterns/previews   # PM export

With --from-table, the manifest is the TABLE's catalog; preview pixels still
come from the local library (matched by basename), and entries missing
locally are fetched from the table (slow: boards serve ~45 KB/s).

--from-bundle points at a dune-weaver-website Pattern Manager export
(previews.json + shard-*.zip of <name>.thr.webp files). Those webps are
512px ALPHA MASKS of the pattern path (RGB is all zero, full-bleed: rho=1 at
the canvas edge) — higher-quality strokes than this script's own polyline
render, and already the same kind of thing a tile now is, so they only need
resampling into the dish inset. Patterns missing from the bundle fall back to
rendering the .thr as a mask here.

Requires: Pillow, numpy (both in the miniforge base env).
"""

import argparse
import io
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw
import numpy as np

DEFAULT_PATTERNS = Path("/Volumes/SSD/projects/dune-weaver-pi/patterns")

# Dish geometry. The COLOURS deliberately live in the firmware now
# (src/ui/theme.h + thr_preview.c's base map) — a tile carries coverage only,
# so retheming the panel does not invalidate a card. This must stay in step
# with thr_preview.c's class_map_build(): margin = size*12/512, and the
# pattern's rho=1 lands exactly on the dish edge.
MARGIN_NUM, MARGIN_DEN = 12, 512
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
    """Render one .thr to a packed 4-bit coverage mask, firmware geometry."""
    big = size * SUPERSAMPLE
    margin = big * MARGIN_NUM // MARGIN_DEN
    cx = big / 2.0
    radius = cx - margin

    # 8-bit grey = coverage. No dish, no ring, no background: the panel draws
    # those from its own theme and composites this through them.
    img = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(img)

    pts = [
        (cx + rho * radius * np.cos(theta), cx + rho * radius * np.sin(theta))
        for theta, rho in parse_thr(text)
    ]
    # PORTING_NOTES §6 spec is "~2 px at 512", so the stroke has to scale with
    # the tile or small tiles turn into solid discs: a flat 2 px is 1.7x the
    # spec at 300, thick enough to close the gaps between neighbouring passes.
    stroke = max(1, round(2.0 * big / MARGIN_DEN))
    if len(pts) >= 2:
        draw.line(pts, fill=255, width=stroke, joint="curve")
    elif len(pts) == 1:
        x, y = pts[0]
        w = SUPERSAMPLE
        draw.ellipse([x - w, y - w, x + w, y + w], fill=255)

    return pack_a4(img.resize((size, size), Image.LANCZOS))


def pack_a4(mask: Image.Image) -> bytes:
    """8-bit coverage -> 4 bits/px, two pixels per byte, LOW nibble first.

    Rounds rather than truncates: `a >> 4` would make 255 read as 15 but 254
    as 15 too while pushing every mid-tone a half-step dark, and these masks
    are mostly mid-tone antialiased hairlines."""
    a = np.asarray(mask, dtype=np.uint16)
    q = ((a * 15 + 127) // 255).astype(np.uint8)  # 0..15, round-to-nearest
    if q.shape[1] % 2:
        raise ValueError(f"tile size must be even, got {q.shape[1]}")
    lo = q[:, 0::2]
    hi = q[:, 1::2]
    return ((hi << 4) | lo).tobytes()


def load_bundle(bundle_dir: Path) -> tuple[dict, dict]:
    """key ('name.thr', lowercased) -> (zip Path, entry name) from a Pattern
    Manager export, plus the open ZipFile per shard. Indexed straight off the
    shard listings; previews.json is only the shard integrity index and isn't
    needed here.

    The handles stay open for the whole run: there are only ~8 shards, and
    reopening one per pattern re-read its central directory 1167 times, off
    the SD card the export usually sits on."""
    index = {}
    shards = {}
    for zp in sorted(bundle_dir.glob("shard-*.zip")):
        if zp.name.startswith("._"):
            continue  # AppleDouble metadata, not a zip
        z = zipfile.ZipFile(zp)
        shards[zp] = z
        for name in z.namelist():
            base = name.rsplit("/", 1)[-1]
            if base.startswith("._") or not base.lower().endswith(".thr.webp"):
                continue
            index.setdefault(base[: -len(".webp")].lower(), (zp, name))
    return index, shards


def render_bundle_tile(webp_bytes: bytes, size: int) -> bytes:
    """Resample a Pattern Manager alpha mask into the dish inset, pack to 4 bit.

    The export is already exactly what a tile now is — coverage, full-bleed
    with rho=1 at the canvas edge — so this only has to place it: the dish
    occupies size-2*margin px, inset by margin on both axes."""
    margin = size * MARGIN_NUM // MARGIN_DEN
    dish_d = size - 2 * margin

    alpha = Image.open(io.BytesIO(webp_bytes)).split()[-1]
    tile = Image.new("L", (size, size), 0)
    tile.paste(alpha.resize((dish_d, dish_d), Image.LANCZOS), (margin, margin))
    return pack_a4(tile)


def local_index(patterns_dir: Path) -> dict:
    """basename-lowercase -> local Path, for cross-folder matching."""
    index = {}
    for p in sorted(patterns_dir.rglob("*.thr")):
        if p.name.startswith("._"):
            continue  # macOS AppleDouble metadata on FAT cards
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
    ap.add_argument("--from-bundle", type=Path, metavar="DIR",
                    help="Pattern Manager export dir (shard-*.zip of .thr.webp "
                         "alpha masks) as the preferred preview pixel source")
    ap.add_argument("--size", type=int, default=300,
                    help="tile size in px. Must match PV_SD_SIZE_PX in "
                         "src/render/thr_preview.c (default 300) — the firmware "
                         "checks the file LENGTH (size*size/2) and treats a "
                         "mismatch as 'no tile for this pattern'. One size only: "
                         "the panel resamples the single 8-bit mask to whatever "
                         "a widget displays at, which is cheap.")
    ap.add_argument("--force", action="store_true", help="re-render existing tiles")
    args = ap.parse_args()

    if args.size % 2:
        sys.exit(f"--size must be even (4-bit tiles pack two pixels per byte): {args.size}")

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
            str(p.relative_to(args.patterns))
            for p in args.patterns.rglob("*.thr")
            if not p.name.startswith("._")  # macOS AppleDouble metadata
        )
        print(f"manifest: {len(manifest)} patterns from {args.patterns}")

    (out / "patterns.json").write_text(json.dumps(manifest, indent=0))

    bundle = {}
    shards = {}
    if args.from_bundle:
        if not args.from_bundle.is_dir():
            sys.exit(f"bundle dir not found: {args.from_bundle}")
        bundle, shards = load_bundle(args.from_bundle)
        print(f"bundle: {len(bundle)} preview masks from {args.from_bundle}")

    tile_bytes = args.size * args.size // 2
    print(f"tiles: {args.size}px 4-bit masks, {tile_bytes:,} B each -> {previews}")

    # Anything already there at the wrong length is a different format (an
    # RGB565 card from before 2026-08-26 is 180,000 B) or a truncated write.
    # Rewrite it regardless of --force: leaving it would look "already
    # present" while the panel rejects every one of them as a missing tile.
    stale = [d for d in sorted(out.glob("previews@*")) if d.is_dir()]
    if stale:
        print(f"NOTE: {len(stale)} stale per-size dir(s) the firmware no longer reads: "
              + ", ".join(d.name for d in stale))
        print("      remove them by hand once you're happy with the new tiles.")

    index = local_index(args.patterns) if args.patterns.is_dir() else {}
    done = bundled = skipped = missing = refreshed = 0
    for i, rel in enumerate(manifest):
        key = preview_key(rel)
        dst = previews / (key + ".bin")
        if dst.exists():
            wrong_size = dst.stat().st_size != tile_bytes
            if not (args.force or wrong_size):
                skipped += 1
                continue
            if wrong_size:
                refreshed += 1

        hit = bundle.get(key)
        if hit is not None:
            zp, entry = hit
            dst.write_bytes(render_bundle_tile(shards[zp].read(entry), args.size))
            bundled += 1
        else:
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
        if (done + bundled) % 50 == 0:
            print(f"  written {done + bundled} ({i + 1}/{len(manifest)})")

    for z in shards.values():
        z.close()

    print(f"done: {bundled} from bundle, {done} rendered, "
          f"{skipped} already present, {missing} without a source"
          + (f" ({refreshed} rewritten from a previous format)" if refreshed else ""))
    print(f"card root: {out}  ({len(manifest)} manifest entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
