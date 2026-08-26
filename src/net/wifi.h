// WiFi STA management. Auto-connects with stored credentials at boot; the
// Control page drives scan/join for provisioning.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

// netif + event loop + STA start. If settings hold credentials, begins
// connecting (with retry/backoff) in the background. If src/net/wifi_secrets.h
// exists (gitignored; defines WIFI_SSID/WIFI_PASS) it seeds empty settings.
esp_err_t wifi_init(void);

bool wifi_is_connected(void);

// Current IPv4 as string, "" when not connected.
const char *wifi_ip(void);

// Switch credentials and reconnect; persists to settings on first GOT_IP.
esp_err_t wifi_join(const char *ssid, const char *pass);

// Blocking scan (call from the jobs task, never the LVGL task). Fills up to
// max records ordered by RSSI; returns count or <0 on error.
int wifi_scan(wifi_ap_record_t *aps, int max);

// Fired on connectivity changes from the WiFi event task. The callback must
// take lvgl_port_lock itself if it touches LVGL.
typedef void (*wifi_event_cb_t)(bool connected);
void wifi_set_event_cb(wifi_event_cb_t cb);
