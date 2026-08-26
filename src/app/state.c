// Central app state: connection state machine + /sand_status poll loop.
// Port of the reference backend.py poll/connect logic (PORTING_NOTES §1
// "Resilience", §2 auto-discovery, §3 connect-edge loads).
//
// Threading model:
//   - One dedicated poll task owns all blocking firmware I/O here. It is
//     separate from the jobs worker so a slow job (SD file fetch, upload)
//     never starves the status poll.
//   - app_state_t is guarded by s_mutex (state_lock/state_unlock).
//   - UI updates happen under lvgl_port_lock; listeners are invoked there and
//     re-read state under state_lock. Lock order is always state -> release ->
//     lvgl (never nested the other way), so there is no ordering cycle.
//   - state_add_listener is NOT thread-safe: it must only be called during
//     ui_init(), which main.c runs before state_init() spawns the poll task.
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "../net/discovery.h"
#include "../net/settings.h"
#include "../net/wifi.h"
#include "../ui/ui.h"

static const char *TAG = "state";

// PORTING_NOTES §1 Resilience — exact values from the reference app.
#define POLL_BASE_MS 1000
#define POLL_LOWHEAP_MS 30000
#define HEAP_LARGEST_WARN 20000
#define FAIL_THRESHOLD 3

#define POLL_TASK_STACK 8192  // bytes, internal RAM (HTTP + JSON run here)
#define POLL_TASK_PRIO 4
#define BOOT_WIFI_WAIT_MS 500   // recheck cadence while waiting for WiFi
#define IDLE_SLEEP_MS 1000      // cadence when no table URL is configured
#define DISCOVERY_MAX 8

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_poll_task;

static app_state_t s_state;

// Listener registry — writes only during ui_init (single-task app_main
// context, before the poll task exists), reads from publish().
#define MAX_LISTENERS 16
static state_listener_t s_listeners[MAX_LISTENERS];
static int s_listener_count;

// Poll-control variables. Written by the poll task and by the LVGL task
// (state_connect_url / state_disconnect); mutated under s_mutex.
static int s_fail_streak;
static bool s_time_synced;     // one-shot /sand_time push per connection
static bool s_boot_scan_done;  // boot auto-discovery runs at most once
static bool s_low_heap;        // last good status had heap_largest < WARN
static long s_last_response_ms;

// Connection generation: bumped on every begin_connect/disconnect. A poll can
// block up to 12 s; a result whose generation no longer matches (user switched
// tables or disconnected mid-poll) is discarded instead of resurrecting the
// old connection.
static unsigned s_conn_gen;

// State's own copy of the base URL. The poll loop and connect_edge read this
// under s_mutex instead of fw_base_url(), whose buffer the LVGL task can
// rewrite concurrently.
static char s_base_url[128];

static void ensure_mutex(void)
{
    // First calls (state_add_listener / early state_lock) happen from
    // app_main before any other task exists, so lazy creation is race-free.
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void state_lock(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void state_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

app_state_t *state_get(void)
{
    return &s_state;
}

void state_add_listener(state_listener_t cb)
{
    if (cb == NULL) {
        return;
    }
    if (s_listener_count >= MAX_LISTENERS) {
        ESP_LOGE(TAG, "listener table full (%d)", MAX_LISTENERS);
        return;
    }
    s_listeners[s_listener_count++] = cb;
}

// Push the current state to the UI: connection header on every page, then
// every registered listener. Runs with lvgl_port_lock held so listeners may
// touch widgets directly (they re-read details under state_lock themselves).
static void publish(void)
{
    conn_state_t conn;
    char name[sizeof(s_state.table_name)];

    state_lock();
    conn = s_state.conn;
    strlcpy(name, s_state.table_name, sizeof(name));
    state_unlock();

    lvgl_port_lock(0);
    ui_set_connection(conn == CONN_CONNECTED, name);
    for (int i = 0; i < s_listener_count; i++) {
        s_listeners[i]();
    }
    lvgl_port_unlock();
}

static void set_conn(conn_state_t conn, const char *status_line)
{
    state_lock();
    s_state.conn = conn;
    strlcpy(s_state.status_line, status_line, sizeof(s_state.status_line));
    state_unlock();
    publish();
}

// Point the client at a table and (re)start the connect handshake. The next
// good status fires the connect edge. Never blocks (safe from LVGL context).
static void begin_connect(const char *url)
{
    fw_set_base_url(url);
    const char *base = fw_base_url();
    if (base == NULL) {
        base = "";
    }

    char line[sizeof(s_state.status_line)];
    snprintf(line, sizeof(line), "Connecting to %s...", base);

    state_lock();
    s_conn_gen++;
    strlcpy(s_base_url, base, sizeof(s_base_url));
    s_fail_streak = 0;       // fresh table, fresh streak
    s_time_synced = false;   // re-push /sand_time on this connection
    s_boot_scan_done = true; // an explicit target supersedes boot discovery
    s_state.conn = CONN_CONNECTING;
    strlcpy(s_state.status_line, line, sizeof(s_state.status_line));
    state_unlock();
    publish();
}

// PORTING_NOTES §1: reading Playlist/ClearPattern back, sideway/random show
// as adaptive (the UI has no chips for them).
static const char *fw_clear_to_ui(const char *fw)
{
    if (strcasecmp(fw, "in") == 0) {
        return "clear_center";
    }
    if (strcasecmp(fw, "out") == 0) {
        return "clear_perimeter";
    }
    if (strcasecmp(fw, "none") == 0) {
        return "none";
    }
    return "adaptive";
}

// PORTING_NOTES §3: board NVS wins for playlist settings on connect.
static void seed_playlist_settings(void)
{
    app_settings_t *cfg = settings_get();
    char buf[24];
    bool changed = false;

    settings_lock();
    if (fw_setting("Playlist/Mode", buf, sizeof(buf))) {
        const char *mode = (strcasecmp(buf, "single") == 0) ? "single" : "loop";
        if (strcmp(cfg->playlist_run_mode, mode) != 0) {
            strlcpy(cfg->playlist_run_mode, mode, sizeof(cfg->playlist_run_mode));
            changed = true;
        }
    }
    if (fw_setting("Playlist/Shuffle", buf, sizeof(buf))) {
        bool shuffle = strcasecmp(buf, "ON") == 0;
        if (cfg->playlist_shuffle != shuffle) {
            cfg->playlist_shuffle = shuffle;
            changed = true;
        }
    }
    if (fw_setting("Playlist/PauseTime", buf, sizeof(buf))) {
        long secs = strtol(buf, NULL, 10);
        if (secs >= 0 && (uint32_t)secs != cfg->pause_between_s) {
            cfg->pause_between_s = (uint32_t)secs;
            changed = true;
        }
    }
    if (fw_setting("Playlist/ClearPattern", buf, sizeof(buf))) {
        const char *ui = fw_clear_to_ui(buf);
        if (strcmp(cfg->playlist_clear, ui) != 0) {
            strlcpy(cfg->playlist_clear, ui, sizeof(cfg->playlist_clear));
            changed = true;
        }
    }
    settings_unlock();

    if (changed && settings_save() != ESP_OK) {
        ESP_LOGW(TAG, "could not persist board playlist settings");
    }
}

// Connect edge: first good status after boot/connecting/lost/needs-password.
// Blocking (runs in the poll task): settings + LED-ring detection + one-shot
// time sync + persist the working URL.
static void connect_edge(fw_status_t *st)
{
    bool has_ring = false;
    esp_err_t err = fw_load_settings();
    if (err == ESP_OK) {
        has_ring = fw_has_led_ring();
        char buf[16];
        if (fw_setting("THR/Feed", buf, sizeof(buf))) {
            int feed = atoi(buf);
            if (feed > 0) {
                st->feed = feed;  // base feed setting seeds the speed control
            }
        }
        seed_playlist_settings();
    } else {
        ESP_LOGW(TAG, "settings load on connect failed: %s", esp_err_to_name(err));
    }

    state_lock();
    s_state.has_led_ring = has_ring;
    bool need_time_sync = !s_time_synced;
    state_unlock();

    // Board-local quiet hours / autostart schedules run on board time — push
    // it once per connection (tz "" until a TZ setting exists; see fw_client).
    if (need_time_sync) {
        err = fw_sync_time("");
        if (err == ESP_OK) {
            state_lock();
            s_time_synced = true;
            state_unlock();
        } else {
            ESP_LOGD(TAG, "time sync failed (%s); will retry on next connect",
                     esp_err_to_name(err));
        }
    }

    // Persist the URL that actually answered so the next boot skips discovery.
    char base[sizeof(s_base_url)];
    state_lock();
    strlcpy(base, s_base_url, sizeof(base));
    state_unlock();
    if (base[0] != '\0') {
        app_settings_t *cfg = settings_get();
        settings_lock();
        bool url_changed = strcmp(cfg->table_url, base) != 0;
        if (url_changed) {
            strlcpy(cfg->table_url, base, sizeof(cfg->table_url));
        }
        settings_unlock();
        if (url_changed && settings_save() != ESP_OK) {
            ESP_LOGW(TAG, "could not persist table url");
        }
    }
    ESP_LOGI(TAG, "connected to %s (led ring: %s)", base, has_ring ? "yes" : "no");
}

static void poll_once(void)
{
    fw_status_t st;
    state_lock();
    unsigned gen = s_conn_gen;
    state_unlock();

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = fw_get_status(&st);
    long elapsed_ms = (long)((esp_timer_get_time() - t0) / 1000);

    // Discard results from a superseded connection (the user disconnected or
    // switched tables while this poll was in flight — up to 12 s).
    state_lock();
    bool stale = gen != s_conn_gen;
    state_unlock();
    if (stale) {
        return;
    }

    if (err == ESP_OK) {
        state_lock();
        s_fail_streak = 0;
        s_last_response_ms = st.response_ms > 0 ? st.response_ms : elapsed_ms;
        s_low_heap = st.heap_largest >= 0 && st.heap_largest < HEAP_LARGEST_WARN;
        bool edge = s_state.conn != CONN_CONNECTED;
        state_unlock();

        if (edge) {
            connect_edge(&st);
            state_lock();
            stale = gen != s_conn_gen;  // connect_edge blocks too
            state_unlock();
            if (stale) {
                return;
            }
        }

        state_lock();
        s_state.conn = CONN_CONNECTED;
        strlcpy(s_state.status_line, "Connected", sizeof(s_state.status_line));
        if (st.hostname[0] != '\0') {
            strlcpy(s_state.table_name, st.hostname, sizeof(s_state.table_name));
        }
        s_state.status = st;
        s_state.has_status = true;
        state_unlock();
        publish();
        return;
    }

    // Failed — treat as a slow response for backoff so a struggling board
    // gets polled less, not hammered.
    state_lock();
    s_fail_streak++;
    s_last_response_ms = elapsed_ms;
    int streak = s_fail_streak;
    conn_state_t conn = s_state.conn;
    state_unlock();

    if (err == FW_ERR_UNAUTHORIZED) {
        // Deterministic — no fail-streak grace (PORTING_NOTES §1 Auth).
        set_conn(CONN_NEEDS_PASSWORD, "Table requires a password - set it below.");
        return;
    }
    if (streak >= FAIL_THRESHOLD) {
        if (conn != CONN_LOST) {
            ESP_LOGW(TAG, "table unreachable: %s", esp_err_to_name(err));
        }
        set_conn(CONN_LOST, "Table connection lost, retrying...");
        return;
    }
    // Below threshold: one slow/failed poll means "busy", not "gone" — keep
    // the current connection state, but still wake the listeners.
    ESP_LOGD(TAG, "status poll failed (%d/%d): %s", streak, FAIL_THRESHOLD,
             esp_err_to_name(err));
    publish();
}

// Boot auto-discovery: runs once, only when no table URL is saved. Waits for
// WiFi first (a scan without a network would land on "No table found" and
// never retry). Exactly one table found -> auto-connect; anything else asks
// the user to pick (Control page).
static void boot_flow(void)
{
    state_lock();
    conn_state_t conn = s_state.conn;
    state_unlock();

    if (!wifi_is_connected()) {
        if (conn != CONN_SEARCHING) {
            set_conn(CONN_SEARCHING, "Looking for table...");
        }
        return;  // loop re-enters after BOOT_WIFI_WAIT_MS
    }

    state_lock();
    s_boot_scan_done = true;
    state_unlock();
    set_conn(CONN_SEARCHING, "Looking for table...");

    esp_err_t err = discovery_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "discovery init failed: %s", esp_err_to_name(err));
        set_conn(CONN_IDLE, "No table found. Enter the table address.");
        return;
    }

    table_info_t tables[DISCOVERY_MAX];
    int n = discovery_scan(tables, DISCOVERY_MAX, 3000);
    if (n == 1) {
        ESP_LOGI(TAG, "auto-connecting to the only table found: %s (%s)",
                 tables[0].name, tables[0].url);
        begin_connect(tables[0].url);
        return;  // next loop iteration polls it
    }
    if (n > 1) {
        ESP_LOGI(TAG, "discovery found %d tables - waiting for a manual pick", n);
        set_conn(CONN_IDLE, "Several tables found. Pick one on the Control page.");
        return;
    }
    set_conn(CONN_IDLE, "No table found. Enter the table address.");
}

static void poll_task(void *arg)
{
    (void)arg;
    publish();  // surface the boot state before the first (possibly slow) poll

    for (;;) {
        uint32_t delay_ms;
        char base[sizeof(s_base_url)];
        state_lock();
        strlcpy(base, s_base_url, sizeof(base));
        state_unlock();

        if (base[0] == '\0') {
            state_lock();
            bool scan_done = s_boot_scan_done;
            state_unlock();
            if (!scan_done) {
                boot_flow();
                delay_ms = BOOT_WIFI_WAIT_MS;
            } else {
                delay_ms = IDLE_SLEEP_MS;  // idle until state_connect_url
            }
        } else {
            state_lock();
            conn_state_t conn = s_state.conn;
            state_unlock();
            if (conn == CONN_CONNECTING && !wifi_is_connected()) {
                // Boot with a saved URL while WiFi is still associating: a
                // poll can only fail instantly and would flash a spurious
                // "connection lost". Once connected, WiFi drops go through
                // the normal fail streak instead.
                delay_ms = BOOT_WIFI_WAIT_MS;
            } else {
                poll_once();
                state_lock();
                delay_ms = s_low_heap ? POLL_LOWHEAP_MS : POLL_BASE_MS;
                if (s_last_response_ms > (long)delay_ms) {
                    delay_ms = (uint32_t)s_last_response_ms;
                }
                state_unlock();
            }
        }

        // Sleep, but let state_poll_now() cut the wait short. A single task
        // also means polls can never overlap/stack against a busy board.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

void state_init(void)
{
    ensure_mutex();

    app_settings_t *cfg = settings_get();
    fw_set_password(cfg->table_password);

    state_lock();
    s_state.conn = CONN_IDLE;
    strlcpy(s_state.status_line, "Looking for table...", sizeof(s_state.status_line));
    s_state.table_name[0] = '\0';
    s_state.has_status = false;
    s_state.has_led_ring = false;
    state_unlock();

    if (cfg->table_url[0] != '\0') {
        begin_connect(cfg->table_url);  // saved table: skip discovery
    }

    if (xTaskCreate(poll_task, "state_poll", POLL_TASK_STACK, NULL,
                    POLL_TASK_PRIO, &s_poll_task) != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
    }
}

void state_connect_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return;
    }

    // Normalize like the reference client: default scheme, no trailing '/'.
    char norm[sizeof(((app_settings_t *)0)->table_url)];
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(norm, sizeof(norm), "http://%s", url);
    } else {
        strlcpy(norm, url, sizeof(norm));
    }
    size_t len = strlen(norm);
    while (len > 0 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }

    ESP_LOGI(TAG, "connecting to table: %s", norm);
    begin_connect(norm);
    state_poll_now();
}

void state_disconnect(void)
{
    ESP_LOGI(TAG, "disconnecting from table");
    fw_set_base_url("");

    state_lock();
    s_conn_gen++;
    s_base_url[0] = '\0';
    s_fail_streak = 0;
    s_time_synced = false;
    s_boot_scan_done = true;  // an explicit disconnect must not trigger a rescan
    s_state.conn = CONN_IDLE;
    strlcpy(s_state.status_line, "Disconnected.", sizeof(s_state.status_line));
    s_state.table_name[0] = '\0';
    s_state.has_status = false;
    s_state.has_led_ring = false;
    state_unlock();
    publish();
    state_poll_now();
}

void state_poll_now(void)
{
    if (s_poll_task != NULL) {
        xTaskNotifyGive(s_poll_task);
    }
}
