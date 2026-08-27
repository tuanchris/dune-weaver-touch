#pragma once

#include <stdbool.h>

#include "lvgl.h"

// Builds the root layout: page stack + bottom navigation. Call once, with the
// LVGL port lock held.
void ui_init(void);

// Tab indices (order matches the reference app's bottom nav).
enum { UI_TAB_BROWSE = 0, UI_TAB_PLAYLISTS, UI_TAB_CONTROL, UI_TAB_LIGHT, UI_TAB_NOW_PLAYING };

// Switch tab programmatically (e.g. jump to Now Playing after starting a
// pattern). LVGL lock must be held.
void ui_navigate_to(int tab);

// Update the connection dot + table name in every page header. LVGL lock must
// be held. name NULL/"" renders as "No table".
void ui_set_connection(bool connected, const char *name);

// Show/hide a small dot on a tab's icon — currently only Control, when a
// firmware update is waiting. LVGL lock must be held.
void ui_set_tab_badge(int tab, bool on);

// Themed modal error popup (single OK button). LVGL lock must be held.
// Message should already be the friendly text (fw_friendly_error + prefix).
void ui_show_error(const char *msg);

// --- Shared building blocks (recipes from the QML components) ---

// Full-size page container: bg fill, vertical flex, no scroll of its own.
lv_obj_t *ui_page_root(lv_obj_t *parent);

// 90px header bar: connection dot, table name, page title. Returns the header
// so pages can append trailing widgets.
lv_obj_t *ui_page_header(lv_obj_t *page, const char *title);

// Pill button (ModernControlButton): radius = h/2, filled or 1px outlined.
lv_obj_t *ui_pill_button(lv_obj_t *parent, const char *text, lv_color_t color, bool filled);

// Theme-styled on-screen keyboard (100% x TH_KEYBOARD_HEIGHT, bottom-aligned
// in parent). Caller binds the textarea, event cbs, and hidden flag.
lv_obj_t *ui_keyboard_create(lv_obj_t *parent);

// Centered icon + title + hint placeholder (grid/list empty states).
lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *symbol, const char *title, const char *hint);

// Turn a scrolling container into a PAGED one. Kills the drag gesture on
// `scroller` (dragging redraws all 1024x600 every frame on this panel — see
// PORTING_NOTES §6) and returns a narrow column of Up/Down buttons, created
// in `parent`, that step it one viewport at a time. The arrows dim when
// there is nothing further that way, and follow content changes.
// The column is TH_TOUCH_TARGET wide plus padding; give `parent` a row flow
// and let the scroller flex-grow beside it.
lv_obj_t *ui_page_stepper(lv_obj_t *parent, lv_obj_t *scroller);

// Theme colour -> packed RGB565, for the raw pixel buffers preview tiles use
// (thr_preview_get's `corner`).
static inline uint16_t ui_rgb565(lv_color_t c)
{
    return (uint16_t)(((c.red >> 3) << 11) | ((c.green >> 2) << 5) | (c.blue >> 3));
}
