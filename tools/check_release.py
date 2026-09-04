#!/usr/bin/env python3

# Verify that a release directory is actually installable.
#
#   python3 tools/check_release.py releases/v0.1.3      # a published release
#   python3 tools/check_release.py release/v0.1.4       # one just staged
#   python3 tools/check_release.py releases/*           # everything on main
#
# Why this exists: v0.1.3 shipped firmware-800x480.bin next to a manifest that
# never mentioned it. Every byte a 5/7 owner needed was published, the release
# looked complete, and the web installer still offered them only the 1024x600
# build -- which flashes cleanly and boots unreadable. Nothing failed, so
# nobody noticed until someone with a 7 tried it.
#
# The rule that catches that class: a release is a CLOSED set. Every file in
# the directory must be reachable from the manifest, every image the manifest
# names must exist and hash, and every board in release_spec.BOARDS must be
# offered. Anything staged but unreferenced is a panel nobody can install.
#
# Exits non-zero with one line per problem. tools/build_release.py runs it on
# the staged directory before declaring success, and .github/workflows/
# check-releases.yml runs it over releases/* on every push, so a hand-edited
# manifest is checked too.

import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_spec as spec  # noqa: E402

# esp_app_desc_t sits at a fixed offset in every ESP-IDF app image: 24-byte
# image header + 8-byte first segment header. Reading it is how we tell a real
# app image from a bootloader, and one env's build from another's.
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
IMAGE_MAGIC = 0xE9


def app_desc(blob):
    """(version, project) from an ESP-IDF app image, or None if it isn't one."""
    if not blob or blob[0] != IMAGE_MAGIC:
        return None
    d = blob[APP_DESC_OFFSET:APP_DESC_OFFSET + 0x100]
    if len(d) < 0x60 or struct.unpack("<I", d[:4])[0] != APP_DESC_MAGIC:
        return None
    text = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")  # noqa: E731
    return text(d[16:48]), text(d[48:80])


def leaves(node):
    """Every installable leaf under a manifest choice node."""
    if node.get("images"):
        yield node
    for child in node.get("choices", []):
        for leaf in leaves(child):
            yield leaf


def check(path):
    problems = []
    def bad(msg):
        problems.append("%s: %s" % (path, msg))

    manifest_path = os.path.join(path, "manifest.json")
    if not os.path.exists(manifest_path):
        bad("no manifest.json")
        return problems
    try:
        manifest = json.load(open(manifest_path))
    except ValueError as err:
        bad("manifest.json does not parse: %s" % err)
        return problems

    images = manifest.get("images") or {}
    if not images:
        bad("manifest lists no images")
        return problems

    version = str(manifest.get("version", "")).lstrip("v")

    # 1. Every image the manifest names exists, at the size and hash it claims.
    #    A truncated or re-copied file passes esptool and bricks the panel.
    for name, image in sorted(images.items()):
        blob_path = os.path.join(path, image.get("path", ""))
        if not os.path.exists(blob_path):
            bad("image %s -> %s is missing" % (name, image.get("path")))
            continue
        blob = open(blob_path, "rb").read()
        if len(blob) != image.get("size"):
            bad("image %s is %d B, manifest says %s"
                % (name, len(blob), image.get("size")))
        digest = hashlib.sha256(blob).hexdigest()
        if digest != (image.get("signature") or {}).get("value"):
            bad("image %s does not match its SHA2-256" % name)

        # 2. Anything written to the app slot must BE an app, of this project
        #    and this version. Catches a stale build and the version.txt trap
        #    (PlatformIO does not reconfigure on a version bump, so an image
        #    can silently report the previous version and break OTA compare).
        if image.get("offset") == spec.APP_OFFSET:
            desc = app_desc(blob)
            if not desc:
                bad("image %s sits at the app offset but is not an app image"
                    % name)
            else:
                app_version, project = desc
                if project != spec.PROJECT:
                    bad("image %s is project %r, expected %r"
                        % (name, project, spec.PROJECT))
                if version and app_version != version:
                    bad("image %s reports version %r, release is %r"
                        % (name, app_version, version))

    # 3. Every file in the directory is reachable from the manifest. THIS is
    #    the v0.1.3 check: a staged image nothing offers is a panel nobody can
    #    install.
    referenced = set(i.get("path") for i in images.values())
    for name in sorted(os.listdir(path)):
        if name.endswith(".bin") and name not in referenced:
            bad("%s is in the release but no manifest image points at it"
                % name)

    # 4. Every leaf installs images that exist, and every image is installed by
    #    some leaf.
    installable = manifest.get("installable") or {}
    installed = set()
    found_leaf = False
    for leaf in leaves(installable):
        found_leaf = True
        for name in leaf["images"]:
            if name not in images:
                bad("choice %r installs unknown image %s"
                    % (leaf.get("name"), name))
            installed.add(name)
    if not found_leaf:
        bad("manifest offers nothing installable")
    for name in sorted(set(images) - installed):
        bad("image %s is declared but no choice installs it" % name)

    # 5. Every board is offered. Adding a panel to release_spec.BOARDS without
    #    the manifest catching up fails here rather than in someone's hands.
    #    Releases older than the second image predate the question.
    offered = set(c.get("name") for c in installable.get("choices", []))
    # Per BOARD, not one global cutoff: a release cannot offer a panel that did
    # not exist when it was built, so requiring one is a bug in this check
    # rather than a finding. See release_spec.BOARDS["since"].
    ver = spec.version_key(version)
    boards = [b for b in spec.BOARDS if ver and ver >= spec.version_key(b["since"])]
    for board in boards:
        if board["name"] not in offered:
            bad("board %r is not offered by the manifest" % board["name"])
            continue
        # Not just the app: a board is its bootloader and partition table too,
        # and those are per board now. The CrowPanel 7.0 is 4 MB, so a release
        # carrying its app image against the 16 MB table would flash cleanly
        # and reboot-loop in flash init -- unreadable, and blamed on the board.
        for key, offset, _ in spec.board_images(board):
            if key not in images:
                bad("board %r needs image %s, which the release does not have"
                    % (board["name"], key))
            elif images[key].get("offset") != offset:
                bad("board %r writes %s at %s, manifest says %s"
                    % (board["name"], key, offset, images[key].get("offset")))

    # 6. The panels must not share one binary. If both app images are the same
    #    bytes, one env did not rebuild and half the panels get the wrong
    #    geometry -- the exact failure the second image exists to prevent.
    digests = {}
    for board in boards:
        image = images.get(board["app"][0])
        blob_path = image and os.path.join(path, image.get("path", ""))
        if blob_path and os.path.exists(blob_path):
            digests.setdefault(
                hashlib.sha256(open(blob_path, "rb").read()).hexdigest(),
                []).append(board["name"])
    for shared in [b for b in digests.values() if len(b) > 1]:
        bad("these boards share one app image: %s" % ", ".join(shared))

    return problems


def main(argv):
    paths = [p.rstrip("/") for p in argv[1:]]
    if not paths:
        sys.exit("usage: check_release.py <release dir> [...]")

    problems = []
    for path in paths:
        if not os.path.isdir(path):
            problems.append("%s: not a directory" % path)
            continue
        found = check(path)
        known = spec.KNOWN_BROKEN.get(os.path.basename(path))
        if known and found:
            # Published and unfixable: say so every run, but do not fail on it.
            print("%-24s KNOWN BROKEN (%s)" % (path, known))
            for line in found:
                print("      %s" % line)
            continue
        problems += found
        print("%-24s %s" % (path, "OK" if not found else
                            "%d problem(s)" % len(found)))

    if problems:
        print("\n".join("  - " + p for p in problems), file=sys.stderr)
        sys.exit("\n%d problem(s); this release is not installable as staged."
                 % len(problems))


if __name__ == "__main__":
    main(sys.argv)
