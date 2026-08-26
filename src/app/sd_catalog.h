// Pattern manifest from the panel's local TF card (/sdcard/patterns.json,
// same JSON-array format the table serves from /sand_patterns). Purely
// filesystem-based — no board coupling — so the desktop sim exercises it via
// its /sdcard path remap. Blocking file I/O: call from the jobs task.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "../net/fw_client.h"  // fw_str_list_t

// True when the manifest file exists (the "insert the SD card" hint inverts
// this). Cheap stat(); safe from any task.
bool sd_catalog_present(void);

// Read + parse the manifest. Entries are normalized like the table's list
// (relative to /patterns, no leading slash). Caller owns the list
// (fw_str_list_free). ESP_ERR_NOT_FOUND when no card/manifest. When the
// manifest is absent this first retries the card mount (weak hook onto
// board/sdcard.c), so a card popped in after boot appears on the next
// Browse refresh.
esp_err_t sd_catalog_get(fw_str_list_t *out);
