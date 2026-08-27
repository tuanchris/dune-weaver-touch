#pragma once

#include "lvgl.h"

#include "../../net/fw_client.h"

lv_obj_t *page_browse_create(lv_obj_t *parent);
lv_obj_t *page_playlists_create(lv_obj_t *parent);
lv_obj_t *page_control_create(lv_obj_t *parent);
lv_obj_t *page_light_create(lv_obj_t *parent);
lv_obj_t *page_now_playing_create(lv_obj_t *parent);

// Browse owns the pattern catalogue (SD manifest, else the table's list).
// The playlist picker borrows it instead of holding a second 1200-entry copy.
// LVGL ctx only: Browse swaps the list under lvgl_port_lock, so the returned
// pointer and its strings are valid just for the duration of the lock you
// already hold — copy anything you need to keep.
const fw_str_list_t *page_browse_pattern_list(void);
