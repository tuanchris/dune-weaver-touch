#include "ui.h"

#include "pages/pages.h"
#include "screen_sleep.h"
#include "theme.h"

#define TAB_COUNT 5

typedef struct {
    const char *icon;
    const char *label;
    lv_obj_t *(*create)(lv_obj_t *parent);
} tab_def_t;

// Material Icons Round, matching the reference nav bar glyph-for-glyph.
static const tab_def_t TABS[TAB_COUNT] = {
    {TH_ICON_SEARCH, "Browse", page_browse_create},
    {TH_ICON_QUEUE_MUSIC, "Playlists", page_playlists_create},
    {TH_ICON_TUNE, "Control", page_control_create},
    {TH_ICON_LIGHT_MODE, "Light", page_light_create},
    {TH_ICON_PLAY_CIRCLE, "Now Playing", page_now_playing_create},
};

static lv_obj_t *s_pages[TAB_COUNT];
static lv_obj_t *s_tab_btns[TAB_COUNT];
static int s_active_tab = 0;

// One connection dot + name label per page header
static lv_obj_t *s_header_dots[TAB_COUNT + 2];
static lv_obj_t *s_header_names[TAB_COUNT + 2];
static int s_header_count = 0;

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    // LVGL makes every object scrollable by default; nothing in this UI is
    // dragged (ui_page_stepper re-enables the ones it drives). See ui.h.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void select_tab(int idx)
{
    s_active_tab = idx;
    for (int i = 0; i < TAB_COUNT; i++) {
        if (i == idx) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_color_t c = (i == idx) ? th.accent : th.text2;
        lv_obj_t *icon = lv_obj_get_child(s_tab_btns[i], 0);
        lv_obj_t *label = lv_obj_get_child(s_tab_btns[i], 1);
        lv_obj_set_style_text_color(icon, c, 0);
        lv_obj_set_style_text_color(label, c, 0);
    }
}

static void tab_clicked(lv_event_t *e)
{
    select_tab((int)(intptr_t)lv_event_get_user_data(e));
}

#ifdef UI_DEBUG_TAB_CYCLE
#include "esp_heap_caps.h"
#include "esp_log.h"
static void ui_debug_tab_tick(lv_timer_t *t)
{
    (void)t;
    int next = (s_active_tab + 1) % TAB_COUNT;
    ESP_LOGI("ui_dbg", "-> tab %d (%s), free int %u, psram %u", next, TABS[next].label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    select_tab(next);
}
#endif

static lv_obj_t *make_tab_button(lv_obj_t *nav, int idx)
{
    lv_obj_t *btn = plain(nav);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 4, 0);  // icon+label must fit TH_NAV_HEIGHT
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, TABS[idx].icon);
    lv_obj_set_style_text_font(icon, TH_FONT_TITLE, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, TABS[idx].label);
    lv_obj_set_style_text_font(label, TH_FONT_EYEBROW, 0);

    return btn;
}

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, th.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, th.text, 0);
    lv_obj_set_style_text_font(scr, TH_FONT_BODY, 0);

    lv_obj_t *content = plain(scr);
    lv_obj_set_size(content, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - TH_NAV_HEIGHT);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *nav = plain(scr);
    lv_obj_set_size(nav, LV_PCT(100), TH_NAV_HEIGHT);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, th.surface, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nav, th.border, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < TAB_COUNT; i++) {
        s_pages[i] = TABS[i].create(content);
        s_tab_btns[i] = make_tab_button(nav, i);
    }
    select_tab(0);

    screen_sleep_init();

#ifdef UI_DEBUG_TAB_CYCLE
    // Crash repro: walk the tabs automatically (remove before release)
    lv_timer_create(ui_debug_tab_tick, 4000, NULL);
#endif
}

lv_obj_t *ui_page_root(lv_obj_t *parent)
{
    lv_obj_t *page = plain(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    return page;
}

lv_obj_t *ui_page_header(lv_obj_t *page, const char *title)
{
    lv_obj_t *header = plain(page);
    lv_obj_set_size(header, LV_PCT(100), TH_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header, th.surface, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, th.border, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, TH_SPACE_LG, 0);
    lv_obj_set_style_pad_column(header, TH_SPACE_MD, 0);

    // Connection dot + table name (wired to live state in a later increment)
    lv_obj_t *dot = plain(header);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, th.danger, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(header);
    lv_label_set_text(name, "No table");
    lv_obj_set_style_text_font(name, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(name, th.text2, 0);

    // Chevron: the name is tappable (switch-table popup), like the reference.
    lv_obj_t *chev = lv_label_create(header);
    lv_label_set_text(chev, TH_ICON_EXPAND_MORE);
    lv_obj_set_style_text_font(chev, TH_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(chev, th.text3, 0);

    if (s_header_count < (int)(sizeof(s_header_dots) / sizeof(s_header_dots[0]))) {
        s_header_dots[s_header_count] = dot;
        s_header_names[s_header_count] = name;
        s_header_count++;
    }

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title_label, th.text, 0);

    return header;
}

static void pill_pressed(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_set_style_transform_scale_x(btn, 249, 0);  // ~0.97 of 256
    lv_obj_set_style_transform_scale_y(btn, 249, 0);
}

static void pill_released(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_set_style_transform_scale_x(btn, 256, 0);
    lv_obj_set_style_transform_scale_y(btn, 256, 0);
}

lv_obj_t *ui_pill_button(lv_obj_t *parent, const char *text, lv_color_t color, bool filled)
{
    lv_obj_t *btn = plain(parent);
    lv_obj_set_height(btn, TH_TOUCH_TARGET);
    lv_obj_set_style_radius(btn, TH_RADIUS_PILL, 0);
    lv_obj_set_style_pad_hor(btn, TH_SPACE_LG, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(btn, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(btn, LV_PCT(50), 0);
    lv_obj_add_event_cb(btn, pill_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, pill_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn, pill_released, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, TH_FONT_BODY, 0);

    if (filled) {
        lv_obj_set_style_bg_color(btn, color, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        // Filled-button contrast rule from ModernControlButton: light fills get
        // ink text, dark fills get bone.
        bool light_fill = lv_color_brightness(color) > 140;
        lv_obj_set_style_text_color(label, lv_color_hex(light_fill ? 0x241a0c : 0xfdf8ee), 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn, color, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_text_color(label, color, 0);
    }
    return btn;
}

// Shared on-screen keyboard, styled from the theme (LVGL's default keyboard
// is the stock light one — jarring against the night palette). One recipe
// for every page: 280 px tall, bottom-aligned in its parent, surface ground,
// card keys, amber-tinted control keys.
// ---------------------------------------------------------- paged scrolling

typedef struct {
    lv_obj_t *scroller;
    lv_obj_t *up;
    lv_obj_t *down;
} ui_stepper_t;

// A snapped element sits this far below the top edge — smaller than the gap
// between elements, so the previous one stays fully off-screen.
#define SNAP_INSET TH_SPACE_XS

static void stepper_refresh(ui_stepper_t *st)
{
    lv_obj_set_style_text_color(lv_obj_get_child(st->up, 0),
                                lv_obj_get_scroll_top(st->scroller) > 0 ? th.text : th.text3, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(st->down, 0),
                                lv_obj_get_scroll_bottom(st->scroller) > 0 ? th.text : th.text3, 0);
}

// Distance of a child's top edge from the top of the scroller's BOX,
// independent of where it is scrolled to right now. Box, not content box:
// LVGL clips children to the bounding box, so anything scrolled into the
// padding band is still drawn — measuring against the content box leaves a
// sliver of the previous element peeking in above the one you snapped to.
static int32_t child_offset(lv_obj_t *scroller, lv_obj_t *child)
{
    lv_area_t sc;
    lv_area_t cc;
    lv_obj_get_coords(scroller, &sc);
    lv_obj_get_coords(child, &cc);
    return cc.y1 - sc.y1 + lv_obj_get_scroll_y(scroller);
}

// Step a whole VIEWPORT but land on an element boundary, so a tap never
// leaves a card sliced in half at the top of the screen.
//   down: go to the last child that still starts inside the current view, so
//         the first clipped card becomes the first full one — advances as far
//         as possible without skipping anything.
//   up:   the mirror — the earliest child that is still within one viewport
//         back, so you retreat a full screen where the content allows it.
// A single element taller than the viewport has no boundary to snap to, so
// both directions fall back to a plain viewport step and scroll through it.
static void stepper_clicked(lv_event_t *e)
{
    ui_stepper_t *st = lv_event_get_user_data(e);
    bool up = (lv_event_get_current_target_obj(e) == st->up);
    int32_t vh = lv_obj_get_height(st->scroller);
    int32_t cur = lv_obj_get_scroll_y(st->scroller);
    // Compare against the element offset currently AT the top edge, not the
    // raw scroll position: the two differ by SNAP_INSET, and searching from
    // the raw one re-selects the element already at the top, which lands you
    // back where you started — a dead stop partway down a long list.
    int32_t anchor = cur + SNAP_INSET;
    int32_t best = 0;
    int32_t next = 0;  // nearest element below the fold, however far
    bool found = false;
    bool have_next = false;

    uint32_t n = lv_obj_get_child_count(st->scroller);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(st->scroller, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        int32_t off = child_offset(st->scroller, child);
        if (up) {
            if (off < anchor && off >= anchor - vh && (!found || off < best)) {
                best = off;
                found = true;
            }
        } else {
            if (off > anchor && (!have_next || off < next)) {
                next = off;
                have_next = true;
            }
            if (off > anchor && off <= anchor + vh - 1 && (!found || off > best)) {
                best = off;
                found = true;
            }
        }
    }

    int32_t target;
    if (found) {
        target = best - SNAP_INSET;
    } else {
        target = up ? cur - vh : cur + vh;  // element taller than the viewport
        // ...but never scroll past the top of whatever comes next.
        if (!up && have_next && next - SNAP_INSET < target) {
            target = next - SNAP_INSET;
        }
    }
    if (target < 0) {
        target = 0;
    }
    if (target == cur) {
        target = up ? cur - vh : cur + vh;  // never stall
    }
    lv_obj_scroll_by_bounded(st->scroller, 0, cur - target, LV_ANIM_OFF);
    stepper_refresh(st);
}

static void stepper_content_changed(lv_event_t *e)
{
    stepper_refresh(lv_event_get_user_data(e));
}

static void stepper_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static lv_obj_t *stepper_btn(lv_obj_t *parent, const char *glyph, ui_stepper_t *st)
{
    lv_obj_t *btn = plain(parent);
    lv_obj_set_size(btn, TH_TOUCH_TARGET, TH_TOUCH_TARGET);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    // A filled disc, so the arrows read as buttons rather than as decoration
    // floating in the margin.
    lv_obj_set_style_bg_color(btn, th.card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, th.border, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_bg_color(btn, th.pressed, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, stepper_clicked, LV_EVENT_CLICKED, st);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, glyph);
    lv_obj_set_style_text_font(icon, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(icon, th.text3, 0);
    lv_obj_center(icon);
    return btn;
}

#ifdef UI_DEBUG_STEP
// Hands-free stepper check: drive one scroller's Down button and log where it
// got to, so "the end of a long list is unreachable" is testable without a
// finger. Set UI_DEBUG_STEP_TAB to pick the page.
#include "esp_log.h"
static void debug_step_tick(lv_timer_t *t)
{
    static bool navigated;
    if (!navigated) {
        navigated = true;
        ui_navigate_to(UI_DEBUG_STEP_TAB);
        return;
    }
    ui_stepper_t *st = lv_timer_get_user_data(t);
    if (!lv_obj_is_visible(st->scroller)) {
        return;
    }
    ESP_LOGI("ui_dbg", "step: y=%d top=%d bottom=%d", (int)lv_obj_get_scroll_y(st->scroller),
             (int)lv_obj_get_scroll_top(st->scroller),
             (int)lv_obj_get_scroll_bottom(st->scroller));
    if (lv_obj_get_scroll_bottom(st->scroller) <= 0) {
        lv_obj_scroll_to_y(st->scroller, 0, LV_ANIM_OFF);
    } else {
        lv_obj_send_event(st->down, LV_EVENT_CLICKED, NULL);
    }
}
#endif

lv_obj_t *ui_page_stepper(lv_obj_t *parent, lv_obj_t *scroller)
{
    // Keep LV_OBJ_FLAG_SCROLLABLE (lv_obj_scroll_by needs the scroll bounds)
    // but take every direction away from the indev, so a drag does nothing.
    lv_obj_add_flag(scroller, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroller, LV_DIR_NONE);

    ui_stepper_t *st = lv_malloc(sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->scroller = scroller;

    lv_obj_t *col = plain(parent);
    lv_obj_set_size(col, TH_TOUCH_TARGET + 2 * TH_SPACE_XS, LV_PCT(100));
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, TH_SPACE_MD, 0);

    st->up = stepper_btn(col, LV_SYMBOL_UP, st);
    st->down = stepper_btn(col, LV_SYMBOL_DOWN, st);
    lv_obj_add_event_cb(col, stepper_deleted, LV_EVENT_DELETE, st);
    // Content arriving later (a list rebuild) changes what is reachable.
    lv_obj_add_event_cb(scroller, stepper_content_changed, LV_EVENT_CHILD_CHANGED, st);
    lv_obj_add_event_cb(scroller, stepper_content_changed, LV_EVENT_SIZE_CHANGED, st);
    stepper_refresh(st);
#ifdef UI_DEBUG_STEP
    lv_timer_create(debug_step_tick, 1200, st);
#endif
    return col;
}

lv_obj_t *ui_keyboard_create(lv_obj_t *parent)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), TH_KEYBOARD_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_set_style_bg_color(kb, th.surface, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb, th.border, 0);
    lv_obj_set_style_border_width(kb, 1, 0);
    lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, TH_SPACE_SM, 0);
    lv_obj_set_style_pad_gap(kb, TH_SPACE_XS, 0);

    lv_obj_set_style_bg_color(kb, th.card, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, TH_RADIUS_SM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, th.text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, TH_FONT_BODY, LV_PART_ITEMS);
    // A key press flashes accent. th.pressed was the obvious choice but it is
    // only ~9/255 per channel away from th.card — invisible at a glance, and
    // the fingertip covers the key anyway, so the confirmation has to be big
    // enough to read from the edges.
    lv_obj_set_style_bg_color(kb, th.accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, th.on_accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    // Control keys (shift, ?123, ok, backspace) carry LV_STATE_CHECKED.
    lv_obj_set_style_bg_color(kb, th.accent_soft, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, th.accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, th.accent, LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, th.on_accent,
                                LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    return kb;
}

void ui_navigate_to(int tab)
{
    if (tab >= 0 && tab < TAB_COUNT) {
        select_tab(tab);
    }
}

void ui_set_connection(bool connected, const char *name)
{
    const char *shown = (name != NULL && name[0] != '\0') ? name : (connected ? "Table" : "No table");
    for (int i = 0; i < s_header_count; i++) {
        lv_obj_set_style_bg_color(s_header_dots[i], connected ? th.ok : th.danger, 0);
        lv_label_set_text(s_header_names[i], shown);
    }
}

static void error_ok_clicked(lv_event_t *e)
{
    lv_obj_delete((lv_obj_t *)lv_event_get_user_data(e));
}

void ui_show_error(const char *msg)
{
    lv_obj_t *scrim = plain(lv_layer_top());
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind the dialog

    lv_obj_t *card = plain(scrim);
    lv_obj_set_width(card, 570);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, th.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, TH_RADIUS_MD, 0);
    lv_obj_set_style_border_color(card, th.border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, TH_SPACE_XL, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, TH_SPACE_LG, 0);

    lv_obj_t *text = lv_label_create(card);
    lv_label_set_text(text, msg);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text, TH_FONT_BODY, 0);
    lv_obj_set_style_text_color(text, th.text, 0);

    lv_obj_t *ok = ui_pill_button(card, "OK", th.accent, true);
    lv_obj_set_width(ok, 180);
    lv_obj_add_event_cb(ok, error_ok_clicked, LV_EVENT_CLICKED, scrim);
}

lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *symbol, const char *title, const char *hint)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, TH_SPACE_MD, 0);

    lv_obj_t *icon = lv_label_create(box);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, TH_FONT_DISPLAY, 0);
    lv_obj_set_style_text_color(icon, th.text3, 0);

    lv_obj_t *title_label = lv_label_create(box);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, TH_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title_label, th.text2, 0);

    if (hint != NULL) {
        lv_obj_t *hint_label = lv_label_create(box);
        lv_label_set_text(hint_label, hint);
        lv_obj_set_style_text_font(hint_label, TH_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(hint_label, th.text3, 0);
        lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    return box;
}
