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

// Backlight off does NOT stop the panel on this board. EXIO2 gates only the
// AP3032 backlight boost; the LCD's own DISP pin is tied high through R30
// (Waveshare ESP32-S3-Touch-LCD-5 schematic — the 4.3 shares that net, this
// board does not), and the RGB peripheral keeps scanning regardless. So a
// motionless shield means one identical frame driven into the glass for as
// long as the table idles, and wherever the PCB runs warm the ions in the
// cell settle into that frame: a blotch invisible on white and on a saturated
// primary, plain at mid grey, flickering there, clearing only after hours
// unpowered. Alternating the field cancels the bias instead of building it.
#define REPOLARIZE_PERIOD_S 30

// The field can be white for a whole REPOLARIZE_PERIOD_S and a wake tap can
// land in the middle of it, so the panel stays dark until black is on the
// GLASS — not merely handed to the driver. LVGL sends REFR_READY once the
// flush callback returns, but this display runs two PSRAM framebuffers with
// avoid_tearing (display.c), so the buffer flush accepted is not necessarily
// the one the RGB peripheral is scanning yet. Counting frames covers the swap;
// at 24 Hz this is ~125 ms of extra wake latency and it is not perceptible.
#define WAKE_BLACK_FRAMES 3

static bool s_asleep;
static lv_obj_t *s_shield;
static uint32_t s_asleep_s;
static bool s_shield_light;
static uint8_t s_wake_frames;

// Registered for the life of the app, idle unless a wake is counting down.
static void refr_ready(lv_event_t *e)
{
    (void)e;
    if (s_wake_frames > 0 && --s_wake_frames == 0) {
        board_backlight(true);
        ESP_LOGI(TAG, "backlight on (black confirmed on glass)");
    }
}

// The shield is an opaque cover on lv_layer_sys (above every page, dialog, and
// keyboard). On the wake touch it holds until RELEASED so the entire gesture is
// swallowed — GT911 wake touches can carry bogus coordinates, and a wake tap
// must never activate whatever happens to be under the finger.
static void shield_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "wake tap (field was %s)", s_shield_light ? "WHITE" : "black");
        lv_obj_set_style_bg_color(s_shield, lv_color_black(), 0);
        lv_obj_invalidate(s_shield);
        s_shield_light = false;
        s_wake_frames = WAKE_BLACK_FRAMES;
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
    s_asleep_s = 0;
    s_shield_light = false;
    s_wake_frames = 0;
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
        if (++s_asleep_s % REPOLARIZE_PERIOD_S == 0 && s_shield != NULL) {
            s_shield_light = !s_shield_light;
            lv_obj_set_style_bg_color(
                s_shield, s_shield_light ? lv_color_white() : lv_color_black(), 0);
            ESP_LOGI(TAG, "repolarize -> %s", s_shield_light ? "WHITE" : "black");
        }
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
    lv_display_add_event_cb(lv_display_get_default(), refr_ready, LV_EVENT_REFR_READY, NULL);
}
