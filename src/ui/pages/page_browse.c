// Browse: pattern grid with lazy circular previews, search-on-Enter, and a
// full-screen pattern detail overlay (clear-mode chips + weave / add-to-
// playlist). Reference: qml/pages/ModernPatternListPage.qml and
// PatternDetailPage.qml; contract in docs/PORTING_NOTES.md §5 "Browse" + §6.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "../../app/jobs.h"
#include "../../app/sd_catalog.h"
#include "../../app/state.h"
#include "../../net/fw_client.h"
#include "../../render/thr_preview.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "page_browse";

// Four columns like the reference grid (QML shows 4 cells across 800; at
// 1024 that's 230 px cards — also the closer match to the QML cell's
// physical size on this 237 PPI panel).
#define CARD_W 230
#define CARD_H 258
#define CARD_PREVIEW_PX 204
#define DETAIL_PREVIEW_PX 480
#define PREVIEW_SRC_PX 300  // one render size app-wide (thr_preview.h note)
#define PREVIEW_TICK_MS 600
#define PREVIEW_MAX_ATTEMPTS 3
#define ADDED_FEEDBACK_MS 2000
// Cards are built in chunks behind a "Show more" card: real tables carry
// 1000+ patterns, and a card per pattern exhausts internal RAM (every small
// LVGL alloc is internal via SPIRAM_MALLOC_ALWAYSINTERNAL) — the WiFi driver
// then starves ("wifi:m f null" storms, red dot). See STATE.md 2026-08-25.
#define GRID_CHUNK 48

// Preview pipeline state, packed into the preview slot's user_data:
// low 4 bits = state, upper bits = fetch attempts so far.
enum { PV_NONE = 0, PV_INFLIGHT, PV_DONE, PV_FAILED };
#define PV_STATE(ud) ((int)((intptr_t)(ud)) & 0xF)
#define PV_ATTEMPTS(ud) ((int)((intptr_t)(ud)) >> 4)
#define PV_PACK(st, at) ((void *)(intptr_t)(((at) << 4) | (st)))

enum { PV_KIND_CARD = 0, PV_KIND_DETAIL };

typedef struct {
    int kind;        // PV_KIND_*
    int gen;         // s_generation (card) or s_detail_gen (detail) at submit
    lv_obj_t *card;  // card kind only; valid iff gen still matches
    char rel[128];
} preview_ctx_t;

typedef struct {
    int gen;
    char rel[128];
    char clear[20];  // adaptive|clear_center|clear_perimeter|none
} run_ctx_t;

typedef struct {
    int gen;
    char name[64];  // playlist name, no .txt
    char rel[128];
} add_ctx_t;

static const struct {
    const char *label;
    const char *value;
} CLEAR_MODES[4] = {
    {"Adaptive clear", "adaptive"},
    {"Clear from center", "clear_center"},
    {"Clear from edge", "clear_perimeter"},
    {"Keep the sand", "none"},
};

// ---- data (owned by the page; only touched in LVGL context / under lock) ----
static fw_str_list_t s_list;
static bool s_loaded;        // first successful fetch happened
static bool s_loading;       // a load job is queued/in flight
static bool s_from_sd;       // s_list came from the local SD manifest
static int s_built;          // pattern cards currently in the grid (chunking)
static char s_filter[64];    // applied search text (Enter / focus loss only)
static int s_generation;     // bumped on every grid rebuild (job guards)

static bool s_preview_pending;  // at most ONE preview job in flight
// After a failed fetch, no new preview jobs for 10 s (PORTING_NOTES §6 —
// hammering an unreachable/busy board would starve the status poll).
#define PREVIEW_BACKOFF_MS 10000
static uint32_t s_backoff_at;
static bool s_backoff_armed;

// ---- widgets ----
static lv_obj_t *s_page;
static lv_obj_t *s_grid;
static lv_obj_t *s_empty;
static lv_obj_t *s_sd_banner;  // "insert the pattern card" complaint strip
static lv_obj_t *s_count_label;
static lv_obj_t *s_refresh_icon;
static lv_obj_t *s_search_ta;
static lv_obj_t *s_kb;

// ---- detail overlay ----
static lv_obj_t *s_detail;      // full-screen overlay on lv_layer_top
static lv_obj_t *s_detail_slot; // 480px preview circle
static lv_obj_t *s_detail_add_btn;
static lv_obj_t *s_clear_chips;
static int s_detail_gen;        // bumped on open and close (job guards)
static bool s_detail_have_img;
static int s_detail_attempts;
static char s_detail_rel[128];
static int s_clear_idx;
static bool s_run_pending;
static bool s_add_pending;
static bool s_added_showing;

static lv_obj_t *s_popup;  // playlist picker (child of lv_layer_top)

static void rebuild_grid(void);
static void open_detail(int idx, lv_obj_t *card);
static void close_detail(void);

// ------------------------------------------------------------ small helpers

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

// "sub/foo_bar.thr" -> "foo_bar" (basename, .thr suffix stripped)
static void display_name(const char *rel, char *out, size_t out_len)
{
    const char *base = strrchr(rel, '/');
    base = (base != NULL) ? base + 1 : rel;
    strlcpy(out, base, out_len);
    size_t n = strlen(out);
    if (n >= 4 && strcasecmp(out + n - 4, ".thr") == 0) {
        out[n - 4] = '\0';
    }
}

// Case-insensitive substring match on the rel path (pattern_model.py filter)
static bool ci_contains(const char *hay, const char *needle)
{
    if (needle[0] == '\0') {
        return true;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p != '\0'; p++) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static int cmp_rel_ci(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcasecmp(sa, sb);
}

// -------------------------------------------------------------- grid cards

static void card_clicked(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_current_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(card);
    if (idx < 0 || idx >= s_list.count) {
        return;
    }
    open_detail(idx, card);
}

static void make_card(int idx)
{
    lv_obj_t *card = plain(s_grid);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_row(card, TH_SPACE_XS, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, NULL);

    // Child 0: circular preview slot (card-color placeholder dish until the
    // rendered .thr preview arrives via the lazy loader)
    lv_obj_t *slot = plain(card);
    lv_obj_set_size(slot, CARD_PREVIEW_PX, CARD_PREVIEW_PX);
    lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(slot, true, 0);  // square preview -> circle
    lv_obj_set_style_bg_color(slot, th.card, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, th.border, 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(slot, PV_PACK(PV_NONE, 0));

    lv_obj_t *dot = lv_label_create(slot);
    lv_label_set_text(dot, TH_ICON_CIRCLE);
    lv_obj_set_style_text_font(dot, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(dot, th.text3, 0);
    lv_obj_center(dot);

    // Child 1: name (basename, ".thr" stripped, one line with dots)
    char name[96];
    display_name(s_list.items[idx], name, sizeof(name));
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, name);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, th.text, 0);
}

// Unpin the preview descriptor a card's image holds (stored in the image
// widget's user_data by preview_job). LVGL ctx.
static void release_card_preview(lv_obj_t *card)
{
    lv_obj_t *slot = lv_obj_get_child(card, 0);
    if (slot == NULL || PV_STATE(lv_obj_get_user_data(slot)) != PV_DONE) {
        return;
    }
    lv_obj_t *img = lv_obj_get_child(slot, 0);
    if (img != NULL) {
        thr_preview_release((const lv_image_dsc_t *)lv_obj_get_user_data(img));
    }
}

static void more_clicked(lv_event_t *e);

// Card-sized "Show more" tile at the end of a chunked grid. user_data -1
// keeps it out of the preview loader and click-to-detail paths.
static void make_more_card(int remaining)
{
    lv_obj_t *card = plain(s_grid);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, th.card, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(card, more_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text_fmt(label, TH_ICON_EXPAND_MORE "  Show more\n%d remaining", remaining);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, th.text2, 0);
    lv_obj_center(label);
}

// Append up to max_new cards for filter matches beyond s_built, then a
// "Show more" tile if matches remain. Returns the total match count.
static int append_cards(int max_new)
{
    int matched = 0;
    int added = 0;
    for (int i = 0; i < s_list.count; i++) {
        if (!ci_contains(s_list.items[i], s_filter)) {
            continue;
        }
        if (matched >= s_built && added < max_new) {
            make_card(i);
            added++;
        }
        matched++;
    }
    s_built += added;
    if (matched > s_built) {
        make_more_card(matched - s_built);
    }
    return matched;
}

static void more_clicked(lv_event_t *e)
{
    (void)e;
    uint32_t n = lv_obj_get_child_count(s_grid);
    if (n > 0) {  // drop the "Show more" tile; append replaces it
        lv_obj_delete(lv_obj_get_child(s_grid, (int32_t)n - 1));
    }
    int matched = append_cards(GRID_CHUNK);
    lv_label_set_text_fmt(s_count_label, "%d patterns", matched);
    // No scroll reset: the new cards continue where the user already is.
}

static void rebuild_grid(void)
{
    s_generation++;  // orphan any in-flight preview job's card pointer
    uint32_t n_cards = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n_cards; i++) {
        release_card_preview(lv_obj_get_child(s_grid, (int32_t)i));
    }
    lv_obj_clean(s_grid);
    if (s_empty != NULL) {
        lv_obj_delete(s_empty);
        s_empty = NULL;
    }

    s_built = 0;
    int shown = append_cards(GRID_CHUNK);

    lv_label_set_text_fmt(s_count_label, "%d patterns", shown);
    lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);

    // The complaint the SD scheme asks for: no prepared card in the slot.
    if (s_sd_banner != NULL) {
        if (!s_from_sd && !sd_catalog_present()) {
            lv_obj_remove_flag(s_sd_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sd_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (shown == 0) {
        lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        bool searching = (s_filter[0] != '\0');
        s_empty = ui_empty_state(s_page,
                                 searching ? TH_ICON_SEARCH : TH_ICON_QUEUE_MUSIC,
                                 searching ? "No patterns found" : "No patterns yet",
                                 searching ? "Try a different search term"
                                           : "Insert the pattern SD card, or connect\nto a table on the Control page");
    } else {
        lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------- preview loader

// Blocking fetch+render; jobs task only. Widget updates under lvgl lock,
// guarded by the generation captured at submit time.
static void preview_job(void *arg)
{
    preview_ctx_t *ctx = arg;
    const lv_image_dsc_t *dsc = NULL;
    esp_err_t err = thr_preview_get(ctx->rel, PREVIEW_SRC_PX, &dsc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "preview %s: %s", ctx->rel, esp_err_to_name(err));
    }

    lvgl_port_lock(0);
    s_preview_pending = false;
    if (err != ESP_OK) {
        // Global backoff: don't hammer a board that just failed a fetch.
        s_backoff_at = lv_tick_get();
        s_backoff_armed = true;
    }
    bool attached = false;  // did the pinned dsc end up on a widget?
    if (ctx->kind == PV_KIND_CARD && ctx->gen == s_generation) {
        // gen matches => the grid was not rebuilt, ctx->card is still alive
        lv_obj_t *slot = lv_obj_get_child(ctx->card, 0);
        int attempts = PV_ATTEMPTS(lv_obj_get_user_data(slot));
        if (err == ESP_OK) {
            lv_obj_clean(slot);
            lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(slot, 0, 0);
            lv_obj_t *img = lv_image_create(slot);
            lv_obj_set_size(img, CARD_PREVIEW_PX, CARD_PREVIEW_PX);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
            lv_image_set_src(img, dsc);
            lv_obj_set_user_data(img, (void *)dsc);  // for release on rebuild
            lv_obj_center(img);
            lv_obj_set_user_data(slot, PV_PACK(PV_DONE, attempts));
            attached = true;
        } else {
            attempts++;
            int st = (attempts >= PREVIEW_MAX_ATTEMPTS) ? PV_FAILED : PV_NONE;
            lv_obj_set_user_data(slot, PV_PACK(st, attempts));
        }
    } else if (ctx->kind == PV_KIND_DETAIL && ctx->gen == s_detail_gen && s_detail_slot != NULL) {
        if (err == ESP_OK) {
            lv_obj_clean(s_detail_slot);
            lv_obj_set_style_bg_opa(s_detail_slot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_detail_slot, 0, 0);
            lv_obj_t *img = lv_image_create(s_detail_slot);
            lv_obj_set_size(img, DETAIL_PREVIEW_PX, DETAIL_PREVIEW_PX);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
            lv_image_set_src(img, dsc);
            lv_obj_set_user_data(img, (void *)dsc);  // for release on close
            lv_obj_center(img);
            s_detail_have_img = true;
            attached = true;
        } else {
            s_detail_attempts++;
        }
    }
    if (!attached) {
        thr_preview_release(dsc);  // pinned for us but never shown (NULL-safe)
    }
    lvgl_port_unlock();
    free(ctx);
}

static bool submit_preview(int kind, int gen, lv_obj_t *card, const char *rel)
{
    preview_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return false;
    }
    ctx->kind = kind;
    ctx->gen = gen;
    ctx->card = card;
    strlcpy(ctx->rel, rel, sizeof(ctx->rel));
    if (jobs_submit(preview_job, ctx) != ESP_OK) {
        free(ctx);
        return false;
    }
    s_preview_pending = true;
    return true;
}

// LVGL timer: every 600 ms pick ONE target lacking a preview (open detail
// first, then the first visible grid card) and fetch it in the background.
static void preview_tick(lv_timer_t *t)
{
    (void)t;
    if (s_preview_pending) {
        return;
    }
    if (s_backoff_armed) {
        if (lv_tick_elaps(s_backoff_at) < PREVIEW_BACKOFF_MS) {
            return;
        }
        s_backoff_armed = false;
    }

    if (s_detail != NULL && !s_detail_have_img && s_detail_attempts < PREVIEW_MAX_ATTEMPTS) {
        submit_preview(PV_KIND_DETAIL, s_detail_gen, NULL, s_detail_rel);
        return;
    }

    uint32_t n = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *card = lv_obj_get_child(s_grid, i);
        lv_obj_t *slot = lv_obj_get_child(card, 0);
        void *ud = lv_obj_get_user_data(slot);
        if (PV_STATE(ud) != PV_NONE) {
            continue;
        }
        if (!lv_obj_is_visible(card)) {
            continue;
        }
        int idx = (int)(intptr_t)lv_obj_get_user_data(card);
        if (idx < 0 || idx >= s_list.count) {
            continue;
        }
        if (submit_preview(PV_KIND_CARD, s_generation, card, s_list.items[idx])) {
            lv_obj_set_user_data(slot, PV_PACK(PV_INFLIGHT, PV_ATTEMPTS(ud)));
        }
        return;  // one preview job at a time
    }
}

// ------------------------------------------------------------ pattern load

static void load_job(void *arg)
{
    (void)arg;
    fw_str_list_t list = {0};
    // Local SD manifest first (instant, works offline); the table's
    // /sand_patterns only when no prepared card is in the slot.
    bool from_sd = true;
    esp_err_t err = sd_catalog_get(&list);
    if (err != ESP_OK) {
        from_sd = false;
        err = fw_get_patterns(&list);
    }
    if (err == ESP_OK && list.count > 1) {
        qsort(list.items, list.count, sizeof(char *), cmp_rel_ci);
    }

    lvgl_port_lock(0);
    s_loading = false;
    lv_obj_set_style_text_color(s_refresh_icon, th.text2, 0);
    if (err == ESP_OK) {
        if (s_list.items != NULL) {
            fw_str_list_free(&s_list);
        }
        s_list = list;  // page owns the strings now
        s_loaded = true;
        s_from_sd = from_sd;
        rebuild_grid();
        ESP_LOGI(TAG, "loaded %d patterns (%s)", s_list.count,
                 from_sd ? "SD manifest" : "table");
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "Couldn't load the patterns. %s", fw_friendly_error(err));
        ui_show_error(msg);
    }
    lvgl_port_unlock();
}

// LVGL context only
static void start_load(bool user_initiated)
{
    if (s_loading) {
        return;
    }
    s_loading = true;
    lv_obj_set_style_text_color(s_refresh_icon, th.accent, 0);
    if (jobs_submit(load_job, NULL) != ESP_OK) {
        s_loading = false;
        lv_obj_set_style_text_color(s_refresh_icon, th.text2, 0);
        if (user_initiated) {
            ui_show_error("Couldn't refresh the patterns. The app is busy - try again.");
        }
    }
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    start_load(true);
}

// Runs in the LVGL task (state takes the port lock before calling)
static void on_state_changed(void)
{
    state_lock();
    conn_state_t conn = state_get()->conn;
    state_unlock();
    if (conn == CONN_CONNECTED && !s_loaded) {
        start_load(false);
    }
}

// ------------------------------------------------------------------ search

// Apply the textarea content as the filter. Called ONLY on Enter (READY),
// keyboard close (CANCEL) or focus loss — never per keystroke.
static void search_apply(lv_event_t *e)
{
    (void)e;
    if (s_kb != NULL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    const char *txt = lv_textarea_get_text(s_search_ta);
    char next[sizeof(s_filter)];
    strlcpy(next, (txt != NULL) ? txt : "", sizeof(next));
    if (strcmp(next, s_filter) != 0) {
        strlcpy(s_filter, next, sizeof(s_filter));
        rebuild_grid();
    }
}

static void search_focused(lv_event_t *e)
{
    (void)e;
    if (s_kb != NULL) {
        lv_keyboard_set_textarea(s_kb, s_search_ta);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

// --------------------------------------------------------- detail: weaving

static void run_job(void *arg)
{
    run_ctx_t *ctx = arg;
    esp_err_t err = fw_run_pattern(ctx->rel, ctx->clear);

    lvgl_port_lock(0);
    s_run_pending = false;
    if (err == ESP_OK) {
        if (ctx->gen == s_detail_gen) {
            close_detail();
        }
        ui_navigate_to(UI_TAB_NOW_PLAYING);
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "Couldn't start the pattern. %s", fw_friendly_error(err));
        ui_show_error(msg);
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
    if (s_run_pending) {
        return;
    }
    run_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->gen = s_detail_gen;
    strlcpy(ctx->rel, s_detail_rel, sizeof(ctx->rel));
    strlcpy(ctx->clear, CLEAR_MODES[s_clear_idx].value, sizeof(ctx->clear));
    if (jobs_submit(run_job, ctx) != ESP_OK) {
        free(ctx);
        ui_show_error("Couldn't start the pattern. The app is busy - try again.");
        return;
    }
    s_run_pending = true;
}

// -------------------------------------------------- detail: add to playlist

static void close_popup(void)
{
    if (s_popup != NULL) {
        lv_obj_delete(s_popup);
        s_popup = NULL;
    }
}

static void added_restore_cb(lv_timer_t *t)
{
    int gen = (int)(intptr_t)lv_timer_get_user_data(t);
    if (gen != s_detail_gen || s_detail_add_btn == NULL) {
        return;  // overlay was closed/reopened; nothing to restore
    }
    s_added_showing = false;
    lv_obj_t *label = lv_obj_get_child(s_detail_add_btn, 0);
    lv_label_set_text(label, TH_ICON_QUEUE_MUSIC "  Add to playlist");
    lv_obj_set_style_border_color(s_detail_add_btn, th.accent, 0);
    lv_obj_set_style_text_color(label, th.accent, 0);
}

// LVGL lock held by caller
static void flip_added(void)
{
    s_added_showing = true;
    lv_obj_t *label = lv_obj_get_child(s_detail_add_btn, 0);
    lv_label_set_text(label, TH_ICON_CHECK "  Added");
    lv_obj_set_style_border_color(s_detail_add_btn, th.ok, 0);
    lv_obj_set_style_text_color(label, th.ok, 0);
    lv_timer_t *t = lv_timer_create(added_restore_cb, ADDED_FEEDBACK_MS,
                                    (void *)(intptr_t)s_detail_gen);
    lv_timer_set_repeat_count(t, 1);
}

// Fetch /sd/playlists/<name>.txt, append "/patterns/<rel>", re-upload.
static void add_job(void *arg)
{
    add_ctx_t *ctx = arg;
    char path[96];
    snprintf(path, sizeof(path), "/playlists/%s.txt", ctx->name);

    char *buf = NULL;
    size_t len = 0;
    char *content = NULL;
    esp_err_t err = fw_fetch_sd(path, &buf, &len);
    if (err == ESP_OK) {
        size_t need = len + strlen(ctx->rel) + 16;  // "/patterns/" + \n + NUL
        content = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (content == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            size_t pos = len;
            memcpy(content, buf, len);
            if (pos > 0 && content[pos - 1] != '\n') {
                content[pos++] = '\n';
            }
            pos += snprintf(content + pos, need - pos, "/patterns/%s\n", ctx->rel);
            err = fw_upload_playlist(ctx->name, content);
        }
    }
    free(buf);
    free(content);

    lvgl_port_lock(0);
    s_add_pending = false;
    if (err == ESP_OK) {
        if (ctx->gen == s_detail_gen && s_detail_add_btn != NULL) {
            flip_added();
        }
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "Couldn't add to the playlist. %s", fw_friendly_error(err));
        ui_show_error(msg);
    }
    lvgl_port_unlock();
    free(ctx);
}

static void playlist_row_clicked(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    const char *name = lv_label_get_text(lv_obj_get_child(row, 1));

    add_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        close_popup();
        return;
    }
    ctx->gen = s_detail_gen;
    strlcpy(ctx->name, name, sizeof(ctx->name));
    strlcpy(ctx->rel, s_detail_rel, sizeof(ctx->rel));
    if (jobs_submit(add_job, ctx) != ESP_OK) {
        free(ctx);
        ui_show_error("Couldn't add to the playlist. The app is busy - try again.");
    } else {
        s_add_pending = true;
    }
    close_popup();
}

static void popup_cancel_clicked(lv_event_t *e)
{
    (void)e;
    close_popup();
}

static void popup_scrim_clicked(lv_event_t *e)
{
    // Only when the scrim itself was tapped (children handle their own taps)
    if (lv_event_get_target_obj(e) == s_popup) {
        close_popup();
    }
}

// LVGL lock held by caller (built from the playlists job)
static void show_playlist_popup(const fw_str_list_t *list)
{
    close_popup();

    s_popup = plain(lv_layer_top());
    lv_obj_set_size(s_popup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_50, 0);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_popup, popup_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = plain(s_popup);
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
    lv_obj_set_style_pad_row(card, TH_SPACE_MD, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Add to playlist");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, th.text, 0);

    if (list->count > 0) {
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
            lv_obj_set_style_border_color(row, th.border, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_pad_hor(row, TH_SPACE_MD, 0);
            lv_obj_set_style_pad_column(row, TH_SPACE_MD, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(row, playlist_row_clicked, LV_EVENT_CLICKED, NULL);

            lv_obj_t *icon = lv_label_create(row);
            lv_label_set_text(icon, TH_ICON_QUEUE_MUSIC);
            lv_obj_set_style_text_font(icon, TH_FONT_CAPTION, 0);
            lv_obj_set_style_text_color(icon, th.accent, 0);

            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, name);
            lv_obj_set_flex_grow(label, 1);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);
            lv_obj_set_style_text_color(label, th.text, 0);
        }
    } else {
        lv_obj_t *empty = plain(card);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_height(empty, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(empty, TH_SPACE_SM, 0);
        lv_obj_set_style_pad_ver(empty, TH_SPACE_MD, 0);

        lv_obj_t *icon = lv_label_create(empty);
        lv_label_set_text(icon, TH_ICON_QUEUE_MUSIC);
        lv_obj_set_style_text_font(icon, TH_FONT_DISPLAY, 0);
        lv_obj_set_style_text_color(icon, th.text3, 0);

        lv_obj_t *l1 = lv_label_create(empty);
        lv_label_set_text(l1, "No playlists yet");
        lv_obj_set_style_text_font(l1, TH_FONT_BODY, 0);
        lv_obj_set_style_text_color(l1, th.text2, 0);

        lv_obj_t *l2 = lv_label_create(empty);
        lv_label_set_text(l2, "Create one on the Playlists page first");
        lv_obj_set_style_text_font(l2, TH_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(l2, th.text3, 0);
    }

    lv_obj_t *cancel = ui_pill_button(card, "Cancel", th.text2, false);
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_add_event_cb(cancel, popup_cancel_clicked, LV_EVENT_CLICKED, NULL);
}

static void playlists_job(void *arg)
{
    int gen = (int)(intptr_t)arg;
    fw_str_list_t list = {0};
    esp_err_t err = fw_get_playlists(&list);

    lvgl_port_lock(0);
    s_add_pending = false;
    if (gen == s_detail_gen && s_detail != NULL) {
        if (err == ESP_OK) {
            show_playlist_popup(&list);
        } else {
            char msg[192];
            snprintf(msg, sizeof(msg), "Couldn't load the playlists. %s", fw_friendly_error(err));
            ui_show_error(msg);
        }
    }
    lvgl_port_unlock();
    if (err == ESP_OK) {
        fw_str_list_free(&list);  // popup copied everything it needs
    }
}

static void add_clicked(lv_event_t *e)
{
    (void)e;
    if (s_add_pending || s_added_showing) {
        return;
    }
    if (jobs_submit(playlists_job, (void *)(intptr_t)s_detail_gen) != ESP_OK) {
        ui_show_error("Couldn't load the playlists. The app is busy - try again.");
        return;
    }
    s_add_pending = true;
}

// --------------------------------------------------------- detail overlay

// LVGL lock held / LVGL context
static void close_detail(void)
{
    if (s_detail == NULL) {
        return;
    }
    close_popup();
    if (s_detail_have_img && s_detail_slot != NULL) {
        lv_obj_t *img = lv_obj_get_child(s_detail_slot, 0);
        if (img != NULL) {
            thr_preview_release((const lv_image_dsc_t *)lv_obj_get_user_data(img));
        }
    }
    lv_obj_delete(s_detail);
    s_detail = NULL;
    s_detail_slot = NULL;
    s_detail_add_btn = NULL;
    s_clear_chips = NULL;
    s_detail_have_img = false;
    s_added_showing = false;
    s_detail_gen++;  // orphan any in-flight detail preview / added-timer
}

static void detail_back_clicked(lv_event_t *e)
{
    (void)e;
    close_detail();
}

static void update_clear_chips(void)
{
    if (s_clear_chips == NULL) {
        return;
    }
    uint32_t n = lv_obj_get_child_count(s_clear_chips);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *chip = lv_obj_get_child(s_clear_chips, (int32_t)i);
        bool sel = ((int)i == s_clear_idx);
        lv_obj_set_style_bg_color(chip, th.accent_soft, 0);
        lv_obj_set_style_bg_opa(chip, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(chip, sel ? th.accent_soft : th.pressed, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(chip, sel ? th.accent : th.border, 0);
        lv_obj_t *label = lv_obj_get_child(chip, 0);
        lv_obj_set_style_text_color(label, sel ? th.accent : th.text2, 0);
    }
}

static void clear_chip_clicked(lv_event_t *e)
{
    s_clear_idx = (int)(intptr_t)lv_event_get_user_data(e);
    update_clear_chips();
}

static void open_detail(int idx, lv_obj_t *card)
{
    if (s_detail != NULL) {
        return;
    }
    strlcpy(s_detail_rel, s_list.items[idx], sizeof(s_detail_rel));
    s_detail_gen++;
    s_detail_have_img = false;
    s_detail_attempts = 0;
    s_clear_idx = 0;  // adaptive is the default
    s_added_showing = false;
    if (s_kb != NULL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }

    // The detail always acquires its own pin via preview_tick: reusing the
    // card's dsc would leave the overlay pointing at pixels the grid releases
    // on its next rebuild. A RAM-cache hit makes the re-get near-instant.
    (void)card;

    char name[96];
    display_name(s_detail_rel, name, sizeof(name));

    s_detail = plain(lv_layer_top());
    lv_obj_set_size(s_detail, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_detail, th.bg, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);

    // Header: back + clean pattern name
    lv_obj_t *header = plain(s_detail);
    lv_obj_set_size(header, LV_PCT(100), TH_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header, th.surface, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, th.border, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_column(header, TH_SPACE_MD, 0);

    lv_obj_t *back = plain(header);
    lv_obj_set_size(back, 66, 66);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, detail_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, TH_ICON_BACK);
    lv_obj_set_style_text_font(back_icon, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(back_icon, th.text, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, name);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, th.text, 0);

    // Body: preview left (55), actions right (45)
    lv_obj_t *body = plain(s_detail);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    lv_obj_t *left = plain(body);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 55);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_detail_slot = plain(left);
    lv_obj_set_size(s_detail_slot, DETAIL_PREVIEW_PX, DETAIL_PREVIEW_PX);
    lv_obj_set_style_radius(s_detail_slot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_detail_slot, true, 0);  // square preview -> circle
    lv_obj_set_flex_flow(s_detail_slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_detail_slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_detail_slot, TH_SPACE_SM, 0);
    lv_obj_remove_flag(s_detail_slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_detail_slot, LV_OBJ_FLAG_SCROLLABLE);
    // Empty dish until the lazy loader fetches it (preview_tick; a RAM-cache
    // hit fills it in well under a second)
    lv_obj_set_style_bg_color(s_detail_slot, th.surface, 0);
    lv_obj_set_style_bg_opa(s_detail_slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_detail_slot, th.border, 0);
    lv_obj_set_style_border_width(s_detail_slot, 1, 0);
    lv_obj_t *dish_icon = lv_label_create(s_detail_slot);
    lv_label_set_text(dish_icon, TH_ICON_CIRCLE);
    lv_obj_set_style_text_font(dish_icon, TH_FONT_DISPLAY, 0);
    lv_obj_set_style_text_color(dish_icon, th.text3, 0);
    lv_obj_t *dish_hint = lv_label_create(s_detail_slot);
    lv_label_set_text(dish_hint, "No preview yet");
    lv_obj_set_style_text_font(dish_hint, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(dish_hint, th.text2, 0);

    lv_obj_t *right = plain(body);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 45);
    lv_obj_set_style_bg_color(right, th.surface, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(right, th.border, 0);
    lv_obj_set_style_border_width(right, 1, 0);
    lv_obj_set_style_border_side(right, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right, TH_SPACE_XL, 0);
    lv_obj_set_style_pad_row(right, TH_SPACE_SM, 0);

    lv_obj_t *eyebrow = lv_label_create(right);
    lv_label_set_text(eyebrow, "BEFORE WEAVING");
    lv_obj_set_style_text_font(eyebrow, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_color(eyebrow, th.text3, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 2, 0);

    // Clear-mode chips, two per row
    s_clear_chips = plain(right);
    lv_obj_set_width(s_clear_chips, LV_PCT(100));
    lv_obj_set_height(s_clear_chips, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clear_chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_clear_chips, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_column(s_clear_chips, TH_SPACE_SM, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *chip = plain(s_clear_chips);
        lv_obj_set_size(chip, LV_PCT(48), TH_TOUCH_TARGET);
        lv_obj_set_style_radius(chip, TH_RADIUS_PILL, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(chip, clear_chip_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(chip);
        lv_label_set_text(label, CLEAR_MODES[i].label);
        lv_obj_set_width(label, LV_PCT(90));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, TH_FONT_CAPTION, 0);
        lv_obj_center(label);
    }
    update_clear_chips();

    lv_obj_t *spacer = plain(right);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *weave = ui_pill_button(right, TH_ICON_PLAY "  Weave this pattern", th.accent, true);
    lv_obj_set_width(weave, LV_PCT(100));
    lv_obj_set_height(weave, TH_CONTROL_HEIGHT);
    lv_obj_add_event_cb(weave, weave_clicked, LV_EVENT_CLICKED, NULL);

    s_detail_add_btn = ui_pill_button(right, TH_ICON_QUEUE_MUSIC "  Add to playlist", th.accent, false);
    lv_obj_set_width(s_detail_add_btn, LV_PCT(100));
    lv_obj_add_event_cb(s_detail_add_btn, add_clicked, LV_EVENT_CLICKED, NULL);
}

// ------------------------------------------------------------- page build

lv_obj_t *page_browse_create(lv_obj_t *parent)
{
    s_page = ui_page_root(parent);
    lv_obj_t *header = ui_page_header(s_page, "Browse");

    // Pattern count
    s_count_label = lv_label_create(header);
    lv_label_set_text(s_count_label, "0 patterns");
    lv_obj_set_style_text_font(s_count_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_count_label, th.text3, 0);

    // Refresh
    lv_obj_t *refresh = plain(header);
    lv_obj_set_size(refresh, 60, 60);
    lv_obj_set_style_radius(refresh, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(refresh, th.pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(refresh, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(refresh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);
    s_refresh_icon = lv_label_create(refresh);
    lv_label_set_text(s_refresh_icon, TH_ICON_REFRESH);
    lv_obj_set_style_text_font(s_refresh_icon, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_refresh_icon, th.text2, 0);
    lv_obj_center(s_refresh_icon);

    lv_obj_t *spacer = plain(header);
    lv_obj_set_flex_grow(spacer, 1);

    // Search pill: filters ONLY on Enter (READY) or focus loss
    s_search_ta = lv_textarea_create(header);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, TH_ICON_SEARCH "  Search");
    lv_textarea_set_max_length(s_search_ta, sizeof(s_filter) - 1);
    lv_obj_set_size(s_search_ta, 300, 60);
    lv_obj_set_style_radius(s_search_ta, TH_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(s_search_ta, th.bg, 0);
    lv_obj_set_style_bg_opa(s_search_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_search_ta, th.border, 0);
    lv_obj_set_style_border_width(s_search_ta, 1, 0);
    lv_obj_set_style_border_color(s_search_ta, th.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(s_search_ta, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_ver(s_search_ta, 16, 0);
    lv_obj_set_style_text_font(s_search_ta, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_search_ta, th.text, 0);
    lv_obj_set_style_text_color(s_search_ta, th.text3, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_search_ta, search_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_focused, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_DEFOCUSED, NULL);

    // Shared on-screen keyboard, hidden until the search field is focused
    s_kb = ui_keyboard_create(lv_layer_top());
    lv_keyboard_set_textarea(s_kb, s_search_ta);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    // Missing-SD complaint strip (hidden while a prepared card is in)
    s_sd_banner = plain(s_page);
    lv_obj_set_width(s_sd_banner, LV_PCT(100));
    lv_obj_set_style_bg_color(s_sd_banner, th.card, 0);
    lv_obj_set_style_bg_opa(s_sd_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sd_banner, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(s_sd_banner, th.accent, 0);
    lv_obj_set_style_border_width(s_sd_banner, 1, 0);
    lv_obj_set_style_pad_all(s_sd_banner, TH_SPACE_SM, 0);
    lv_obj_add_flag(s_sd_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *banner_label = lv_label_create(s_sd_banner);
    lv_label_set_text(banner_label,
                      LV_SYMBOL_SD_CARD "  No pattern card - insert the prepared microSD "
                      "for instant patterns and previews");
    lv_obj_set_width(banner_label, LV_PCT(100));
    lv_label_set_long_mode(banner_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(banner_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(banner_label, th.text2, 0);

    // Scrollable card grid
    s_grid = plain(s_page);
    lv_obj_set_width(s_grid, LV_PCT(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_grid, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_row(s_grid, TH_SPACE_MD, 0);
    lv_obj_set_style_pad_column(s_grid, TH_SPACE_MD, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);

    rebuild_grid();  // empty state until the first load

    state_add_listener(on_state_changed);
    lv_timer_create(preview_tick, PREVIEW_TICK_MS, NULL);

    // A prepared SD card makes Browse independent of the table: load the
    // local manifest right away instead of waiting for a connection.
    if (sd_catalog_present()) {
        start_load(false);
    }

    return s_page;
}
