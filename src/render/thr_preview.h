// .thr -> circular preview rendering + cache. Look and transform are
// docs/PORTING_NOTES.md §6: dish fill #1b1712, ring #3e362c, stroke #d8b578,
// x = cx + rho*R*cos(theta), y = cy + rho*R*sin(theta), rho clamped to [0,1].
// Pattern files come from the board via fw_fetch_sd (slow!) and rendered
// RGB565 tiles are cached in the LittleFS "storage" partition.
#pragma once

#include "esp_err.h"
#include "lvgl.h"

// Mounts the storage partition (label "storage") and prepares scratch buffers.
esp_err_t thr_preview_init(void);

// Get (from RAM LRU / FS cache) or fetch+render the preview for a pattern.
// BLOCKING (network + render): jobs task only. The returned descriptor is
// PINNED for the caller: it stays valid until thr_preview_release(dsc), so a
// page must release every descriptor it acquired exactly once — after
// detaching it from the widget (lv_image_set_src elsewhere / widget deleted).
// size_px is the square canvas size (use one size app-wide to keep the cache
// coherent; 300 fits the browse grid, Now Playing scales up acceptably).
esp_err_t thr_preview_get(const char *rel_path, int size_px, const lv_image_dsc_t **out);

// Unpin a descriptor from thr_preview_get. Safe from any task. NULL is a no-op.
void thr_preview_release(const lv_image_dsc_t *dsc);

// Drop the RAM LRU (e.g. after switching tables). Pinned entries linger until
// their release, then free.
void thr_preview_clear_ram(void);
