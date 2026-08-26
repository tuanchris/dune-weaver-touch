// WiFi STA management (contract: wifi.h). Auto-connects with stored
// credentials, retries forever with a fixed 5 s backoff (the table lives on
// the same LAN — if the AP is down we just keep knocking), and starts SNTP
// once after the first IP (the table's quiet hours depend on /sand_time,
// which needs a real clock; docs/PORTING_NOTES.md section 1).
//
// Threading: the event handler and the retry timer run in their own tasks
// (default event loop / esp_timer). The wifi_event_cb_t fires from the event
// loop task — it must take lvgl_port_lock itself before touching LVGL.
#include "wifi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "settings.h"

// Gitignored dev convenience: defines WIFI_SSID / WIFI_PASS to seed a blank
// device without going through on-screen provisioning.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

static const char *TAG = "wifi";

#define WIFI_RETRY_DELAY_MS 5000

static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static wifi_event_cb_t s_cb;
static volatile bool s_connected;
static volatile bool s_have_creds;
static volatile bool s_scanning;    // suppresses auto-reconnect while a scan runs
static volatile bool s_persist_pending;  // wifi_join creds await first GOT_IP
static bool s_sntp_started;
static char s_ip[16];               // "255.255.255.255" + NUL; "" when down
static char s_join_ssid[33];
static char s_join_pass[65];

// Runs in the WiFi/IP event task (or wherever the edge is detected); the
// callback is responsible for its own locking.
static void fire_cb(bool connected)
{
    wifi_event_cb_t cb = s_cb;
    if (cb != NULL) {
        cb(connected);
    }
}

static esp_err_t apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    // sta.ssid/password are fixed-size fields the driver reads with a bounded
    // strnlen: a full 32-char SSID legitimately has no NUL, so strncpy's
    // truncate-without-terminator + zero-pad is exactly right here (strlcpy
    // would clip 32-char SSIDs to 31).
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);  // ESP_ERR_INVALID_STATE when idle; fine
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)WIFI_RETRY_DELAY_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retry timer: %s", esp_err_to_name(err));
    }
}

// esp_timer task context.
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_have_creds && !s_scanning && !s_connected) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reconnect: %s", esp_err_to_name(err));
            schedule_retry();
        }
    }
}

// Default event loop task context.
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_have_creds && !s_scanning) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            bool was_connected = s_connected;
            s_connected = false;
            s_ip[0] = '\0';
            ESP_LOGW(TAG, "disconnected (reason %d)", d ? d->reason : -1);
            if (was_connected) {
                fire_cb(false);
            }
            if (s_have_creds && !s_scanning) {
                schedule_retry();  // infinite retry, 5 s backoff
            }
            break;
        }
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        bool was_connected = s_connected;
        s_connected = true;
        ESP_LOGI(TAG, "got ip %s", s_ip);

        // wifi_join credentials are only trusted (and persisted) once they
        // actually produced an IP.
        if (s_persist_pending) {
            s_persist_pending = false;
            app_settings_t *s = settings_get();
            settings_lock();
            strlcpy(s->wifi_ssid, s_join_ssid, sizeof(s->wifi_ssid));
            strlcpy(s->wifi_pass, s_join_pass, sizeof(s->wifi_pass));
            settings_unlock();
            if (settings_save() != ESP_OK) {
                ESP_LOGW(TAG, "could not persist wifi credentials");
            }
        }

        if (!s_sntp_started) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_cfg.wait_for_sync = false;  // fire-and-forget; nothing blocks on sync
            if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
                s_sntp_started = true;
            } else {
                ESP_LOGW(TAG, "sntp init failed");
            }
        }

        if (!was_connected) {
            fire_cb(true);
        }
    }
}

#if defined(WIFI_SSID) && defined(WIFI_PASS)
static void seed_from_secrets(void)
{
    app_settings_t *s = settings_get();
    if (s->wifi_ssid[0] != '\0') {
        return;  // stored credentials win over the compile-time seed
    }
    ESP_LOGI(TAG, "seeding wifi credentials from wifi_secrets.h");
    strlcpy(s->wifi_ssid, WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_pass, WIFI_PASS, sizeof(s->wifi_pass));
    if (settings_save() != ESP_OK) {
        ESP_LOGW(TAG, "could not persist seeded credentials");
    }
}
#else
static void seed_from_secrets(void)
{
}
#endif

esp_err_t wifi_init(void)
{
    seed_from_secrets();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = already created
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "default sta netif");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL),
                        TAG, "ip handler");

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");

    const app_settings_t *s = settings_get();
    if (s->wifi_ssid[0] != '\0') {
        ESP_RETURN_ON_ERROR(apply_sta_config(s->wifi_ssid, s->wifi_pass), TAG, "sta config");
        s_have_creds = true;  // STA_START kicks off the first connect
    } else {
        ESP_LOGW(TAG, "no wifi credentials; waiting for provisioning");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_LOGI(TAG, "started (ssid=%s)", s->wifi_ssid[0] ? s->wifi_ssid : "(unset)");
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

const char *wifi_ip(void)
{
    return s_ip;
}

esp_err_t wifi_join(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass == NULL) {
        pass = "";
    }
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    strlcpy(s_join_pass, pass, sizeof(s_join_pass));
    s_persist_pending = true;  // persisted by the GOT_IP handler
    s_have_creds = true;

    esp_timer_stop(s_retry_timer);
    ESP_RETURN_ON_ERROR(apply_sta_config(ssid, pass), TAG, "sta config");
    esp_wifi_disconnect();  // aborts any in-flight attempt; no-op when idle
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // The disconnect above lands as STA_DISCONNECTED, whose handler
        // retries with the new config in 5 s — so this is not fatal.
        ESP_LOGW(TAG, "connect: %s (retry scheduled)", esp_err_to_name(err));
    }
    return ESP_OK;
}

static int ap_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *rb = (const wifi_ap_record_t *)b;
    return (int)rb->rssi - (int)ra->rssi;
}

int wifi_scan(wifi_ap_record_t *aps, int max)
{
    if (aps == NULL || max <= 0) {
        return -1;
    }

    // Pause the reconnect machinery: scan_start fails with ESP_ERR_WIFI_STATE
    // while a connect attempt is in flight. Scanning while *associated* is
    // fine (just slower), so an established connection is left alone.
    s_scanning = true;
    esp_timer_stop(s_retry_timer);
    if (!s_connected) {
        esp_wifi_disconnect();  // abort a possible in-flight attempt
    }

    int count = -1;
    esp_err_t err = esp_wifi_scan_start(NULL, true);  // blocking, all channels
    if (err == ESP_OK) {
        uint16_t num = (uint16_t)((max > UINT16_MAX) ? UINT16_MAX : max);
        err = esp_wifi_scan_get_ap_records(&num, aps);  // frees the driver list
        if (err == ESP_OK) {
            qsort(aps, num, sizeof(aps[0]), ap_rssi_desc);
            count = (int)num;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();  // don't leak the driver's list on error
    }

    // Restore: if credentials exist and we're not associated, resume
    // connecting right away (the scan already cost several seconds).
    s_scanning = false;
    if (s_have_creds && !s_connected) {
        if (esp_wifi_connect() != ESP_OK) {
            schedule_retry();
        }
    }
    return count;
}

void wifi_set_event_cb(wifi_event_cb_t cb)
{
    s_cb = cb;
}
