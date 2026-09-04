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

MCU = spec.MCU


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

print("Building Dune Weaver Touch release %s (%d boards)\n"
      % (tag, len(spec.BOARDS)))

rel_path = os.path.join("release", tag)
if os.path.exists(rel_path):
    shutil.rmtree(rel_path)
os.makedirs(rel_path)

manifest_images = {}
staged = []

# One pass per board. Each names the env it builds from and every file that
# env contributes -- app image, bootloader, partition table -- so adding a
# board is a spec edit, not a new block here. The three hand-written blocks
# this replaced were what let the CrowPanel 7.0's 4 MB partition table have
# nowhere to go.
#
# A file two boards genuinely share (otadata, and the 16 MB partition table)
# is staged once: same manifest key, same filename, so the second board's copy
# is a no-op. It is verified rather than assumed -- if two envs ever produce
# DIFFERENT bytes under one name, that is exactly the silent mix-up this
# release path exists to prevent, so it fails here.
for board in spec.BOARDS:
    env = board["env"]
    print("\nBuilding %s (env: %s)\n" % (board["name"], env))
    run([PIO, "run", "-e", env])
    build_dir = os.path.join(".pio", "build", env)

    for key, offset, out_name in spec.board_images(board):
        # The env's own artifact name, which is never the release's: every env
        # builds "firmware.bin" and "bootloader.bin".
        src_name = ("firmware.bin" if offset == spec.APP_OFFSET else
                    "bootloader.bin" if offset == spec.BOOTLOADER_OFFSET else
                    "partitions.bin" if offset == spec.PARTITIONS_OFFSET else
                    "ota_data_initial.bin")
        src = os.path.join(build_dir, src_name)
        if not os.path.exists(src):
            sys.exit("Missing build artifact: %s" % src)
        dst = os.path.join(rel_path, out_name)
        if os.path.exists(dst):
            if sha256(dst) != sha256(src):
                sys.exit("%s: %s and an earlier board both stage %s, with "
                         "DIFFERENT bytes. One of them would be installed on "
                         "the wrong panel; give it its own filename in "
                         "release_spec.BOARDS." % (board["name"], env, out_name))
            continue
        shutil.copyfile(src, dst)
        staged.append(out_name)
        manifest_images[key] = {
            "size": os.path.getsize(dst),
            "offset": offset,
            "path": out_name,
            "signature": {"algorithm": "SHA2-256", "value": sha256(dst)},
        }
        print("  %-32s %8s  %9d B" % (out_name, offset, os.path.getsize(dst)))

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
