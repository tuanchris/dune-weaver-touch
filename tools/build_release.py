#!/usr/bin/env python3

# Build a Dune Weaver Touch release: the flat .bin images esptool writes, a
# manifest.json an ESP Web Tools installer can consume, and a convenience zip.
#
#   python3 tools/build_release.py          # build + assemble release/<tag>/
#   python3 tools/build_release.py -v       # verbose pio output
#
# The tag comes from DW_RELEASE_TAG (exported by CI) or `git describe`. Manifest
# image `path` fields are bare filenames so they map 1:1 onto GitHub release
# assets, which live in a flat namespace.
#
# Modeled on dune-weaver-firmware's build-dw-release.py, minus what this board
# does not have: one target instead of two (so no MCU filename prefixes), no
# boot_app0 (the partition table is factory-only, there is no otadata), and no
# filesystem image (previews live on the TF card, not in flash -- there is no
# data partition at all; see PORTING_NOTES section 6).

import hashlib
import json
import os
import shutil
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile

VERBOSE = "-v" in sys.argv
PIO = shutil.which("pio") or shutil.which("platformio") or "/opt/homebrew/bin/pio"

REPO = "https://github.com/tuanchris/dune-weaver-touch"

ENV = "waveshare-5b"
MCU = "esp32s3"
BOARD_DESC = "Waveshare ESP32-S3-Touch-LCD-5B (16MB flash, 8MB PSRAM)"

# The offsets esptool writes each image to on a fresh install. These are NOT the
# generic ESP32 ones: the S3 boots its bootloader from 0x0, not 0x1000. Getting
# this wrong produces a board that flashes cleanly and then will not boot.
# Mirrored from .pio/build/<env>/flasher_args.json -- if the partition table
# moves, re-read that file rather than editing these from memory.
IMAGES = [
    # name,         offset,     source filename in .pio/build/<env>/
    ("bootloader", "0x0", "bootloader.bin"),
    ("partitions", "0x8000", "partitions.bin"),
    ("firmware", "0x10000", "firmware.bin"),
]


def run(cmd):
    print("+", " ".join(cmd))
    if VERBOSE:
        rc = subprocess.run(cmd).returncode
    else:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for raw in proc.stdout:
            line = raw.decode("utf8", "replace")
            low = line.lower()
            if "took" in line or "RAM:" in line or "Flash:" in line or (
                    "error" in low and "compiling" not in low):
                print(line, end="")
        proc.wait()
        rc = proc.returncode
    if rc != 0:
        sys.exit("Command failed (%d): %s" % (rc, " ".join(cmd)))


def git(*args):
    return subprocess.check_output(["git", *args]).strip().decode("utf-8")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# CI exports the tag that TRIGGERED the release as DW_RELEASE_TAG. `git describe`
# tie-breaks arbitrarily when one commit carries several tags -- cutting a final
# release at its last RC's commit staged the whole thing under the RC's name in
# the firmware repo -- so an explicit tag always wins. Unset (a local build)
# keeps the describe behaviour.
tag = os.environ.get("DW_RELEASE_TAG", "").strip() or git("describe", "--tags", "--abbrev=0")
try:
    if git("rev-parse", tag + "^{commit}") != git("rev-parse", "HEAD"):
        raise subprocess.CalledProcessError(1, "rev-parse")
except subprocess.CalledProcessError:
    print("WARNING: HEAD is not exactly on tag %s. Tag this commit first to ship\n"
          "         a release whose assets match the tagged source." % tag)

print("Building Dune Weaver Touch release %s (env: %s)\n" % (tag, ENV))

rel_path = os.path.join("release", tag)
if os.path.exists(rel_path):
    shutil.rmtree(rel_path)
os.makedirs(rel_path)

run([PIO, "run", "-e", ENV])

build_dir = os.path.join(".pio", "build", ENV)
manifest_images = {}
staged = []

for name, offset, src_name in IMAGES:
    src = os.path.join(build_dir, src_name)
    if not os.path.exists(src):
        sys.exit("Missing build artifact: %s" % src)
    dst = os.path.join(rel_path, src_name)
    shutil.copyfile(src, dst)
    staged.append(src_name)
    manifest_images["%s-%s" % (MCU, name)] = {
        "size": os.path.getsize(dst),
        "offset": offset,
        "path": src_name,
        "signature": {"algorithm": "SHA2-256", "value": sha256(dst)},
    }
    print("  %-14s %8s  %9d B" % (src_name, offset, os.path.getsize(dst)))

image_names = ["%s-%s" % (MCU, n) for n, _, _ in IMAGES]

manifest = {
    "name": "Dune Weaver Touch",
    "version": tag,
    "source_url": "%s/tree/%s" % (REPO, tag),
    "release_url": "%s/releases/tag/%s" % (REPO, tag),
    "images": manifest_images,
    "installable": {
        "name": "installable",
        "description": "Things you can install",
        "choice-name": "Board",
        "choices": [{
            "name": MCU,
            "description": BOARD_DESC,
            "choice-name": "Installation type",
            "choices": [{
                "name": "touch-panel",
                "description": "Dune Weaver Touch panel (WiFi + mDNS table discovery)",
                "choice-name": "Installation type",
                "choices": [
                    {
                        "name": "fresh-install",
                        "description": "Complete install, erasing all previous data "
                                       "including saved WiFi credentials.",
                        "erase": True,
                        "images": image_names,
                    },
                    {
                        # NVS survives, so the panel comes back on its own WiFi and
                        # table. Only the app partition is rewritten.
                        "name": "firmware-update",
                        "description": "Update firmware only, preserving NVS (WiFi "
                                       "credentials, table address, screen settings).",
                        "erase": False,
                        "images": ["%s-firmware" % MCU],
                    },
                ],
            }],
        }],
    },
}

manifest_path = os.path.join(rel_path, "manifest.json")
with open(manifest_path, "w") as f:
    f.write(json.dumps(manifest, indent=2) + "\n")
staged.append("manifest.json")

zip_name = "dune-weaver-touch-%s-%s.zip" % (tag, MCU)
# ZipFile defaults to ZIP_STORED, which makes the "convenience" zip LARGER than
# the loose assets it bundles (1.66 MB vs 1.06 MB). The app image deflates well.
with ZipFile(os.path.join(rel_path, zip_name), "w", ZIP_DEFLATED) as z:
    for name in staged:
        z.write(os.path.join(rel_path, name), name)

print("\nStaged %s/:" % rel_path)
for name in sorted(os.listdir(rel_path)):
    print("  %-42s %9d B" % (name, os.path.getsize(os.path.join(rel_path, name))))
