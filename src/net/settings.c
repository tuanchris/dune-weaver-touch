// Persistent app settings in NVS (contract: settings.h). One namespace
// ("dwt"), one NVS entry per field, so adding fields later never invalidates
// existing data. Defaults mirror the Pi app's touch_settings.json
// (docs/PORTING_NOTES.md section 3).
#include "settings.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

static SemaphoreHandle_t s_lock;

#define SETTINGS_NAMESPACE "dwt"

// NVS keys (max 15 chars each).
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_TABLE_URL "table_url"
#define KEY_TABLE_PW "table_pw"
#define KEY_SCREEN_TO "screen_to"
#define KEY_PAUSE_S "pause_s"
#define KEY_PL_SHUFFLE "pl_shuffle"
#define KEY_PL_MODE "pl_mode"
#define KEY_PL_CLEAR "pl_clear"
#define KEY_DARK_MODE "dark_mode"

static app_settings_t s_settings;

static void set_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->screen_timeout_s = 300;
    s->pause_between_s = 10800;
    s->playlist_shuffle = true;
    strlcpy(s->playlist_run_mode, "loop", sizeof(s->playlist_run_mode));
    strlcpy(s->playlist_clear, "adaptive", sizeof(s->playlist_clear));
    s->dark_mode = true;
}

// Missing key (or a value that no longer fits) keeps the caller's default.
static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    char buf[128];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
        strlcpy(out, buf, cap);
    }
}

static void load_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) == ESP_OK) {
        *out = v;
    }
}

static void load_bool(nvs_handle_t h, const char *key, bool *out)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *out = (v != 0);
    }
}

void settings_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

void settings_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t settings_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), wiping", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    set_defaults(&s_settings);

    nvs_handle_t h;
    err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings yet, using defaults");
        return ESP_OK;  // first boot: namespace doesn't exist until first save
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");

    load_str(h, KEY_WIFI_SSID, s_settings.wifi_ssid, sizeof(s_settings.wifi_ssid));
    load_str(h, KEY_WIFI_PASS, s_settings.wifi_pass, sizeof(s_settings.wifi_pass));
    load_str(h, KEY_TABLE_URL, s_settings.table_url, sizeof(s_settings.table_url));
    load_str(h, KEY_TABLE_PW, s_settings.table_password, sizeof(s_settings.table_password));
    load_u32(h, KEY_SCREEN_TO, &s_settings.screen_timeout_s);
    load_u32(h, KEY_PAUSE_S, &s_settings.pause_between_s);
    load_bool(h, KEY_PL_SHUFFLE, &s_settings.playlist_shuffle);
    load_str(h, KEY_PL_MODE, s_settings.playlist_run_mode, sizeof(s_settings.playlist_run_mode));
    load_str(h, KEY_PL_CLEAR, s_settings.playlist_clear, sizeof(s_settings.playlist_clear));
    load_bool(h, KEY_DARK_MODE, &s_settings.dark_mode);
    nvs_close(h);

    ESP_LOGI(TAG, "loaded (table_url=%s, ssid=%s)",
             s_settings.table_url[0] ? s_settings.table_url : "(unset)",
             s_settings.wifi_ssid[0] ? s_settings.wifi_ssid : "(unset)");
    return ESP_OK;
}

app_settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open rw");

    settings_lock();  // consistent snapshot of the string fields
    esp_err_t err = ESP_OK;
    esp_err_t e;
    // Write every key, remember the first failure, commit once at the end.
    if ((e = nvs_set_str(h, KEY_WIFI_SSID, s_settings.wifi_ssid)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_str(h, KEY_WIFI_PASS, s_settings.wifi_pass)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_str(h, KEY_TABLE_URL, s_settings.table_url)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_str(h, KEY_TABLE_PW, s_settings.table_password)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_u32(h, KEY_SCREEN_TO, s_settings.screen_timeout_s)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_u32(h, KEY_PAUSE_S, s_settings.pause_between_s)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_u8(h, KEY_PL_SHUFFLE, s_settings.playlist_shuffle ? 1 : 0)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_str(h, KEY_PL_MODE, s_settings.playlist_run_mode)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_str(h, KEY_PL_CLEAR, s_settings.playlist_clear)) != ESP_OK && err == ESP_OK) err = e;
    if ((e = nvs_set_u8(h, KEY_DARK_MODE, s_settings.dark_mode ? 1 : 0)) != ESP_OK && err == ESP_OK) err = e;

    settings_unlock();

    if ((e = nvs_commit(h)) != ESP_OK && err == ESP_OK) {
        err = e;
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}
