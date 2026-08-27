// Light page: power + brightness, per-effect appearance, the full effect
// catalogue and the firmware-native ball tracker. Mirrors LedControlPage.qml;
// the catalogue, per-effect inputs and translation rules are
// docs/PORTING_NOTES.md §4 (UI 0-100 <-> fw 0-255 brightness, colors without
// '#', power/ball effect memory, live /sand_status.led wins over NVS).
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "../../app/jobs.h"
#include "../../app/state.h"
#include "../../net/fw_client.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "page_light";

// ---------------------------------------------------------------------------
// Catalogue (index = firmware effect id, PORTING_NOTES §4 — do not reorder)
// ---------------------------------------------------------------------------

static const char *const EFFECTS[] = {
    "off",       "static",    "rainbow",   "breathe",  "colorloop",
    "theater",   "scan",      "running",   "sine",     "gradient",
    "sinelon",   "twinkle",   "sparkle",   "fire",     "candle",
    "meteor",    "bouncing",  "wipe",      "dualscan", "juggle",
    "multicomet", "glitter",  "dissolve",  "ripple",   "drip",
    "lightning", "fireworks", "plasma",    "heartbeat", "strobe",
    "police",    "chase",     "railway",   "pacifica", "aurora",
    "pride",     "colorwaves", "bpm",      "ball",
};
#define EFFECT_COUNT ((int)(sizeof(EFFECTS) / sizeof(EFFECTS[0])))
#define BALL_ID 38
#define RAINBOW_ID 2

// Display labels: capitalized, with the reference app's multi-word specials.
static const char *const EFFECT_LABELS[] = {
    "Off",       "Static",    "Rainbow",   "Breathe",  "Color loop",
    "Theater",   "Scan",      "Running",   "Sine",     "Gradient",
    "Sinelon",   "Twinkle",   "Sparkle",   "Fire",     "Candle",
    "Meteor",    "Bouncing",  "Wipe",      "Dual scan", "Juggle",
    "Multi-comet", "Glitter", "Dissolve",  "Ripple",   "Drip",
    "Lightning", "Fireworks", "Plasma",    "Heartbeat", "Strobe",
    "Police",    "Chase",     "Railway",   "Pacifica", "Aurora",
    "Pride",     "Color waves", "BPM",     "Ball",
};

static const char *const PALETTES[] = {
    "rainbow", "ocean", "lava", "forest", "party", "cloud", "heat", "sunset",
};
static const char *const PALETTE_LABELS[] = {
    "Rainbow", "Ocean", "Lava", "Forest", "Party", "Cloud", "Heat", "Sunset",
};
#define PALETTE_COUNT ((int)(sizeof(PALETTES) / sizeof(PALETTES[0])))

// Which inputs each effect actually uses (EFFECT_INPUTS table in the
// reference app). Speed is separate: every effect except off/static.
#define IN_COLOR 0x1
#define IN_COLOR2 0x2
#define IN_PALETTE 0x4
static const uint8_t EFFECT_INPUTS[EFFECT_COUNT] = {
    [0] = 0,                       // off
    [1] = IN_COLOR,                // static
    [2] = IN_PALETTE,              // rainbow
    [3] = IN_COLOR,                // breathe
    [4] = IN_PALETTE,              // colorloop
    [5] = IN_COLOR,                // theater
    [6] = IN_COLOR,                // scan
    [7] = IN_COLOR,                // running
    [8] = IN_COLOR,                // sine
    [9] = IN_COLOR | IN_COLOR2,    // gradient
    [10] = IN_PALETTE,             // sinelon
    [11] = IN_PALETTE,             // twinkle
    [12] = IN_COLOR,               // sparkle
    [13] = IN_PALETTE,             // fire
    [14] = IN_COLOR,               // candle
    [15] = IN_COLOR,               // meteor
    [16] = IN_COLOR,               // bouncing
    [17] = IN_COLOR | IN_COLOR2,   // wipe
    [18] = IN_COLOR | IN_COLOR2,   // dualscan
    [19] = IN_PALETTE,             // juggle
    [20] = IN_PALETTE,             // multicomet
    [21] = IN_PALETTE,             // glitter
    [22] = IN_COLOR | IN_COLOR2,   // dissolve
    [23] = IN_PALETTE,             // ripple
    [24] = IN_COLOR,               // drip
    [25] = 0,                      // lightning (speed only)
    [26] = IN_PALETTE,             // fireworks
    [27] = IN_PALETTE,             // plasma
    [28] = IN_COLOR,               // heartbeat
    [29] = IN_COLOR,               // strobe
    [30] = 0,                      // police (speed only)
    [31] = IN_COLOR | IN_COLOR2,   // chase
    [32] = IN_COLOR | IN_COLOR2,   // railway
    [33] = 0,                      // pacifica (speed only)
    [34] = 0,                      // aurora (speed only)
    [35] = 0,                      // pride (speed only)
    [36] = IN_PALETTE,             // colorwaves
    [37] = IN_PALETTE,             // bpm
    [38] = IN_COLOR | IN_COLOR2,   // ball
};

// Preset swatches: muted UI hex shown, full-saturation hex sent (no '#').
typedef struct {
    uint32_t swatch;   // displayed
    const char *send;  // sent to the firmware
} preset_color_t;

static const preset_color_t PRESETS[] = {
    {0xe8e8e8, "ffffff"},  // White
    {0xd4a574, "ffaa55"},  // Warm
    {0xc45c5c, "ff0000"},  // Red
    {0xd4875c, "ff8800"},  // Orange
    {0xc9b95c, "ffff00"},  // Yellow
    {0x5cb85c, "00ff00"},  // Green
    {0x5cb8b8, "00ffff"},  // Cyan
    {0x5c7cc4, "0000ff"},  // Blue
    {0x8b5cc4, "8800ff"},  // Purple
    {0xc45c99, "ff00ff"},  // Pink
};
#define PRESET_COUNT ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))
#define SWATCH_SECONDARY 0x100

static const char *const DIR_LABELS[2] = {"Clockwise", "Counter-CW"};
static const char *const DIR_VALUES[2] = {"cw", "ccw"};

// 3-column chip grids (grid dsc arrays must outlive the widgets)
#define GC LV_GRID_CONTENT
static const int32_t GRID_COLS3[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                     LV_GRID_TEMPLATE_LAST};
static const int32_t GRID_ROWS13[] = {GC, GC, GC, GC, GC, GC, GC, GC, GC, GC, GC, GC, GC,
                                      LV_GRID_TEMPLATE_LAST};
static const int32_t GRID_ROWS3[] = {GC, GC, GC, LV_GRID_TEMPLATE_LAST};
#undef GC

// ---------------------------------------------------------------------------
// Page state (mutated only in the LVGL task or under lvgl_port_lock)
// ---------------------------------------------------------------------------

#define EFFECT_NONE (-1)     // nothing known yet — behaves like "off"
#define EFFECT_UNKNOWN (-2)  // firmware reported a name outside the catalogue

static int s_cur_effect = EFFECT_NONE;
static int s_last_effect = RAINBOW_ID;    // restored on power-on
static int s_last_non_ball = RAINBOW_ID;  // restored when the ball is toggled off
static int s_cur_palette = 0;
static int s_brightness = 100;  // UI 0-100
static int s_speed = 128;
static int s_fgbright = 255;
static int s_bgbright = 255;
static int s_ball_size = 3;
static int s_ball_align = 0;
static char s_ball_bg[24] = "static";
static char s_direction[4] = "cw";
static char s_color[8] = "ffffff";
static char s_color2[8] = "000000";

static bool s_has_ring = false;
static bool s_cfg_requested = false;
static bool s_live_effect_seen = false;
static bool s_live_bright_seen = false;

// Widgets
static lv_obj_t *s_notice;
static lv_obj_t *s_power_row, *s_power_label, *s_power_switch;
static lv_obj_t *s_bright_row;
static lv_obj_t *s_card_appear;
static lv_obj_t *s_color_block, *s_color_caption;
static lv_obj_t *s_color2_block;
static lv_obj_t *s_palette_block;
static lv_obj_t *s_speed_row;
static lv_obj_t *s_right_col;
static lv_obj_t *s_ball_switch, *s_ball_hint, *s_ball_controls;
static lv_obj_t *s_bg_color_row, *s_bg_bright_row;
static lv_obj_t *s_effect_chips[EFFECT_COUNT];  // [BALL_ID] stays NULL
static lv_obj_t *s_palette_chips[PALETTE_COUNT];
static lv_obj_t *s_dir_chips[2];
static lv_obj_t *s_bg_chips[EFFECT_COUNT + 1];
static const char *s_bg_values[EFFECT_COUNT + 1];
static int s_bg_count = 0;
static lv_obj_t *s_sw_primary[PRESET_COUNT];
static lv_obj_t *s_sw_secondary[PRESET_COUNT];
static lv_obj_t *s_sw_dot[PRESET_COUNT];
static lv_obj_t *s_sw_bg[PRESET_COUNT];

// Slider binding: live label on drag, one fw_led() submit on release.
typedef struct {
    const char *param;  // fw_led query key
    int *store;
    lv_obj_t *slider;
    lv_obj_t *value_label;
    const char *fmt;  // for the value label
    int step;
    bool to_255;  // UI 0-100 -> fw 0-255
} slider_ctl_t;

static slider_ctl_t s_ctl_bright = {.param = "brightness", .store = &s_brightness,
                                    .fmt = "%d%%", .step = 5, .to_255 = true};
static slider_ctl_t s_ctl_speed = {.param = "speed", .store = &s_speed, .fmt = "%d", .step = 1};
static slider_ctl_t s_ctl_fg = {.param = "fgbright", .store = &s_fgbright, .fmt = "%d", .step = 1};
static slider_ctl_t s_ctl_size = {.param = "size", .store = &s_ball_size, .fmt = "%d", .step = 1};
static slider_ctl_t s_ctl_align = {.param = "align", .store = &s_ball_align, .fmt = "%d°", .step = 1};
static slider_ctl_t s_ctl_bgb = {.param = "bgbright", .store = &s_bgbright, .fmt = "%d", .step = 1};

static void refresh_all(void);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    // LVGL makes every object scrollable by default; nothing in this UI is
    // dragged (ui_page_stepper re-enables the ones it drives). See ui.h.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static int effect_id_by_name(const char *name)
{
    for (int i = 0; i < EFFECT_COUNT; i++) {
        if (strcmp(EFFECTS[i], name) == 0) {
            return i;
        }
    }
    return EFFECT_UNKNOWN;
}

static uint8_t current_inputs(void)
{
    if (s_cur_effect == EFFECT_UNKNOWN) {
        return IN_COLOR | IN_COLOR2 | IN_PALETTE;  // unknown effect: show everything
    }
    if (s_cur_effect < 0 || s_cur_effect >= EFFECT_COUNT) {
        return 0;
    }
    return EFFECT_INPUTS[s_cur_effect];
}

static bool speed_shown(void)
{
    if (s_cur_effect == EFFECT_UNKNOWN) {
        return true;
    }
    return s_cur_effect > 1 && s_cur_effect < EFFECT_COUNT;  // not off/static
}

// Effect-memory bookkeeping shared by every path that changes the effect.
static void apply_effect_local(int id)
{
    if (id > 0) {
        s_last_effect = id;
    }
    if (id > 0 && id != BALL_ID) {
        s_last_non_ball = id;
    }
    s_cur_effect = id;
}

static int clamp_setting_int(const char *s, int def, int lo, int hi)
{
    if (s == NULL || s[0] == '\0') {
        return def;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) {
        return def;
    }
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return (int)v;
}

// ---------------------------------------------------------------------------
// Background work (jobs task)
// ---------------------------------------------------------------------------

typedef struct {
    char query[96];
} led_job_ctx_t;

static void led_job(void *arg)
{
    led_job_ctx_t *ctx = arg;
    esp_err_t err = fw_led(ctx->query);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fw_led(%s) failed: 0x%x", ctx->query, err);
        char msg[192];
        snprintf(msg, sizeof(msg), "Couldn't update the light. %s", fw_friendly_error(err));
        lvgl_port_lock(0);
        ui_show_error(msg);
        lvgl_port_unlock();
    } else {
        state_poll_now();  // reconcile live effect/brightness quickly
    }
    free(ctx);
}

// Called from LVGL event callbacks only: never blocks, queues the HTTP write.
static void submit_led(const char *fmt, ...)
{
    led_job_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        ui_show_error("Couldn't update the light. Out of memory - try again.");
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->query, sizeof(ctx->query), fmt, ap);
    va_end(ap);
    if (jobs_submit(led_job, ctx) != ESP_OK) {
        free(ctx);
        ui_show_error("Couldn't update the light. Too many pending commands - try again.");
    }
}

// On connect: seed everything the status poll doesn't carry from the board's
// $LED/* NVS (already cached by fw_load_settings on the connect edge). Live
// status values win for effect/brightness.
static void led_config_job(void *arg)
{
    (void)arg;
    char v[40];

    int effect = 0;  // absent key -> "off", like the reference backend
    if (fw_setting("LED/Effect", v, sizeof(v))) {
        effect = effect_id_by_name(v);  // EFFECT_UNKNOWN -> leave current selection
    }
    int palette = 0;  // absent -> "rainbow"
    if (fw_setting("LED/Palette", v, sizeof(v))) {
        palette = -1;
        for (int i = 0; i < PALETTE_COUNT; i++) {
            if (strcmp(PALETTES[i], v) == 0) {
                palette = i;
                break;
            }
        }
    }
    int brightness_ui = 100;
    if (fw_setting("LED/Brightness", v, sizeof(v))) {
        int raw = clamp_setting_int(v, 255, 0, 255);
        brightness_ui = (raw * 100 + 127) / 255;
    }
    int speed = 128;
    if (fw_setting("LED/Speed", v, sizeof(v))) {
        speed = clamp_setting_int(v, 128, 1, 255);
    }
    char color[8] = "ffffff";
    if (fw_setting("LED/Color", v, sizeof(v))) {
        strlcpy(color, v[0] == '#' ? v + 1 : v, sizeof(color));
    }
    char color2[8] = "000000";
    if (fw_setting("LED/Color2", v, sizeof(v))) {
        strlcpy(color2, v[0] == '#' ? v + 1 : v, sizeof(color2));
    }
    int fgbright = 255;
    if (fw_setting("LED/BallBright", v, sizeof(v))) {
        fgbright = clamp_setting_int(v, 255, 0, 255);
    }
    int bgbright = 255;
    if (fw_setting("LED/BallBgBright", v, sizeof(v))) {
        bgbright = clamp_setting_int(v, 255, 0, 255);
    }
    int size = 3;  // fw clamps 1-200; the UI slider runs 1-30
    if (fw_setting("LED/BallSize", v, sizeof(v))) {
        size = clamp_setting_int(v, 3, 1, 30);
    }
    int align = 0;
    if (fw_setting("LED/Align", v, sizeof(v))) {
        align = clamp_setting_int(v, 0, 0, 359);
    }
    char bg[24] = "static";
    if (fw_setting("LED/BallBg", v, sizeof(v)) && v[0] != '\0') {
        for (char *p = v; *p != '\0'; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        strlcpy(bg, v, sizeof(bg));
    }
    char dir[4] = "cw";
    if (fw_setting("LED/Direction", v, sizeof(v))) {
        for (char *p = v; *p != '\0'; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        if (strcmp(v, "ccw") == 0) {
            strlcpy(dir, "ccw", sizeof(dir));
        }
    }

    lvgl_port_lock(0);
    if (effect >= 0) {
        if (!s_live_effect_seen) {
            s_cur_effect = effect;
        }
        if (effect > 0) {
            s_last_effect = effect;
        }
        if (effect > 0 && effect != BALL_ID) {
            s_last_non_ball = effect;
        }
    }
    if (palette >= 0) {
        s_cur_palette = palette;
    }
    if (!s_live_bright_seen) {
        s_brightness = brightness_ui;
    }
    s_speed = speed;
    s_fgbright = fgbright;
    s_bgbright = bgbright;
    s_ball_size = size;
    s_ball_align = align;
    strlcpy(s_color, color, sizeof(s_color));
    strlcpy(s_color2, color2, sizeof(s_color2));
    strlcpy(s_ball_bg, bg, sizeof(s_ball_bg));
    strlcpy(s_direction, dir, sizeof(s_direction));
    refresh_all();
    lvgl_port_unlock();
}

// ---------------------------------------------------------------------------
// Widget recipes (DwSwitch / DwSlider / ChoiceChip / SettingsCard ports)
// ---------------------------------------------------------------------------

static lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = plain(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border_light, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_LG, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, TH_SPACE_MD, 0);
    return card;
}

static lv_obj_t *make_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_color(l, th.text3, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

static lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, th.text2, 0);
    return l;
}

// DwSwitch: 84x48 pill; on = accent fill + on_accent knob.
static lv_obj_t *make_switch(lv_obj_t *parent)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 84, 48);
    lv_obj_set_style_bg_color(sw, th.pressed, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, th.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, th.accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(sw, th.accent_pressed, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, th.text2, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, th.on_accent, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(sw, -6, LV_PART_KNOB);  // 36px knob in the 48px pill
    return sw;
}

// DwSlider: pressed-color track, accent indicator, 42px accent knob.
static lv_obj_t *make_slider(lv_obj_t *parent, int min, int max, int value)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_height(s, 12);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, th.pressed, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, th.accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, th.accent, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 15, LV_PART_KNOB);  // 12 + 2*15 = 42px knob
    lv_obj_set_style_bg_color(s, th.accent_pressed, LV_PART_KNOB | LV_STATE_PRESSED);
    return s;
}

static void slider_changed(lv_event_t *e)
{
    slider_ctl_t *ctl = lv_event_get_user_data(e);
    lv_obj_t *slider = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(slider);
    if (ctl->step > 1) {
        int q = ((v + ctl->step / 2) / ctl->step) * ctl->step;
        if (q != v) {
            lv_slider_set_value(slider, q, LV_ANIM_OFF);
            v = q;
        }
    }
    lv_label_set_text_fmt(ctl->value_label, ctl->fmt, v);
}

static void slider_released(lv_event_t *e)
{
    slider_ctl_t *ctl = lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (ctl->step > 1) {
        v = ((v + ctl->step / 2) / ctl->step) * ctl->step;
    }
    *ctl->store = v;
    submit_led("%s=%d", ctl->param, ctl->to_255 ? (v * 255 + 50) / 100 : v);
}

static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *name, int name_width,
                                 int min, int max, slider_ctl_t *ctl)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TH_SPACE_MD, 0);

    lv_obj_t *name_label = make_caption(row, name);
    lv_obj_set_width(name_label, name_width);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    lv_obj_t *slider = make_slider(row, min, max, *ctl->store);
    lv_obj_set_flex_grow(slider, 1);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, 66);
    lv_obj_set_style_text_font(value, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(value, th.text, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(value, ctl->fmt, *ctl->store);

    ctl->slider = slider;
    ctl->value_label = value;
    lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, ctl);
    lv_obj_add_event_cb(slider, slider_released, LV_EVENT_RELEASED, ctl);
    return row;
}

// ChoiceChip: pill outline, accent-soft fill when selected.
static lv_obj_t *make_chip(lv_obj_t *parent, const char *text)
{
    lv_obj_t *chip = plain(parent);
    lv_obj_set_height(chip, 66);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, th.border, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(chip, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(chip, TH_SPACE_SM, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(chip);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, th.text2, 0);
    lv_obj_center(l);
    return chip;
}

static void chip_set_selected(lv_obj_t *chip, bool selected)
{
    lv_obj_set_style_border_color(chip, selected ? th.accent : th.border, 0);
    lv_obj_set_style_bg_color(chip, selected ? th.accent_soft : th.surface, 0);
    lv_obj_set_style_bg_opa(chip, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), selected ? th.accent : th.text2, 0);
}

static lv_obj_t *make_grid3(lv_obj_t *parent, const int32_t *rows_dsc)
{
    lv_obj_t *grid = plain(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, GRID_COLS3, rows_dsc);
    lv_obj_set_style_pad_row(grid, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_column(grid, TH_SPACE_SM, 0);
    return grid;
}

// ---------------------------------------------------------------------------
// Event callbacks (LVGL task; submit jobs, never block)
// ---------------------------------------------------------------------------

static void power_toggled(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (on) {
        int id = s_last_effect;
        if (id <= 0 || id >= EFFECT_COUNT) {
            id = RAINBOW_ID;
        }
        apply_effect_local(id);
        submit_led("effect=%s", EFFECTS[id]);
    } else {
        apply_effect_local(0);
        submit_led("effect=off");
    }
    refresh_all();
}

static void ball_toggled(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (on) {
        apply_effect_local(BALL_ID);
        submit_led("effect=ball");
    } else {
        int id = s_last_non_ball;
        if (id <= 0 || id >= EFFECT_COUNT || id == BALL_ID) {
            id = RAINBOW_ID;
        }
        apply_effect_local(id);
        submit_led("effect=%s", EFFECTS[id]);
    }
    refresh_all();
}

static void effect_chip_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    apply_effect_local(id);
    submit_led("effect=%s", EFFECTS[id]);
    refresh_all();
}

static void palette_chip_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    s_cur_palette = id;
    submit_led("palette=%s", PALETTES[id]);
    refresh_all();
}

static void dir_chip_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_direction, DIR_VALUES[idx], sizeof(s_direction));
    submit_led("direction=%s", DIR_VALUES[idx]);
    refresh_all();
}

static void bg_chip_clicked(lv_event_t *e)
{
    const char *value = lv_event_get_user_data(e);
    strlcpy(s_ball_bg, value, sizeof(s_ball_bg));
    submit_led("bg=%s", value);
    refresh_all();
}

static void swatch_clicked(lv_event_t *e)
{
    int packed = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = packed & 0xff;
    if (packed & SWATCH_SECONDARY) {
        strlcpy(s_color2, PRESETS[idx].send, sizeof(s_color2));
        submit_led("color2=%s", PRESETS[idx].send);
    } else {
        strlcpy(s_color, PRESETS[idx].send, sizeof(s_color));
        submit_led("color=%s", PRESETS[idx].send);
    }
    refresh_all();
}

// Preset swatches: display the muted hex, send the saturated hex.
static lv_obj_t *make_swatch_row(lv_obj_t *parent, bool secondary, lv_obj_t *out[])
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(row, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_column(row, TH_SPACE_SM, 0);
    for (int i = 0; i < PRESET_COUNT; i++) {
        lv_obj_t *sw = plain(row);
        lv_obj_set_size(sw, 72, 72);
        lv_obj_set_style_radius(sw, TH_RADIUS_SM, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(PRESETS[i].swatch), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sw, 1, 0);
        lv_obj_set_style_border_color(sw, th.border, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, swatch_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i | (secondary ? SWATCH_SECONDARY : 0)));
        out[i] = sw;
    }
    return row;
}

// ---------------------------------------------------------------------------
// Refresh (LVGL task or under lvgl_port_lock)
// ---------------------------------------------------------------------------

static void sync_slider(slider_ctl_t *ctl, int value)
{
    if (ctl->slider == NULL || lv_obj_has_state(ctl->slider, LV_STATE_PRESSED)) {
        return;  // don't fight an active drag
    }
    lv_slider_set_value(ctl->slider, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(ctl->value_label, ctl->fmt, value);
}

static void refresh_swatch_row(lv_obj_t *const row[], const char *current)
{
    for (int i = 0; i < PRESET_COUNT; i++) {
        bool sel = strcasecmp(PRESETS[i].send, current) == 0;
        lv_obj_set_style_border_width(row[i], sel ? 3 : 1, 0);
        lv_obj_set_style_border_color(row[i], sel ? th.accent : th.border, 0);
    }
}

// Skip the (frequent) no-op refreshes from the 1 Hz status listener.
typedef struct {
    int effect, palette, brightness, speed, fgbright, bgbright, size, align;
    bool ring;
    char bg[24];
    char dir[4];
    char color[8];
    char color2[8];
} ui_snapshot_t;

static ui_snapshot_t s_shown = {.effect = INT_MIN};

static void refresh_all(void)
{
    ui_snapshot_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.effect = s_cur_effect;
    cur.palette = s_cur_palette;
    cur.brightness = s_brightness;
    cur.speed = s_speed;
    cur.fgbright = s_fgbright;
    cur.bgbright = s_bgbright;
    cur.size = s_ball_size;
    cur.align = s_ball_align;
    cur.ring = s_has_ring;
    strlcpy(cur.bg, s_ball_bg, sizeof(cur.bg));
    strlcpy(cur.dir, s_direction, sizeof(cur.dir));
    strlcpy(cur.color, s_color, sizeof(cur.color));
    strlcpy(cur.color2, s_color2, sizeof(cur.color2));
    if (memcmp(&cur, &s_shown, sizeof(cur)) == 0) {
        return;
    }
    s_shown = cur;

    bool ring = s_has_ring;
    bool ball = (s_cur_effect == BALL_ID);
    bool power = (s_cur_effect != 0 && s_cur_effect != EFFECT_NONE);
    uint8_t inputs = current_inputs();
    bool speed_ok = speed_shown();

    // Table light card
    set_hidden(s_notice, ring);
    set_hidden(s_power_row, !ring);
    set_hidden(s_bright_row, !ring);
    lv_label_set_text(s_power_label, power ? "On" : "Off");
    if (power) {
        lv_obj_add_state(s_power_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_power_switch, LV_STATE_CHECKED);
    }
    sync_slider(&s_ctl_bright, s_brightness);

    // Appearance card (hidden while the ball owns the ring)
    set_hidden(s_card_appear, !(ring && !ball && (inputs != 0 || speed_ok)));
    set_hidden(s_color_block, (inputs & IN_COLOR) == 0);
    set_hidden(s_color2_block, (inputs & IN_COLOR2) == 0);
    set_hidden(s_palette_block, (inputs & IN_PALETTE) == 0);
    set_hidden(s_speed_row, !speed_ok);
    lv_label_set_text(s_color_caption, (inputs & IN_COLOR2) ? "Primary" : "Colour");
    refresh_swatch_row(s_sw_primary, s_color);
    refresh_swatch_row(s_sw_secondary, s_color2);
    for (int i = 0; i < PALETTE_COUNT; i++) {
        chip_set_selected(s_palette_chips[i], i == s_cur_palette);
    }
    sync_slider(&s_ctl_speed, s_speed);

    // Right column: effect catalogue + ball tracker
    set_hidden(s_right_col, !ring);
    for (int i = 0; i < EFFECT_COUNT; i++) {
        if (s_effect_chips[i] != NULL) {
            chip_set_selected(s_effect_chips[i], i == s_cur_effect);
        }
    }
    if (ball) {
        lv_obj_add_state(s_ball_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_ball_switch, LV_STATE_CHECKED);
    }
    lv_label_set_text(s_ball_hint, ball ? "Following the sand ball - replaces the effect above."
                                        : "A glowing dot that follows the sand ball.");
    set_hidden(s_ball_controls, !ball);
    refresh_swatch_row(s_sw_dot, s_color);
    sync_slider(&s_ctl_fg, s_fgbright);
    for (int i = 0; i < 2; i++) {
        chip_set_selected(s_dir_chips[i], strcmp(DIR_VALUES[i], s_direction) == 0);
    }
    sync_slider(&s_ctl_size, s_ball_size);
    sync_slider(&s_ctl_align, s_ball_align);
    for (int i = 0; i < s_bg_count; i++) {
        chip_set_selected(s_bg_chips[i], strcmp(s_bg_values[i], s_ball_bg) == 0);
    }
    set_hidden(s_bg_color_row, strcmp(s_ball_bg, "static") != 0);
    refresh_swatch_row(s_sw_bg, s_color2);
    set_hidden(s_bg_bright_row, strcmp(s_ball_bg, "off") == 0);
    sync_slider(&s_ctl_bgb, s_bgbright);
}

// ---------------------------------------------------------------------------
// State listener (runs in the LVGL task; state module holds lvgl_port_lock)
// ---------------------------------------------------------------------------

static void on_state_changed(void)
{
    state_lock();
    const app_state_t *st = state_get();
    conn_state_t conn = st->conn;
    bool ring = st->has_led_ring;
    char effect[24];
    strlcpy(effect, st->has_status ? st->status.led_effect : "", sizeof(effect));
    int brightness = st->has_status ? st->status.led_brightness : -1;
    state_unlock();

    s_has_ring = ring;

    if (conn != CONN_CONNECTED) {
        s_cfg_requested = false;  // re-read $LED/* on the next connect
    }
    if (conn == CONN_CONNECTING) {
        // Fresh table: forget the previous table's live values.
        s_live_effect_seen = false;
        s_live_bright_seen = false;
    }
    if (conn == CONN_CONNECTED && ring && !s_cfg_requested) {
        s_cfg_requested = true;
        if (jobs_submit(led_config_job, NULL) != ESP_OK) {
            ESP_LOGW(TAG, "LED config job dropped (queue full); will retry");
            s_cfg_requested = false;
        }
    }

    // Live /sand_status.led wins over NVS mid-run.
    if (effect[0] != '\0') {
        int id = effect_id_by_name(effect);
        s_live_effect_seen = true;
        s_cur_effect = id;
        if (id > 0) {
            s_last_effect = id;
        }
        if (id > 0 && id != BALL_ID) {
            s_last_non_ball = id;
        }
    }
    if (brightness >= 0) {
        s_live_bright_seen = true;
        s_brightness = (brightness * 100 + 127) / 255;
    }

    refresh_all();
}

// ---------------------------------------------------------------------------
// Page construction
// ---------------------------------------------------------------------------

lv_obj_t *page_light_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_page_root(parent);
    ui_page_header(page, "Light");

    lv_obj_t *body = plain(page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    // ---- One column, stepped: light state, appearance, effects, ball ----
    // Was two side-by-side columns with a stepper each, which meant two
    // independent scroll positions and two pairs of arrows on one screen.
    lv_obj_t *col = plain(body);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(col, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_row(col, TH_SPACE_MD, 0);

    // TABLE LIGHT card
    lv_obj_t *card_light = make_card(col);
    make_section_label(card_light, "TABLE LIGHT");

    s_notice = lv_label_create(card_light);
    lv_label_set_text(s_notice,
                      "No light ring is set up for this table. "
                      "Configure one in the Dune Weaver web interface.");
    lv_obj_set_width(s_notice, LV_PCT(100));
    lv_label_set_long_mode(s_notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_notice, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_notice, th.text2, 0);

    s_power_row = plain(card_light);
    lv_obj_set_width(s_power_row, LV_PCT(100));
    lv_obj_set_height(s_power_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_power_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_power_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_power_row, TH_SPACE_MD, 0);

    s_power_label = lv_label_create(s_power_row);
    lv_label_set_text(s_power_label, "Off");
    lv_obj_set_style_text_font(s_power_label, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_power_label, th.text, 0);
    lv_obj_set_flex_grow(s_power_label, 1);

    s_power_switch = make_switch(s_power_row);
    lv_obj_add_event_cb(s_power_switch, power_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright_row = make_slider_row(card_light, "Brightness", 132, 0, 100, &s_ctl_bright);

    // APPEARANCE card
    s_card_appear = make_card(col);
    make_section_label(s_card_appear, "APPEARANCE");

    s_color_block = plain(s_card_appear);
    lv_obj_set_width(s_color_block, LV_PCT(100));
    lv_obj_set_height(s_color_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_color_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_color_block, TH_SPACE_SM, 0);
    s_color_caption = make_caption(s_color_block, "Colour");
    make_swatch_row(s_color_block, false, s_sw_primary);

    s_color2_block = plain(s_card_appear);
    lv_obj_set_width(s_color2_block, LV_PCT(100));
    lv_obj_set_height(s_color2_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_color2_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_color2_block, TH_SPACE_SM, 0);
    make_caption(s_color2_block, "Secondary");
    make_swatch_row(s_color2_block, true, s_sw_secondary);

    s_palette_block = plain(s_card_appear);
    lv_obj_set_width(s_palette_block, LV_PCT(100));
    lv_obj_set_height(s_palette_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_palette_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_palette_block, TH_SPACE_SM, 0);
    make_caption(s_palette_block, "Palette");
    lv_obj_t *palette_grid = make_grid3(s_palette_block, GRID_ROWS3);
    for (int i = 0; i < PALETTE_COUNT; i++) {
        lv_obj_t *chip = make_chip(palette_grid, PALETTE_LABELS[i]);
        lv_obj_set_grid_cell(chip, LV_GRID_ALIGN_STRETCH, i % 3, 1, LV_GRID_ALIGN_CENTER,
                             i / 3, 1);
        lv_obj_add_event_cb(chip, palette_chip_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_palette_chips[i] = chip;
    }

    s_speed_row = make_slider_row(s_card_appear, "Speed", 132, 1, 255, &s_ctl_speed);

    // ---- Effect catalogue + ball tracker, same column, below ----
    // Still its own container, purely so refresh_all can hide the whole group
    // with one set_hidden when the table reports no LED ring (line ~826).
    // Height is CONTENT, not 100%, or it would claim a full viewport of its
    // own inside the scroller and leave a page of blank space above the cards.
    s_right_col = plain(col);
    lv_obj_set_width(s_right_col, LV_PCT(100));
    lv_obj_set_height(s_right_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_right_col, 0, 0);  // col already pads
    lv_obj_set_style_pad_row(s_right_col, TH_SPACE_MD, 0);

    // EFFECT card (full catalogue minus 'ball' — it has its own card)
    lv_obj_t *card_effect = make_card(s_right_col);
    make_section_label(card_effect, "EFFECT");
    lv_obj_t *effect_grid = make_grid3(card_effect, GRID_ROWS13);
    int cell = 0;
    for (int id = 0; id < EFFECT_COUNT; id++) {
        if (id == BALL_ID) {
            continue;
        }
        lv_obj_t *chip = make_chip(effect_grid, EFFECT_LABELS[id]);
        lv_obj_set_grid_cell(chip, LV_GRID_ALIGN_STRETCH, cell % 3, 1, LV_GRID_ALIGN_CENTER,
                             cell / 3, 1);
        lv_obj_add_event_cb(chip, effect_chip_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)id);
        s_effect_chips[id] = chip;
        cell++;
    }

    // BALL TRACKER card
    lv_obj_t *card_ball = make_card(s_right_col);

    lv_obj_t *ball_header = plain(card_ball);
    lv_obj_set_width(ball_header, LV_PCT(100));
    lv_obj_set_height(ball_header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ball_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ball_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ball_header, TH_SPACE_MD, 0);

    lv_obj_t *ball_titles = plain(ball_header);
    lv_obj_set_height(ball_titles, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ball_titles, 1);
    lv_obj_set_flex_flow(ball_titles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ball_titles, TH_SPACE_XS, 0);
    make_section_label(ball_titles, "BALL TRACKER");
    s_ball_hint = lv_label_create(ball_titles);
    lv_label_set_text(s_ball_hint, "A glowing dot that follows the sand ball.");
    lv_obj_set_width(s_ball_hint, LV_PCT(100));
    lv_label_set_long_mode(s_ball_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ball_hint, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_ball_hint, th.text2, 0);

    s_ball_switch = make_switch(ball_header);
    lv_obj_add_event_cb(s_ball_switch, ball_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    s_ball_controls = plain(card_ball);
    lv_obj_set_width(s_ball_controls, LV_PCT(100));
    lv_obj_set_height(s_ball_controls, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ball_controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ball_controls, TH_SPACE_MD, 0);

    make_caption(s_ball_controls, "The dot");

    lv_obj_t *dot_color_block = plain(s_ball_controls);
    lv_obj_set_width(dot_color_block, LV_PCT(100));
    lv_obj_set_height(dot_color_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dot_color_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dot_color_block, TH_SPACE_SM, 0);
    make_caption(dot_color_block, "Colour");
    make_swatch_row(dot_color_block, false, s_sw_dot);

    make_slider_row(s_ball_controls, "Brightness", 150, 0, 255, &s_ctl_fg);

    lv_obj_t *dir_row = plain(s_ball_controls);
    lv_obj_set_width(dir_row, LV_PCT(100));
    lv_obj_set_height(dir_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dir_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dir_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dir_row, TH_SPACE_MD, 0);
    lv_obj_t *dir_label = make_caption(dir_row, "Direction");
    lv_obj_set_width(dir_label, 150);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *chip = make_chip(dir_row, DIR_LABELS[i]);
        lv_obj_set_flex_grow(chip, 1);
        lv_obj_add_event_cb(chip, dir_chip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_dir_chips[i] = chip;
    }

    make_slider_row(s_ball_controls, "Glow size", 150, 1, 30, &s_ctl_size);
    make_slider_row(s_ball_controls, "Alignment", 150, 0, 359, &s_ctl_align);

    lv_obj_t *divider = plain(s_ball_controls);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, th.border, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    make_caption(s_ball_controls, "Behind the dot");

    // Background options: Solid / Off / every other effect
    lv_obj_t *bg_grid = make_grid3(s_ball_controls, GRID_ROWS13);
    s_bg_count = 0;
    s_bg_values[s_bg_count++] = "static";
    s_bg_values[s_bg_count++] = "off";
    for (int id = 0; id < EFFECT_COUNT; id++) {
        if (id == 0 || id == 1 || id == BALL_ID) {  // off/static/ball
            continue;
        }
        s_bg_values[s_bg_count++] = EFFECTS[id];
    }
    for (int i = 0; i < s_bg_count; i++) {
        const char *label = (i == 0) ? "Solid"
                            : (i == 1) ? "Off"
                                       : EFFECT_LABELS[effect_id_by_name(s_bg_values[i])];
        lv_obj_t *chip = make_chip(bg_grid, label);
        lv_obj_set_grid_cell(chip, LV_GRID_ALIGN_STRETCH, i % 3, 1, LV_GRID_ALIGN_CENTER,
                             i / 3, 1);
        lv_obj_add_event_cb(chip, bg_chip_clicked, LV_EVENT_CLICKED, (void *)s_bg_values[i]);
        s_bg_chips[i] = chip;
    }

    s_bg_color_row = plain(s_ball_controls);
    lv_obj_set_width(s_bg_color_row, LV_PCT(100));
    lv_obj_set_height(s_bg_color_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bg_color_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bg_color_row, TH_SPACE_SM, 0);
    make_caption(s_bg_color_row, "Bg colour");
    make_swatch_row(s_bg_color_row, true, s_sw_bg);

    s_bg_bright_row = make_slider_row(s_ball_controls, "Bg brightness", 150, 0, 255, &s_ctl_bgb);

    ui_page_stepper(body, col);  // one scroller, one pair of arrows (see ui.h)

    state_add_listener(on_state_changed);
    refresh_all();  // initial no-ring look: notice only, right column hidden
    return page;
}
