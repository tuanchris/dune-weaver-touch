// HTTP client for the table's FluidNC firmware API (contract: fw_client.h +
// docs/PORTING_NOTES.md §1; semantics cross-checked against the Pi app's
// firmware_client.py and the backend's fluidnc_client.py).
//
// Threading model:
//   - s_wire_mutex serializes every actual HTTP request (the board's web
//     server is single-threaded anyway). Held for the whole request incl.
//     503/transient retry sleeps.
//   - s_cfg_mutex guards base_url/password only and is held for microseconds
//     (memcpy), never across a request — so fw_set_base_url / fw_set_password
//     / fw_base_url (called from LVGL callbacks) are never blocked behind an
//     in-flight request. Each request snapshots the config at start.
//   - s_cache_mutex guards the patterns ETag cache and the settings cache
//     (short holds only: linear scans over small arrays).
// All fw_* calls that hit the network are BLOCKING: jobs/poll tasks only.
#include "fw_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "fw_client";

// Per-route timeouts (PORTING_NOTES §1).
#define TIMEOUT_DEFAULT_MS 6000
#define TIMEOUT_STATUS_MS 12000   // /sand_status: boards answer 0.1-14 s mid-pattern
#define TIMEOUT_MOTION_MS 95000   // /sand_home, /sand_goto
#define TIMEOUT_SD_MS 45000       // /sd/ fetch: boards serve 30-60 KB/s
#define TIMEOUT_UPLOAD_MS 60000   // POST /upload

// 503 "busy: low memory" load-shedding retry (3 total tries, expo backoff
// with jitter) and the single transient (timeout/conn) retry.
#define RETRY_503_ATTEMPTS 3
#define RETRY_503_BASE_MS 300
#define TRANSIENT_RETRY_DELAY_MS 500

// Response bodies grow in SPIRAM up to this hard cap (largest real body is
// the ~1000-file pattern manifest, well under 100 KB).
#define FW_BODY_MAX (2 * 1024 * 1024)
#define FW_READ_CHUNK 2048

// Path (everything after the base URL) and full-URL buffer sizes. Worst case
// is /command?plain= with a fully percent-encoded $Sand/Run command
// (~165 chars * 3) — comfortably under FW_PATH_MAX.
#define FW_PATH_MAX 576
#define FW_ENC_MAX 512

// Idle-wait before $Playlist/Run while the table is running (PORTING_NOTES:
// NVS writes are idle-gated; stop, then poll every 0.5 s up to 15 s).
#define IDLE_WAIT_TIMEOUT_MS 15000
#define IDLE_POLL_INTERVAL_MS 500

typedef struct {
    char *key;
    char *val;
} fw_setting_kv_t;

// One in-flight HTTP response (body owned by the SPIRAM heap).
typedef struct {
    char *body;      // NUL-terminated; free() when done
    size_t len;      // bytes, excluding the NUL
    int status;      // HTTP status code
    long elapsed_ms; // round-trip of the successful attempt
    char etag[64];   // captured ETag header, "" if none
} fw_response_t;

static SemaphoreHandle_t s_wire_mutex;  // serializes HTTP requests
static SemaphoreHandle_t s_cfg_mutex;   // base_url/password, short-hold only
static SemaphoreHandle_t s_cache_mutex; // patterns + settings caches

static char s_base_url[128]; // normalized "http://host[:port]", "" = no table
static char s_password[65];  // plaintext X-Sand-Key, "" = none

// /sand_patterns ETag cache: the list handed out on a 304 revalidation.
static char **s_pat_cache;
static int s_pat_count;
static char s_pat_etag[64];

// /sand_settings cache: flat "Ns/Key" -> value copies, replaced wholesale.
static fw_setting_kv_t *s_settings_kv;
static int s_settings_count;

// ------------------------------------------------------------------ helpers

static char *dup_str_spiram(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static void str_list_release(char **items, int count)
{
    if (items == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static esp_err_t str_list_dup(char *const *src, int count, fw_str_list_t *out)
{
    out->items = NULL;
    out->count = 0;
    if (count <= 0) {
        return ESP_OK;
    }
    char **items = heap_caps_malloc((size_t)count * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (items == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; i++) {
        items[i] = dup_str_spiram(src[i]);
        if (items[i] == NULL) {
            str_list_release(items, i);
            return ESP_ERR_NO_MEM;
        }
    }
    out->items = items;
    out->count = count;
    return ESP_OK;
}

static esp_err_t json_array_to_list(const cJSON *arr, fw_str_list_t *out)
{
    out->items = NULL;
    out->count = 0;
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) {
        return ESP_OK;
    }
    char **items = heap_caps_malloc((size_t)n * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (items == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || it->valuestring == NULL) {
            continue; // tolerate junk entries; the contract says strings
        }
        items[count] = dup_str_spiram(it->valuestring);
        if (items[count] == NULL) {
            str_list_release(items, count);
            return ESP_ERR_NO_MEM;
        }
        count++;
    }
    out->items = items;
    out->count = count;
    return ESP_OK;
}

// Percent-encode like Python's urllib quote: everything but the unreserved
// set [A-Za-z0-9_.~-] is escaped ('/' kept only for SD path fetches).
// Bounded; always NUL-terminates, silently truncating if dst is too small.
static void url_encode(char *dst, size_t cap, const char *src, bool keep_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        unsigned char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                    c == '~' || c == '-' || (keep_slash && c == '/');
        if (safe) {
            if (o + 1 >= cap) {
                break;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= cap) {
                break;
            }
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

// UI clear-mode name -> firmware clear= name (PORTING_NOTES clear-mode map).
// Firmware-native names pass through; anything unknown degrades to "none".
static const char *map_clear_ui(const char *ui)
{
    if (ui == NULL) {
        return "none";
    }
    if (strcmp(ui, "adaptive") == 0) {
        return "adaptive";
    }
    if (strcmp(ui, "clear_center") == 0 || strcmp(ui, "in") == 0) {
        return "in";
    }
    if (strcmp(ui, "clear_perimeter") == 0 || strcmp(ui, "out") == 0) {
        return "out";
    }
    return "none";
}

// "<name>[.txt]" -> "<name>" ($Playlist/Run and the /upload part names take
// the bare name / full SD path respectively; never double-append .txt).
static void strip_txt_suffix(const char *name, char *out, size_t cap)
{
    strlcpy(out, name, cap);
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".txt") == 0) {
        out[len - 4] = '\0';
    }
}

// ------------------------------------------------------------- HTTP engine

// Captures the ETag response header into the fw_response_t (needed for the
// /sand_patterns conditional-GET cache).
static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data != NULL &&
        evt->header_key != NULL && evt->header_value != NULL &&
        strcasecmp(evt->header_key, "ETag") == 0) {
        fw_response_t *resp = (fw_response_t *)evt->user_data;
        strlcpy(resp->etag, evt->header_value, sizeof(resp->etag));
    }
    return ESP_OK;
}

// One request attempt: manual open/fetch_headers/read loop, body into a
// growing SPIRAM buffer (NUL-terminated, FW_BODY_MAX cap). Error results are
// already normalized: ESP_ERR_TIMEOUT (socket timeout), ESP_FAIL (connect /
// transport failure), ESP_ERR_NO_MEM. Never leaks raw esp_http_client codes.
static esp_err_t http_attempt(const char *url, const char *password,
                              int timeout_ms, const char *if_none_match,
                              const char *post_body, size_t post_len,
                              const char *post_content_type,
                              fw_response_t *resp)
{
    resp->body = NULL;
    resp->len = 0;
    resp->status = 0;
    resp->etag[0] = '\0';

    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = on_http_event,
        .user_data = resp,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        // TX buffer must hold the request line: /command?plain= URLs run
        // ~550 bytes fully percent-encoded, over the 512-byte default.
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (post_body != NULL) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (post_content_type != NULL) {
            esp_http_client_set_header(client, "Content-Type", post_content_type);
        }
    }
    if (password[0] != '\0') {
        esp_http_client_set_header(client, "X-Sand-Key", password);
    }
    if (if_none_match != NULL && if_none_match[0] != '\0') {
        esp_http_client_set_header(client, "If-None-Match", if_none_match);
    }

    esp_err_t err = esp_http_client_open(client, post_body != NULL ? (int)post_len : 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(client);
        return ESP_FAIL; // connect-level failure
    }
    if (post_body != NULL &&
        esp_http_client_write(client, post_body, (int)post_len) != (int)post_len) {
        ESP_LOGD(TAG, "short write: %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t hdr = esp_http_client_fetch_headers(client);
    if (hdr < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return (hdr == -ESP_ERR_HTTP_EAGAIN) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    resp->status = esp_http_client_get_status_code(client);

    size_t cap = (hdr > 0 && hdr < FW_BODY_MAX) ? (size_t)hdr + 1 : 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    err = ESP_OK;
    while (true) {
        size_t room = cap - total - 1;
        if (room == 0) {
            if (cap > FW_BODY_MAX) {
                ESP_LOGE(TAG, "body exceeds %d bytes: %s", FW_BODY_MAX, url);
                err = ESP_ERR_NO_MEM;
                break;
            }
            size_t next = cap * 2;
            if (next > FW_BODY_MAX + 1) {
                next = FW_BODY_MAX + 1;
            }
            char *grown = heap_caps_realloc(buf, next, MALLOC_CAP_SPIRAM);
            if (grown == NULL) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            buf = grown;
            cap = next;
            room = cap - total - 1;
        }
        int want = room > FW_READ_CHUNK ? FW_READ_CHUNK : (int)room;
        int n = esp_http_client_read(client, buf + total, want);
        if (n < 0) {
            err = (n == -ESP_ERR_HTTP_EAGAIN) ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        if (n == 0) {
            break; // body complete (or connection closed by the board)
        }
        total += (size_t)n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[total] = '\0';
    resp->body = buf;
    resp->len = total;
    return ESP_OK;
}

static void response_discard(fw_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
    resp->len = 0;
}

// Serialized request with the contract's retry + error normalization:
//   503 -> up to RETRY_503_ATTEMPTS tries, 300*2^n + rand(0..300) ms backoff,
//          then FW_ERR_BUSY;
//   timeout/conn failure -> 1 retry after 500 ms iff retry_transient (never
//          for $-commands or file ops: double-fire hazard), then
//          ESP_ERR_TIMEOUT / ESP_FAIL;
//   401 -> FW_ERR_UNAUTHORIZED. 304 returns ESP_OK (check resp->status).
// On ESP_OK the caller owns resp->body (free()).
static esp_err_t fw_perform(const char *path, int timeout_ms, bool retry_transient,
                            const char *if_none_match,
                            const char *post_body, size_t post_len,
                            const char *post_content_type, fw_response_t *resp)
{
    if (s_wire_mutex == NULL) {
        return ESP_ERR_INVALID_STATE; // fw_client_init not called
    }

    // Snapshot the target under the short-hold config mutex; the request then
    // runs against a stable copy even if the UI retargets mid-flight.
    char base[sizeof(s_base_url)];
    char password[sizeof(s_password)];
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    strlcpy(base, s_base_url, sizeof(base));
    strlcpy(password, s_password, sizeof(password));
    xSemaphoreGive(s_cfg_mutex);
    if (base[0] == '\0') {
        return ESP_ERR_INVALID_STATE; // no table selected
    }

    char url[sizeof(base) + FW_PATH_MAX];
    snprintf(url, sizeof(url), "%s%s", base, path);

    esp_err_t err = ESP_OK;
    int attempt_503 = 0;
    int transient_left = retry_transient ? 1 : 0;

    xSemaphoreTake(s_wire_mutex, portMAX_DELAY);
    while (true) {
        int64_t start_us = esp_timer_get_time();
        err = http_attempt(url, password, timeout_ms, if_none_match,
                           post_body, post_len, post_content_type, resp);
        resp->elapsed_ms = (long)((esp_timer_get_time() - start_us) / 1000);

        if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL) {
            if (transient_left > 0) {
                transient_left--;
                ESP_LOGD(TAG, "transient %s on %s; retrying in %d ms",
                         esp_err_to_name(err), path, TRANSIENT_RETRY_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(TRANSIENT_RETRY_DELAY_MS));
                continue;
            }
            break;
        }
        if (err != ESP_OK) {
            break; // ESP_ERR_NO_MEM etc. — not retryable
        }
        if (resp->status == 503 && attempt_503 < RETRY_503_ATTEMPTS - 1) {
            attempt_503++;
            uint32_t delay_ms = RETRY_503_BASE_MS * (1u << attempt_503) + (esp_random() % 301u);
            ESP_LOGD(TAG, "board 503 (low memory) on %s; retry %d/%d in %u ms",
                     path, attempt_503, RETRY_503_ATTEMPTS - 1, (unsigned)delay_ms);
            response_discard(resp);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue;
        }
        break;
    }
    xSemaphoreGive(s_wire_mutex);

    if (err != ESP_OK) {
        return err;
    }
    if (resp->status == 401) {
        response_discard(resp);
        return FW_ERR_UNAUTHORIZED;
    }
    if (resp->status == 503) {
        response_discard(resp);
        return FW_ERR_BUSY;
    }
    if (resp->status >= 400) {
        ESP_LOGW(TAG, "HTTP %d on %s", resp->status, path);
        response_discard(resp);
        return ESP_FAIL;
    }
    return ESP_OK; // 2xx or 304
}

// GET where the caller only cares about success (action routes).
static esp_err_t fw_simple_get(const char *path, int timeout_ms, bool retry_transient)
{
    fw_response_t resp;
    esp_err_t err = fw_perform(path, timeout_ms, retry_transient, NULL, NULL, 0, NULL, &resp);
    if (err == ESP_OK) {
        response_discard(&resp);
    }
    return err;
}

// -------------------------------------------------------------- JSON guards

static void json_copy_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        strlcpy(out, it->valuestring, cap);
    }
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : fallback;
}

static bool json_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    if (cJSON_IsNumber(it)) {
        return it->valuedouble != 0;
    }
    return fallback;
}

// Playlist pause_*_secs guard: non-numeric or negative -> -1 (unknown/not
// pausing), matching the PORTING_NOTES field contract.
static int json_secs(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it) || it->valuedouble < 0) {
        return -1;
    }
    return (int)it->valuedouble;
}

// Strip the first matching SD prefix only, in this exact order.
static const char *strip_file_prefix(const char *path)
{
    static const char *const prefixes[] = { "/sd/patterns/", "/patterns/", "/sd/", "/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], len) == 0) {
            return path + len;
        }
    }
    return path;
}

// ----------------------------------------------------------------- lifecycle

esp_err_t fw_client_init(void)
{
    if (s_wire_mutex != NULL) {
        return ESP_OK; // already initialized
    }
    s_wire_mutex = xSemaphoreCreateMutex();
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_cache_mutex = xSemaphoreCreateMutex();
    if (s_wire_mutex == NULL || s_cfg_mutex == NULL || s_cache_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

void fw_set_base_url(const char *url)
{
    // Normalize outside the lock: trim whitespace, strip trailing '/',
    // prepend "http://" when no scheme (mirrors normalize_base_url()).
    char norm[sizeof(s_base_url)] = "";
    if (url != NULL) {
        const char *start = url;
        while (*start != '\0' && isspace((unsigned char)*start)) {
            start++;
        }
        size_t len = strlen(start);
        while (len > 0 && (isspace((unsigned char)start[len - 1]) || start[len - 1] == '/')) {
            len--;
        }
        if (len > 0) {
            bool has_scheme = (len >= 7 && strncmp(start, "http://", 7) == 0) ||
                              (len >= 8 && strncmp(start, "https://", 8) == 0);
            snprintf(norm, sizeof(norm), "%s%.*s", has_scheme ? "" : "http://",
                     (int)len, start);
        }
    }
    if (s_cfg_mutex == NULL) {
        ESP_LOGE(TAG, "fw_set_base_url before fw_client_init");
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    strlcpy(s_base_url, norm, sizeof(s_base_url));
    xSemaphoreGive(s_cfg_mutex);
    ESP_LOGI(TAG, "target table: %s", norm[0] != '\0' ? norm : "(none)");
}

const char *fw_base_url(void)
{
    // Wait-free read of the internal buffer. Setter and this getter are both
    // UI-task calls in practice; the request path never reads this directly
    // (it snapshots under s_cfg_mutex), so no torn read can hit the wire.
    return s_base_url;
}

void fw_set_password(const char *plaintext)
{
    if (s_cfg_mutex == NULL) {
        ESP_LOGE(TAG, "fw_set_password before fw_client_init");
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    strlcpy(s_password, plaintext != NULL ? plaintext : "", sizeof(s_password));
    xSemaphoreGive(s_cfg_mutex);
}

// -------------------------------------------------------------------- status

esp_err_t fw_get_status(fw_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->heap_largest = -1;
    out->led_brightness = -1;
    out->playlist.pause_remaining = -1;
    out->playlist.pause_total = -1;

    fw_response_t resp;
    esp_err_t err = fw_perform("/sand_status", TIMEOUT_STATUS_MS, true,
                               NULL, NULL, 0, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    out->response_ms = resp.elapsed_ms;

    cJSON *root = cJSON_Parse(resp.body);
    response_discard(&resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // "Hold:0" -> "Hold": split on ':', keep [0].
    json_copy_str(root, "state", out->state, sizeof(out->state));
    char *colon = strchr(out->state, ':');
    if (colon != NULL) {
        *colon = '\0';
    }

    json_copy_str(root, "hostname", out->hostname, sizeof(out->hostname));

    const cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (cJSON_IsString(file) && file->valuestring != NULL) {
        strlcpy(out->file, strip_file_prefix(file->valuestring), sizeof(out->file));
    }

    out->running = json_bool(root, "running", false);
    out->feed = json_int(root, "feed", 0);

    const cJSON *prog = cJSON_GetObjectItemCaseSensitive(root, "progress");
    if (cJSON_IsNumber(prog)) {
        out->progress = (float)prog->valuedouble;
        if (out->progress < 0.0f) {
            out->progress = 0.0f; // board reports negative = unknown
        }
    }

    out->heap_largest = json_int(root, "heap_largest", -1);

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led");
    if (cJSON_IsObject(led)) {
        json_copy_str(led, "effect", out->led_effect, sizeof(out->led_effect));
        out->led_brightness = json_int(led, "brightness", -1);
    }

    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "playlist");
    if (cJSON_IsObject(pl)) {
        out->playlist.active = json_bool(pl, "active", false);
        out->playlist.index = json_int(pl, "index", 0);
        out->playlist.total = json_int(pl, "total", 0);
        json_copy_str(pl, "name", out->playlist.name, sizeof(out->playlist.name));
        out->playlist.clearing = json_bool(pl, "clearing", false);
        out->playlist.pause_remaining = json_secs(pl, "pause_remaining");
        out->playlist.pause_total = json_secs(pl, "pause_total");
        json_copy_str(pl, "next", out->playlist.next, sizeof(out->playlist.next));
        json_copy_str(pl, "last", out->playlist.last, sizeof(out->playlist.last));
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// --------------------------------------------------------------------- lists

esp_err_t fw_get_patterns(fw_str_list_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->items = NULL;
    out->count = 0;
    if (s_cache_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char etag[sizeof(s_pat_etag)];
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    strlcpy(etag, s_pat_etag, sizeof(etag));
    xSemaphoreGive(s_cache_mutex);

    fw_response_t resp;
    esp_err_t err = fw_perform("/sand_patterns", TIMEOUT_DEFAULT_MS, true,
                               etag[0] != '\0' ? etag : NULL, NULL, 0, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }

    if (resp.status == 304) {
        // Unchanged: hand out a deep copy of the cached list.
        response_discard(&resp);
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        err = str_list_dup(s_pat_cache, s_pat_count, out);
        xSemaphoreGive(s_cache_mutex);
        return err;
    }

    cJSON *root = cJSON_Parse(resp.body != NULL ? resp.body : "");
    char new_etag[sizeof(s_pat_etag)];
    strlcpy(new_etag, resp.etag, sizeof(new_etag));
    response_discard(&resp);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = json_array_to_list(root, out);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }

    // Replace the cache with a copy of what we returned. If caching fails,
    // clear the stored ETag too — a later 304 must never hit an empty cache.
    fw_str_list_t cache_copy;
    esp_err_t cache_err = str_list_dup(out->items, out->count, &cache_copy);
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    str_list_release(s_pat_cache, s_pat_count);
    if (cache_err == ESP_OK) {
        s_pat_cache = cache_copy.items;
        s_pat_count = cache_copy.count;
        strlcpy(s_pat_etag, new_etag, sizeof(s_pat_etag)); // "" when no ETag sent
    } else {
        s_pat_cache = NULL;
        s_pat_count = 0;
        s_pat_etag[0] = '\0';
    }
    xSemaphoreGive(s_cache_mutex);
    return ESP_OK;
}

esp_err_t fw_get_playlists(fw_str_list_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->items = NULL;
    out->count = 0;

    fw_response_t resp;
    esp_err_t err = fw_perform("/sand_playlists", TIMEOUT_DEFAULT_MS, true,
                               NULL, NULL, 0, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(resp.body);
    response_discard(&resp);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = json_array_to_list(root, out);
    cJSON_Delete(root);
    return err;
}

void fw_str_list_free(fw_str_list_t *l)
{
    if (l == NULL) {
        return;
    }
    str_list_release(l->items, l->count);
    l->items = NULL;
    l->count = 0;
}

// ------------------------------------------------------------------ settings

static void settings_release(fw_setting_kv_t *kv, int count)
{
    if (kv == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(kv[i].key);
        free(kv[i].val);
    }
    free(kv);
}

esp_err_t fw_load_settings(void)
{
    if (s_cache_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    fw_response_t resp;
    esp_err_t err = fw_perform("/sand_settings", TIMEOUT_DEFAULT_MS, true,
                               NULL, NULL, 0, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_Parse(resp.body);
    response_discard(&resp);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int n = cJSON_GetArraySize(root); // counts an object's children too
    fw_setting_kv_t *kv = NULL;
    int count = 0;
    if (n > 0) {
        kv = heap_caps_malloc((size_t)n * sizeof(*kv), MALLOC_CAP_SPIRAM);
        if (kv == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        const cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (it->string == NULL || !cJSON_IsString(it) || it->valuestring == NULL) {
                continue; // contract: flat string -> string map
            }
            kv[count].key = dup_str_spiram(it->string);
            kv[count].val = dup_str_spiram(it->valuestring);
            if (kv[count].key == NULL || kv[count].val == NULL) {
                free(kv[count].key);
                free(kv[count].val);
                settings_release(kv, count);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM; // old cache stays valid
            }
            count++;
        }
    }
    cJSON_Delete(root);

    // Replace wholesale under the short-hold cache lock.
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    settings_release(s_settings_kv, s_settings_count);
    s_settings_kv = kv;
    s_settings_count = count;
    xSemaphoreGive(s_cache_mutex);
    ESP_LOGI(TAG, "settings cache: %d entries", count);
    return ESP_OK;
}

bool fw_setting(const char *key, char *out, size_t out_len)
{
    if (key == NULL || out == NULL || out_len == 0 || s_cache_mutex == NULL) {
        return false;
    }
    out[0] = '\0';
    bool found = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < s_settings_count; i++) {
        if (strcmp(s_settings_kv[i].key, key) == 0) {
            strlcpy(out, s_settings_kv[i].val, out_len);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_cache_mutex);
    return found;
}

bool fw_has_led_ring(void)
{
    if (s_cache_mutex == NULL) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < s_settings_count; i++) {
        if (strncmp(s_settings_kv[i].key, "LED/", 4) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_cache_mutex);
    return found;
}

// ------------------------------------------------------------------- actions

esp_err_t fw_stop(void)
{
    return fw_simple_get("/sand_stop", TIMEOUT_DEFAULT_MS, true);
}

esp_err_t fw_pause(void)
{
    return fw_simple_get("/sand_pause", TIMEOUT_DEFAULT_MS, true);
}

esp_err_t fw_resume(void)
{
    return fw_simple_get("/sand_resume", TIMEOUT_DEFAULT_MS, true);
}

esp_err_t fw_home(void)
{
    return fw_simple_get("/sand_home", TIMEOUT_MOTION_MS, true);
}

esp_err_t fw_goto_rho(float rho)
{
    char path[48];
    snprintf(path, sizeof(path), "/sand_goto?rho=%.4f", (double)rho);
    return fw_simple_get(path, TIMEOUT_MOTION_MS, true);
}

esp_err_t fw_set_feed(int mm)
{
    char path[48];
    snprintf(path, sizeof(path), "/sand_feed?mm=%d", mm);
    return fw_simple_get(path, TIMEOUT_DEFAULT_MS, true);
}

esp_err_t fw_led(const char *query)
{
    if (query == NULL || query[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // Callers pass a pre-formed urlencoded query; appended verbatim.
    char path[FW_PATH_MAX];
    snprintf(path, sizeof(path), "/sand_led?%s", query);
    return fw_simple_get(path, TIMEOUT_DEFAULT_MS, true);
}

esp_err_t fw_command(const char *dollar_cmd)
{
    if (dollar_cmd == NULL || dollar_cmd[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char enc[FW_ENC_MAX];
    url_encode(enc, sizeof(enc), dollar_cmd, false);
    char path[FW_PATH_MAX];
    snprintf(path, sizeof(path), "/command?plain=%s", enc);
    // NEVER a transient retry: a timed-out $Playlist/Run or $Bye may still
    // execute once the board's queue drains — re-sending double-fires it.
    return fw_simple_get(path, TIMEOUT_DEFAULT_MS, false);
}

esp_err_t fw_run_pattern(const char *rel_path, const char *clear_ui)
{
    if (rel_path == NULL || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    while (*rel_path == '/') {
        rel_path++;
    }
    const char *fw_clear = map_clear_ui(clear_ui);
    char cmd[224];
    if (strcmp(fw_clear, "none") == 0) {
        snprintf(cmd, sizeof(cmd), "$SD/Run=/patterns/%s", rel_path);
    } else {
        snprintf(cmd, sizeof(cmd), "$Sand/Run=/patterns/%s clear=%s", rel_path, fw_clear);
    }
    return fw_command(cmd);
}

// Poll /sand_status until state==Idle && !running (0.5 s cadence).
static esp_err_t wait_for_idle(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        fw_status_t st;
        if (fw_get_status(&st) == ESP_OK &&
            strcmp(st.state, "Idle") == 0 && !st.running) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_INTERVAL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t fw_run_playlist(const char *name, int pause_s, const char *clear_ui,
                          const char *mode, bool shuffle)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // $Playlist/* NVS writes are idle-gated: a run-while-running stops the
    // board first, then waits for Idle (PORTING_NOTES). A failed status read
    // does not block the attempt — the commands themselves will tell.
    fw_status_t st;
    if (fw_get_status(&st) == ESP_OK &&
        (st.running || (strcmp(st.state, "Idle") != 0 && strcmp(st.state, "Alarm") != 0))) {
        esp_err_t err = fw_stop();
        if (err != ESP_OK) {
            return err;
        }
        err = wait_for_idle(IDLE_WAIT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "table did not reach Idle before $Playlist/Run");
            return err; // ESP_ERR_TIMEOUT
        }
    }

    char cmd[160];
    esp_err_t err;
    if (mode != NULL && (strcmp(mode, "single") == 0 || strcmp(mode, "loop") == 0)) {
        snprintf(cmd, sizeof(cmd), "$Playlist/Mode=%s", mode);
        err = fw_command(cmd);
        if (err != ESP_OK) {
            return err;
        }
    }
    snprintf(cmd, sizeof(cmd), "$Playlist/Shuffle=%s", shuffle ? "ON" : "OFF");
    err = fw_command(cmd);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(cmd, sizeof(cmd), "$Playlist/PauseTime=%d", pause_s);
    err = fw_command(cmd);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(cmd, sizeof(cmd), "$Playlist/ClearPattern=%s", map_clear_ui(clear_ui));
    err = fw_command(cmd);
    if (err != ESP_OK) {
        return err;
    }

    char base_name[96];
    strip_txt_suffix(name, base_name, sizeof(base_name));
    snprintf(cmd, sizeof(cmd), "$Playlist/Run=%s", base_name);
    return fw_command(cmd);
}

esp_err_t fw_playlist_skip(void)
{
    return fw_command("$Playlist/Skip");
}

esp_err_t fw_sync_time(const char *posix_tz)
{
    // Only push a clock we actually have: before SNTP syncs, time(NULL) sits
    // in 1970 and would clobber the board's wall clock.
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    if (tm_utc.tm_year + 1900 <= 2020) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[192];
    if (posix_tz != NULL && posix_tz[0] != '\0') {
        char tz_enc[128];
        url_encode(tz_enc, sizeof(tz_enc), posix_tz, false);
        snprintf(path, sizeof(path), "/sand_time?epoch=%lld&tz=%s",
                 (long long)now, tz_enc);
    } else {
        snprintf(path, sizeof(path), "/sand_time?epoch=%lld", (long long)now);
    }
    return fw_simple_get(path, TIMEOUT_DEFAULT_MS, true);
}

// ------------------------------------------------------------------ file ops

esp_err_t fw_fetch_sd(const char *sd_path, char **buf, size_t *len)
{
    if (sd_path == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *buf = NULL;
    if (len != NULL) {
        *len = 0;
    }

    // Path arrives without the /sd prefix ("/patterns/foo bar.thr"); prepend
    // "/sd" and percent-encode each segment (keeping '/').
    while (*sd_path == '/') {
        sd_path++;
    }
    char enc[FW_ENC_MAX];
    url_encode(enc, sizeof(enc), sd_path, true);
    char path[FW_PATH_MAX];
    snprintf(path, sizeof(path), "/sd/%s", enc);

    fw_response_t resp;
    esp_err_t err = fw_perform(path, TIMEOUT_SD_MS, true, NULL, NULL, 0, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    *buf = resp.body; // caller frees; SPIRAM heap, NUL-terminated
    if (len != NULL) {
        *len = resp.len;
    }
    return ESP_OK;
}

esp_err_t fw_upload_playlist(const char *name, const char *content)
{
    if (name == NULL || name[0] == '\0' || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char base_name[80];
    strip_txt_suffix(name, base_name, sizeof(base_name));
    char fname[96]; // full SD path is the multipart part name (Pi convention)
    snprintf(fname, sizeof(fname), "/playlists/%s.txt", base_name);

    char boundary[40];
    snprintf(boundary, sizeof(boundary), "----dwtouch%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random());

    // Body: a "<name>S" text field carrying the decimal byte length, then the
    // file part (application/octet-stream) — the ESP3D upload contract.
    size_t content_len = strlen(content);
    size_t body_cap = content_len + 640;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int head = snprintf(body, body_cap,
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"%sS\"\r\n"
                        "\r\n"
                        "%u\r\n"
                        "--%s\r\n"
                        "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "\r\n",
                        boundary, fname, (unsigned)content_len,
                        boundary, fname, fname);
    if (head < 0 || (size_t)head + content_len >= body_cap) {
        free(body);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(body + head, content, content_len);
    size_t off = (size_t)head + content_len;
    int tail = snprintf(body + off, body_cap - off, "\r\n--%s--\r\n", boundary);
    if (tail < 0 || (size_t)tail >= body_cap - off) {
        free(body);
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = off + (size_t)tail;

    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);

    // No transient retry: a timed-out upload may still have landed.
    fw_response_t resp;
    esp_err_t err = fw_perform("/upload?path=/playlists", TIMEOUT_UPLOAD_MS, false,
                               NULL, body, total, ctype, &resp);
    free(body);
    if (err == ESP_OK) {
        response_discard(&resp);
    }
    return err;
}

esp_err_t fw_delete_playlist(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char base_name[80];
    strip_txt_suffix(name, base_name, sizeof(base_name));
    char fname[96];
    snprintf(fname, sizeof(fname), "/playlists/%s.txt", base_name);
    char enc[288];
    url_encode(enc, sizeof(enc), fname, false);
    char path[FW_PATH_MAX];
    snprintf(path, sizeof(path),
             "/upload?action=delete&filename=%s&path=/playlists&dontlist=yes", enc);
    // File ops never retry transients (the delete may already have happened).
    return fw_simple_get(path, TIMEOUT_DEFAULT_MS, false);
}

// -------------------------------------------------------------------- errors

const char *fw_friendly_error(esp_err_t err)
{
    // ASCII only: the bundled LVGL fonts carry no em dash / curly quotes.
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "The table didn't respond in time - it may be busy. Try again.";
    case FW_ERR_UNAUTHORIZED:
        return "The table rejected the password. Set it under Table connection.";
    case ESP_FAIL:
        return "Can't reach the table. Check that it's powered on and on your network.";
    case FW_ERR_BUSY:
        return "The table is busy (low memory). Try again in a moment.";
    default:
        return esp_err_to_name(err);
    }
}
