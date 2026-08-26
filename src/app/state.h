// Central app state: connection state machine + the /sand_status poll loop.
// Model is docs/PORTING_NOTES.md §1 (poll pacing, fail threshold, 401 rule)
// and §2/§3 (connect-edge loads). Pages read state and register listeners;
// they never poll the firmware themselves.
#pragma once

#include <stdbool.h>

#include "../net/fw_client.h"

typedef enum {
    CONN_IDLE,           // no table URL known yet
    CONN_SEARCHING,      // mDNS auto-discovery in progress
    CONN_CONNECTING,     // URL chosen, waiting for first good status
    CONN_CONNECTED,
    CONN_LOST,           // >= 3 consecutive poll failures; still retrying
    CONN_NEEDS_PASSWORD, // deterministic 401
} conn_state_t;

typedef struct {
    conn_state_t conn;
    char table_name[64];   // hostname from status, else "" (UI shows "No table")
    char status_line[96];  // human "reconnectStatus" style message
    fw_status_t status;    // last good status
    bool has_status;
    bool has_led_ring;     // any LED/ key present in /sand_settings
} app_state_t;

// Starts the poll task. On boot: if settings.table_url set, connect to it;
// else run one mDNS scan and auto-connect when exactly one table is found.
// Connect edge fires: fw_load_settings, LED config read, one-shot
// fw_sync_time (SNTP first; tz "" until a TZ setting exists).
void state_init(void);

// Reads must hold the lock (the poll task writes concurrently).
void state_lock(void);
void state_unlock(void);
app_state_t *state_get(void);  // valid only between state_lock/unlock

// Listener runs IN THE LVGL TASK CONTEXT (state takes lvgl_port_lock before
// calling), after every status update or connection change. Widgets may be
// touched directly; re-read state under state_lock. Max 16 listeners.
typedef void (*state_listener_t)(void);
void state_add_listener(state_listener_t cb);

// Explicit table switch (Control page tap / manual URL). Resets time-sync and
// failure streak; persists url to settings on first good status.
void state_connect_url(const char *url);
void state_disconnect(void);

// Wake the poll loop now (after actions, for snappy UI reconciliation).
void state_poll_now(void);
