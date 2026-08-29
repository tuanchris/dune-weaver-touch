"""Pre-build hook: make ESP-IDF tolerate a CRC error on the SDIO probe (CMD52).

Why this exists (espressif/esp-idf#14000, IDFGH-13055). The SD spec says CRC
must be OFF in SPI mode for everything but CMD0/CMD8, but IDF turns it ON
during init (sdmmc_sd.c -> sdmmc_send_cmd_crc_on_off(card, true)). The card
KEEPS that state across an ESP32 reset, because a soft reset does not cut card
power. The next init then sends CMD52 with CRC checking live, the card answers
"command CRC error", and sdmmc_card_init aborts:

    sdspi_transaction: cmd=52, R1 response: command CRC error
    sdmmc_io: sdmmc_io_reset: unexpected return: 0x109
    vfs_fat_sdmmc: sdmmc_card_init failed (0x109)
    sdcard: no TF card mounted (ESP_ERR_INVALID_CRC)

The card is fine; it never even reaches CMD0, so nothing about the filesystem,
the partition map or the card's contents can affect it. The product-level bite
is that ANY reboot that does not power-cycle the card can lose the card until
it is physically unplugged -- including the reboot after an OTA update.

Upstream fixed it by tolerating ESP_ERR_INVALID_CRC here. That fix is in IDF
v5.3+, v5.4, v5.5.1 and v5.5.2, but NOT in 5.5.0. platformio.ini pins
framework-espidf 3.50502.0 (IDF 5.5.2), so this script is normally a NO-OP --
it exists so a machine that resolves an older framework still gets a working
SD card rather than a silent, confusing regression.

Safe to delete once nothing can resolve an IDF below 5.5.1.
"""

import os

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

RELPATH = os.path.join("components", "sdmmc", "sdmmc_io.c")

# The 5.5.0 tolerance condition, and the upstream replacement.
OLD = ("    if (err == ESP_ERR_TIMEOUT || "
       "(host_is_spi(card) && err == ESP_ERR_NOT_SUPPORTED)) {")
NEW = ("    if (err == ESP_ERR_TIMEOUT || "
       "(host_is_spi(card) && err == ESP_ERR_NOT_SUPPORTED) || "
       "err == ESP_ERR_INVALID_CRC) {")

MARKER = "ESP_ERR_INVALID_CRC"


def main():
    try:
        fw = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
    except Exception as exc:  # framework not installed yet / different layout
        print("sdmmc patch: cannot locate framework-espidf (%s), skipping" % exc)
        return

    if not fw:
        print("sdmmc patch: framework-espidf not resolved, skipping")
        return

    path = os.path.join(fw, RELPATH)
    if not os.path.isfile(path):
        print("sdmmc patch: %s not found, skipping" % path)
        return

    with open(path, "r", encoding="utf-8") as fh:
        src = fh.read()

    # Already fixed (IDF >= 5.5.1, or a previous run of this script).
    if MARKER in src:
        return

    if OLD not in src:
        # Unknown IDF layout: say so loudly rather than silently shipping a
        # build whose SD card dies on the first OTA reboot.
        print("sdmmc patch: WARNING - tolerance condition not found in %s. "
              "SD may fail to mount after a reset without power cycle "
              "(esp-idf#14000). Check the IDF version." % path)
        return

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src.replace(OLD, NEW, 1))
    print("sdmmc patch: applied CMD52 CRC tolerance to %s (esp-idf#14000)" % path)


main()
