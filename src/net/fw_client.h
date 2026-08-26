// HTTP client for the table's FluidNC firmware API. The contract (routes,
// timeouts, retry rules, semantics) is docs/PORTING_NOTES.md §1 — implement it
// exactly. All calls are BLOCKING: call from the jobs/poll tasks only, never
// from the LVGL task. Thread-safe (internal mutex).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define FW_ERR_UNAUTHORIZED 0x7001  // HTTP 401 — table requires a password
#define FW_ERR_BUSY 0x7002          // 503 low-memory shed persisted after retries

esp_err_t fw_client_init(void);

// "" clears. URL is normalized (scheme added, trailing '/' stripped).
void fw_set_base_url(const char *url);
const char *fw_base_url(void);
void fw_set_password(const char *plaintext);  // sent as X-Sand-Key on every request

typedef struct {
    char state[16];     // GRBL state, substate already stripped ("Hold:0" -> "Hold")
    char hostname[33];
    char file[128];     // rel path, /sd/patterns|/patterns|/sd|/ prefixes stripped
    bool running;
    int feed;           // mm/min
    float progress;     // 0..1; negative-from-board normalized to 0
    int heap_largest;   // -1 when absent
    char led_effect[24];
    int led_brightness; // 0..255, -1 when absent
    struct {
        bool active;
        int index;           // 0-based
        int total;
        char name[64];
        bool clearing;
        int pause_remaining; // s, -1 = not pausing
        int pause_total;     // s, -1 = unknown
        char next[128];      // "" when unknown
        char last[128];      // what's on the sand now
    } playlist;
    long response_ms;   // measured round-trip, for adaptive poll pacing
} fw_status_t;

esp_err_t fw_get_status(fw_status_t *out);  // 12 s timeout

typedef struct {
    char **items;
    int count;
} fw_str_list_t;

// ETag-aware: sends If-None-Match, keeps an internal cached copy on 304.
esp_err_t fw_get_patterns(fw_str_list_t *out);
esp_err_t fw_get_playlists(fw_str_list_t *out);
void fw_str_list_free(fw_str_list_t *l);

// GET /sand_settings into an internal cache (call on connect), then read keys.
esp_err_t fw_load_settings(void);
// Returns false if key absent. key like "THR/Feed", "LED/Effect".
bool fw_setting(const char *key, char *out, size_t out_len);
// True if any "LED/"-prefixed key exists (ring-detection rule).
bool fw_has_led_ring(void);

// Actions (lifeline routes; short timeouts unless noted)
esp_err_t fw_stop(void);
esp_err_t fw_pause(void);
esp_err_t fw_resume(void);
esp_err_t fw_home(void);                       // 95 s timeout
esp_err_t fw_goto_rho(float rho);              // /sand_goto?rho=, 95 s timeout
esp_err_t fw_set_feed(int mm);
// Raw urlencoded query, e.g. "effect=rainbow&brightness=128" (no leading '?').
esp_err_t fw_led(const char *query);
// GET /command?plain=<urlencoded>. NEVER retried (double-fire hazard).
esp_err_t fw_command(const char *dollar_cmd);
// clear_ui: adaptive|clear_center|clear_perimeter|none ->
//   $Sand/Run ... clear=adaptive|in|out, or $SD/Run for none.
esp_err_t fw_run_pattern(const char *rel_path, const char *clear_ui);
// Applies $Playlist/* settings (stopping + waiting for Idle first if needed,
// per PORTING_NOTES "run-while-running"), then $Playlist/Run.
esp_err_t fw_run_playlist(const char *name, int pause_s, const char *clear_ui,
                          const char *mode, bool shuffle);
esp_err_t fw_playlist_skip(void);
// /sand_time?epoch=&tz= — epoch from SNTP; tz string param optional ("" = omit).
esp_err_t fw_sync_time(const char *posix_tz);

// Raw SD file fetch (45 s timeout, boards serve 30-60 KB/s). Caller frees *buf.
esp_err_t fw_fetch_sd(const char *sd_path, char **buf, size_t *len);

// Playlist file ops (ESP3D /upload protocol, Pi-backend multipart convention).
esp_err_t fw_upload_playlist(const char *name, const char *content);
esp_err_t fw_delete_playlist(const char *name);

// Human error text for a fw_* result (PORTING_NOTES friendly-error map).
const char *fw_friendly_error(esp_err_t err);
