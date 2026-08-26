// Local TF card (panel-side pattern manifest + pre-rendered previews).
// FAT filesystem mounted at /sdcard. Card layout (prepared by
// tools/make_pattern_sd.py, see docs/PORTING_NOTES.md §8):
//   /sdcard/patterns.json        — JSON array of paths relative to /patterns
//   /sdcard/previews/<key>.bin   — raw RGB565 300x300, key = basename
//                                  lowercased (incl. ".thr")
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SDCARD_MOUNT "/sdcard"

// Mount attempt; ESP_OK when a card is mounted (idempotent). A missing or
// unreadable card is NOT fatal — callers degrade and the UI shows a hint.
esp_err_t sdcard_mount(void);

bool sdcard_mounted(void);

// Unmount + mount again — the retry hook for a card inserted after boot.
esp_err_t sdcard_remount(void);
