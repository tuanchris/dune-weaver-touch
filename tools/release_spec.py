#!/usr/bin/env python3

# What a Dune Weaver Touch release IS: the envs to build, the images to stage,
# the offsets to write them at, and the manifest tree that offers them.
#
# It lives apart from build_release.py so that tools/check_release.py can
# verify a release against the same declaration that produced it. That split
# is not tidiness -- v0.1.3 shipped firmware-800x480.bin with a manifest that
# never mentioned it, so the 800x480 panels the image existed for could not be
# installed over USB at all, and nothing in the release path noticed. Adding a
# panel here now makes check_release fail until the manifest offers it.

PROJECT = "dune-weaver-touch"
REPO = "https://github.com/tuanchris/" + PROJECT
MCU = "esp32s3"

# The offsets esptool writes to on a fresh install. These are NOT the generic
# ESP32 ones: the S3 boots its bootloader from 0x0, not 0x1000. Getting this
# wrong produces a board that flashes cleanly and then will not boot.
# Mirrored from .pio/build/<env>/flasher_args.json -- if a partition table
# moves, re-read that file rather than editing these from memory.
#
# 0x20000, NOT the 0x10000 you remember: ota_0 starts at the first 64 KB
# boundary above otadata. partitions-4mb.csv keeps nvs, otadata and ota_0 at
# these same offsets on purpose, so only the TABLE differs on a 4 MB board and
# nothing here becomes per-board.
BOOTLOADER_OFFSET = "0x0"
PARTITIONS_OFFSET = "0x8000"
# otadata decides which slot boots. A fresh install MUST write it: the bytes
# already at 0xF000 on a panel coming off the old factory-only table are the
# tail of its phy_init partition, and a stale/garbage otadata is how you get a
# board that flashes cleanly and boots the wrong slot.
OTADATA_OFFSET = "0xF000"
APP_OFFSET = "0x20000"

# Built once and shared by every board. MEASURED, not assumed (2026-09-04):
# ota_data_initial.bin is byte-identical across waveshare-7, crowpanel-adv-5
# and crowpanel-7. The other two are not, which is why they live per board:
#   bootloader   differs on ALL THREE -- console config and, on the 7.0-HMI,
#                the 4 MB flash size in the image header
#   partitions   waveshare-7 == crowpanel-adv-5; crowpanel-7 differs (4 MB)
# Re-measure before sharing anything else here; the last time this was assumed
# rather than checked, a release shipped an image nobody could install.
OTADATA = ("%s-otadata" % MCU, "ota_data_initial.bin")
# The 16 MB table, shared by every board that is not the 4 MB CrowPanel 7.0.
PARTITIONS_16MB = ("%s-partitions" % MCU, "partitions.bin")

def version_key(version):
    """A sortable key for "v0.1.6-rc2", "0.1.6" or "v0.1.3", ordering a
    prerelease BEFORE the release it precedes and rc1 before rc2.

    A plain (0, 1, 6) tuple cannot do this -- it makes v0.1.6-rc1 and v0.1.6
    equal -- and that is not academic: BOARDS["since"] has to be able to say
    "from v0.1.6-rc2", because v0.1.6-rc1 is already published without the
    board. Without the ordering the only way to add a board is to skip a
    version number, which is a worse answer than parsing the suffix.

    (ota.c's version_cmp deliberately does the OPPOSITE and treats -rc1 as
    equal to the release, so a tester is never offered a downgrade. Different
    question, different rule.)
    """
    text = str(version).lstrip("vV")
    core, _, suffix = text.partition("-")
    try:
        parts = tuple(int(part) for part in core.split("."))
    except ValueError:
        return ()
    if not suffix:
        return (parts, 1, 0)  # a final release outranks all its prereleases
    digits = "".join(c for c in suffix if c.isdigit())
    return (parts, 0, int(digits) if digits else 0)


# The panels a release ships an app image for. Nothing on the wire tells them
# apart -- same MCU, same USB bridge -- so the web installer has to ASK, which
# is why each appears as a "Board" choice rather than being resolved here.
# Flashing the wrong one leaves a panel that boots and is unreadable.
#
# Board REVISIONS are deliberately absent. They are resolved on the device (the
# CrowPanel Advance probes its expander, and its backlight ladders land
# correctly under either v1.1 or v1.2+ encoding), never by asking the user, who
# mostly does not know what revision they own. Add a board here only when the
# IMAGE differs; if the difference can be resolved at runtime, resolve it there.
#
# Each board names the env it builds from and the files that env contributes.
# `bootloader` and `partitions` default to nothing shared -- a board must say
# which it uses, because getting a bootloader from the wrong env is silent.
BOARDS = [
    {
        # ONE image for both: they differ only in whether the glass has square
        # pixels, and this is the 7's build, so a 5 gets the 7's 7.6% aspect
        # correction. The 5 is untested hardware; revisit when one is measured,
        # and until then it beats a 1024x600 build.
        "name": "ESP32-S3-Touch-LCD-7 or -5",
        "since": "v0.1.3",
        "description": "Waveshare 800x480 panel, 7\" or 5\" (16MB flash, 8MB "
                       "PSRAM). Runs the dark theme. Not the 5B, which is "
                       "1024x600.",
        "env": "waveshare-7",
        "app": ("%s-firmware-800x480" % MCU, "firmware-800x480.bin"),
        "bootloader": ("%s-bootloader" % MCU, "bootloader.bin"),
        "partitions": PARTITIONS_16MB,
    },
    {
        "name": "Elecrow CrowPanel Advance 5.0-HMI",
        "since": "v0.1.5",
        "description": "Elecrow 5\" 800x480 panel (16MB flash, 8MB PSRAM). "
                       "Runs the dark theme. Set the Function Select DIP to "
                       "1 1 (both ON) or the TF card will not be detected.",
        "env": "crowpanel-adv-5",
        "app": ("%s-firmware-crowpanel-adv-5" % MCU,
                "firmware-crowpanel-adv-5.bin"),
        # Own bootloader: no USB-Serial-JTAG (GPIO19/20 are the I2S mic), so
        # the console is UART0 and CONFIG_ESP_CONSOLE_* changes the bytes.
        "bootloader": ("%s-bootloader-crowpanel-adv-5" % MCU,
                       "bootloader-crowpanel-adv-5.bin"),
        "partitions": PARTITIONS_16MB,
    },
    {
        # The ORIGINAL CrowPanel 7.0 (DIS07070H), not the Advance series and
        # not the Waveshare 7. The only 4 MB board: a 16 MB image header
        # asserts in flash init and reboot-loops, so its partition table AND
        # bootloader are its own. That is the whole reason this spec grew a
        # per-board partition table.
        "name": "Elecrow CrowPanel 7.0-HMI",
        "since": "v0.1.6-rc2",
        "description": "Elecrow 7\" 800x480 panel, the original HMI series "
                       "(4MB flash, 8MB PSRAM). Runs the dark theme. Not the "
                       "CrowPanel Advance, which is a different board.",
        "env": "crowpanel-7",
        "app": ("%s-firmware-crowpanel-7" % MCU, "firmware-crowpanel-7.bin"),
        "bootloader": ("%s-bootloader-crowpanel-7" % MCU,
                       "bootloader-crowpanel-7.bin"),
        "partitions": ("%s-partitions-4mb" % MCU, "partitions-4mb.bin"),
    },
]

# The Waveshare 5B (1024x600, firmware.bin) was dropped in v0.1.6-rc2. It is
# not a build problem -- the waveshare-5b envs still exist and still build, and
# the theme tokens and Browse grid are still per-panel -- it is a PRODUCT
# decision: the web installer has never offered it (SUPPORTED_BOARDS in
# dune-weaver-website filters to the CrowPanels), so no release since v0.1.5
# has been installable on one anyway.
#
# The consequence to know: ota.c on a 5B still pulls releases/<tag>/
# firmware.bin, which no longer exists, so its update check 404s. That fails
# SAFE -- the panel keeps running what it has and reports the update failed --
# but a 5B in the field can no longer update itself. Put the board back here
# and the image returns; nothing else has to change.

# Each board's "since" is the release that FIRST offered it, and check_release
# requires a board only of releases at or after it. Without that, adding a
# board retroactively breaks every older release: v0.1.5 added the CrowPanel
# Advance and turned check-releases.yml red on v0.1.3 and v0.1.4, where it
# stayed for two releases because only the Release workflow was being watched.
# A published release cannot grow a board it never built, so demanding one is
# a bug in the check, not a finding.
#
# Releases before the first "since" are genuinely single-board and are held to
# the integrity rules only.
MULTI_BOARD_SINCE = min((b["since"] for b in BOARDS), key=version_key)

# Published releases with defects that CANNOT be fixed -- the bytes are on
# GitHub and panels have installed them. Recorded, reported, and not failed, so
# CI stays a signal about releases we can still affect rather than a permanent
# red that everyone learns to scroll past. Only add to this after confirming
# the release is genuinely unfixable; a staged release is never known-broken.
KNOWN_BROKEN = {
    # Its manifest offers a single board called "esp32s3" and never references
    # the firmware-800x480.bin staged beside it -- the v0.1.3 bug, repeated one
    # release later, before the spec existed to catch it. 800x480 owners could
    # not install v0.1.4 at all; v0.1.5 is their next working release.
    "v0.1.4": "manifest offers one board and omits firmware-800x480.bin "
              "(the v0.1.3 defect, repeated); unfixable, superseded by v0.1.5",
}


def board_images(board):
    """[(manifest key, offset, staged filename)] a fresh install writes for one
    board, in flash order with the app last. This is the single place that says
    what a board is MADE of: build_release stages exactly this, and
    check_release requires exactly this."""
    return [
        (board["bootloader"][0], BOOTLOADER_OFFSET, board["bootloader"][1]),
        (board["partitions"][0], PARTITIONS_OFFSET, board["partitions"][1]),
        (OTADATA[0], OTADATA_OFFSET, OTADATA[1]),
        (board["app"][0], APP_OFFSET, board["app"][1]),
    ]


def shared_images_for(board):
    """The manifest keys a fresh install writes BESIDE the app image. Every one
    is per board now: bootloaders differ on all three boards, and the 4 MB
    CrowPanel 7.0 needs its own partition table too. Only otadata is common."""
    return [key for key, _, _ in board_images(board)
            if key != board["app"][0]]


def board_choices(board):
    """The two installation types for one panel, differing only in how much of
    the board they rewrite."""
    return {
        "name": board["name"],
        "description": board["description"],
        "choice-name": "Installation type",
        "choices": [
            {
                "name": "fresh-install",
                "description": "Complete install, erasing all previous data "
                               "including saved WiFi credentials.",
                "erase": True,
                "images": shared_images_for(board) + [board["app"][0]],
            },
            {
                # NVS is left alone, so the panel comes back on its own WiFi
                # and table. otadata ships WITH the app on purpose: this
                # writes ota_0, and if otadata still pointed at ota_1 the
                # board would boot the older slot and the update would look
                # like it silently did nothing.
                "name": "firmware-update",
                "description": "Update firmware only, preserving NVS (WiFi "
                               "credentials, table address, screen settings).",
                "erase": False,
                "images": [OTADATA[0], board["app"][0]],
            },
        ],
    }


def build_manifest(tag, images):
    """The manifest an ESP Web Tools installer consumes. `images` maps image
    name -> {size, offset, path, signature}."""
    return {
        "name": "Dune Weaver Touch",
        "version": tag,
        "source_url": "%s/tree/%s" % (REPO, tag),
        "release_url": "%s/releases/tag/%s" % (REPO, tag),
        "images": images,
        # Board first, because it is the one question the installer cannot
        # answer for the user.
        "installable": {
            "name": "installable",
            "description": "Things you can install",
            "choice-name": "Board",
            "choices": [board_choices(b) for b in BOARDS],
        },
    }
