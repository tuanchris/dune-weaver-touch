#include "screen_sleep.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "../net/settings.h"

// Not board.h: it drags in driver/i2c_master.h, which the desktop sim can't
// provide. The sim links a logging stub for this symbol instead.
extern esp_err_t board_backlight(bool on);

static const char *TAG = "screen_sleep";

#define CHECK_PERIOD_MS 1000

static bool s_asleep;
static lv_obj_t *s_shield;

// The shield is an opaque black cover on lv_layer_sys (above every page,
// dialog, and keyboard). While the backlight is off it costs nothing to
// show; on the wake touch it lights the panel at PRESSED but stays until
// RELEASED so the entire gesture is swallowed — GT911 wake touches can carry
// bogus coordinates, and a wake tap must never activate whatever happens to
// be under the finger.
static void shield_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        board_backlight(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_shield != NULL) {
            lv_obj_delete(s_shield);
            s_shield = NULL;
        }
        s_asleep = false;
        ESP_LOGI(TAG, "woken by touch (gesture swallowed)");
    }
}

static void sleep_now(void)
{
    s_asleep = true;
    s_shield = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_shield);
    lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_shield, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shield, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_shield, shield_event, LV_EVENT_ALL, NULL);
    board_backlight(false);
    ESP_LOGI(TAG, "screen sleeping after %us idle",
             (unsigned)settings_get()->screen_timeout_s);
}

static void check_timer(lv_timer_t *t)
{
    (void)t;
    if (s_asleep) {
        return;
    }
    uint32_t timeout_s = settings_get()->screen_timeout_s;  // live: chips apply now
    if (timeout_s == 0) {
        return;  // "Never"
    }
    if (lv_display_get_inactive_time(NULL) >= timeout_s * 1000u) {
        sleep_now();
    }
}

void screen_sleep_init(void)
{
    lv_timer_create(check_timer, CHECK_PERIOD_MS, NULL);
}
