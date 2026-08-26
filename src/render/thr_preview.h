// Circular pattern previews, read from the pattern TF card ONLY.
// ONE folder, ONE size: the card ships a 300x300 4-bit ALPHA MASK (45,000 B)
// at /sdcard/previews/<basename lowercased>.bin (docs/PORTING_NOTES.md §7a,
// written by tools/make_pattern_sd.py). A tile is stroke coverage only — this
// module composites it over a dish built from th.preview_* at load, so the
// look is a firmware decision and retheming needs no card re-prep. The panel
// does NOT rasterize .thr files and does NOT cache tiles in flash — no card
// means no previews.
#pragma once

#include "esp_err.h"
#include "lvgl.h"

// Prepares the module (cheap; no mount, no allocation). The card itself is
// mounted by board/sdcard.c and may be inserted later.
esp_err_t thr_preview_init(void);

// Get this pattern's preview from the RAM LRU, else read it off the card.
// BLOCKING (SD read): jobs task only. The returned descriptor is PINNED for
// the caller: it stays valid until thr_preview_release(dsc), so a page must
// release every descriptor it acquired exactly once — after detaching it
// from the widget (lv_image_set_src elsewhere / widget deleted).
//
// size_px is the square canvas size. Any size works: the card's single mask
// is bilinear-resampled to it, which is cheap because it runs on one 8-bit
// channel (~3 ms for 300 -> 160). Ask for the size the widget actually
// displays at — a draw-time LVGL transform (src size != widget size) costs
// far more per frame than this one-time resample.
//
// `corner` is the RGB565 the square area OUTSIDE the dish is painted, and it
// must be the colour of whatever the image sits on. Match it and the tile
// reads as a circle with no mask: LVGL's clip_corner renders every clipped
// child through two ARGB8888 layers + a rounded-rect mask EVERY FRAME
// (lv_refr.c), which is ruinous for a scrolling grid. Use ui_rgb565() to
// convert a theme colour. It is part of the cache key.
//
// Errors worth distinguishing:
//   ESP_ERR_NOT_FOUND     — card is in, but has no tile for this pattern.
//                           PERMANENT for this card: do not retry.
//   ESP_ERR_INVALID_STATE — no card in the slot. A hot-insert can fix it, so
//                           retrying later (after sdcard_remount) is valid.
esp_err_t thr_preview_get(const char *rel_path, int size_px, uint16_t corner,
                          const lv_image_dsc_t **out);

// Unpin a descriptor from thr_preview_get. Safe from any task. NULL is a no-op.
void thr_preview_release(const lv_image_dsc_t *dsc);

// Drop the RAM LRU (e.g. after swapping cards). Pinned entries linger until
// their release, then free.
void thr_preview_clear_ram(void);
