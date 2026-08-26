// Control page: WiFi provisioning (new vs the Pi app, which used OS WiFi),
// table connection + password, mDNS discovery, table motion, and screen
// settings. Reference: qml/pages/TableControlPage.qml + PORTING_NOTES §5.
//
// Threading: all blocking work (WiFi scan/join, mDNS browse, fw_* HTTP) runs
// on the jobs task; job completions take lvgl_port_lock() to touch widgets.
// The state listener runs in the LVGL task context (state.c locks for us).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "../../app/jobs.h"
#include "../../app/state.h"
#include "../../net/discovery.h"
#include "../../net/fw_client.h"
#include "../../net/settings.h"
#include "../../net/wifi.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "page_control";

// QML metrics x 1.5 (see theme.h)
#define ROW_HEIGHT 90   // info rects (QML 60)
#define BTN_WIDTH 174   // row buttons (QML 116)
#define BTN_HEIGHT 66   // row buttons / text fields (QML 44)
#define KB_HEIGHT 280
#define MAX_APS 10
#define MAX_TABLES 8

// ---- WiFi card ----
static lv_obj_t *s_wifi_row;
static lv_obj_t *s_wifi_title;
static lv_obj_t *s_wifi_sub;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_scan_label;
static char s_ap_ssid[MAX_APS][33];
static int s_ap_count;
static bool s_scanning;
static bool s_joining;
static char s_join_ssid[33];

// WiFi join modal
static lv_obj_t *s_join_scrim;
static lv_obj_t *s_join_ta;
static int s_join_idx = -1;
static char s_join_target[33];  // SSID snapshot from when the modal opened

// ---- Table connection card ----
static lv_obj_t *s_conn_row;
static lv_obj_t *s_conn_title;
static lv_obj_t *s_conn_sub;
static lv_obj_t *s_disconnect_btn;
static lv_obj_t *s_pw_ta;

// ---- Tables on your network card ----
static lv_obj_t *s_tables_list;
static lv_obj_t *s_tables_empty;
static lv_obj_t *s_refresh_label;
static lv_obj_t *s_manual_ta;
static lv_obj_t *s_manual_btn;
static table_info_t s_tables[MAX_TABLES];
static int s_table_count;
static bool s_discovering;

// ---- Table card (Home/Center/Edge/Restart, gated on CONN_CONNECTED) ----
typedef enum { ACT_HOME = 0, ACT_CENTER, ACT_EDGE, ACT_RESTART, ACT_COUNT } table_act_t;
static lv_obj_t *s_table_btns[ACT_COUNT];

// ---- This screen card ----
static const struct {
    const char *label;
    uint32_t secs;
} SLEEP_OPTS[] = {{"30 s", 30}, {"1 m", 60}, {"5 m", 300}, {"10 m", 600}, {"Never", 0}};
#define SLEEP_OPT_COUNT (int)(sizeof(SLEEP_OPTS) / sizeof(SLEEP_OPTS[0]))
static lv_obj_t *s_sleep_chips[SLEEP_OPT_COUNT];

// Shared on-demand keyboard on lv_layer_top
static lv_obj_t *s_kb;

// ---------------------------------------------------------------------------
// Generic builders (SettingsCard / SectionLabel / info rect recipes)
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

// SettingsCard: surface bg, TH_RADIUS_MD, 1px border_light, TH_SPACE_LG pad
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

// SectionLabel: uppercase eyebrow (caller passes uppercase text)
static lv_obj_t *make_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_letter_space(label, 2, 0);
    lv_obj_set_style_text_color(label, th.text3, 0);
    return label;
}

static lv_obj_t *make_divider(lv_obj_t *parent)
{
    lv_obj_t *line = plain(parent);
    lv_obj_set_size(line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(line, th.border_light, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

static lv_obj_t *make_hrow(lv_obj_t *parent)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TH_SPACE_SM, 0);
    return row;
}

// Info rect: rounded panel with title + optional subtitle, grows in its row.
static lv_obj_t *make_info_rect(lv_obj_t *parent, lv_obj_t **out_title, lv_obj_t **out_sub)
{
    lv_obj_t *rect = plain(parent);
    lv_obj_set_flex_grow(rect, 1);
    lv_obj_set_height(rect, ROW_HEIGHT);
    lv_obj_set_style_radius(rect, TH_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(rect, th.card, 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rect, th.border, 0);
    lv_obj_set_style_border_width(rect, 1, 0);
    lv_obj_set_style_pad_left(rect, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_right(rect, TH_SPACE_MD, 0);
    lv_obj_set_flex_flow(rect, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rect, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rect, 3, 0);

    lv_obj_t *title = lv_label_create(rect);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(title, th.text2, 0);

    lv_obj_t *sub = lv_label_create(rect);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(sub, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(sub, th.text2, 0);
    lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);

    *out_title = title;
    if (out_sub != NULL) {
        *out_sub = sub;
    }
    return rect;
}

// Row-sized pill button (QML 116x44 ModernControlButton)
static lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, lv_color_t color, bool filled)
{
    lv_obj_t *btn = ui_pill_button(parent, text, color, filled);
    lv_obj_set_size(btn, BTN_WIDTH, BTN_HEIGHT);
    lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), TH_FONT_CAPTION, 0);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    return btn;
}

static void set_btn_enabled(lv_obj_t *btn, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static bool text_has_content(const char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        if (*s != ' ') {
            return true;
        }
    }
    return false;
}

static void trimmed_copy(char *dst, size_t dst_len, const char *src)
{
    while (*src == ' ') {
        src++;
    }
    strlcpy(dst, src, dst_len);
    size_t len = strlen(dst);
    while (len > 0 && dst[len - 1] == ' ') {
        dst[--len] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Shared keyboard (one lv_keyboard on lv_layer_top, created on demand)
// ---------------------------------------------------------------------------

static void kb_hide(void)
{
    if (s_kb != NULL) {
        lv_keyboard_set_textarea(s_kb, NULL);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void kb_ready_cb(lv_event_t *e)
{
    (void)e;  // both LV_EVENT_READY (checkmark) and LV_EVENT_CANCEL close it
    kb_hide();
}

static void kb_show(lv_obj_t *ta)
{
    if (s_kb == NULL) {
        s_kb = ui_keyboard_create(lv_layer_top());
        lv_obj_add_event_cb(s_kb, kb_ready_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, kb_ready_cb, LV_EVENT_CANCEL, NULL);
    }
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(s_kb, -1);  // keep above any modal scrim on layer_top
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void ta_clicked(lv_event_t *e)
{
    kb_show(lv_event_get_target_obj(e));
}

// Themed single-line text field (QML TextField recipe: bg fill, radius 22x1.5)
static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, 120);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_height(ta, BTN_HEIGHT);
    lv_obj_set_style_bg_color(ta, th.bg, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, BTN_HEIGHT / 2, 0);
    lv_obj_set_style_border_color(ta, th.border, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, th.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(ta, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_ver(ta, (BTN_HEIGHT - 26) / 2, 0);
    lv_obj_set_style_text_font(ta, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(ta, th.text, 0);
    lv_obj_set_style_text_color(ta, th.text3, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(ta, ta_clicked, LV_EVENT_CLICKED, NULL);
    return ta;
}

// ---------------------------------------------------------------------------
// Job-side error surfacing
// ---------------------------------------------------------------------------

// Called from the jobs task: prefix + friendly text under the LVGL lock.
static void show_fw_error_locked(const char *prefix, esp_err_t err)
{
    char msg[192];
    snprintf(msg, sizeof(msg), "%s%s", prefix, fw_friendly_error(err));
    lvgl_port_lock(0);
    ui_show_error(msg);
    lvgl_port_unlock();
}

// Called from LVGL callbacks when jobs_submit() reports a full queue.
static void show_queue_full(void)
{
    ui_show_error("Still working on the last request - give it a moment and try again.");
}

static void settings_save_job(void *arg)
{
    (void)arg;
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings_save: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// Card 1: WIFI
// ---------------------------------------------------------------------------

static void wifi_row_refresh(void)  // LVGL task context only
{
    if (s_joining) {
        char text[48];
        snprintf(text, sizeof(text), "Joining %s...", s_join_ssid);
        lv_label_set_text(s_wifi_title, text);
        lv_obj_set_style_text_color(s_wifi_title, th.text, 0);
        lv_obj_add_flag(s_wifi_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_wifi_row, th.card, 0);
        lv_obj_set_style_border_color(s_wifi_row, th.border, 0);
        return;
    }
    if (wifi_is_connected()) {
        const char *ssid = settings_get()->wifi_ssid;
        lv_label_set_text(s_wifi_title, ssid[0] != '\0' ? ssid : "Connected");
        lv_obj_set_style_text_color(s_wifi_title, th.ok, 0);
        const char *ip = wifi_ip();
        if (ip != NULL && ip[0] != '\0') {
            lv_label_set_text(s_wifi_sub, ip);
            lv_obj_remove_flag(s_wifi_sub, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_sub, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_wifi_row, th.ok_soft, 0);
        lv_obj_set_style_border_color(s_wifi_row, th.ok, 0);
    } else {
        lv_label_set_text(s_wifi_title, "Not connected");
        lv_obj_set_style_text_color(s_wifi_title, th.text2, 0);
        lv_obj_add_flag(s_wifi_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_wifi_row, th.card, 0);
        lv_obj_set_style_border_color(s_wifi_row, th.border, 0);
    }
}

// WiFi connectivity edge, fired from the WiFi event task.
static void on_wifi_event(bool connected)
{
    lvgl_port_lock(0);
    if (connected) {
        s_joining = false;
    }
    wifi_row_refresh();
    lvgl_port_unlock();
}

typedef struct {
    char ssid[33];
    char pass[65];
} join_ctx_t;

static void wifi_join_job(void *arg)
{
    join_ctx_t *ctx = (join_ctx_t *)arg;
    esp_err_t err = wifi_join(ctx->ssid, ctx->pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_join(%s): %s", ctx->ssid, esp_err_to_name(err));
        lvgl_port_lock(0);
        s_joining = false;
        wifi_row_refresh();
        ui_show_error("Join failed - couldn't switch WiFi networks. Try again.");
        lvgl_port_unlock();
    }
    free(ctx);
}

static void join_modal_close(void)
{
    kb_hide();  // detach the textarea BEFORE it is deleted with the modal
    if (s_join_scrim != NULL) {
        lv_obj_delete(s_join_scrim);
        s_join_scrim = NULL;
        s_join_ta = NULL;
    }
}

static void join_cancel_clicked(lv_event_t *e)
{
    (void)e;
    join_modal_close();
}

static void join_clicked(lv_event_t *e)
{
    (void)e;
    // Use the SSID captured when the modal opened: a scan landing while the
    // modal is up rewrites s_ap_ssid[] and s_join_idx would point elsewhere.
    if (s_join_target[0] == '\0' || s_join_ta == NULL) {
        return;
    }
    join_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ui_show_error("Out of memory - try again.");
        return;
    }
    strlcpy(ctx->ssid, s_join_target, sizeof(ctx->ssid));
    const char *pass = lv_textarea_get_text(s_join_ta);
    strlcpy(ctx->pass, pass != NULL ? pass : "", sizeof(ctx->pass));

    if (jobs_submit(wifi_join_job, ctx) != ESP_OK) {
        free(ctx);
        show_queue_full();
        return;
    }
    s_joining = true;
    strlcpy(s_join_ssid, ctx->ssid, sizeof(s_join_ssid));
    join_modal_close();
    wifi_row_refresh();
}

// Password entry for a tapped AP: modal card + shared keyboard on layer_top.
static void open_join_modal(int idx)
{
    if (idx < 0 || idx >= s_ap_count) {
        return;
    }
    join_modal_close();
    s_join_idx = idx;
    strlcpy(s_join_target, s_ap_ssid[idx], sizeof(s_join_target));

    s_join_scrim = plain(lv_layer_top());
    lv_obj_set_size(s_join_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_join_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_join_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(s_join_scrim, LV_OBJ_FLAG_CLICKABLE);  // swallow background taps

    lv_obj_t *card = plain(s_join_scrim);
    lv_obj_set_width(card, 640);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, TH_SPACE_XL);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_LG, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, TH_SPACE_MD, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "Join %s", s_ap_ssid[idx]);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, th.text, 0);

    lv_obj_t *row = make_hrow(card);
    s_join_ta = make_textarea(row, "WiFi password", true);

    lv_obj_t *btns = make_hrow(card);
    lv_obj_t *cancel = make_small_button(btns, "Cancel", th.text2, false);
    lv_obj_set_width(cancel, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, join_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *join = make_small_button(btns, "Join", th.accent, true);
    lv_obj_set_width(join, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(join, 1);
    lv_obj_add_event_cb(join, join_clicked, LV_EVENT_CLICKED, NULL);

    kb_show(s_join_ta);
}

static void ap_row_clicked(lv_event_t *e)
{
    open_join_modal((int)(intptr_t)lv_event_get_user_data(e));
}

// Rebuild the AP list from scan results. LVGL lock must be held.
static void rebuild_ap_list(const wifi_ap_record_t *aps, int n)
{
    lv_obj_clean(s_wifi_list);
    s_ap_count = 0;
    for (int i = 0; i < n && s_ap_count < MAX_APS; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (ssid[0] == '\0') {
            continue;  // hidden network
        }
        bool dup = false;
        for (int j = 0; j < s_ap_count; j++) {
            if (strcmp(s_ap_ssid[j], ssid) == 0) {
                dup = true;  // wifi_scan orders by RSSI; keep the strongest
                break;
            }
        }
        if (dup) {
            continue;
        }
        int idx = s_ap_count++;
        strlcpy(s_ap_ssid[idx], ssid, sizeof(s_ap_ssid[idx]));

        lv_obj_t *ap_row = plain(s_wifi_list);
        lv_obj_set_width(ap_row, LV_PCT(100));
        lv_obj_set_height(ap_row, ROW_HEIGHT);
        lv_obj_set_style_radius(ap_row, TH_RADIUS_SM, 0);
        lv_obj_set_style_bg_color(ap_row, th.card, 0);
        lv_obj_set_style_bg_opa(ap_row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ap_row, th.pressed, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(ap_row, th.border, 0);
        lv_obj_set_style_border_width(ap_row, 1, 0);
        lv_obj_set_style_pad_hor(ap_row, TH_SPACE_LG, 0);
        lv_obj_set_flex_flow(ap_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ap_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ap_row, TH_SPACE_SM, 0);
        lv_obj_add_flag(ap_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ap_row, ap_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

        lv_obj_t *name = lv_label_create(ap_row);
        lv_label_set_text(name, s_ap_ssid[idx]);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, TH_FONT_BODY, 0);
        lv_obj_set_style_text_color(name, th.text, 0);

        lv_obj_t *rssi = lv_label_create(ap_row);
        lv_label_set_text_fmt(rssi, "%d dBm", (int)aps[i].rssi);
        lv_obj_set_style_text_font(rssi, TH_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(rssi, th.text2, 0);
    }
    if (s_ap_count > 0) {
        lv_obj_remove_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_scan_job(void *arg)
{
    (void)arg;
    wifi_ap_record_t *aps = calloc(MAX_APS, sizeof(*aps));
    int n = (aps != NULL) ? wifi_scan(aps, MAX_APS) : -1;

    lvgl_port_lock(0);
    s_scanning = false;
    lv_label_set_text(s_scan_label, "Scan");
    set_btn_enabled(s_scan_btn, true);
    if (n < 0) {
        ui_show_error("Scan failed - couldn't search for WiFi networks. Try again.");
    } else {
        rebuild_ap_list(aps, n);
    }
    lvgl_port_unlock();
    free(aps);
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    if (s_scanning) {
        return;
    }
    if (jobs_submit(wifi_scan_job, NULL) != ESP_OK) {
        show_queue_full();
        return;
    }
    s_scanning = true;
    lv_label_set_text(s_scan_label, "Scanning...");
    set_btn_enabled(s_scan_btn, false);
}

static void build_wifi_card(lv_obj_t *column)
{
    lv_obj_t *card = make_card(column);
    make_section_label(card, "WIFI");

    lv_obj_t *row = make_hrow(card);
    s_wifi_row = make_info_rect(row, &s_wifi_title, &s_wifi_sub);
    lv_label_set_text(s_wifi_title, "Not connected");

    s_scan_btn = make_small_button(row, "Scan", th.text2, false);
    s_scan_label = lv_obj_get_child(s_scan_btn, 0);
    lv_obj_add_event_cb(s_scan_btn, scan_clicked, LV_EVENT_CLICKED, NULL);

    s_wifi_list = plain(card);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_height(s_wifi_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, TH_SPACE_SM, 0);
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Card 2: TABLE CONNECTION
// ---------------------------------------------------------------------------

typedef struct {
    char pass[65];
} pw_ctx_t;

static void save_password_job(void *arg)
{
    pw_ctx_t *ctx = (pw_ctx_t *)arg;
    fw_set_password(ctx->pass);  // "" clears
    app_settings_t *cfg = settings_get();
    strlcpy(cfg->table_password, ctx->pass, sizeof(cfg->table_password));
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings_save: %s", esp_err_to_name(err));
    }
    state_poll_now();

    lvgl_port_lock(0);
    lv_textarea_set_placeholder_text(
        s_pw_ta, ctx->pass[0] != '\0' ? "Table password (saved)" : "Table password (if set)");
    lvgl_port_unlock();
    free(ctx);
}

static void save_password_clicked(lv_event_t *e)
{
    (void)e;
    pw_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ui_show_error("Out of memory - try again.");
        return;
    }
    const char *txt = lv_textarea_get_text(s_pw_ta);
    strlcpy(ctx->pass, txt != NULL ? txt : "", sizeof(ctx->pass));
    if (jobs_submit(save_password_job, ctx) != ESP_OK) {
        free(ctx);
        show_queue_full();
        return;
    }
    lv_textarea_set_text(s_pw_ta, "");
    kb_hide();
}

static void disconnect_clicked(lv_event_t *e)
{
    (void)e;
    state_disconnect();
}

static void build_conn_card(lv_obj_t *column)
{
    lv_obj_t *card = make_card(column);
    make_section_label(card, "TABLE CONNECTION");

    lv_obj_t *row = make_hrow(card);
    s_conn_row = make_info_rect(row, &s_conn_title, &s_conn_sub);
    lv_label_set_text(s_conn_title, "Not connected");

    s_disconnect_btn = make_small_button(row, "Disconnect", th.danger, false);
    lv_obj_add_flag(s_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_disconnect_btn, disconnect_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pw_row = make_hrow(card);
    bool saved = settings_get()->table_password[0] != '\0';
    s_pw_ta = make_textarea(pw_row, saved ? "Table password (saved)" : "Table password (if set)", true);
    lv_textarea_set_max_length(s_pw_ta, 64);

    lv_obj_t *save = make_small_button(pw_row, "Save", th.accent, true);
    lv_obj_add_event_cb(save, save_password_clicked, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Card 3: TABLES ON YOUR NETWORK
// ---------------------------------------------------------------------------

static void table_connect_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_table_count) {
        state_connect_url(s_tables[idx].url);
    }
}

// Rebuild discovery rows. LVGL lock must be held.
static void rebuild_table_list(const table_info_t *found, int n)
{
    // Filter the currently connected table (it already sits in the card above)
    state_lock();
    bool connected = state_get()->conn == CONN_CONNECTED;
    state_unlock();
    const char *current = settings_get()->table_url;

    lv_obj_clean(s_tables_list);
    s_table_count = 0;
    for (int i = 0; i < n && s_table_count < MAX_TABLES; i++) {
        if (connected && strcmp(found[i].url, current) == 0) {
            continue;
        }
        int idx = s_table_count++;
        s_tables[idx] = found[i];

        lv_obj_t *row = make_hrow(s_tables_list);
        lv_obj_t *title = NULL;
        lv_obj_t *sub = NULL;
        make_info_rect(row, &title, &sub);
        lv_label_set_text(title, s_tables[idx].name[0] != '\0' ? s_tables[idx].name : s_tables[idx].url);
        lv_obj_set_style_text_color(title, th.text, 0);
        lv_label_set_text(sub, s_tables[idx].url);
        lv_obj_remove_flag(sub, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *connect = make_small_button(row, "Connect", th.accent, true);
        lv_obj_add_event_cb(connect, table_connect_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
    if (s_table_count > 0) {
        lv_obj_remove_flag(s_tables_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tables_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tables_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_tables_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void discovery_job(void *arg)
{
    (void)arg;
    table_info_t *found = calloc(MAX_TABLES, sizeof(*found));
    int n = -1;
    if (found != NULL && discovery_init() == ESP_OK) {
        n = discovery_scan(found, MAX_TABLES, 3000);
    }

    lvgl_port_lock(0);
    s_discovering = false;
    lv_label_set_text(s_refresh_label, "Refresh");
    if (n < 0) {
        ui_show_error("Refresh failed - couldn't search for tables. Check WiFi and try again.");
    } else {
        rebuild_table_list(found, n);
    }
    lvgl_port_unlock();
    free(found);
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    if (s_discovering) {
        return;
    }
    if (jobs_submit(discovery_job, NULL) != ESP_OK) {
        show_queue_full();
        return;
    }
    s_discovering = true;
    lv_label_set_text(s_refresh_label, "Scanning...");
}

static void manual_ta_changed(lv_event_t *e)
{
    (void)e;
    set_btn_enabled(s_manual_btn, text_has_content(lv_textarea_get_text(s_manual_ta)));
}

static void manual_connect_clicked(lv_event_t *e)
{
    (void)e;
    char url[128];
    const char *txt = lv_textarea_get_text(s_manual_ta);
    trimmed_copy(url, sizeof(url), txt != NULL ? txt : "");
    if (url[0] == '\0') {
        return;
    }
    state_connect_url(url);
    lv_textarea_set_text(s_manual_ta, "");
    set_btn_enabled(s_manual_btn, false);
    kb_hide();
}

static void build_tables_card(lv_obj_t *column)
{
    lv_obj_t *card = make_card(column);

    lv_obj_t *head = make_hrow(card);
    lv_obj_t *eyebrow = make_section_label(head, "TABLES ON YOUR NETWORK");
    lv_obj_set_flex_grow(eyebrow, 1);
    lv_obj_t *refresh = make_small_button(head, "Refresh", th.text2, false);
    s_refresh_label = lv_obj_get_child(refresh, 0);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    s_tables_list = plain(card);
    lv_obj_set_width(s_tables_list, LV_PCT(100));
    lv_obj_set_height(s_tables_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_tables_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tables_list, TH_SPACE_SM, 0);
    lv_obj_add_flag(s_tables_list, LV_OBJ_FLAG_HIDDEN);

    s_tables_empty = lv_label_create(card);
    lv_label_set_text(s_tables_empty, "No tables found. Tap Refresh, or enter the address below.");
    lv_obj_set_width(s_tables_empty, LV_PCT(100));
    lv_label_set_long_mode(s_tables_empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_tables_empty, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_tables_empty, th.text3, 0);

    lv_obj_t *manual_row = make_hrow(card);
    s_manual_ta = make_textarea(manual_row, "IP or host address", false);
    lv_obj_add_event_cb(s_manual_ta, manual_ta_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_manual_btn = make_small_button(manual_row, "Connect", th.accent, true);
    set_btn_enabled(s_manual_btn, false);
    lv_obj_add_event_cb(s_manual_btn, manual_connect_clicked, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Card 4: TABLE (Home / Center / Edge / Restart)
// ---------------------------------------------------------------------------

static const char *ACT_ERR_PREFIX[ACT_COUNT] = {
    "Home failed: ",
    "Center failed: ",
    "Edge failed: ",
    "Restart failed: ",
};

static void table_act_job(void *arg)
{
    table_act_t act = (table_act_t)(intptr_t)arg;
    esp_err_t err = ESP_OK;
    switch (act) {
    case ACT_HOME:
        err = fw_home();
        break;
    case ACT_CENTER:
        err = fw_goto_rho(0.0f);
        break;
    case ACT_EDGE:
        err = fw_goto_rho(1.0f);
        break;
    case ACT_RESTART:
        err = fw_command("$Bye");  // board reboot; never retried
        break;
    default:
        return;
    }
    if (err != ESP_OK) {
        show_fw_error_locked(ACT_ERR_PREFIX[act], err);
    } else {
        state_poll_now();
    }
}

static void table_act_clicked(lv_event_t *e)
{
    table_act_t act = (table_act_t)(intptr_t)lv_event_get_user_data(e);
    state_lock();
    bool connected = state_get()->conn == CONN_CONNECTED;
    state_unlock();
    if (!connected) {
        return;  // buttons are also visually disabled by the state listener
    }
    if (jobs_submit(table_act_job, (void *)(intptr_t)act) != ESP_OK) {
        show_queue_full();
    }
}

static lv_obj_t *make_table_btn(lv_obj_t *parent, const char *text, lv_color_t color, table_act_t act)
{
    lv_obj_t *btn = ui_pill_button(parent, text, color, false);
    lv_obj_set_height(btn, TH_TOUCH_TARGET);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_state(btn, LV_STATE_DISABLED);  // enabled by the state listener
    lv_obj_add_event_cb(btn, table_act_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    s_table_btns[act] = btn;
    return btn;
}

static lv_obj_t *make_dw_switch(lv_obj_t *parent, bool checked);  // defined with the screen card

// --- Auto play ($Playlist/Autostart): switch + playlist picker --------------
// The firmware autostarts a NAMED playlist, so turning the switch on opens a
// picker; off sends an empty value. The NVS write is idle-gated on the board,
// so after every write the actual value is read back and the switch reflects
// what really stuck.

static lv_obj_t *s_autoplay_sw;
static lv_obj_t *s_autoplay_popup;
static bool s_autoplay_on;
static bool s_autoplay_busy;

typedef struct {
    char name[64];  // "" = clear autostart
} autoplay_ctx_t;

// LVGL ctx: reflect a value without re-triggering VALUE_CHANGED logic.
static void autoplay_reflect(bool on)
{
    s_autoplay_on = on;
    if (s_autoplay_sw == NULL) {
        return;
    }
    if (on) {
        lv_obj_add_state(s_autoplay_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_autoplay_sw, LV_STATE_CHECKED);
    }
}

// jobs task: read the current Playlist/Autostart from the settings cache.
static void autoplay_seed_job(void *arg)
{
    (void)arg;
    char buf[64];
    bool on = fw_setting("Playlist/Autostart", buf, sizeof(buf)) && text_has_content(buf);
    lvgl_port_lock(0);
    autoplay_reflect(on);
    lvgl_port_unlock();
}

// jobs task: write, then re-read what actually stuck.
static void autoplay_set_job(void *arg)
{
    autoplay_ctx_t *ctx = arg;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "$Playlist/Autostart=%s", ctx->name);
    esp_err_t err = fw_command(cmd);

    bool on = s_autoplay_on;
    if (err == ESP_OK && fw_load_settings() == ESP_OK) {
        char buf[64];
        on = fw_setting("Playlist/Autostart", buf, sizeof(buf)) && text_has_content(buf);
    }
    lvgl_port_lock(0);
    s_autoplay_busy = false;
    autoplay_reflect(on);
    lvgl_port_unlock();
    if (err != ESP_OK) {
        show_fw_error_locked("Couldn't change auto play. ", err);
    }
    free(ctx);
}

static void autoplay_close_popup(void)
{
    if (s_autoplay_popup != NULL) {
        lv_obj_delete(s_autoplay_popup);
        s_autoplay_popup = NULL;
    }
}

static void autoplay_popup_cancel(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e)) {
        autoplay_close_popup();
    }
}

static void autoplay_pick_row(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    const char *name = lv_label_get_text(lv_obj_get_child(row, 0));

    autoplay_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        autoplay_close_popup();
        return;
    }
    strlcpy(ctx->name, name, sizeof(ctx->name));
    if (jobs_submit(autoplay_set_job, ctx) != ESP_OK) {
        free(ctx);
        show_queue_full();
    } else {
        s_autoplay_busy = true;
    }
    autoplay_close_popup();
}

// LVGL lock held (built from the list job)
static void autoplay_show_picker(const fw_str_list_t *list)
{
    autoplay_close_popup();
    s_autoplay_popup = plain(lv_layer_top());
    lv_obj_set_size(s_autoplay_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_autoplay_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_autoplay_popup, LV_OPA_50, 0);
    lv_obj_add_flag(s_autoplay_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_autoplay_popup, autoplay_popup_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = plain(s_autoplay_popup);
    lv_obj_set_width(card, 510);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_LG, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, TH_SPACE_SM, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Auto play which playlist?");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, th.text, 0);

    if (list->count == 0) {
        lv_obj_t *empty = lv_label_create(card);
        lv_label_set_text(empty, "No playlists yet - create one on the Playlists page first");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(empty, TH_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(empty, th.text3, 0);
    }

    int rows_h = list->count * (84 + TH_SPACE_SM);
    if (rows_h > 340) {
        rows_h = 340;
    }
    lv_obj_t *rows = plain(card);
    lv_obj_set_size(rows, LV_PCT(100), rows_h);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rows, TH_SPACE_SM, 0);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);

    for (int i = 0; i < list->count; i++) {
        char name[64];
        strlcpy(name, list->items[i], sizeof(name));
        size_t n = strlen(name);
        if (n >= 4 && strcasecmp(name + n - 4, ".txt") == 0) {
            name[n - 4] = '\0';
        }
        lv_obj_t *row = plain(rows);
        lv_obj_set_size(row, LV_PCT(100), 84);
        lv_obj_set_style_bg_color(row, th.card, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th.pressed, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, TH_RADIUS_SM, 0);
        lv_obj_set_style_pad_hor(row, TH_SPACE_MD, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, autoplay_pick_row, LV_EVENT_CLICKED, NULL);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, name);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);
        lv_obj_set_style_text_color(label, th.text, 0);
    }

    lv_obj_t *cancel = ui_pill_button(card, "Cancel", th.text2, false);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_add_event_cb(cancel, autoplay_popup_cancel, LV_EVENT_CLICKED, NULL);
}

static void autoplay_list_job(void *arg)
{
    (void)arg;
    fw_str_list_t list = {0};
    esp_err_t err = fw_get_playlists(&list);
    if (err != ESP_OK) {
        show_fw_error_locked("Couldn't load the playlists. ", err);
        return;
    }
    lvgl_port_lock(0);
    autoplay_show_picker(&list);
    lvgl_port_unlock();
    fw_str_list_free(&list);
}

static void autoplay_toggled(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool want_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    state_lock();
    bool connected = state_get()->conn == CONN_CONNECTED;
    state_unlock();
    if (!connected || s_autoplay_busy) {
        autoplay_reflect(s_autoplay_on);  // snap the knob back
        return;
    }
    if (!want_on) {
        autoplay_ctx_t *ctx = calloc(1, sizeof(*ctx));  // name "" clears
        if (ctx == NULL || jobs_submit(autoplay_set_job, ctx) != ESP_OK) {
            free(ctx);
            autoplay_reflect(s_autoplay_on);
            show_queue_full();
            return;
        }
        s_autoplay_busy = true;
        return;
    }
    // Turning ON needs a named playlist: revert the knob, open the picker
    autoplay_reflect(s_autoplay_on);
    if (jobs_submit(autoplay_list_job, NULL) != ESP_OK) {
        show_queue_full();
    }
}

static void build_table_card(lv_obj_t *column)
{
    lv_obj_t *card = make_card(column);
    make_section_label(card, "TABLE");

    lv_obj_t *row = make_hrow(card);
    lv_obj_t *home = make_table_btn(row, "Home", th.accent, ACT_HOME);
    lv_obj_set_flex_grow(home, 1);
    lv_obj_t *center = make_table_btn(row, "Center", th.accent, ACT_CENTER);
    lv_obj_set_flex_grow(center, 1);
    lv_obj_t *edge = make_table_btn(row, "Edge", th.accent, ACT_EDGE);
    lv_obj_set_flex_grow(edge, 1);

    make_divider(card);

    lv_obj_t *ap_row = make_hrow(card);
    lv_obj_t *ap_col = plain(ap_row);
    lv_obj_set_flex_grow(ap_col, 1);
    lv_obj_set_height(ap_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ap_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ap_col, 3, 0);
    lv_obj_t *ap_title = lv_label_create(ap_col);
    lv_label_set_text(ap_title, "Auto play");
    lv_obj_set_style_text_font(ap_title, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(ap_title, th.text, 0);
    lv_obj_t *ap_sub = lv_label_create(ap_col);
    lv_label_set_text(ap_sub, "Start playing when the table powers on");
    lv_obj_set_style_text_font(ap_sub, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(ap_sub, th.text3, 0);
    s_autoplay_sw = make_dw_switch(ap_row, false);
    lv_obj_add_event_cb(s_autoplay_sw, autoplay_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    make_divider(card);

    lv_obj_t *restart = make_table_btn(card, "Restart table", th.text2, ACT_RESTART);
    lv_obj_set_width(restart, LV_PCT(100));
}

// ---------------------------------------------------------------------------
// Card 5: THIS SCREEN
// ---------------------------------------------------------------------------

static void sleep_chip_set_selected(int idx, bool selected)
{
    lv_obj_t *chip = s_sleep_chips[idx];
    lv_obj_set_style_bg_color(chip, th.accent_soft, 0);
    lv_obj_set_style_bg_opa(chip, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    // QML: selected wins over pressed (pressedColor shows only when unselected)
    lv_obj_set_style_bg_color(chip, selected ? th.accent_soft : th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(chip, selected ? th.accent : th.border, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), selected ? th.accent : th.text2, 0);
}

static void sleep_chip_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < SLEEP_OPT_COUNT; i++) {
        sleep_chip_set_selected(i, i == idx);
    }
    settings_get()->screen_timeout_s = SLEEP_OPTS[idx].secs;
    if (jobs_submit(settings_save_job, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "settings save deferred: job queue full");
    }
    // Actually turning the backlight off after the timeout is a later
    // increment (board_backlight + first-touch swallow hook).
}

static void night_mode_toggled(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    settings_get()->dark_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (jobs_submit(settings_save_job, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "settings save deferred: job queue full");
    }
    // Takes effect on restart; live retheme is a later increment.
}

// ChoiceChip recipe: TH_TOUCH_TARGET pill; selected = accent_soft fill +
// accent border + accent text, else transparent + border + text2.
static lv_obj_t *make_choice_chip(lv_obj_t *parent, const char *text, int idx)
{
    lv_obj_t *chip = plain(parent);
    lv_obj_set_height(chip, TH_TOUCH_TARGET);
    lv_obj_set_flex_grow(chip, 1);
    lv_obj_set_style_radius(chip, TH_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_bg_color(chip, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chip, sleep_chip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, TH_FONT_CAPTION, 0);

    s_sleep_chips[idx] = chip;
    return chip;
}

// DwSwitch recipe: ember track when on, quiet when off (QML 56x32 -> 84x48)
static lv_obj_t *make_dw_switch(lv_obj_t *parent, bool checked)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 84, 48);
    lv_obj_set_style_bg_color(sw, th.pressed, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, th.border, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, th.accent_pressed, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, th.accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, th.text2, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, th.on_accent, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(sw, -6, LV_PART_KNOB);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static void build_screen_card(lv_obj_t *column)
{
    lv_obj_t *card = make_card(column);
    make_section_label(card, "THIS SCREEN");

    lv_obj_t *sleep_label = lv_label_create(card);
    lv_label_set_text(sleep_label, "Sleeps after");
    lv_obj_set_style_text_font(sleep_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(sleep_label, th.text2, 0);

    lv_obj_t *chips = make_hrow(card);
    uint32_t current = settings_get()->screen_timeout_s;
    for (int i = 0; i < SLEEP_OPT_COUNT; i++) {
        make_choice_chip(chips, SLEEP_OPTS[i].label, i);
        sleep_chip_set_selected(i, SLEEP_OPTS[i].secs == current);
    }

    make_divider(card);

    lv_obj_t *night_row = make_hrow(card);
    lv_obj_t *night_label = lv_label_create(night_row);
    lv_label_set_text(night_label, "Night mode");
    lv_obj_set_flex_grow(night_label, 1);
    lv_obj_set_style_text_font(night_label, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(night_label, th.text, 0);

    lv_obj_t *sw = make_dw_switch(night_row, settings_get()->dark_mode);
    lv_obj_add_event_cb(sw, night_mode_toggled, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---------------------------------------------------------------------------
// State listener: keeps card 2 (connection) and card 4 (table) in sync
// ---------------------------------------------------------------------------

static void on_state_changed(void)  // runs in the LVGL task (state.c locks)
{
    state_lock();
    app_state_t *st = state_get();
    conn_state_t conn = st->conn;
    char name[64];
    strlcpy(name, st->table_name, sizeof(name));
    char line[96];
    strlcpy(line, st->status_line, sizeof(line));
    state_unlock();

    bool connected = (conn == CONN_CONNECTED);

    // Card 2: status row
    if (connected) {
        lv_obj_set_style_bg_color(s_conn_row, th.ok_soft, 0);
        lv_obj_set_style_border_color(s_conn_row, th.ok, 0);
        lv_label_set_text(s_conn_title, name[0] != '\0' ? name : "Connected");
        lv_obj_set_style_text_color(s_conn_title, th.ok, 0);
        const char *url = fw_base_url();
        if (url != NULL && url[0] != '\0') {
            lv_label_set_text(s_conn_sub, url);
            lv_obj_remove_flag(s_conn_sub, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_conn_sub, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(s_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_color(s_conn_row, th.card, 0);
        lv_obj_set_style_border_color(s_conn_row, th.border, 0);
        lv_label_set_text(s_conn_title, "Not connected");
        lv_obj_set_style_text_color(s_conn_title, th.text2, 0);
        if (line[0] != '\0') {
            lv_label_set_text(s_conn_sub, line);
            lv_obj_remove_flag(s_conn_sub, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_conn_sub, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // Card 4: motion + restart gate on CONN_CONNECTED
    for (int i = 0; i < ACT_COUNT; i++) {
        set_btn_enabled(s_table_btns[i], connected);
    }

    // On the connect edge, read Playlist/Autostart (fw_load_settings already
    // ran in state's connect_edge) to seed the Auto play switch.
    static bool was_connected;
    if (connected && !was_connected) {
        if (jobs_submit(autoplay_seed_job, NULL) != ESP_OK) {
            ESP_LOGW(TAG, "autoplay seed deferred: job queue full");
        }
    }
    if (!connected) {
        autoplay_reflect(false);
    }
    was_connected = connected;
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

lv_obj_t *page_control_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_page_root(parent);
    ui_page_header(page, "Control");

    // One vertically scrolling body holding both columns (QML ScrollView)
    lv_obj_t *body = plain(page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_style_pad_hor(body, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_ver(body, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_column(body, TH_SPACE_LG, 0);

    lv_obj_t *left = plain(body);
    lv_obj_set_width(left, LV_PCT(47));
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, TH_SPACE_LG, 0);

    lv_obj_t *right = plain(body);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, TH_SPACE_LG, 0);

    build_wifi_card(left);
    build_conn_card(left);
    build_tables_card(left);
    build_table_card(right);
    build_screen_card(right);

    // ui_init() runs before state_init()/wifi_init() (see main.c), so these
    // only record callbacks; the first events arrive once those tasks start.
    state_add_listener(on_state_changed);
    wifi_set_event_cb(on_wifi_event);

    return page;
}
