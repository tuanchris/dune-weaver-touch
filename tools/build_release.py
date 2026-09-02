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
# WHAT a release contains -- envs, images, offsets, the manifest tree -- lives
# in tools/release_spec.py, so tools/check_release.py can verify a release
# against the same declaration that built it. This script is only the doing.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_release  # noqa: E402
import release_spec as spec  # noqa: E402

VERBOSE = "-v" in sys.argv
PIO = shutil.which("pio") or shutil.which("platformio") or "/opt/homebrew/bin/pio"

ENV = spec.ENV
ENV_800X480 = spec.ENV_800X480
IMAGE_800X480 = spec.IMAGE_800X480
MCU = spec.MCU
IMAGES = spec.IMAGES


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

# Build and stage the 800x480 image alongside. Two consumers, one file:
# ota.c fetches it by name on those panels, and the manifest below offers it
# as the second Board choice so the web installer can write it over USB.
print("\nBuilding the 800x480 image (env: %s)\n" % ENV_800X480)
run([PIO, "run", "-e", ENV_800X480])
src_800 = os.path.join(".pio", "build", ENV_800X480, "firmware.bin")
if not os.path.exists(src_800):
    sys.exit("Missing build artifact: %s" % src_800)
dst_800 = os.path.join(rel_path, IMAGE_800X480)
shutil.copyfile(src_800, dst_800)
staged.append(IMAGE_800X480)
# Same slot as the 5B app image -- it IS the app image, for the other panel.
manifest_images["%s-firmware-800x480" % MCU] = {
    "size": os.path.getsize(dst_800),
    "offset": spec.APP_OFFSET,
    "path": IMAGE_800X480,
    "signature": {"algorithm": "SHA2-256", "value": sha256(dst_800)},
}
print("  %-14s %8s  %9d B" % (IMAGE_800X480, spec.APP_OFFSET, os.path.getsize(dst_800)))

# Build and stage the CrowPanel Advance 5.0. Two images, not one: it needs its
# own bootloader (UART0 console rather than USB-Serial-JTAG, which changes the
# bytes). partitions and otadata are byte-identical to the 5B's and stay shared.
print("\nBuilding the CrowPanel Advance 5.0 images (env: %s)\n" % spec.ENV_CROWPANEL)
run([PIO, "run", "-e", spec.ENV_CROWPANEL])
crow_dir = os.path.join(".pio", "build", spec.ENV_CROWPANEL)
for src_name, out_name, image_key, offset in (
        ("firmware.bin", spec.IMAGE_CROWPANEL,
         "%s-firmware-crowpanel-adv-5" % MCU, spec.APP_OFFSET),
        ("bootloader.bin", spec.BOOTLOADER_CROWPANEL,
         "%s-bootloader-crowpanel-adv-5" % MCU, "0x0"),
):
    src = os.path.join(crow_dir, src_name)
    if not os.path.exists(src):
        sys.exit("Missing build artifact: %s" % src)
    dst = os.path.join(rel_path, out_name)
    shutil.copyfile(src, dst)
    staged.append(out_name)
    manifest_images[image_key] = {
        "size": os.path.getsize(dst),
        "offset": offset,
        "path": out_name,
        "signature": {"algorithm": "SHA2-256", "value": sha256(dst)},
    }
    print("  %-14s %8s  %9d B" % (out_name, offset, os.path.getsize(dst)))

manifest = spec.build_manifest(tag, manifest_images)

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

# A release is only done if it is installable. v0.1.3 staged an image the
# manifest never mentioned and published anyway, so this is not advisory: a
# failure here means do not tag, do not publish.
print()
problems = check_release.check(rel_path)
if problems:
    print("\n".join("  - " + p for p in problems), file=sys.stderr)
    sys.exit("%s is NOT installable as staged (%d problem(s)); nothing was "
             "published." % (rel_path, len(problems)))
print("%s checks out: every image hashes, every board is offered." % rel_path)
