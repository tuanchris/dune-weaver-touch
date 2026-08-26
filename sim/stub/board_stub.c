// Desktop stub for the board symbols UI-layer code links against.
// The SDL window has no backlight; the shield object (opaque black cover
// from screen_sleep.c) IS the visible "screen off" state in the sim.
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "board_stub";

esp_err_t board_backlight(bool on)
{
    ESP_LOGI(TAG, "backlight %s", on ? "ON" : "OFF");
    return ESP_OK;
}
