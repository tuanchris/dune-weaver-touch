#include "app/jobs.h"
#include "app/state.h"
#include "board/board.h"
#include "board/display.h"
#include "board/sdcard.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "net/fw_client.h"
#include "net/settings.h"
#include "net/wifi.h"
#include "render/thr_preview.h"
#include "ui/theme.h"
#include "ui/ui.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());  // before the panel: NVS does flash ops
    ESP_ERROR_CHECK(board_init());
    // Before ui_init: the Browse page checks for the pattern card at build
    // time (manifest + previews live on the panel's local TF card).
    if (sdcard_mount() != ESP_OK) {
        ESP_LOGW(TAG, "no pattern TF card - Browse will nag for one");
    }
    ESP_ERROR_CHECK(display_init());

    lvgl_port_lock(0);
    theme_init();
    theme_set_dark(settings_get()->dark_mode);
    ui_init();
    lvgl_port_unlock();

    // First frame is built; light the panel
    ESP_ERROR_CHECK(board_backlight(true));

    ESP_ERROR_CHECK(jobs_init());
    ESP_ERROR_CHECK(fw_client_init());
    if (thr_preview_init() != ESP_OK) {
        ESP_LOGW(TAG, "preview cache unavailable (storage partition)");
    }
    ESP_ERROR_CHECK(wifi_init());
    state_init();

    ESP_LOGI(TAG, "dune-weaver-touch up");
}
