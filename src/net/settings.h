// Persistent app settings in NVS (the ESP32 equivalent of touch_settings.json).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char table_url[128];       // normalized base URL ("http://192.168.1.10"), "" if unset
    char table_password[65];   // plaintext X-Sand-Key, "" if unset
    uint32_t screen_timeout_s; // 0 = never; default 60 (the "1 m" chip)
    uint32_t pause_between_s;  // default 10800 (3 h)
    bool playlist_shuffle;     // default true
    char playlist_run_mode[8]; // "loop" | "single"; default "loop"
    char playlist_clear[20];   // UI name (adaptive/clear_center/clear_perimeter/none)
    bool dark_mode;            // default: 5B false (flicker), 7 true — settings.c
} app_settings_t;

// nvs_flash_init + load (missing keys get defaults).
esp_err_t settings_init(void);

// Singleton; mutate fields then call settings_save().
app_settings_t *settings_get(void);

// Guard for cross-task mutation of string fields (LVGL task, WiFi event task
// and the poll task all write). Single-word scalar writes from the LVGL task
// may skip it; every writer of a char[] field must hold it. settings_save
// takes it internally, so never call settings_save while holding it.
void settings_lock(void);
void settings_unlock(void);

esp_err_t settings_save(void);
