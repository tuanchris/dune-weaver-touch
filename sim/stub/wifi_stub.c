// Desktop stub for net/wifi.h: the host Mac IS the network. Connected by
// default, join/scan succeed instantly with plausible fakes.
//
// Set DWT_SIM_WIFI=off to start disconnected instead. The offline path is not
// otherwise reachable here, and it is a real layout: Control moves the WIFI
// card to the top when the panel has no network. Joining still "succeeds", so
// one run covers both orders.
#include "net/wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wifi_event_cb_t s_cb;
static bool s_connected = true;

esp_err_t wifi_init(void)
{
    const char *env = getenv("DWT_SIM_WIFI");
    if (env != NULL && (strcmp(env, "off") == 0 || strcmp(env, "0") == 0)) {
        s_connected = false;
    }
    if (s_cb != NULL) {
        s_cb(s_connected);
    }
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

const char *wifi_ip(void)
{
    return "127.0.0.1";
}

esp_err_t wifi_join(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    s_connected = true;
    if (s_cb != NULL) {
        s_cb(true);
    }
    return ESP_OK;
}

int wifi_scan(wifi_ap_record_t *aps, int max)
{
    static const struct { const char *ssid; int8_t rssi; } fake[] = {
        { "The Bears' Wi-Fi Network", -48 },
        { "SimNet 5G", -61 },
        { "Neighbours of the Beast", -83 },
    };
    int n = (int)(sizeof(fake) / sizeof(fake[0]));
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        memset(&aps[i], 0, sizeof(aps[i]));
        snprintf((char *)aps[i].ssid, sizeof(aps[i].ssid), "%s", fake[i].ssid);
        aps[i].rssi = fake[i].rssi;
        aps[i].authmode = WIFI_AUTH_WPA2_PSK;
    }
    return n;
}

void wifi_set_event_cb(wifi_event_cb_t cb)
{
    s_cb = cb;
}
