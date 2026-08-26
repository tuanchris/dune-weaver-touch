#include "app/jobs.h"
#include "app/state.h"
#include "board/board.h"
#include "board/display.h"
#include "board/sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "net/fw_client.h"
#include "net/settings.h"
#include "net/wifi.h"
#include "render/thr_preview.h"
#include "ui/theme.h"
#include "ui/ui.h"

static const char *TAG = "app";

// Internal RAM is this device's scarcest resource (a starved WiFi driver
// storms "wifi:m f null" at beacon rate) — keep a heartbeat of the numbers
// on the console so exhaustion is visible before it bites.
#define HEAP_REPORT_PERIOD_US (10 * 1000 * 1000)

static void heap_report_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "heap: internal free=%u largest=%u | psram free=%u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

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
        ESP_LOGW(TAG, "preview cache unavailable");
    }
    ESP_ERROR_CHECK(wifi_init());
    state_init();

    const esp_timer_create_args_t heap_args = {
        .callback = heap_report_cb,
        .name = "heap_report",
    };
    esp_timer_handle_t heap_timer = NULL;
    if (esp_timer_create(&heap_args, &heap_timer) == ESP_OK) {
        esp_timer_start_periodic(heap_timer, HEAP_REPORT_PERIOD_US);
    }

    ESP_LOGI(TAG, "dune-weaver-touch up");
}
