// Playlists page: list of the table's SD playlists + an in-place detail view
// with pattern editing and run settings. Reference implementation:
// dune-weaver-pi/dune-weaver-touch/qml/pages/ModernPlaylistPage.qml.
// Contract: docs/PORTING_NOTES.md §1 (playlist txt format, $Playlist/*) + §5.
//
// Threading model: all fw_* calls run in jobs (jobs_submit); widgets are only
// touched from LVGL callbacks or from jobs under lvgl_port_lock(). Stale job
// results are discarded via generation counters (bumped/read under the lock).

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "../../app/jobs.h"
#include "../../app/state.h"
#include "../../net/fw_client.h"
#include "../../net/settings.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "page_playlists";

#define MAX_PLAYLISTS 40        // rows shown in the list view
#define MAX_PL_NAME 64          // playlist name (no .txt), matches fw name fields
#define DET_ROW_DISPLAY_MAX 150 // rows rendered in detail (array itself is uncapped)
#define ICON_BTN_SIZE 66        // QML 44 x 1.5 scale

// ---------------------------------------------------------------- page state

// List view
static lv_obj_t *s_list_view;
static lv_obj_t *s_list_cont;
static lv_obj_t *s_list_count_label;
static lv_obj_t *s_row_sub_labels[MAX_PLAYLISTS];
static char s_pl_names[MAX_PLAYLISTS][MAX_PL_NAME];
static int s_pl_count;
static uint32_t s_list_gen; // bumped on every reload request (under lvgl lock)

// Detail view
static lv_obj_t *s_det_view;
static lv_obj_t *s_det_title;
static lv_obj_t *s_det_count_label;
static lv_obj_t *s_det_list_cont;
static char s_det_name[MAX_PL_NAME];
static char **s_det_items; // owned raw playlist lines (SD paths)
static int s_det_count;
static uint32_t s_det_gen;
static bool s_showing_detail;
static bool s_det_busy;    // a rewrite is in flight; edits wait for the reload

// Run-settings chips
static lv_obj_t *s_chip_shuffle;
static lv_obj_t *s_mode_chips[2];
static lv_obj_t *s_pause_chips[12];
static lv_obj_t *s_clear_chips[4];

// Modal (one at a time: create dialog or delete confirm)
static lv_obj_t *s_modal_scrim;
static lv_obj_t *s_modal_ta;

static const char *MODE_VALUES[2] = {"loop", "single"};
static const char *MODE_LABELS[2] = {"Loop forever", "Play once"};

// PORTING_NOTES §5: rest-between options and their seconds.
static const struct {
    const char *label;
    int secs;
} PAUSE_OPTS[12] = {
    {"0 s", 0},     {"1 m", 60},     {"5 m", 300},    {"15 m", 900},
    {"30 m", 1800}, {"1 h", 3600},   {"2 h", 7200},   {"3 h", 10800},
    {"4 h", 14400}, {"5 h", 18000},  {"6 h", 21600},  {"12 h", 43200},
};

static const struct {
    const char *label;
    const char *value;
} CLEAR_OPTS[4] = {
    {"Adaptive clear", "adaptive"},
    {"Clear from center", "clear_center"},
    {"Clear from edge", "clear_perimeter"},
    {"Keep the sand", "none"},
};

static void request_list_reload(void);
static void request_det_reload(void);
static void chips_refresh(void);
static void list_row_clicked(lv_event_t *e);

// ------------------------------------------------------------ small helpers

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

static void trimmed_copy(char *dst, size_t dst_len, const char *src)
{
    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) {
        n--;
    }
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// "/Evening.txt" -> "Evening" (playlist display / API name)
static void strip_txt_copy(char *dst, size_t dst_len, const char *src)
{
    while (*src == '/') {
        src++;
    }
    strlcpy(dst, src, dst_len);
    size_t n = strlen(dst);
    if (n >= 4 && strcmp(dst + n - 4, ".txt") == 0) {
        dst[n - 4] = '\0';
    }
}

// "/patterns/sub/x.thr" -> "x" (pattern display name)
static void display_name_of(const char *raw, char *dst, size_t dst_len)
{
    const char *base = strrchr(raw, '/');
    base = (base != NULL) ? base + 1 : raw;
    strlcpy(dst, base, dst_len);
    size_t n = strlen(dst);
    if (n >= 4 && strcmp(dst + n - 4, ".thr") == 0) {
        dst[n - 4] = '\0';
    }
}

// PORTING_NOTES §1: "/patterns/…" pass; "/sd/…" strip "/sd"; else prepend.
static void normalize_sd_path(const char *in, char *out, size_t out_len)
{
    if (strncmp(in, "/patterns/", 10) == 0) {
        strlcpy(out, in, out_len);
    } else if (strncmp(in, "/sd/", 4) == 0) {
        strlcpy(out, in + 3, out_len);
    } else {
        while (*in == '/') {
            in++;
        }
        snprintf(out, out_len, "/patterns/%s", in);
    }
}

// Walk playlist file bytes (not NUL-terminated); skip blanks and '#' comments.
// items == NULL: count only. Otherwise fill up to max_items heap copies.
static int playlist_scan(const char *buf, size_t len, char **items, int max_items)
{
    int count = 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        while (start < end && isspace((unsigned char)buf[start])) {
            start++;
        }
        while (end > start && isspace((unsigned char)buf[end - 1])) {
            end--;
        }
        if (end == start || buf[start] == '#') {
            continue;
        }
        if (items != NULL) {
            if (count >= max_items) {
                break;
            }
            size_t n = end - start;
            char *copy = malloc(n + 1);
            if (copy == NULL) {
                break;
            }
            memcpy(copy, buf + start, n);
            copy[n] = '\0';
            items[count] = copy;
        }
        count++;
    }
    return count;
}

static void free_str_array(char **items, int count)
{
    if (items == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static int cmp_playlist_names(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcasecmp(sa, sb);
}

// Call under lvgl_port_lock.
static void show_error_prefixed(const char *prefix, esp_err_t err)
{
    char msg[192];
    snprintf(msg, sizeof(msg), "%s %s", prefix, fw_friendly_error(err));
    ui_show_error(msg);
}

// From LVGL callbacks only. Caller frees ctx (and nested buffers) on false.
static bool submit_or_report(job_fn_t fn, void *ctx)
{
    if (jobs_submit(fn, ctx) == ESP_OK) {
        return true;
    }
    ESP_LOGW(TAG, "job queue full");
    ui_show_error("The panel is busy right now. Try that again in a moment.");
    return false;
}

// --------------------------------------------------------- widget recipes

static lv_obj_t *eyebrow(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_color(label, th.text3, 0);
    lv_obj_set_style_text_letter_space(label, 2, 0);
    return label;
}

// Circular icon button, transparent until pressed (QML 44px circles x 1.5).
static lv_obj_t *icon_circle(lv_obj_t *parent, const char *symbol, lv_color_t color)
{
    lv_obj_t *btn = plain(parent);
    lv_obj_set_size(btn, ICON_BTN_SIZE, ICON_BTN_SIZE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(icon, color, 0);
    lv_obj_center(icon);
    return btn;
}

// ChoiceChip recipe (qml/components/ChoiceChip.qml, x 1.5 scale).
static lv_obj_t *chip_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *chip = plain(parent);
    lv_obj_set_height(chip, ICON_BTN_SIZE);
    lv_obj_set_style_radius(chip, TH_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, th.border, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(chip, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(label, th.text2, 0);
    lv_obj_center(label);
    return chip;
}

static void chip_set_selected(lv_obj_t *chip, bool selected)
{
    lv_obj_set_style_bg_color(chip, th.accent_soft, 0);
    lv_obj_set_style_bg_opa(chip, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(chip, selected ? th.accent : th.border, 0);
    // No pressed flash on an already-selected chip (matches ChoiceChip.qml).
    lv_obj_set_style_bg_color(chip, selected ? th.accent_soft : th.pressed, LV_STATE_PRESSED);
    lv_obj_t *label = lv_obj_get_child(chip, 0);
    lv_obj_set_style_text_color(label, selected ? th.accent : th.text2, 0);
}

static lv_obj_t *chip_row(lv_obj_t *parent)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, TH_SPACE_SM, 0);
    return row;
}

// ---------------------------------------------------------- run settings

static void save_settings_job(void *arg)
{
    (void)arg;
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
}

// NVS commit can stall; keep it off the LVGL task.
static void schedule_settings_save(void)
{
    if (jobs_submit(save_settings_job, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "job queue full; settings save dropped");
    }
}

static void chips_refresh(void)
{
    if (s_chip_shuffle == NULL) {
        return;
    }
    app_settings_t *st = settings_get();

    static struct {
        uint32_t pause;
        bool shuffle;
        char mode[8];
        char clear[20];
        bool valid;
    } last;
    if (last.valid && last.pause == st->pause_between_s && last.shuffle == st->playlist_shuffle &&
        strcmp(last.mode, st->playlist_run_mode) == 0 && strcmp(last.clear, st->playlist_clear) == 0) {
        return; // called from the 1 Hz state listener; skip redundant restyles
    }
    last.pause = st->pause_between_s;
    last.shuffle = st->playlist_shuffle;
    strlcpy(last.mode, st->playlist_run_mode, sizeof(last.mode));
    strlcpy(last.clear, st->playlist_clear, sizeof(last.clear));
    last.valid = true;

    chip_set_selected(s_chip_shuffle, st->playlist_shuffle);
    for (int i = 0; i < 2; i++) {
        chip_set_selected(s_mode_chips[i], strcmp(st->playlist_run_mode, MODE_VALUES[i]) == 0);
    }
    for (int i = 0; i < 12; i++) {
        chip_set_selected(s_pause_chips[i], st->pause_between_s == (uint32_t)PAUSE_OPTS[i].secs);
    }
    for (int i = 0; i < 4; i++) {
        chip_set_selected(s_clear_chips[i], strcmp(st->playlist_clear, CLEAR_OPTS[i].value) == 0);
    }
}

// These persist locally and ride along with fw_run_playlist; they are NOT
// pushed to the board on tap (board NVS wins on connect — PORTING_NOTES §3).
static void shuffle_clicked(lv_event_t *e)
{
    (void)e;
    app_settings_t *st = settings_get();
    st->playlist_shuffle = !st->playlist_shuffle;
    chips_refresh();
    schedule_settings_save();
}

static void mode_clicked(lv_event_t *e)
{
    const char *value = lv_event_get_user_data(e);
    strlcpy(settings_get()->playlist_run_mode, value, sizeof(settings_get()->playlist_run_mode));
    chips_refresh();
    schedule_settings_save();
}

static void pause_clicked(lv_event_t *e)
{
    settings_get()->pause_between_s = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    chips_refresh();
    schedule_settings_save();
}

static void clear_clicked(lv_event_t *e)
{
    const char *value = lv_event_get_user_data(e);
    strlcpy(settings_get()->playlist_clear, value, sizeof(settings_get()->playlist_clear));
    chips_refresh();
    schedule_settings_save();
}

// ------------------------------------------------------------------ modals

static void modal_close(void)
{
    if (s_modal_scrim != NULL) {
        // Async: the close may originate from an event of a child (keyboard).
        lv_obj_delete_async(s_modal_scrim);
        s_modal_scrim = NULL;
        s_modal_ta = NULL;
    }
}

static void modal_cancel_clicked(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static lv_obj_t *modal_card(int width, lv_align_t align, int y_ofs)
{
    s_modal_scrim = plain(lv_layer_top());
    lv_obj_set_size(s_modal_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(s_modal_scrim, LV_OBJ_FLAG_CLICKABLE); // swallow taps behind

    lv_obj_t *card = plain(s_modal_scrim);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, align, 0, y_ofs);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_LG, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, TH_SPACE_MD, 0);
    return card;
}

static lv_obj_t *modal_title(lv_obj_t *card, const char *text)
{
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(label, th.text, 0);
    return label;
}

// --- New playlist ---

typedef struct {
    char name[MAX_PL_NAME];
} create_ctx_t;

static void create_job(void *arg)
{
    create_ctx_t *ctx = arg;
    char content[MAX_PL_NAME + 8];
    snprintf(content, sizeof(content), "# %s\n", ctx->name);
    esp_err_t err = fw_upload_playlist(ctx->name, content);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "create playlist '%s' failed: %s", ctx->name, esp_err_to_name(err));
        lvgl_port_lock(0);
        show_error_prefixed("Couldn't create the playlist.", err);
        lvgl_port_unlock();
    } else {
        request_list_reload();
    }
    free(ctx);
}

static void create_confirmed(lv_event_t *e)
{
    (void)e;
    if (s_modal_ta == NULL) {
        return;
    }
    char name[MAX_PL_NAME];
    trimmed_copy(name, sizeof(name), lv_textarea_get_text(s_modal_ta));
    if (name[0] == '\0') {
        return; // "Create" is a no-op until a name is typed (QML disables it)
    }
    create_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    strlcpy(ctx->name, name, sizeof(ctx->name));
    modal_close();
    if (!submit_or_report(create_job, ctx)) {
        free(ctx);
    }
}

static void create_modal_open(lv_event_t *e)
{
    (void)e;
    if (s_modal_scrim != NULL) {
        return;
    }
    // Card sits high so the keyboard fits underneath.
    lv_obj_t *card = modal_card(640, LV_ALIGN_TOP_MID, TH_SPACE_SM);
    modal_title(card, "New playlist");

    lv_obj_t *ta = lv_textarea_create(card);
    s_modal_ta = ta;
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Playlist name");
    lv_textarea_set_max_length(ta, MAX_PL_NAME - 1);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, TH_TOUCH_TARGET);
    lv_obj_set_style_bg_color(ta, th.bg, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, TH_TOUCH_TARGET / 2, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, th.border, 0);
    lv_obj_set_style_border_color(ta, th.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(ta, TH_SPACE_LG, 0);
    lv_obj_set_style_text_font(ta, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(ta, th.text, 0);
    lv_obj_set_style_text_color(ta, th.text3, LV_PART_TEXTAREA_PLACEHOLDER);

    lv_obj_t *btn_row = chip_row(card);
    lv_obj_t *cancel = ui_pill_button(btn_row, "Cancel", th.text2, false);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, modal_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *create = ui_pill_button(btn_row, "Create", th.accent, true);
    lv_obj_set_flex_grow(create, 1);
    lv_obj_add_event_cb(create, create_confirmed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = ui_keyboard_create(s_modal_scrim);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, create_confirmed, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, modal_cancel_clicked, LV_EVENT_CANCEL, NULL);
}

// --- Delete playlist ---

static void show_list_view(void)
{
    s_showing_detail = false;
    s_det_gen++; // invalidate any in-flight detail load
    lv_obj_add_flag(s_det_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
}

typedef struct {
    char name[MAX_PL_NAME];
} delete_ctx_t;

static void delete_job(void *arg)
{
    delete_ctx_t *ctx = arg;
    esp_err_t err = fw_delete_playlist(ctx->name);
    lvgl_port_lock(0);
    if (err != ESP_OK) {
        show_error_prefixed("Couldn't delete the playlist.", err);
    } else {
        show_list_view();
    }
    lvgl_port_unlock();
    if (err == ESP_OK) {
        request_list_reload();
    }
    free(ctx);
}

static void delete_confirmed(lv_event_t *e)
{
    (void)e;
    delete_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    strlcpy(ctx->name, s_det_name, sizeof(ctx->name));
    modal_close();
    if (!submit_or_report(delete_job, ctx)) {
        free(ctx);
    }
}

static void delete_modal_open(lv_event_t *e)
{
    (void)e;
    if (s_modal_scrim != NULL) {
        return;
    }
    lv_obj_t *card = modal_card(640, LV_ALIGN_CENTER, 0);
    modal_title(card, "Delete playlist?");

    char body[MAX_PL_NAME + 96];
    snprintf(body, sizeof(body),
             "\"%s\" will be removed from the table. Its patterns stay in your library.", s_det_name);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, body);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, th.text2, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *btn_row = chip_row(card);
    lv_obj_t *cancel = ui_pill_button(btn_row, "Cancel", th.text2, false);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, modal_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del = ui_pill_button(btn_row, "Delete", th.danger, true);
    lv_obj_set_flex_grow(del, 1);
    lv_obj_add_event_cb(del, delete_confirmed, LV_EVENT_CLICKED, NULL);
}

// ------------------------------------------------------------- detail view

static void free_det_items(void)
{
    free_str_array(s_det_items, s_det_count);
    s_det_items = NULL;
    s_det_count = 0;
}

typedef struct {
    char name[MAX_PL_NAME];
    char *content;
} rewrite_ctx_t;

static void playlist_rewrite_job(void *arg)
{
    rewrite_ctx_t *ctx = arg;
    esp_err_t err = fw_upload_playlist(ctx->name, ctx->content);
    bool reload_detail = false;
    lvgl_port_lock(0);
    if (err != ESP_OK) {
        s_det_busy = false;  // failed rewrite: allow the next edit
        show_error_prefixed("Couldn't update the playlist.", err);
    } else {
        reload_detail = s_showing_detail && strcmp(ctx->name, s_det_name) == 0;
        if (!reload_detail) {
            s_det_busy = false;
        }
    }
    lvgl_port_unlock();
    if (err == ESP_OK) {
        if (reload_detail) {
            request_det_reload();
        }
        request_list_reload(); // pattern counts changed
    }
    free(ctx->content);
    free(ctx);
}

// Rewrite = whole file: "# <name>" header + every remaining normalized line.
static char *build_content_without(int skip_idx)
{
    size_t cap = strlen(s_det_name) + 8;
    for (int i = 0; i < s_det_count; i++) {
        if (i == skip_idx) {
            continue;
        }
        cap += strlen(s_det_items[i]) + 16; // slack for "/patterns" prefix + '\n'
    }
    char *content = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (content == NULL) {
        content = malloc(cap);
    }
    if (content == NULL) {
        return NULL;
    }
    size_t off = (size_t)snprintf(content, cap, "# %s\n", s_det_name);
    for (int i = 0; i < s_det_count && off < cap; i++) {
        if (i == skip_idx) {
            continue;
        }
        char norm[192];
        normalize_sd_path(s_det_items[i], norm, sizeof(norm));
        off += (size_t)snprintf(content + off, cap - off, "%s\n", norm);
    }
    return content;
}

static void det_remove_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_det_count || s_det_items == NULL) {
        return;
    }
    if (s_det_busy) {
        // A rewrite built from the current snapshot is still in flight; a
        // second one would be built from stale items and resurrect the first
        // removal. Ignore taps until the detail reload lands.
        return;
    }
    rewrite_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    strlcpy(ctx->name, s_det_name, sizeof(ctx->name));
    ctx->content = build_content_without(idx);
    if (ctx->content == NULL) {
        free(ctx);
        return;
    }
    if (!submit_or_report(playlist_rewrite_job, ctx)) {
        free(ctx->content);
        free(ctx);
        return;
    }
    s_det_busy = true;
}

// Runs under lvgl lock (job) or in the LVGL task.
static void det_row_create(int idx)
{
    lv_obj_t *row = plain(s_det_list_cont);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 72); // QML 48 x 1.5
    lv_obj_set_style_bg_color(row, th.card, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, TH_RADIUS_SM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, th.border_light, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_right(row, TH_SPACE_XS, 0);
    lv_obj_set_style_pad_column(row, TH_SPACE_SM, 0);

    lv_obj_t *num = lv_label_create(row);
    lv_label_set_text_fmt(num, "%d", idx + 1);
    lv_obj_set_width(num, 30);
    lv_obj_set_style_text_font(num, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(num, th.text3, 0);
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);

    char shown[96];
    display_name_of(s_det_items[idx], shown, sizeof(shown));
    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, shown);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(name, th.text, 0);

    lv_obj_t *remove = plain(row);
    lv_obj_set_size(remove, 60, 60);
    lv_obj_set_style_radius(remove, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(remove, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(remove, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(remove, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(remove, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(remove, det_remove_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *x = lv_label_create(remove);
    lv_label_set_text(x, TH_ICON_CLOSE);
    lv_obj_set_style_text_font(x, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(x, th.text3, 0);
    lv_obj_center(x);
}

static void det_rebuild(void)
{
    lv_obj_clean(s_det_list_cont);
    lv_label_set_text_fmt(s_det_count_label, "%d patterns", s_det_count);
    if (s_det_count == 0) {
        ui_empty_state(s_det_list_cont, TH_ICON_MUSIC_NOTE, "No patterns yet", NULL);
        return;
    }
    int shown = (s_det_count < DET_ROW_DISPLAY_MAX) ? s_det_count : DET_ROW_DISPLAY_MAX;
    for (int i = 0; i < shown; i++) {
        det_row_create(i);
    }
    if (s_det_count > shown) {
        lv_obj_t *more = lv_label_create(s_det_list_cont);
        lv_label_set_text_fmt(more, "+%d more", s_det_count - shown);
        lv_obj_set_style_text_font(more, TH_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(more, th.text3, 0);
    }
}

// Takes ownership of items. Call under lvgl lock.
static void det_apply(char **items, int count)
{
    free_det_items();
    s_det_items = items;
    s_det_count = count;
    s_det_busy = false;  // fresh snapshot on screen; edits are safe again
    det_rebuild();
}

typedef struct {
    uint32_t gen;
    char name[MAX_PL_NAME];
} det_job_ctx_t;

static void det_load_job(void *arg)
{
    det_job_ctx_t *ctx = arg;
    char path[96];
    snprintf(path, sizeof(path), "/playlists/%s.txt", ctx->name);

    char *buf = NULL;
    size_t len = 0;
    esp_err_t err = fw_fetch_sd(path, &buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch %s failed: %s", path, esp_err_to_name(err));
        lvgl_port_lock(0);
        if (ctx->gen == s_det_gen) {
            det_apply(NULL, 0);
            show_error_prefixed("Couldn't load the playlist.", err);
        }
        lvgl_port_unlock();
        free(ctx);
        return;
    }

    int total = playlist_scan(buf, len, NULL, 0);
    char **items = NULL;
    if (total > 0) {
        items = calloc((size_t)total, sizeof(char *));
        total = (items != NULL) ? playlist_scan(buf, len, items, total) : 0;
    }
    free(buf);

    lvgl_port_lock(0);
    if (ctx->gen == s_det_gen) {
        det_apply(items, total);
    } else {
        free_str_array(items, total);
    }
    lvgl_port_unlock();
    free(ctx);
}

static void request_det_reload(void)
{
    det_job_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    lvgl_port_lock(0);
    ctx->gen = ++s_det_gen;
    strlcpy(ctx->name, s_det_name, sizeof(ctx->name));
    lvgl_port_unlock();
    if (jobs_submit(det_load_job, ctx) != ESP_OK) {
        ESP_LOGW(TAG, "job queue full; detail load dropped");
        free(ctx);
    }
}

// LVGL task only.
static void detail_open(const char *name)
{
    strlcpy(s_det_name, name, sizeof(s_det_name));
    s_showing_detail = true;
    lv_label_set_text(s_det_title, name);
    lv_label_set_text(s_det_count_label, "");
    free_det_items();
    lv_obj_clean(s_det_list_cont);

    lv_obj_t *loading = lv_label_create(s_det_list_cont);
    lv_label_set_text(loading, "Loading...");
    lv_obj_set_style_text_font(loading, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(loading, th.text3, 0);

    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_det_view, LV_OBJ_FLAG_HIDDEN);
    chips_refresh();
    request_det_reload();
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    show_list_view();
}

// --- Weave this playlist ---

typedef struct {
    char name[MAX_PL_NAME];
    int pause_s;
    char clear[20];
    char mode[8];
    bool shuffle;
} weave_ctx_t;

static void weave_job(void *arg)
{
    weave_ctx_t *ctx = arg;
    esp_err_t err = fw_run_playlist(ctx->name, ctx->pause_s, ctx->clear, ctx->mode, ctx->shuffle);
    lvgl_port_lock(0);
    if (err == ESP_OK) {
        ui_navigate_to(UI_TAB_NOW_PLAYING);
    } else {
        show_error_prefixed("Couldn't start the playlist.", err);
    }
    lvgl_port_unlock();
    if (err == ESP_OK) {
        state_poll_now();
    }
    free(ctx);
}

static void weave_clicked(lv_event_t *e)
{
    (void)e;
    weave_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    app_settings_t *st = settings_get();
    strlcpy(ctx->name, s_det_name, sizeof(ctx->name));
    ctx->pause_s = (int)st->pause_between_s;
    strlcpy(ctx->clear, st->playlist_clear, sizeof(ctx->clear));
    strlcpy(ctx->mode, st->playlist_run_mode, sizeof(ctx->mode));
    ctx->shuffle = st->playlist_shuffle;
    if (!submit_or_report(weave_job, ctx)) {
        free(ctx);
    }
}

// -------------------------------------------------------------- list view

// Returns the "<n> patterns" sub-label so the lazy count pass can fill it.
static lv_obj_t *list_row_create(int idx)
{
    lv_obj_t *row = plain(s_list_cont);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 114); // QML 76 x 1.5
    lv_obj_set_style_bg_color(row, th.surface, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, th.border, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_column(row, TH_SPACE_LG, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, list_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *badge = plain(row);
    lv_obj_set_size(badge, ICON_BTN_SIZE, ICON_BTN_SIZE);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, th.accent_soft, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_t *icon = lv_label_create(badge);
    lv_label_set_text(icon, TH_ICON_MUSIC_NOTE);
    lv_obj_set_style_text_font(icon, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(icon, th.accent, 0);
    lv_obj_center(icon);

    lv_obj_t *col = plain(row);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 3, 0);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, s_pl_names[idx]);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(name, th.text, 0);

    lv_obj_t *sub = lv_label_create(col);
    lv_label_set_text(sub, ""); // filled lazily once the file is read
    lv_obj_set_style_text_font(sub, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(sub, th.text2, 0);
    return sub;
}

static void list_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_pl_count) {
        return;
    }
    detail_open(s_pl_names[idx]);
}

// Runs under lvgl lock. Copies names out of the fw list; widgets keep no
// pointers into it.
static void list_rebuild(const fw_str_list_t *list)
{
    lv_obj_clean(s_list_cont);
    memset(s_row_sub_labels, 0, sizeof(s_row_sub_labels));
    s_pl_count = 0;
    lv_label_set_text_fmt(s_list_count_label, "%d playlists", list->count);

    if (list->count == 0) {
        ui_empty_state(s_list_cont, TH_ICON_MUSIC_NOTE, "No playlists yet",
                       "Tap + to gather patterns into a set\nthe table can weave through");
        return;
    }
    int shown = (list->count < MAX_PLAYLISTS) ? list->count : MAX_PLAYLISTS;
    for (int i = 0; i < shown; i++) {
        strip_txt_copy(s_pl_names[i], sizeof(s_pl_names[i]), list->items[i]);
        s_pl_count = i + 1;
        s_row_sub_labels[i] = list_row_create(i);
    }
}

typedef struct {
    uint32_t gen;
} list_job_ctx_t;

static void list_load_job(void *arg)
{
    list_job_ctx_t *ctx = arg;
    fw_str_list_t list = {0};
    esp_err_t err = fw_get_playlists(&list);
    if (err != ESP_OK) {
        // Connect-edge refresh; keep whatever is on screen (reference app
        // logs a warning and keeps old data too). Mutation errors popped
        // their own dialog already.
        ESP_LOGW(TAG, "playlist list load failed: %s", esp_err_to_name(err));
        free(ctx);
        return;
    }
    if (list.count > 1) {
        qsort(list.items, (size_t)list.count, sizeof(char *), cmp_playlist_names);
    }

    lvgl_port_lock(0);
    bool fresh = (ctx->gen == s_list_gen);
    if (fresh) {
        list_rebuild(&list);
    }
    lvgl_port_unlock();

    // Lazy "<n> patterns" fill: playlist files are tiny, but the board serves
    // them slowly — fetch one at a time and bail as soon as the list reloads.
    // Capped, and the FIRST failure aborts the pass: on a busy board this
    // costs one timeout, never a timeout per row (the rows keep their names).
    for (int i = 0; fresh && i < list.count && i < 12; i++) {
        char name[MAX_PL_NAME];
        strip_txt_copy(name, sizeof(name), list.items[i]);
        char path[96];
        snprintf(path, sizeof(path), "/playlists/%s.txt", name);
        char *buf = NULL;
        size_t len = 0;
        if (fw_fetch_sd(path, &buf, &len) != ESP_OK) {
            break;
        }
        int n = playlist_scan(buf, len, NULL, 0);
        free(buf);
        lvgl_port_lock(0);
        fresh = (ctx->gen == s_list_gen);
        if (fresh && i < s_pl_count && s_row_sub_labels[i] != NULL) {
            lv_label_set_text_fmt(s_row_sub_labels[i], "%d patterns", n);
        }
        lvgl_port_unlock();
    }
    fw_str_list_free(&list);
    free(ctx);
}

static void request_list_reload(void)
{
    list_job_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    lvgl_port_lock(0);
    ctx->gen = ++s_list_gen;
    lvgl_port_unlock();
    if (jobs_submit(list_load_job, ctx) != ESP_OK) {
        ESP_LOGW(TAG, "job queue full; playlist reload dropped");
        free(ctx);
    }
}

// ------------------------------------------------------------ construction

static void build_list_view(lv_obj_t *page)
{
    s_list_view = plain(page);
    lv_obj_set_size(s_list_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_list_view, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = ui_page_header(s_list_view, "Playlists");

    s_list_count_label = lv_label_create(header);
    lv_label_set_text(s_list_count_label, "0 playlists");
    lv_obj_set_style_text_font(s_list_count_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_list_count_label, th.text3, 0);

    lv_obj_t *spacer = plain(header);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);

    lv_obj_t *add = plain(header);
    lv_obj_set_size(add, ICON_BTN_SIZE, ICON_BTN_SIZE);
    lv_obj_set_style_radius(add, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(add, th.accent, 0);
    lv_obj_set_style_bg_opa(add, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(add, th.accent_pressed, LV_STATE_PRESSED);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, create_modal_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, TH_ICON_ADD);
    lv_obj_set_style_text_font(plus, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(plus, th.on_accent, 0);
    lv_obj_center(plus);

    s_list_cont = plain(s_list_view);
    lv_obj_set_width(s_list_cont, LV_PCT(100));
    lv_obj_set_flex_grow(s_list_cont, 1);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list_cont, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_row(s_list_cont, TH_SPACE_MD, 0);
    lv_obj_set_scroll_dir(s_list_cont, LV_DIR_VER);

    ui_empty_state(s_list_cont, TH_ICON_MUSIC_NOTE, "No playlists yet",
                   "Tap + to gather patterns into a set\nthe table can weave through");
}

static void build_detail_view(lv_obj_t *page)
{
    s_det_view = plain(page);
    lv_obj_set_size(s_det_view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_det_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_det_view, LV_OBJ_FLAG_HIDDEN);

    // Header: back arrow, name, count, delete (ModernPlaylistPage detail bar)
    lv_obj_t *header = plain(s_det_view);
    lv_obj_set_size(header, LV_PCT(100), TH_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header, th.surface, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, th.border, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_right(header, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_column(header, TH_SPACE_SM, 0);

    lv_obj_t *back = icon_circle(header, TH_ICON_BACK, th.text);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    s_det_title = lv_label_create(header);
    lv_label_set_text(s_det_title, "");
    lv_obj_set_flex_grow(s_det_title, 1);
    lv_label_set_long_mode(s_det_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_det_title, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_det_title, th.text, 0);

    s_det_count_label = lv_label_create(header);
    lv_label_set_text(s_det_count_label, "");
    lv_obj_set_style_text_font(s_det_count_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_det_count_label, th.text3, 0);

    lv_obj_t *del = icon_circle(header, TH_ICON_DELETE, th.danger);
    lv_obj_add_event_cb(del, delete_modal_open, LV_EVENT_CLICKED, NULL);

    // Content: pattern list (~40%) | divider | run controls
    lv_obj_t *content = plain(s_det_view);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

    lv_obj_t *left = plain(content);
    lv_obj_set_size(left, LV_PCT(40), LV_PCT(100));
    lv_obj_set_style_bg_color(left, th.surface, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_row(left, TH_SPACE_MD, 0);

    eyebrow(left, "PATTERNS");

    s_det_list_cont = plain(left);
    lv_obj_set_width(s_det_list_cont, LV_PCT(100));
    lv_obj_set_flex_grow(s_det_list_cont, 1);
    lv_obj_set_flex_flow(s_det_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_det_list_cont, TH_SPACE_SM, 0);
    lv_obj_set_scroll_dir(s_det_list_cont, LV_DIR_VER);

    lv_obj_t *divider = plain(content);
    lv_obj_set_size(divider, 1, LV_PCT(100));
    lv_obj_set_style_bg_color(divider, th.border, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *right = plain(content);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_row(right, TH_SPACE_MD, 0);

    // Weave + shuffle
    lv_obj_t *top_row = chip_row(right);
    lv_obj_t *weave = ui_pill_button(top_row, TH_ICON_PLAY "  Weave this playlist", th.accent, true);
    lv_obj_set_flex_grow(weave, 1);
    lv_obj_set_height(weave, TH_CONTROL_HEIGHT);
    lv_obj_add_event_cb(weave, weave_clicked, LV_EVENT_CLICKED, NULL);

    s_chip_shuffle = chip_create(top_row, "Shuffle");
    lv_obj_set_size(s_chip_shuffle, 165, TH_CONTROL_HEIGHT); // QML 110 x 1.5
    lv_obj_add_event_cb(s_chip_shuffle, shuffle_clicked, LV_EVENT_CLICKED, NULL);

    // Scrollable settings
    lv_obj_t *scroll = plain(right);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scroll, TH_SPACE_MD, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);

    eyebrow(scroll, "PLAY ORDER");
    lv_obj_t *mode_row = chip_row(scroll);
    for (int i = 0; i < 2; i++) {
        s_mode_chips[i] = chip_create(mode_row, MODE_LABELS[i]);
        lv_obj_set_flex_grow(s_mode_chips[i], 1);
        lv_obj_add_event_cb(s_mode_chips[i], mode_clicked, LV_EVENT_CLICKED, (void *)MODE_VALUES[i]);
    }

    eyebrow(scroll, "REST BETWEEN PATTERNS");
    for (int r = 0; r < 2; r++) {
        lv_obj_t *row = chip_row(scroll);
        for (int c = 0; c < 6; c++) {
            int i = r * 6 + c;
            s_pause_chips[i] = chip_create(row, PAUSE_OPTS[i].label);
            lv_obj_set_flex_grow(s_pause_chips[i], 1);
            lv_obj_add_event_cb(s_pause_chips[i], pause_clicked, LV_EVENT_CLICKED,
                                (void *)(intptr_t)PAUSE_OPTS[i].secs);
        }
    }

    eyebrow(scroll, "CLEAR BEFORE EACH PATTERN");
    for (int r = 0; r < 2; r++) {
        lv_obj_t *row = chip_row(scroll);
        for (int c = 0; c < 2; c++) {
            int i = r * 2 + c;
            s_clear_chips[i] = chip_create(row, CLEAR_OPTS[i].label);
            lv_obj_set_flex_grow(s_clear_chips[i], 1);
            lv_obj_add_event_cb(s_clear_chips[i], clear_clicked, LV_EVENT_CLICKED,
                                (void *)CLEAR_OPTS[i].value);
        }
    }

    chips_refresh();
}

// ------------------------------------------------------------- state hook

// Runs with the LVGL lock held (state contract). Reload the list on the
// connect edge; refresh chips because state seeds settings from board NVS.
static void on_state_changed(void)
{
    static bool was_connected = false;
    state_lock();
    bool connected = (state_get()->conn == CONN_CONNECTED);
    state_unlock();
    if (connected && !was_connected) {
        request_list_reload();
    }
    was_connected = connected;
    chips_refresh();
}

lv_obj_t *page_playlists_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_page_root(parent);
    build_list_view(page);
    build_detail_view(page);
    state_add_listener(on_state_changed);
    return page;
}
