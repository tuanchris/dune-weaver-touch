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

# The RELEASE env, deliberately not "waveshare-5b": that one carries
# -DUI_DEBUG_RGB_STOP (the unmeasured sleep-panel-reset experiment) and must
# never ship. See platformio.ini.
ENV = "waveshare-5b-release"
# Second app image, for the 800x480 boards (the 5 and the 7). ota.c fetches
# firmware-800x480.bin on those, and the manifest offers it as the second Board
# choice, so a release without it leaves those panels unable to install OR
# update -- and one that named it firmware.bin would flash a 1024x600 build
# onto them.
ENV_800X480 = "waveshare-7"
IMAGE_800X480 = "firmware-800x480.bin"
# Third app image, for the Elecrow CrowPanel Advance 5.0. It is 800x480 too but
# a DIFFERENT BOARD -- own pin map, own expander, own console -- so it cannot
# share the Waveshare image; ota.c selects by board, never by resolution.
#
# It also needs its OWN bootloader, unlike the 5/7: this board has no
# USB-Serial-JTAG (GPIO19/20 are the I2S mic), so its console is UART0 and
# CONFIG_ESP_CONSOLE_* differs, which changes the bootloader bytes. Measured
# 2026-09-02: bootloader.bin DIFFERS from the 5B's, partitions.bin and
# ota_data_initial.bin are byte-identical and stay shared.
ENV_CROWPANEL = "crowpanel-adv-5"
IMAGE_CROWPANEL = "firmware-crowpanel-adv-5.bin"
BOOTLOADER_CROWPANEL = "bootloader-crowpanel-adv-5.bin"
MCU = "esp32s3"

# The offsets esptool writes each image to on a fresh install. These are NOT the
# generic ESP32 ones: the S3 boots its bootloader from 0x0, not 0x1000. Getting
# this wrong produces a board that flashes cleanly and then will not boot.
# Mirrored from .pio/build/<env>/flasher_args.json -- if the partition table
# moves, re-read that file rather than editing these from memory.
IMAGES = [
    # name,         offset,     source filename in .pio/build/<env>/
    ("bootloader", "0x0", "bootloader.bin"),
    ("partitions", "0x8000", "partitions.bin"),
    # otadata decides which slot boots. A fresh install MUST write it: the bytes
    # already at 0xF000 on a panel coming off the old factory-only table are the
    # tail of its phy_init partition, and a stale/garbage otadata is how you get
    # a board that flashes cleanly and boots the wrong slot.
    ("otadata", "0xF000", "ota_data_initial.bin"),
    # 0x20000, NOT the 0x10000 you remember: ota_0 starts at the first 64 KB
    # boundary above otadata. See partitions.csv.
    ("firmware", "0x20000", "firmware.bin"),
]

# Where the app image goes, read back out of IMAGES so the 800x480 image cannot
# drift from the 5B's.
APP_OFFSET = dict((name, offset) for name, offset, _ in IMAGES)["firmware"]

# The two panels the release ships an app image for. Nothing on the wire tells
# them apart -- same MCU, same USB bridge, same GPIO map -- so the web
# installer has to ASK, which is why both appear as a "Board" choice rather
# than being resolved here. Flashing the wrong one leaves a panel that boots
# and is unreadable.
#
# Only the app image differs. Bootloader, partition table and otadata are taken
# from the 5B build and shared: sdkconfig.waveshare-5b and
# sdkconfig.waveshare-7 are identical (the panel is chosen by build_flags in
# platformio.ini, not by sdkconfig), and partitions.csv is common, so those
# three are the same bytes either way.
BOARDS = [
    {
        "name": "ESP32-S3-Touch-LCD-5B",
        "description": "Waveshare 5\" 1024x600 panel (16MB flash, 8MB PSRAM). "
                       "Runs the light theme, because this panel flickers in "
                       "mid-greys.",
        "image": "%s-firmware" % MCU,
    },
    {
        # ONE image for both: they differ only in whether the glass has square
        # pixels, and this is the 7's build (ENV_800X480), so a 5 gets the 7's
        # 7.6% aspect correction. The 5 is untested hardware; revisit when one
        # is measured, and until then it beats a 1024x600 build.
        "name": "ESP32-S3-Touch-LCD-7 or -5",
        "description": "Waveshare 800x480 panel, 7\" or 5\" (16MB flash, 8MB "
                       "PSRAM). Runs the dark theme. Not the 5B, which is "
                       "1024x600.",
        "image": "%s-firmware-800x480" % MCU,
    },
    {
        "name": "Elecrow CrowPanel Advance 5.0-HMI",
        "description": "Elecrow 5\" 800x480 panel (16MB flash, 8MB PSRAM). "
                       "Runs the dark theme. Set the Function Select DIP to "
                       "1 1 (both ON) or the TF card will not be detected.",
        "image": "%s-firmware-crowpanel-adv-5" % MCU,
        # Own bootloader; see BOOTLOADER_CROWPANEL above.
        "bootloader": "%s-bootloader-crowpanel-adv-5" % MCU,
    },
]

# The first release that shipped an 800x480 image. Everything before it is
# genuinely single-board and can never be fixed -- the bytes were never built --
# so check_release.py holds those to its integrity rules only, not to BOARDS.
MULTI_BOARD_SINCE = (0, 1, 3)


def version_tuple(version):
    """(0, 1, 3) from "v0.1.3", "0.1.3" or "v0.1.3-rc1"; () if unparseable."""
    core = str(version).lstrip("v").split("-")[0]
    try:
        return tuple(int(part) for part in core.split("."))
    except ValueError:
        return ()


# Everything a fresh install writes except the app image, which is per board.
SHARED_IMAGES = ["%s-%s" % (MCU, name) for name, _, _ in IMAGES
                 if name != "firmware"]


def shared_images_for(board):
    """Everything a fresh install writes except the app image. Boards may
    override the bootloader -- the CrowPanel's differs because its console is
    UART0 rather than USB-Serial-JTAG."""
    if "bootloader" not in board:
        return SHARED_IMAGES
    return [board["bootloader"] if name.endswith("-bootloader") else name
            for name in SHARED_IMAGES]


def board_choices(board):
    """The two installation types for one panel, differing only in its app image."""
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
                "images": shared_images_for(board) + [board["image"]],
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
                "images": ["%s-otadata" % MCU, board["image"]],
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
