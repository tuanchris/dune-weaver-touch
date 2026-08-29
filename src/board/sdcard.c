// TF card over SPI. Waveshare ESP32-S3-Touch-LCD-5B: MOSI=GPIO11, SCK=GPIO12,
// MISO=GPIO13, chip select on CH422G EXIO4 (docs.waveshare.com pin table).
// The expander can't toggle CS per SPI transaction, so EXIO4 is held low for
// the whole session and the sdspi device runs with no CS pin — the card is
// the only device on this bus, exactly how the Waveshare demo drives it.
#include "sdcard.h"

#include "board.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_SPI_HOST SPI2_HOST
#define SD_GPIO_MOSI 11
#define SD_GPIO_SCK 12
#define SD_GPIO_MISO 13

static sdmmc_card_t *s_card;
static bool s_bus_ready;

esp_err_t sdcard_mount(void)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    if (!s_bus_ready) {
        const spi_bus_config_t bus_cfg = {
            .mosi_io_num = SD_GPIO_MOSI,
            .miso_io_num = SD_GPIO_MISO,
            .sclk_io_num = SD_GPIO_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            // A preview tile is 83-180 KB; at 4096 that was 20-45 separate
            // DMA transactions per tile, all per-transaction overhead.
            .max_transfer_sz = 65536,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                            TAG, "spi bus");
        s_bus_ready = true;
    }

    ESP_RETURN_ON_ERROR(board_sd_select(true), TAG, "sd cs");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    // 20 MHz, NOT SDMMC_FREQ_HIGHSPEED (40). Measured 2026-08-28: at 40 MHz a
    // 32 GB card fails the high-speed switch itself —
    //   sdmmc_enable_hs_mode_and_check: send_csd returned 0x108
    //   sdmmc_card_init failed (0x108)  -> no TF card mounted
    // — while the SAME card at 20 MHz mounts cleanly and reads fine. A second
    // card tolerated 40. So HIGHSPEED works on some cards and hard-fails the
    // MOUNT on others, which is not a trade worth making for read speed on a
    // device whose cards are chosen by the user. SPI-mode data blocks carry a
    // CRC16 the driver verifies, so a marginal bus shows up as a loud read
    // error (missing previews), never as silent garbage.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = GPIO_NUM_NC;  // CS is the CH422G line, held low above

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  // a bad card is the user's to fix
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        // No card / unreadable card: release CS and report; callers degrade.
        board_sd_select(false);
        s_card = NULL;
        ESP_LOGW(TAG, "no TF card mounted (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "TF card mounted at %s: %llu MB",
             SDCARD_MOUNT,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

bool sdcard_mounted(void)
{
    return s_card != NULL;
}

esp_err_t sdcard_remount(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT, s_card);
        s_card = NULL;
        board_sd_select(false);
    }
    return sdcard_mount();
}
