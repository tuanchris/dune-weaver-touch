// Now Playing: progress ring around the pattern disc (left), transport (right).
// Mirrors ExecutionPage.qml: 55/45 split, arc from 12 o'clock, ember accent.
// Live semantics (ring/pause/dual-ratio, disc source, countdown) are the
// PORTING_NOTES §5 Now Playing spec — exact, don't improvise.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "../../app/jobs.h"
#include "../../app/state.h"
#include "../../render/thr_preview.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "now_playing";

// One preview size app-wide keeps the LittleFS cache coherent (thr_preview.h).
// The disc image's DISPLAYED size (thr_preview resamples the 300 px master
// to it). 300 on the 5B is the master size exactly, so that one blits 1:1.
#if defined(BOARD_WAVESHARE_7)
#define PREVIEW_SIZE_PX 200
#else
#define PREVIEW_SIZE_PX 300
#endif
// Dish/arc stack. 340 on the 5B, as before; the dish is inset by the card
// radius, which is itself per-panel.
#define NP_STACK_PX TH_S(227)

// Montserrat stand-in fonts lack U+00B7 (·); U+2022 (•) is bundled. Swap for
// "\xC2\xB7" once the Outfit fonts are converted with the middle dot included.
#define SEP "\xE2\x80\xA2"

#define SPEED_COUNT 6
static const int SPEED_OPTIONS[SPEED_COUNT] = {50, 100, 150, 200, 300, 500};

typedef enum {
    ACT_PAUSE,
    ACT_RESUME,
    ACT_STOP,
    ACT_SKIP,
} transport_action_t;

typedef struct {
    uint32_t gen;
    char rel[128];
} preview_req_t;

// Widgets (created once; the page is never destroyed)
static lv_obj_t *s_arc;
static lv_obj_t *s_disc_img;
static lv_obj_t *s_dish;
static lv_obj_t *s_dish_label;
static lv_obj_t *s_eyebrow;
static lv_obj_t *s_title;
static lv_obj_t *s_playlist_line;
static lv_obj_t *s_progress_line;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_seg[SPEED_COUNT];
static lv_obj_t *s_seg_label[SPEED_COUNT];

// Rendered-text caches so 1 Hz polls don't invalidate unchanged labels
static char s_eyebrow_cache[16] = "NOW WEAVING";
static char s_title_cache[144] = "Nothing playing";
static char s_playlist_cache[176] = "";
static char s_progress_cache[96] = "";
static char s_dish_cache[40] = "The table is resting";
static int s_arc_value = -1;
static int s_selected_seg = -1;
static int s_title_dim = 1;       // tri-state: -1 unknown, 0 bright, 1 dim
static int s_pause_shows_pause = -1;
static int s_pause_enabled = -1;

// Preview request tracking (LVGL ctx only; jobs check gen under lvgl lock)
static uint32_t s_preview_gen = 0;
static char s_requested[128] = "";
static const lv_image_dsc_t *s_disc_dsc;  // pinned dsc currently attached
// Transient preview failures retry no sooner than 10 s (PORTING_NOTES §6)
#define PREVIEW_RETRY_MS 10000
static char s_failed_rel[128] = "";
static uint32_t s_failed_at;

// Pause countdown, ticked by a 1 s lv_timer between status polls (LVGL ctx)
static bool s_in_pause = false;
static int s_pause_remaining = -1;
static int s_pause_total = -1;

static app_state_t s_snap;  // listener-only snapshot (LVGL task)

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    // LVGL makes every object scrollable by default; nothing in this UI is
    // dragged (ui_page_stepper re-enables the ones it drives). See ui.h.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// --- Small pure helpers -----------------------------------------------------

// Strip the board's SD prefixes ("/patterns/a/b.thr" -> "a/b.thr"); status.file
// arrives pre-stripped from fw_client but playlist next/last are raw.
static const char *strip_sd_prefix(const char *p)
{
    static const char *PREFIXES[] = {"/sd/patterns/", "/patterns/", "/sd/", "/"};
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        size_t n = strlen(PREFIXES[i]);
        if (strncmp(p, PREFIXES[i], n) == 0) {
            return p + n;
        }
    }
    return p;
}

// Basename without the .thr extension, for on-screen labels.
static void display_name(const char *rel, char *out, size_t out_len)
{
    const char *base = strrchr(rel, '/');
    base = (base != NULL) ? base + 1 : rel;
    strlcpy(out, base, out_len);
    size_t n = strlen(out);
    if (n >= 4 && strcmp(out + n - 4, ".thr") == 0) {
        out[n - 4] = '\0';
    }
}

// "H:MM:SS", or "M:SS" when under an hour (ExecutionPage.formatDuration).
static void fmt_duration(int s, char *out, size_t out_len)
{
    if (s < 0) {
        s = 0;
    }
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    if (h > 0) {
        snprintf(out, out_len, "%d:%02d:%02d", h, m, sec);
    } else {
        snprintf(out, out_len, "%d:%02d", m, sec);
    }
}

static int snap_feed_idx(int feed)
{
    if (feed <= 0) {
        return 3;  // 200, the reference default selection
    }
    int best = 0;
    for (int i = 1; i < SPEED_COUNT; i++) {
        if (abs(feed - SPEED_OPTIONS[i]) < abs(feed - SPEED_OPTIONS[best])) {
            best = i;
        }
    }
    return best;
}

// --- Guarded widget setters (LVGL ctx) --------------------------------------

static void set_label_cached(lv_obj_t *label, char *cache, size_t cache_len, const char *text)
{
    if (strcmp(cache, text) == 0) {
        return;
    }
    strlcpy(cache, text, cache_len);
    lv_label_set_text(label, cache);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_arc_value(int v)
{
    v = LV_CLAMP(0, v, 100);
    if (v == s_arc_value) {
        return;
    }
    s_arc_value = v;
    lv_arc_set_value(s_arc, v);
}

static void highlight_segment(int idx)
{
    if (idx == s_selected_seg) {
        return;
    }
    s_selected_seg = idx;
    for (int i = 0; i < SPEED_COUNT; i++) {
        bool sel = (i == idx);
        lv_obj_set_style_bg_opa(s_seg[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_seg[i], sel ? 1 : 0, 0);
        lv_obj_set_style_text_color(s_seg_label[i], sel ? th.accent : th.text2, 0);
    }
}

// --- Background jobs (jobs task: blocking calls, lvgl lock for widgets) ------

static const char *action_prefix(transport_action_t act)
{
    switch (act) {
    case ACT_PAUSE:
        return "Couldn't pause.";
    case ACT_RESUME:
        return "Couldn't resume.";
    case ACT_STOP:
        return "Couldn't stop.";
    case ACT_SKIP:
    default:
        return "Couldn't skip.";
    }
}

static void transport_job(void *arg)
{
    transport_action_t act = (transport_action_t)(intptr_t)arg;
    esp_err_t err;
    switch (act) {
    case ACT_PAUSE:
        err = fw_pause();
        break;
    case ACT_RESUME:
        err = fw_resume();
        break;
    case ACT_STOP:
        err = fw_stop();
        break;
    case ACT_SKIP:
    default:
        err = fw_playlist_skip();
        break;
    }
    if (err == ESP_OK) {
        state_poll_now();
        return;
    }
    ESP_LOGW(TAG, "transport action %d failed: 0x%x", (int)act, err);
    char msg[192];
    snprintf(msg, sizeof(msg), "%s %s", action_prefix(act), fw_friendly_error(err));
    lvgl_port_lock(0);
    ui_show_error(msg);
    lvgl_port_unlock();
}

static void feed_job(void *arg)
{
    int mm = (int)(intptr_t)arg;
    esp_err_t err = fw_set_feed(mm);
    if (err == ESP_OK) {
        state_poll_now();
        return;
    }
    ESP_LOGW(TAG, "set feed %d failed: 0x%x", mm, err);
    char msg[192];
    snprintf(msg, sizeof(msg), "Couldn't change the speed. %s", fw_friendly_error(err));
    lvgl_port_lock(0);
    ui_show_error(msg);
    lvgl_port_unlock();
}

static void preview_job(void *arg)
{
    preview_req_t *req = arg;
    const lv_image_dsc_t *dsc = NULL;
    // The disc sits directly on the page background (the dish below is a
    // sibling placeholder, not a parent), so paint the tile's corners th.bg.
    esp_err_t err = thr_preview_get(req->rel, PREVIEW_SIZE_PX, ui_rgb565(th.bg), &dsc);

    lvgl_port_lock(0);
    if (req->gen != s_preview_gen) {
        // Stale: the disc moved on. The dsc was pinned for us — unpin it.
        thr_preview_release(dsc);
    } else if (err == ESP_OK && dsc != NULL) {
        lv_image_set_src(s_disc_img, dsc);
        set_hidden(s_disc_img, false);
        set_hidden(s_dish, true);
        thr_preview_release(s_disc_dsc);  // unpin what the disc showed before
        s_disc_dsc = dsc;
    } else if (err == ESP_ERR_NOT_FOUND) {
        // The card has no tile for this pattern: permanent, so leave
        // s_requested set and keep showing the plain dish. No retry, no log
        // line every 10 s.
        thr_preview_release(dsc);
    } else {
        ESP_LOGW(TAG, "preview %s failed: 0x%x", req->rel, err);
        // No card / read fault: allow a retry, but not before 10 s.
        strlcpy(s_failed_rel, req->rel, sizeof(s_failed_rel));
        s_failed_at = lv_tick_get();
        s_requested[0] = '\0';
    }
    lvgl_port_unlock();
    free(req);
}

// --- Event callbacks (LVGL ctx: submit and return, never block) --------------

static void submit_transport(transport_action_t act)
{
    // Fast lane: a Stop tap must never wait behind a 45 s preview fetch.
    if (jobs_submit_fast(transport_job, (void *)(intptr_t)act) != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s The panel is busy — try again.", action_prefix(act));
        ui_show_error(msg);
    }
}

static void pause_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    state_lock();
    const app_state_t *s = state_get();
    bool has_file = s->has_status && s->status.file[0] != '\0';
    bool is_hold = s->has_status && strcmp(s->status.state, "Hold") == 0;
    state_unlock();
    if (!has_file) {
        return;  // button is disabled in this state; belt and braces
    }
    submit_transport(is_hold ? ACT_RESUME : ACT_PAUSE);
}

static void stop_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    submit_transport(ACT_STOP);
}

static void skip_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    submit_transport(ACT_SKIP);
}

static void speed_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    highlight_segment(idx);  // optimistic; the next poll re-snaps from feed
    if (jobs_submit_fast(feed_job, (void *)(intptr_t)SPEED_OPTIONS[idx]) != ESP_OK) {
        ui_show_error("Couldn't change the speed. The panel is busy — try again.");
    }
}

// --- Live state -> widgets (all in LVGL ctx) ---------------------------------

// Pause-countdown widgets: the progress line and the resting ring, driven by
// s_pause_remaining (resynced by every poll, ticked locally in between).
static void update_countdown_widgets(void)
{
    if (!s_in_pause) {
        return;
    }
    char dur[16];
    fmt_duration(s_pause_remaining, dur, sizeof(dur));
    char buf[64];
    snprintf(buf, sizeof(buf), "%s until the next pattern", dur);
    set_label_cached(s_progress_line, s_progress_cache, sizeof(s_progress_cache), buf);

    int value = 0;
    if (s_pause_total > 0) {
        value = (int)(100.0f * (1.0f - (float)s_pause_remaining / (float)s_pause_total) + 0.5f);
    }
    set_arc_value(value);
}

static void countdown_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (!s_in_pause || s_pause_remaining < 0) {
        return;
    }
    if (s_pause_remaining > 0) {
        s_pause_remaining--;
    }
    update_countdown_widgets();
}

// Kick a preview render when the disc's source pattern changes. Generation-
// guarded like Browse: a stale job's result is dropped, never mis-applied.
static void request_disc_preview(const char *disc_rel)
{
    if (strcmp(disc_rel, s_requested) == 0) {
        return;
    }
    // A recently failed fetch of this very pattern waits out its 10 s hold.
    if (disc_rel[0] != '\0' && strcmp(disc_rel, s_failed_rel) == 0 &&
        lv_tick_elaps(s_failed_at) < PREVIEW_RETRY_MS) {
        return;
    }
    strlcpy(s_requested, disc_rel, sizeof(s_requested));
    s_preview_gen++;
    set_hidden(s_disc_img, true);
    set_hidden(s_dish, false);
    if (disc_rel[0] == '\0') {
        return;
    }
    preview_req_t *req = malloc(sizeof(preview_req_t));
    if (req == NULL) {
        ESP_LOGW(TAG, "no mem for preview request");
        s_requested[0] = '\0';  // retry on the next status update
        return;
    }
    req->gen = s_preview_gen;
    strlcpy(req->rel, disc_rel, sizeof(req->rel));
    if (jobs_submit(preview_job, req) != ESP_OK) {
        ESP_LOGW(TAG, "job queue full; preview retry on next poll");
        free(req);
        s_requested[0] = '\0';
    }
}

static void refresh_from_state(void)
{
    state_lock();
    s_snap = *state_get();
    state_unlock();

    const fw_status_t *st = &s_snap.status;
    bool has_status = s_snap.has_status;
    bool in_pause = has_status && st->playlist.pause_remaining >= 0;
    bool has_file = has_status && st->file[0] != '\0';
    bool is_hold = has_status && strcmp(st->state, "Hold") == 0;
    bool running = has_status && st->running;

    s_in_pause = in_pause;
    s_pause_remaining = in_pause ? st->playlist.pause_remaining : -1;
    s_pause_total = in_pause ? st->playlist.pause_total : -1;

    // Eyebrow
    set_label_cached(s_eyebrow, s_eyebrow_cache, sizeof(s_eyebrow_cache),
                     in_pause ? "UP NEXT" : "NOW WEAVING");

    // Title: during a pause the pattern the table will weave next, else the
    // one being woven, else the resting placeholder.
    char title[144];
    const char *next_rel = strip_sd_prefix(st->playlist.next);
    if (in_pause && next_rel[0] != '\0') {
        display_name(next_rel, title, sizeof(title));
    } else if (has_file) {
        display_name(st->file, title, sizeof(title));
    } else {
        strlcpy(title, "Nothing playing", sizeof(title));
    }
    set_label_cached(s_title, s_title_cache, sizeof(s_title_cache), title);
    int dim = (has_file || in_pause) ? 0 : 1;
    if (dim != s_title_dim) {
        s_title_dim = dim;
        lv_obj_set_style_text_color(s_title, dim ? th.text3 : th.text, 0);
    }

    // Playlist position line
    bool pl_visible = has_status && st->playlist.active && st->playlist.total > 0;
    set_hidden(s_playlist_line, !pl_visible);
    if (pl_visible) {
        char line[176];
        if (st->playlist.name[0] != '\0') {
            snprintf(line, sizeof(line), "%s " SEP " %d of %d%s", st->playlist.name,
                     st->playlist.index + 1, st->playlist.total,
                     st->playlist.clearing ? " " SEP " clearing" : "");
        } else {
            snprintf(line, sizeof(line), "%d of %d%s", st->playlist.index + 1,
                     st->playlist.total, st->playlist.clearing ? " " SEP " clearing" : "");
        }
        set_label_cached(s_playlist_line, s_playlist_cache, sizeof(s_playlist_cache), line);
    }

    // Progress line + ring: percent woven while weaving, countdown while resting
    set_hidden(s_progress_line, !(has_file || in_pause));
    if (in_pause) {
        update_countdown_widgets();
    } else {
        int pct = has_file ? (int)(st->progress * 100.0f + 0.5f) : 0;
        set_arc_value(pct);
        if (has_file) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d%% woven%s", pct, is_hold ? " " SEP " paused" : "");
            set_label_cached(s_progress_line, s_progress_cache, sizeof(s_progress_cache), buf);
        }
    }

    // Disc: what's being woven; during the rest, what's on the sand (last)
    const char *disc_rel = "";
    char last_rel[128];
    if (in_pause) {
        strlcpy(last_rel, strip_sd_prefix(st->playlist.last), sizeof(last_rel));
        disc_rel = last_rel;
    } else if (has_file) {
        disc_rel = st->file;
    }
    request_disc_preview(disc_rel);
    set_label_cached(s_dish_label, s_dish_cache, sizeof(s_dish_cache),
                     in_pause    ? "Resting between patterns"
                     : has_file  ? "Rendering preview"
                                 : "The table is resting");

    // Transport: Pause while running, Resume while held/idle; needs a file
    int shows_pause = (running && !is_hold) ? 1 : 0;
    if (shows_pause != s_pause_shows_pause) {
        s_pause_shows_pause = shows_pause;
        lv_label_set_text(lv_obj_get_child(s_pause_btn, 0),
                          shows_pause ? TH_ICON_PAUSE "  Pause" : TH_ICON_PLAY "  Resume");
    }
    int enabled = has_file ? 1 : 0;
    if (enabled != s_pause_enabled) {
        s_pause_enabled = enabled;
        if (enabled) {
            lv_obj_remove_state(s_pause_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_pause_btn, LV_STATE_DISABLED);
        }
    }

    // Speed highlight snaps to the nearest option of the live feed
    if (has_status) {
        highlight_segment(snap_feed_idx(st->feed));
    }
}

// --- Page construction -------------------------------------------------------

static lv_obj_t *make_speed_segment(lv_obj_t *pill, int idx)
{
    lv_obj_t *seg = plain(pill);
    lv_obj_set_flex_grow(seg, 1);
    lv_obj_set_height(seg, LV_PCT(100));
    lv_obj_set_style_radius(seg, TH_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(seg, th.bg, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(seg, th.border, 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(seg, speed_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    char text[8];
    snprintf(text, sizeof(text), "%d", SPEED_OPTIONS[idx]);
    lv_obj_t *label = lv_label_create(seg);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(label, th.text2, 0);

    s_seg[idx] = seg;
    s_seg_label[idx] = label;
    return seg;
}

lv_obj_t *page_now_playing_create(lv_obj_t *parent)
{
    lv_obj_t *page = ui_page_root(parent);
    ui_page_header(page, "Now Playing");

    lv_obj_t *body = plain(page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    // Left 55%: the progress ring
    lv_obj_t *left = plain(body);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 55);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Disc and ring overlap in one stack, and the ARC IS CREATED LAST so the
    // ring paints over the disc. The preview tile is a 300 px SQUARE whose
    // corners are opaque th.bg out to r=212, while the ring sits at r=162..170;
    // as a child of the arc (children draw after their parent's own parts)
    // those corners buried the ring everywhere except within ~22 deg of each
    // cardinal point, so it read as four disconnected segments (photo from
    // Tuan, 2026-08-26). Do not re-parent these to the arc.
    lv_obj_t *stack = plain(left);
    lv_obj_set_size(stack, NP_STACK_PX, NP_STACK_PX);

    // Resting dish while idle / while the preview renders
    lv_obj_t *dish = plain(stack);
    lv_obj_set_size(dish, NP_STACK_PX - 2 * TH_RADIUS_MD, NP_STACK_PX - 2 * TH_RADIUS_MD);
    lv_obj_center(dish);
    lv_obj_set_style_radius(dish, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dish, th.surface, 0);
    lv_obj_set_style_bg_opa(dish, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dish, th.border, 0);
    lv_obj_set_style_border_width(dish, 1, 0);
    s_dish = dish;

    s_dish_label = lv_label_create(dish);
    lv_label_set_text(s_dish_label, s_dish_cache);
    lv_obj_set_style_text_font(s_dish_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_dish_label, th.text3, 0);
    lv_obj_center(s_dish_label);

    // The live pattern disc (preview rendered off-task, set by preview_job)
    s_disc_img = lv_image_create(stack);
    lv_obj_set_size(s_disc_img, PREVIEW_SIZE_PX, PREVIEW_SIZE_PX);
    lv_obj_center(s_disc_img);
    lv_obj_add_flag(s_disc_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *arc = lv_arc_create(stack);
    lv_obj_set_size(arc, NP_STACK_PX, NP_STACK_PX);
    lv_obj_center(arc);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);  // the disc shows through
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, th.card, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, th.accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    s_arc = arc;
    s_arc_value = 0;

    // Right 45%: name, state, transport, speed — on surface with a left rule
    lv_obj_t *right = plain(body);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 45);
    lv_obj_set_style_bg_color(right, th.surface, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(right, th.border, 0);
    lv_obj_set_style_border_width(right, 1, 0);
    lv_obj_set_style_border_side(right, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(right, TH_SPACE_XL, 0);
    lv_obj_set_style_pad_ver(right, TH_SPACE_LG, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    s_eyebrow = lv_label_create(right);
    lv_label_set_text(s_eyebrow, s_eyebrow_cache);
    lv_obj_set_style_text_font(s_eyebrow, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_color(s_eyebrow, th.accent, 0);
    lv_obj_set_style_text_letter_space(s_eyebrow, 2, 0);

    s_title = lv_label_create(right);
    lv_label_set_text(s_title, s_title_cache);
    lv_obj_set_style_text_font(s_title, TH_FONT_DISPLAY, 0);
    lv_obj_set_style_text_color(s_title, th.text3, 0);
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(s_title, TH_SPACE_SM, 0);

    s_playlist_line = lv_label_create(right);
    lv_label_set_text(s_playlist_line, "");
    lv_obj_set_style_text_font(s_playlist_line, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_playlist_line, th.text2, 0);
    lv_obj_set_width(s_playlist_line, LV_PCT(100));
    lv_label_set_long_mode(s_playlist_line, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(s_playlist_line, TH_SPACE_XS, 0);
    lv_obj_add_flag(s_playlist_line, LV_OBJ_FLAG_HIDDEN);

    s_progress_line = lv_label_create(right);
    lv_label_set_text(s_progress_line, "");
    lv_obj_set_style_text_font(s_progress_line, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_progress_line, th.text2, 0);
    lv_obj_set_width(s_progress_line, LV_PCT(100));
    lv_label_set_long_mode(s_progress_line, LV_LABEL_LONG_DOT);
    lv_obj_set_style_margin_top(s_progress_line, TH_SPACE_LG, 0);
    lv_obj_add_flag(s_progress_line, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *spacer = plain(right);
    lv_obj_set_flex_grow(spacer, 1);

    // Transport row, flex weights 3:2:2
    lv_obj_t *transport = plain(right);
    lv_obj_set_width(transport, LV_PCT(100));
    lv_obj_set_height(transport, TH_CONTROL_HEIGHT);
    lv_obj_set_flex_flow(transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(transport, TH_SPACE_SM, 0);

    s_pause_btn = ui_pill_button(transport, TH_ICON_PLAY "  Resume", th.accent, true);
    lv_obj_set_flex_grow(s_pause_btn, 3);
    lv_obj_set_height(s_pause_btn, LV_PCT(100));
    lv_obj_set_style_opa(s_pause_btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_state(s_pause_btn, LV_STATE_DISABLED);
    s_pause_shows_pause = 0;
    s_pause_enabled = 0;
    lv_obj_add_event_cb(s_pause_btn, pause_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop = ui_pill_button(transport, TH_ICON_STOP "  Stop", th.danger, false);
    lv_obj_set_flex_grow(stop, 2);
    lv_obj_set_height(stop, LV_PCT(100));
    lv_obj_add_event_cb(stop, stop_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *skip = ui_pill_button(transport, TH_ICON_SKIP_NEXT "  Skip", th.card, true);
    lv_obj_set_flex_grow(skip, 2);
    lv_obj_set_height(skip, LV_PCT(100));
    // th.card is a dark fill; force bone text like the QML cardColor button
    lv_obj_set_style_text_color(lv_obj_get_child(skip, 0), th.text, 0);
    lv_obj_add_event_cb(skip, skip_clicked, LV_EVENT_CLICKED, NULL);

    // Speed (feed is mm/min — the QML "mm/s" label was a bug, fixed here)
    lv_obj_t *speed_eyebrow = lv_label_create(right);
    lv_label_set_text(speed_eyebrow, "SPEED " SEP " MM/MIN");
    lv_obj_set_style_text_font(speed_eyebrow, TH_FONT_EYEBROW, 0);
    lv_obj_set_style_text_color(speed_eyebrow, th.text3, 0);
    lv_obj_set_style_text_letter_space(speed_eyebrow, 2, 0);
    lv_obj_set_style_margin_top(speed_eyebrow, TH_SPACE_LG, 0);

    lv_obj_t *seg_pill = plain(right);
    lv_obj_set_width(seg_pill, LV_PCT(100));
    lv_obj_set_height(seg_pill, 72);
    lv_obj_set_style_radius(seg_pill, TH_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(seg_pill, th.card, 0);
    lv_obj_set_style_bg_opa(seg_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(seg_pill, TH_SPACE_XS, 0);
    lv_obj_set_style_pad_column(seg_pill, 3, 0);
    lv_obj_set_flex_flow(seg_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_margin_top(seg_pill, TH_SPACE_SM, 0);
    for (int i = 0; i < SPEED_COUNT; i++) {
        make_speed_segment(seg_pill, i);
    }
    highlight_segment(3);  // 200 mm/min until the first status arrives

    // Between polls the rest countdown ticks locally, once a second
    lv_timer_create(countdown_tick, 1000, NULL);

    state_add_listener(refresh_from_state);
    return page;
}
