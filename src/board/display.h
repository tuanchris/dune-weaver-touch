#pragma once

#include "esp_err.h"
#include "lvgl.h"

// Brings up the RGB panel (double framebuffer in PSRAM, bounce buffers), the
// GT911 touch controller, and the esp_lvgl_port task. Backlight is left OFF so
// the first frame can be drawn before board_backlight(true).
esp_err_t display_init(void);

lv_display_t *display_lvgl_disp(void);
