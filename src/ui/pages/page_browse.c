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
#include "esp_timer.h"  // batch flush pacing in preview_job

#include "../../app/jobs.h"
#include "../../app/sd_catalog.h"
#include "../../app/state.h"
#include "../../net/fw_client.h"
#include "../../render/thr_preview.h"
#include "../theme.h"
#include "../ui.h"
#include "pages.h"

static const char *TAG = "page_browse";

// PAGED grid, not a scrolling one (2026-08-26, Tuan's direction). This panel
// redraws all 1024x600 every frame a scroll is in motion (full_refresh +
// avoid_tearing on the RGB panel), so dragging is the most expensive thing
// the UI can do while a static page costs nothing. Tapping Prev/Next spends
// one redraw instead of ~30 a second, and it takes the momentum-throw,
// catch-tap and off-screen-unload machinery with it.
//
// Four columns like the reference grid (QML shows 4 cells across 800). Two
// rows is what fits: 600 - 72 header - 72 nav = 456 px, which after the
// grid's padding, the card's own padding and the name label leaves a 160 px
// preview. The Up/Down pager is a column down the right of the grid, so the
// cards get H_RES - PAGER_W of width to share.
#define PAGE_COLS 4
// The card row is CENTRED ON THE SCREEN: the pager column on the right is
// mirrored by an empty band of the same width on the left, so the grid
// between them is centred and the cards land symmetrically about x=512.
//
// Sizes are DERIVED from the two budgets below, not chosen. The preview is
// square, so it is pinned by whichever axis runs out first — and it is width,
// which is why the pager column and the column gap were squeezed to feed it.
//
//   horizontal: 4*CARD_W + 3*GRID_GAP <= 1024 - 2*PAGER_W - 2*GRID_PAD_HOR
//               4*194  + 3*12 = 812   <= 1024 - 168 - 36 = 820   (8 spare)
//   vertical:   2*CARD_H + GRID_GAP   <= 600 - HEADER - NAV - 2*GRID_PAD_VER
//               2*220  + 12  = 452    <= 600 - 60 - 64 - 12 = 464 (12 spare)
//   card:       CARD_W = 2*CARD_PAD + PREVIEW
//               CARD_H = 2*CARD_PAD + PREVIEW + CARD_GAP + caption line (22)
//
// Changing CARD_PREVIEW_PX no longer touches the SD card: tiles are resampled
// from the one 300 px mask (PORTING_NOTES §7a), so this is a pure UI decision.
#if defined(BOARD_PANEL_800X480)
// 7" 800x480. Same derivation, re-run against the smaller budget and the x1.0
// tokens (GRID_GAP 8, GRID_PAD_HOR 12, GRID_PAD_VER 4):
//
//   horizontal: 4*CARD_W + 3*GRID_GAP <= 800 - 2*PAGER_W - 2*GRID_PAD_HOR
//               4*134  + 3*8  = 560   <= 800 - 112 - 24 = 664  (104 spare)
//   vertical:   2*CARD_H + GRID_GAP   <= 480 - HEADER - NAV - 2*GRID_PAD_VER
//               2*163  + 8   = 334    <= 480 - 60 - 64 - 8 = 348 (14 spare)
//   card:       CARD_W = 2*CARD_PAD + TH_SQUARE_W(PREVIEW) = 8 + 126 = 134
//               CARD_H = 2*CARD_PAD + PREVIEW + CARD_GAP + caption(16) = 163
//
// Height is the binding axis here (the 5B's was width), so the preview is
// pinned by the 2-row budget, not by the pager column. That is also why the
// pixel-aspect correction takes its 7.6% out of the WIDTH (TH_SQUARE_W, see
// theme.h): width had 64 px spare, height had 14. The preview is 126x136 px,
// which is square on the glass — do not "fix" it back to 136x136.
#define PAGER_W 56
#define CARD_W (2 * CARD_PAD + TH_SQUARE_W(CARD_PREVIEW_PX))  // 4+4+126 = 134
#define CARD_H 163
#define CARD_PREVIEW_PX 136
#define CARD_PAD 4
#define CARD_GAP 3
#else
#define PAGER_W 84
#define CARD_W 194
#define CARD_H 220
#define CARD_PREVIEW_PX 182
#define CARD_PAD 6
#define CARD_GAP 4
#endif
// One gap constant for both grid axes: page_size_now() measures rows with it,
// so a mismatch between it and pad_row silently miscounts the page.
#define GRID_GAP TH_SPACE_SM
#define GRID_PAD_HOR TH_SPACE_MD
#define GRID_PAD_VER TH_SPACE_XS
#if defined(BOARD_PANEL_800X480)
// 480 would not fit a 480-tall panel. 300 also lands exactly on the master
// size below, so the detail overlay blits 1:1 with no resample at all.
#define DETAIL_PREVIEW_PX 300
#else
#define DETAIL_PREVIEW_PX 480
#endif
// Cards request tiles at CARD_PREVIEW_PX so the grid blits 1:1 — stretching
// the 300 px master through a draw-time transform on every visible card made
// scrolling crawl. The detail overlay keeps the 300 master (one static image;
// the 480 stretch only costs when the overlay repaints). thr_preview derives
// non-300 sizes from the master, so nothing is fetched twice.
#define DETAIL_SRC_PX 300  // master size (thr_preview.h note)
// Previews come off the local card now (no network), so the loader can run
// far tighter than the old 600 ms; the one-job-at-a-time guard still paces it.
#define PREVIEW_TICK_MS 150
#define PREVIEW_MAX_ATTEMPTS 3
// A batch attaches whatever is ready at least this often, so a slow or failing
// card shows progress instead of leaving the page blank until the last tile
// lands. On a healthy card the whole page beats this and costs ONE redraw.
#define PREVIEW_FLUSH_MS 400
#define ADDED_FEEDBACK_MS 2000

// Preview pipeline state, packed into the preview slot's user_data:
// low 4 bits = state, upper bits = fetch attempts so far.
enum { PV_NONE = 0, PV_INFLIGHT, PV_DONE, PV_FAILED };
#define PV_STATE(ud) ((int)((intptr_t)(ud)) & 0xF)
#define PV_ATTEMPTS(ud) ((int)((intptr_t)(ud)) >> 4)
#define PV_PACK(st, at) ((void *)(intptr_t)(((at) << 4) | (st)))

// PREFETCH loads page N+1 into the RAM LRU and attaches nothing: each tile is
// released the moment it lands, which leaves it cached at refs == 0 with a
// fresh LRU stamp. The next real load finds it in RAM, so a Next tap costs one
// redraw instead of a page of SD reads. Residency does not grow — the LRU was
// already filling all PV_RAM_SLOTS as you browsed; prefetch just fills them
// with tiles you are about to want.
enum { PV_KIND_CARD = 0, PV_KIND_DETAIL, PV_KIND_PREFETCH };

// A whole page's worth of tiles travels in one job — see the block comment at
// the preview loader for why. 16 covers any page the measured row count can
// produce (4 columns, and two rows is the ceiling this panel's height allows).
#define PV_BATCH_MAX 16

typedef struct {
    int kind;                       // PV_KIND_*
    int gen;                        // s_generation (card) / s_detail_gen (detail)
    int n;                          // entries in use; 1 for the detail overlay
    lv_obj_t *cards[PV_BATCH_MAX];  // card kind only; valid iff gen still matches
    char rel[PV_BATCH_MAX][128];
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
static char s_filter[64];    // applied search text (Enter / focus loss only)
static int s_generation;     // bumped on every grid rebuild (job guards)
static int s_page_idx;       // 0-based page of filter matches on show
static int s_page_size;      // cards per page (rows measured from the grid)
static int s_matches;        // filter matches across the whole library

static bool s_preview_pending;  // at most ONE preview job in flight
static int s_prefetched_for = -1;  // page whose successor is already prefetched
static lv_timer_t *s_preview_timer;  // kicked when a job lands, so the next
                                     // starts at once instead of waiting out
                                     // the tick (~150 ms idle per tile)
// No card (or an SD read fault): stop trying for 10 s instead of walking the
// whole grid re-discovering that the slot is empty. A per-pattern miss on a
// mounted card is permanent and handled separately (ESP_ERR_NOT_FOUND).
#define PREVIEW_BACKOFF_MS 10000
static uint32_t s_backoff_at;
static bool s_backoff_armed;

// ---- widgets ----
static lv_obj_t *s_page;
static lv_obj_t *s_body;  // grid + pager column, hidden by the empty state
static lv_obj_t *s_grid;
static lv_obj_t *s_empty;
static lv_obj_t *s_sd_banner;  // "insert the pattern card" complaint strip
static lv_obj_t *s_page_label;  // "1-8 of 1232"
static lv_obj_t *s_prev_btn;
static lv_obj_t *s_next_btn;
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
    // LVGL makes every object scrollable by default; nothing in this UI is
    // dragged (ui_page_stepper re-enables the ones it drives). See ui.h.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
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

// The grid never scrolls, so a tap on a card is always meant for that card —
// no coasting list to catch, and none of the guard this used to need.
static void card_clicked(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_current_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(card);
    if (idx < 0 || idx >= s_list.count) {
        return;
    }
    open_detail(idx, card);
}

// Empty dish + centre dot: a card's look before its tile loads, and again
// after an off-screen unload. `ud` carries the new PV_* state to record.
static void slot_show_placeholder(lv_obj_t *slot, void *ud)
{
    lv_obj_clean(slot);
    lv_obj_set_style_bg_color(slot, th.card, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, th.border, 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_user_data(slot, ud);

    lv_obj_t *dot = lv_label_create(slot);
    lv_label_set_text(dot, TH_ICON_CIRCLE);
    lv_obj_set_style_text_font(dot, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(dot, th.text3, 0);
    lv_obj_center(dot);
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
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);  // tight: both axes are scarce
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    // CENTER on the main axis: any slack left by font metrics splits evenly
    // above and below instead of pooling under the caption.
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, NULL);

    // Child 0: circular preview slot (card-color placeholder dish until the
    // card's tile arrives via the lazy loader)
    lv_obj_t *slot = plain(card);
    lv_obj_set_size(slot, TH_SQUARE_W(CARD_PREVIEW_PX), CARD_PREVIEW_PX);
    lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, 0);
    // No clip_corner: the tile paints its own corners in th.surface, so it
    // already reads as a circle. Clipping would push every visible card
    // through two ARGB8888 layers + a mask per frame (thr_preview.h).
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    slot_show_placeholder(slot, PV_PACK(PV_NONE, 0));

    // Child 1: name (basename, ".thr" stripped, one line with dots)
    char name[96];
    display_name(s_list.items[idx], name, sizeof(name));
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, name);
    // Pin to ONE line: with an auto height, DOTS wraps a long name onto a
    // second line and the card overflows CARD_H into the nav bar.
    lv_obj_set_size(label, LV_PCT(100), lv_font_get_line_height(TH_FONT_CAPTION));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, TH_FONT_CAPTION, 0);
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

// Dim a pager arrow that would go nowhere (still tappable, just inert).
static void pager_btn_enable(lv_obj_t *btn, bool on)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? th.text : th.text3, 0);
}

// "17-24 of 1232" + arrow states. Cheap; called on every rebuild.
static void update_pager(void)
{
    if (s_page_label == NULL) {
        return;
    }
    if (s_matches == 0) {
        lv_label_set_text(s_page_label, "0 patterns");
    } else {
        int first = s_page_idx * s_page_size + 1;
        int last = first + s_page_size - 1;
        if (last > s_matches) {
            last = s_matches;
        }
        lv_label_set_text_fmt(s_page_label, "%d-%d of %d", first, last, s_matches);
    }
    pager_btn_enable(s_prev_btn, s_page_idx > 0);
    pager_btn_enable(s_next_btn, (s_page_idx + 1) * s_page_size < s_matches);
}

// How many whole card rows the grid can show. Measured rather than assumed:
// the SD-complaint banner appears and disappears above the grid, and a fixed
// row count would clip a row whenever it is up.
static int page_size_now(void)
{
    lv_obj_update_layout(s_page);
    int32_t h = lv_obj_get_content_height(s_grid);
    int rows = (h + GRID_GAP) / (CARD_H + GRID_GAP);
    if (rows < 1) {
        rows = 1;  // layout not resolved yet, or a very short grid
    }
    return rows * PAGE_COLS;
}

// Build the cards for s_page_idx out of the filter matches. Returns the total
// number of matches (i.e. what the page is a window onto).
static int build_page(void)
{
    int matched = 0;
    int first = s_page_idx * s_page_size;
    for (int i = 0; i < s_list.count; i++) {
        if (!ci_contains(s_list.items[i], s_filter)) {
            continue;
        }
        if (matched >= first && matched < first + s_page_size) {
            make_card(i);
        }
        matched++;
    }
    return matched;
}

static void page_step(int delta)
{
    int next = s_page_idx + delta;
    if (next < 0 || (delta > 0 && (s_page_idx + 1) * s_page_size >= s_matches)) {
        return;
    }
    s_page_idx = next;
    rebuild_grid();
}

static void prev_clicked(lv_event_t *e)
{
    (void)e;
    page_step(-1);
}

static void next_clicked(lv_event_t *e)
{
    (void)e;
    page_step(1);
}

static void rebuild_grid(void)
{
    s_generation++;  // orphan any in-flight preview job's card pointer
    s_prefetched_for = -1;  // whatever we warmed, N+1 is a different page now
    uint32_t n_cards = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n_cards; i++) {
        release_card_preview(lv_obj_get_child(s_grid, (int32_t)i));
    }
    lv_obj_clean(s_grid);
    if (s_empty != NULL) {
        lv_obj_delete(s_empty);
        s_empty = NULL;
    }

    // The complaint the SD scheme asks for: no prepared card in the slot.
    // Set before measuring — the banner takes height away from the grid.
    if (s_sd_banner != NULL) {
        if (!s_from_sd && !sd_catalog_present()) {
            lv_obj_remove_flag(s_sd_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sd_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_page_size = page_size_now();
    int shown = build_page();
    // A shrinking result set (new filter, smaller library) can strand the
    // page index past the end; clamp and rebuild once.
    int pages = (shown + s_page_size - 1) / s_page_size;
    if (s_page_idx > 0 && s_page_idx >= pages) {
        s_page_idx = (pages > 0) ? pages - 1 : 0;
        lv_obj_clean(s_grid);
        shown = build_page();
    }
    s_matches = shown;
    update_pager();

    if (shown == 0) {
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);  // pager goes with it
        bool searching = (s_filter[0] != '\0');
        s_empty = ui_empty_state(s_page,
                                 searching ? TH_ICON_SEARCH : TH_ICON_QUEUE_MUSIC,
                                 searching ? "No patterns found" : "No patterns yet",
                                 searching ? "Try a different search term"
                                           : "Insert the pattern SD card, or connect\nto a table on the Control page");
    } else {
        lv_obj_remove_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------- preview loader

// Tiles load a PAGE at a time and attach under a SINGLE lvgl lock.
//
// Measured on the board 2026-08-26: load_tile costs ~52 ms, but consecutive
// tiles landed ~250 ms apart. The missing ~200 ms is the full-screen redraw
// every attach triggers — `full_refresh` makes ANY invalidation re-render all
// 1024x600 (lv_refr.c forces inv_areas[0] to the whole screen), `avoid_tearing`
// waits for a 41.5 ms VSYNC, and the software render of the widget tree into
// the PSRAM framebuffer costs more than one frame on top. Worse, it cannot
// overlap the SD read: lvgl_port_lock is the same recursive mutex the LVGL
// task holds around lv_timer_handler, so per-tile attaching ping-pongs.
//
// LVGL coalesces every invalidation raised while the lock is held into ONE
// refresh, so a page now pays that cost once instead of eight times.
// Slow cards still show progress: the batch flushes whatever is ready every
// PREVIEW_FLUSH_MS rather than leaving the page blank until the last tile.

// LVGL ctx. Attaches ctx->cards[from..to), releasing any tile it cannot use.
static void attach_tiles(preview_ctx_t *ctx, const lv_image_dsc_t **dsc,
                         const esp_err_t *err, int from, int to)
{
    lvgl_port_lock(0);
    for (int i = from; i < to; i++) {
        bool attached = false;
        // NOT_FOUND = the card simply has no tile for this pattern: expected on
        // a partial card, permanent, and not worth a log line per card.
        bool permanent = (err[i] == ESP_ERR_NOT_FOUND);
        // gen matches => the grid was not rebuilt, ctx->cards[i] is still alive
        if (ctx->gen == s_generation && ctx->cards[i] != NULL) {
            lv_obj_t *slot = lv_obj_get_child(ctx->cards[i], 0);
            int attempts = PV_ATTEMPTS(lv_obj_get_user_data(slot));
            if (err[i] == ESP_OK) {
                lv_obj_clean(slot);
                lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(slot, 0, 0);
                lv_obj_t *img = lv_image_create(slot);
                lv_obj_set_size(img, TH_SQUARE_W(CARD_PREVIEW_PX), CARD_PREVIEW_PX);
                lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
                lv_image_set_src(img, dsc[i]);
                lv_obj_set_user_data(img, (void *)dsc[i]);  // released on rebuild
                lv_obj_center(img);
                lv_obj_set_user_data(slot, PV_PACK(PV_DONE, attempts));
                attached = true;
            } else {
                attempts++;
                int st = (permanent || attempts >= PREVIEW_MAX_ATTEMPTS) ? PV_FAILED : PV_NONE;
                lv_obj_set_user_data(slot, PV_PACK(st, attempts));
            }
        }
        if (!attached) {
            thr_preview_release(dsc[i]);  // pinned for us but never shown (NULL-safe)
        }
    }
    lvgl_port_unlock();
}

// LVGL ctx. The detail overlay is a single big tile, so it has nothing to batch.
static void attach_detail(preview_ctx_t *ctx, const lv_image_dsc_t *dsc, esp_err_t err)
{
    lvgl_port_lock(0);
    bool attached = false;
    if (ctx->gen == s_detail_gen && s_detail_slot != NULL) {
        if (err == ESP_OK) {
            lv_obj_clean(s_detail_slot);
            lv_obj_set_style_bg_opa(s_detail_slot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_detail_slot, 0, 0);
            lv_obj_t *img = lv_image_create(s_detail_slot);
            lv_obj_set_size(img, TH_SQUARE_W(DETAIL_PREVIEW_PX), DETAIL_PREVIEW_PX);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
            lv_image_set_src(img, dsc);
            lv_obj_set_user_data(img, (void *)dsc);  // released on close
            lv_obj_center(img);
            s_detail_have_img = true;
            attached = true;
        } else {
            s_detail_attempts = (err == ESP_ERR_NOT_FOUND) ? PREVIEW_MAX_ATTEMPTS
                                                           : s_detail_attempts + 1;
        }
    }
    if (!attached) {
        thr_preview_release(dsc);  // NULL-safe
    }
    lvgl_port_unlock();
}

// Blocking SD reads; jobs task only.
static void preview_job(void *arg)
{
    preview_ctx_t *ctx = arg;
    const lv_image_dsc_t *dsc[PV_BATCH_MAX];
    esp_err_t err[PV_BATCH_MAX];
    for (int i = 0; i < PV_BATCH_MAX; i++) {
        dsc[i] = NULL;
        err[i] = ESP_ERR_INVALID_STATE;
    }
    bool no_card = false;  // an empty slot fails every card the same way

    if (ctx->kind == PV_KIND_DETAIL) {
        // Corner = whatever the tile sits on, so no mask is needed: the detail
        // image sits on the page background, grid tiles on the card surface.
        err[0] = thr_preview_get(ctx->rel[0], DETAIL_SRC_PX, ui_rgb565(th.bg), &dsc[0]);
        no_card = (err[0] != ESP_OK && err[0] != ESP_ERR_NOT_FOUND);
        if (no_card) {
            ESP_LOGW(TAG, "preview %s: %s", ctx->rel[0], esp_err_to_name(err[0]));
        }
        attach_detail(ctx, dsc[0], err[0]);
    } else if (ctx->kind == PV_KIND_PREFETCH) {
        // Warm the LRU only — no widgets, so no lock and no redraw at all.
        uint16_t corner = ui_rgb565(th.surface);
        for (int i = 0; i < ctx->n; i++) {
            const lv_image_dsc_t *d = NULL;
            esp_err_t e = thr_preview_get(ctx->rel[i], CARD_PREVIEW_PX, corner, &d);
            if (e == ESP_OK) {
                thr_preview_release(d);  // stays cached, unpinned
            } else if (e != ESP_ERR_NOT_FOUND) {
                no_card = true;  // an empty slot fails every card the same way
                break;
            }
        }
    } else {
        uint16_t corner = ui_rgb565(th.surface);
        int flushed = 0;
        int loaded = 0;
        int64_t last_flush = esp_timer_get_time();
        for (int i = 0; i < ctx->n; i++) {
            err[i] = thr_preview_get(ctx->rel[i], CARD_PREVIEW_PX, corner, &dsc[i]);
            loaded = i + 1;
            if (err[i] != ESP_OK && err[i] != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "preview %s: %s", ctx->rel[i], esp_err_to_name(err[i]));
                no_card = true;
                break;
            }
            int64_t now = esp_timer_get_time();
            if (loaded == ctx->n || (now - last_flush) >= PREVIEW_FLUSH_MS * 1000) {
                attach_tiles(ctx, dsc, err, flushed, loaded);
                flushed = loaded;
                last_flush = esp_timer_get_time();
            }
        }
        if (flushed < loaded) {
            attach_tiles(ctx, dsc, err, flushed, loaded);
        }
    }

    lvgl_port_lock(0);
    s_preview_pending = false;
    if (no_card) {
        // No card (or a read fault): back off instead of retrying per card.
        s_backoff_at = lv_tick_get();
        s_backoff_armed = true;
    } else if (s_preview_timer != NULL) {
        lv_timer_ready(s_preview_timer);  // pick up stragglers without a tick wait
    }
    lvgl_port_unlock();
    free(ctx);
}

static bool submit_detail_preview(int gen, const char *rel)
{
    preview_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return false;
    }
    ctx->kind = PV_KIND_DETAIL;
    ctx->gen = gen;
    ctx->n = 1;
    strlcpy(ctx->rel[0], rel, sizeof(ctx->rel[0]));
    if (jobs_submit(preview_job, ctx) != ESP_OK) {
        free(ctx);
        return false;
    }
    s_preview_pending = true;
    return true;
}

// Gather every card on the page still lacking a tile into ONE job.
static bool submit_page_previews(void)
{
    preview_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return false;
    }
    ctx->kind = PV_KIND_CARD;
    ctx->gen = s_generation;
    uint32_t n = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n && ctx->n < PV_BATCH_MAX; i++) {
        lv_obj_t *card = lv_obj_get_child(s_grid, i);
        lv_obj_t *slot = lv_obj_get_child(card, 0);
        void *ud = lv_obj_get_user_data(slot);
        if (PV_STATE(ud) != PV_NONE) {
            continue;
        }
        int idx = (int)(intptr_t)lv_obj_get_user_data(card);
        if (idx < 0 || idx >= s_list.count) {
            continue;
        }
        int k = ctx->n++;
        ctx->cards[k] = card;
        strlcpy(ctx->rel[k], s_list.items[idx], sizeof(ctx->rel[k]));
        lv_obj_set_user_data(slot, PV_PACK(PV_INFLIGHT, PV_ATTEMPTS(ud)));
    }
    if (ctx->n == 0) {
        free(ctx);
        return false;
    }
    if (jobs_submit(preview_job, ctx) != ESP_OK) {
        // Hand the slots back so the next tick retries them.
        for (int k = 0; k < ctx->n; k++) {
            lv_obj_t *slot = lv_obj_get_child(ctx->cards[k], 0);
            void *ud = lv_obj_get_user_data(slot);
            lv_obj_set_user_data(slot, PV_PACK(PV_NONE, PV_ATTEMPTS(ud)));
        }
        free(ctx);
        return false;
    }
    s_preview_pending = true;
    return true;
}

// Collect the rel paths page `page` would show, in filter order. Mirrors
// build_page's walk — same filter, same window — but yields strings instead of
// building widgets, because a prefetch has no cards to hang anything on.
static int fill_page_rels(preview_ctx_t *ctx, int page)
{
    int matched = 0;
    int first = page * s_page_size;
    int last = first + s_page_size;
    for (int i = 0; i < s_list.count && ctx->n < PV_BATCH_MAX; i++) {
        if (!ci_contains(s_list.items[i], s_filter)) {
            continue;
        }
        if (matched >= first) {
            if (matched >= last) {
                break;
            }
            strlcpy(ctx->rel[ctx->n], s_list.items[i], sizeof(ctx->rel[0]));
            ctx->n++;
        }
        matched++;
    }
    return ctx->n;
}

// Warm page N+1 once the visible page is complete. Idle SD bandwidth costs
// nothing here — the UI is just sitting there — and it turns a Next tap into
// one redraw instead of a page of 46 ms reads.
static bool submit_prefetch(void)
{
    if ((s_page_idx + 1) * s_page_size >= s_matches) {
        return false;  // no next page
    }
    preview_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return false;
    }
    ctx->kind = PV_KIND_PREFETCH;
    ctx->gen = s_generation;
    if (fill_page_rels(ctx, s_page_idx + 1) == 0 ||
        jobs_submit(preview_job, ctx) != ESP_OK) {
        free(ctx);
        return false;
    }
    s_preview_pending = true;
    s_prefetched_for = s_page_idx;
    return true;
}

// LVGL timer: an open detail overlay wins, then the visible page's missing
// tiles as one batch, then page N+1 into the LRU. A page is only ever
// s_page_size cards and they are all on screen, so there is nothing to
// unload — leaving the page frees its tiles via rebuild_grid.
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
        submit_detail_preview(s_detail_gen, s_detail_rel);
        return;
    }

    // Only while Browse is actually up: lv_obj_is_visible(s_grid) is false on
    // the other tabs, and loading tiles nobody is looking at just burns SD
    // bandwidth and PSRAM.
    if (!lv_obj_is_visible(s_grid)) {
        return;
    }
    if (submit_page_previews()) {
        return;  // visible cards always win the lane
    }
    if (s_prefetched_for != s_page_idx) {
        submit_prefetch();
    }
}

#ifdef UI_DEBUG_PREVIEW_SCROLL
// Hands-free paging soak: step a page every 1.5 s and report how many tiles
// are resident. The count must stay at one page's worth — a climb means
// rebuild_grid is leaking pins.
static void preview_page_tick(lv_timer_t *t)
{
    (void)t;
    int loaded = 0;
    uint32_t n = lv_obj_get_child_count(s_grid);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *slot = lv_obj_get_child(lv_obj_get_child(s_grid, i), 0);
        if (slot != NULL && PV_STATE(lv_obj_get_user_data(slot)) == PV_DONE) {
            loaded++;
        }
    }
    ESP_LOGI(TAG, "page %d: %d/%u tiles resident (~%d KB)", s_page_idx, loaded,
             (unsigned)n, loaded * CARD_PREVIEW_PX * CARD_PREVIEW_PX * 2 / 1024);
    if ((s_page_idx + 1) * s_page_size >= s_matches) {
        s_page_idx = 0;
        rebuild_grid();
    } else {
        page_step(1);
    }
}
#endif

// ------------------------------------------------------------ pattern load

// Lent to the playlist picker so it does not hold a second copy of a
// 1200-entry catalogue. LVGL ctx only — load_job swaps s_list under the lock.
const fw_str_list_t *page_browse_pattern_list(void)
{
    return &s_list;
}

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
        s_page_idx = 0;  // a new result set starts at its first page
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
        lv_obj_t *rows_row = plain(card);
        lv_obj_set_size(rows_row, LV_PCT(100), rows_h);
        lv_obj_set_flex_flow(rows_row, LV_FLEX_FLOW_ROW);
        lv_obj_t *rows = plain(rows_row);
        lv_obj_set_height(rows, LV_PCT(100));
        lv_obj_set_flex_grow(rows, 1);
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(rows, TH_SPACE_SM, 0);
        ui_page_stepper(rows_row, rows);

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
    lv_obj_set_size(back, 48, 48);
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
    lv_obj_set_size(s_detail_slot, TH_SQUARE_W(DETAIL_PREVIEW_PX), DETAIL_PREVIEW_PX);
    lv_obj_set_style_radius(s_detail_slot, LV_RADIUS_CIRCLE, 0);
    // No clip_corner — the tile's own corners are painted th.bg (see make_card)
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

// Round tap target for the Prev/Next arrows (TH_TOUCH_TARGET, fits the 90 px
// header). Dimming for "nowhere to go" is update_pager's job.
static lv_obj_t *make_pager_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *btn = plain(parent);
    lv_obj_set_size(btn, TH_TOUCH_TARGET, TH_TOUCH_TARGET);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    // Same filled disc as ui_page_stepper's arrows — Browse pages rather than
    // scrolls, so it can't share that helper, but it must look identical.
    lv_obj_set_style_bg_color(btn, th.card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, th.border, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, th.pressed, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, glyph);
    lv_obj_set_style_text_font(icon, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(icon, th.text, 0);
    lv_obj_center(icon);
    return btn;
}

lv_obj_t *page_browse_create(lv_obj_t *parent)
{
    s_page = ui_page_root(parent);
    lv_obj_t *header = ui_page_header(s_page, "Browse");

    // Which slice of the library is on show; the Up/Down buttons that move it
    // live beside the grid (built with the body, below).
    s_page_label = lv_label_create(header);
    lv_label_set_text(s_page_label, "0 patterns");
    lv_obj_set_style_text_font(s_page_label, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_page_label, th.text3, 0);

    // Refresh
    lv_obj_t *refresh = plain(header);
    lv_obj_set_size(refresh, 48, 48);
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
    lv_obj_set_size(s_search_ta, 300, 48);
    lv_obj_set_style_radius(s_search_ta, TH_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(s_search_ta, th.bg, 0);
    lv_obj_set_style_bg_opa(s_search_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_search_ta, th.border, 0);
    lv_obj_set_style_border_width(s_search_ta, 1, 0);
    lv_obj_set_style_border_color(s_search_ta, th.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_hor(s_search_ta, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_ver(s_search_ta, 10, 0);  // centres the line in 48 px
    // No scrollbar: nothing in this UI scrolls, and a one-line field whose
    // text is a hair taller than its content box would otherwise sprout one.
    lv_obj_set_scrollbar_mode(s_search_ta, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_text_font(s_search_ta, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_search_ta, th.text, 0);
    lv_obj_set_style_text_color(s_search_ta, th.text3, LV_PART_TEXTAREA_PLACEHOLDER);
    // Cursor only while actually typing.
    lv_obj_set_style_opa(s_search_ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_opa(s_search_ta, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_search_ta, search_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_focused, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_search_ta, search_apply, LV_EVENT_DEFOCUSED, NULL);

    // Shared on-screen keyboard, hidden until the search field is focused
    s_kb = ui_keyboard_create(lv_layer_top());
    lv_keyboard_set_textarea(s_kb, s_search_ta);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    // Binding the keyboard FOCUSES the field, and we then hide the keyboard —
    // leaving the pill wearing its accent ring and a blinking cursor with
    // nothing to type into. Hand the focus back.
    lv_obj_remove_state(s_search_ta, LV_STATE_FOCUSED);

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

    // Body: the card grid, with the Up/Down pager as a column down its right
    s_body = plain(s_page);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_ROW);

    // Empty band mirroring the pager, so the grid between them is centred.
    lv_obj_t *gutter = plain(s_body);
    lv_obj_set_size(gutter, PAGER_W, LV_PCT(100));

    s_grid = plain(s_body);
    lv_obj_set_height(s_grid, LV_PCT(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    // Centre each row in the grid, so the cards stay centred on screen even
    // when they do not divide the width exactly.
    // Third arg is the TRACK placement: CENTER so the block of rows sits in
    // the middle of the body. START pooled all the leftover height under the
    // last row, which reads as the grid hanging off the header.
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_grid, GRID_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(s_grid, GRID_PAD_VER, 0);  // height is scarce
    lv_obj_set_style_pad_row(s_grid, GRID_GAP, 0);
    lv_obj_set_style_pad_column(s_grid, GRID_GAP, 0);
    // Paged, never scrolled — see the note at the top of this file.
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pager = plain(s_body);
    lv_obj_set_size(pager, PAGER_W, LV_PCT(100));
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(pager, TH_SPACE_MD, 0);
    s_prev_btn = make_pager_btn(pager, LV_SYMBOL_UP, prev_clicked);
    s_next_btn = make_pager_btn(pager, LV_SYMBOL_DOWN, next_clicked);

    rebuild_grid();  // empty state until the first load

    state_add_listener(on_state_changed);
    s_preview_timer = lv_timer_create(preview_tick, PREVIEW_TICK_MS, NULL);
#ifdef UI_DEBUG_PREVIEW_SCROLL
    lv_timer_create(preview_page_tick, 1500, NULL);
#endif

    // A prepared SD card makes Browse independent of the table: load the
    // local manifest right away instead of waiting for a connection.
    if (sd_catalog_present()) {
        start_load(false);
    }

    return s_page;
}
