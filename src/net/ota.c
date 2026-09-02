#include "net/ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/jobs.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

// Where updates come from. The tag comes from the API (the only place that
// knows what "latest" is); the image comes off the default branch, because the
// release ASSET url redirects to release-assets.githubusercontent.com and that
// second host would cost another handshake. releases/<tag>/ is committed by
// .github/workflows/release.yml for this reason.
#define GH_LATEST_URL "https://api.github.com/repos/tuanchris/dune-weaver-touch/releases/latest"
// The image name is PER PANEL. Both boards are esp32s3 and nothing else in the
// release tells them apart, so a single firmware.bin would let an 800x480 panel
// pull a 1024x600 build and come up unreadable. firmware.bin stays the 5B's
// name so panels already in the field keep updating. A release with no matching
// image 404s, which fails the update cleanly rather than flashing the wrong one.
#if defined(BOARD_CROWPANEL_ADV_5)
// The CrowPanel is 800x480 too, but it is a DIFFERENT BOARD -- its own pin
// map, expander and console. Selecting by panel resolution would have it pull
// the Waveshare 7 build and flash a foreign pin map onto itself. The image is
// per BOARD, never per panel. A release without this file 404s, which fails
// the update cleanly.
#define GH_IMAGE_FMT "https://raw.githubusercontent.com/tuanchris/dune-weaver-touch/main/releases/%s/firmware-crowpanel-adv-5.bin"
#elif defined(BOARD_PANEL_800X480)
#define GH_IMAGE_FMT "https://raw.githubusercontent.com/tuanchris/dune-weaver-touch/main/releases/%s/firmware-800x480.bin"
#else
#define GH_IMAGE_FMT "https://raw.githubusercontent.com/tuanchris/dune-weaver-touch/main/releases/%s/firmware.bin"
#endif
// GitHub rejects API requests without one.
#define GH_USER_AGENT "dune-weaver-touch"

static const char *TAG = "ota";

// Mirrors dune-weaver-firmware's UploadStatus, because the "code" member of
// every response is the raw enum and its apps already read those numbers.
typedef enum {
    UP_NONE = 0,
    UP_FAILED = 1,
    UP_CANCELLED = 2,
    UP_SUCCESSFUL = 3,
    UP_ONGOING = 4,
} upload_status_t;

// 8 KB is a deliberate middle: the parser must retain delimiter-length bytes
// between reads, and esp_ota_write to a freshly erased sector is happiest with
// a few KB at a time. Heap, not stack -- the httpd task stack is 8 KB total.
#define OTA_BUF_SZ 8192
#define REBOOT_DELAY_US (1000 * 1000)  // firmware answers, then resets ~1 s later

static httpd_handle_t s_server;
static upload_status_t s_status = UP_NONE;
static volatile int s_pct = -1;
static esp_timer_handle_t s_reboot_timer;

// ---------------------------------------------------------------- responses

const char *ota_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return (d != NULL && d->version[0] != '\0') ? d->version : "unknown";
}

int ota_progress_pct(void)
{
    return s_pct;
}

// The panel has no motion to protect, so unlike the table it is only ever busy
// with an update already in flight. Kept in the contract because the clients
// probe for it.
static bool update_blocked(void)
{
    return s_status == UP_ONGOING;
}

static esp_err_t send_json(httpd_req_t *req, int http_code, const char *status)
{
    char body[192];
    snprintf(body, sizeof(body),
             "{\"status\":\"%s\",\"code\":\"%d\",\"fw\":\"%s\",\"mcu\":\"%s\"}",
             status, (int)s_status, ota_version(), CONFIG_IDF_TARGET);

    const char *line = "200 OK";
    if (http_code == 409) {
        line = "409 Conflict";
    } else if (http_code == 500) {
        line = "500 Internal Server Error";
    } else if (http_code == 400) {
        line = "400 Bad Request";
    }
    httpd_resp_set_status(req, line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "rebooting into the new image");
    esp_restart();
}

// Answer first, restart after. Rebooting inside the handler would drop the
// response and the client would read the update as failed.
static void schedule_reboot(void)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "ota_reboot"};
        if (esp_timer_create(&args, &s_reboot_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_start_once(s_reboot_timer, REBOOT_DELAY_US);
}

// ------------------------------------------------------- multipart streaming

typedef struct {
    char delim[80];  // "\r\n--<boundary>"
    size_t dlen;

    uint8_t *buf;
    size_t cap;
    size_t len;

    bool part_is_file;
    bool name_is_size;  // field "<name>S" carries the declared image size

    size_t declared;    // bytes the client says the image is, 0 = unstated
    char small[24];     // accumulates a non-file field value (the size)
    size_t small_len;

    const esp_partition_t *part;
    esp_ota_handle_t ota;
    bool ota_open;
    size_t written;
    bool magic_checked;
    bool failed;
} mp_t;

static bool mem_find(const uint8_t *hay, size_t hlen, const void *needle, size_t nlen, size_t *at)
{
    if (nlen == 0 || hlen < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

static void mp_consume(mp_t *m, size_t n)
{
    if (n >= m->len) {
        m->len = 0;
        return;
    }
    memmove(m->buf, m->buf + n, m->len - n);
    m->len -= n;
}

static bool ota_open_now(mp_t *m)
{
    m->part = esp_ota_get_next_update_partition(NULL);
    if (m->part == NULL) {
        ESP_LOGE(TAG, "no OTA slot to write (is the partition table factory-only?)");
        return false;
    }
    // The firmware rejects an image that cannot fit before erasing anything;
    // do the same, so a wrong-sized upload costs no flash wear.
    if (m->declared != 0 && m->declared > m->part->size) {
        ESP_LOGE(TAG, "image %u B does not fit slot %s (%u B)",
                 (unsigned)m->declared, m->part->label, (unsigned)m->part->size);
        return false;
    }
    size_t sz = (m->declared != 0) ? m->declared : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(m->part, sz, &m->ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return false;
    }
    m->ota_open = true;
    ESP_LOGI(TAG, "writing to %s (%u B slot), declared %u B",
             m->part->label, (unsigned)m->part->size, (unsigned)m->declared);
    return true;
}

// Feeds one run of part content. Returns false once the transfer is doomed.
static bool mp_content(mp_t *m, const uint8_t *data, size_t len)
{
    if (len == 0 || m->failed) {
        return !m->failed;
    }
    if (!m->part_is_file) {
        if (m->name_is_size && m->small_len + len < sizeof(m->small)) {
            memcpy(m->small + m->small_len, data, len);
            m->small_len += len;
            m->small[m->small_len] = '\0';
            m->declared = (size_t)strtoul(m->small, NULL, 10);
        }
        return true;
    }
    if (!m->magic_checked) {
        // 0xE9 is the app-image magic. It does NOT distinguish ESP32 from S3 --
        // that only surfaces at esp_ota_end -- but it does reject a file that
        // was never a firmware image, before erasing a 4 MB slot for it.
        if (data[0] != 0xE9) {
            ESP_LOGE(TAG, "not a firmware image (first byte 0x%02X, expected 0xE9)", data[0]);
            m->failed = true;
            return false;
        }
        m->magic_checked = true;
    }
    if (!m->ota_open && !ota_open_now(m)) {
        m->failed = true;
        return false;
    }
    esp_err_t err = esp_ota_write(m->ota, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        m->failed = true;
        return false;
    }
    m->written += len;
    if (m->declared > 0) {
        int pct = (int)((uint64_t)m->written * 100 / m->declared);
        if (pct != s_pct) {
            s_pct = pct;
            if (pct % 10 == 0) {
                ESP_LOGI(TAG, "update %d%%", pct);
            }
        }
    }
    return true;
}

// Content-Disposition tells us which field this part is: a filename means the
// image, a name ending in 'S' means the declared size.
static void mp_parse_headers(mp_t *m, const char *hdr, size_t len)
{
    m->part_is_file = false;
    m->name_is_size = false;
    m->small_len = 0;
    m->small[0] = '\0';

    char buf[256];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, hdr, n);
    buf[n] = '\0';

    if (strcasestr(buf, "filename=") != NULL) {
        m->part_is_file = true;
        return;
    }
    const char *p = strcasestr(buf, "name=\"");
    if (p != NULL) {
        p += 6;
        const char *end = strchr(p, '"');
        if (end != NULL && end > p && end[-1] == 'S') {
            m->name_is_size = true;
        }
    }
}

// Parse states. The stream is seeded with a leading CRLF so the very first
// boundary (which has none) matches the same "\r\n--<boundary>" delimiter as
// every later one -- otherwise the first and subsequent cases need separate
// code paths and the first one is the easy one to get subtly wrong.
typedef enum { ST_SEEK, ST_HEADERS, ST_BODY, ST_DONE } mp_state_t;

static esp_err_t updatefw_post(httpd_req_t *req)
{
    char ctype[192];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK ||
        strcasestr(ctype, "multipart/form-data") == NULL) {
        s_status = UP_FAILED;
        ESP_LOGE(TAG, "not multipart/form-data");
        return send_json(req, 400, "failed");
    }
    const char *b = strcasestr(ctype, "boundary=");
    if (b == NULL) {
        s_status = UP_FAILED;
        ESP_LOGE(TAG, "no multipart boundary");
        return send_json(req, 400, "failed");
    }
    b += 9;
    if (*b == '"') {
        b++;
    }

    mp_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        s_status = UP_FAILED;
        return send_json(req, 500, "failed");
    }
    int dn = snprintf(m->delim, sizeof(m->delim), "\r\n--%s", b);
    if (dn <= 4 || dn >= (int)sizeof(m->delim)) {
        free(m);
        s_status = UP_FAILED;
        ESP_LOGE(TAG, "boundary too long");
        return send_json(req, 400, "failed");
    }
    // Trim anything the header tacked on after the boundary token.
    for (char *p = m->delim + 4; *p != '\0'; p++) {
        if (*p == '"' || *p == ';' || *p == ' ' || *p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
    m->dlen = strlen(m->delim);

    m->cap = OTA_BUF_SZ;
    m->buf = malloc(m->cap);
    if (m->buf == NULL) {
        free(m);
        s_status = UP_FAILED;
        return send_json(req, 500, "failed");
    }

    s_status = UP_ONGOING;
    s_pct = 0;

    memcpy(m->buf, "\r\n", 2);  // see ST_SEEK note above
    m->len = 2;

    mp_state_t st = ST_SEEK;
    int remaining = req->content_len;
    bool recv_err = false;

    while (st != ST_DONE && !m->failed) {
        // Top up the buffer, unless the tail alone can still make progress.
        if (remaining > 0 && m->len < m->cap) {
            size_t want = m->cap - m->len;
            if ((int)want > remaining) {
                want = (size_t)remaining;
            }
            int got = httpd_req_recv(req, (char *)m->buf + m->len, want);
            if (got <= 0) {
                if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                ESP_LOGE(TAG, "recv failed (%d) with %d B outstanding", got, remaining);
                recv_err = true;
                break;
            }
            m->len += (size_t)got;
            remaining -= got;
        }

        bool progressed = false;
        while (!m->failed) {
            if (st == ST_SEEK) {
                size_t at;
                if (mem_find(m->buf, m->len, m->delim, m->dlen, &at)) {
                    mp_consume(m, at + m->dlen);
                    if (m->len < 2) {
                        break;  // need the two bytes that say "next part" or "end"
                    }
                    if (m->buf[0] == '-' && m->buf[1] == '-') {
                        st = ST_DONE;
                    } else {
                        mp_consume(m, 2);  // CRLF
                        st = ST_HEADERS;
                    }
                    progressed = true;
                    continue;
                }
                // Keep only what could still be a partial delimiter.
                if (m->len > m->dlen - 1) {
                    mp_consume(m, m->len - (m->dlen - 1));
                }
                break;
            }

            if (st == ST_HEADERS) {
                size_t at;
                if (mem_find(m->buf, m->len, "\r\n\r\n", 4, &at)) {
                    mp_parse_headers(m, (const char *)m->buf, at);
                    mp_consume(m, at + 4);
                    st = ST_BODY;
                    progressed = true;
                    continue;
                }
                if (m->len == m->cap) {
                    ESP_LOGE(TAG, "part headers exceed %u B", (unsigned)m->cap);
                    m->failed = true;
                }
                break;
            }

            // ST_BODY
            size_t at;
            if (mem_find(m->buf, m->len, m->delim, m->dlen, &at)) {
                if (!mp_content(m, m->buf, at)) {
                    break;
                }
                mp_consume(m, at + m->dlen);
                if (m->len < 2) {
                    st = ST_SEEK;  // re-find it once more data lands
                    break;
                }
                if (m->buf[0] == '-' && m->buf[1] == '-') {
                    st = ST_DONE;
                } else {
                    mp_consume(m, 2);
                    st = ST_HEADERS;
                }
                progressed = true;
                continue;
            }
            // No delimiter in view: everything except a possible partial one is
            // content and can be written now.
            if (m->len > m->dlen - 1) {
                size_t safe = m->len - (m->dlen - 1);
                if (!mp_content(m, m->buf, safe)) {
                    break;
                }
                mp_consume(m, safe);
                progressed = true;
            }
            break;
        }

        if (remaining <= 0 && !progressed && st != ST_DONE) {
            // Body exhausted without a closing boundary.
            if (m->written > 0 && !m->failed) {
                ESP_LOGW(TAG, "stream ended without a closing boundary");
            }
            break;
        }
    }

    bool ok = false;
    if (!m->failed && !recv_err && m->ota_open) {
        esp_err_t err = esp_ota_end(m->ota);
        m->ota_open = false;
        if (err != ESP_OK) {
            // The wrong-MCU image lands here: identical magic byte, so the chip
            // ID is only checked once the whole thing has streamed.
            ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        } else if ((err = esp_ota_set_boot_partition(m->part)) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        } else {
            ok = true;
            ESP_LOGI(TAG, "update 100%% - %u B into %s", (unsigned)m->written, m->part->label);
        }
    } else if (m->ota_open) {
        esp_ota_abort(m->ota);
        m->ota_open = false;
    }

    if (ok && m->written == 0) {
        ok = false;  // a POST with no file part is not an update
    }

    free(m->buf);
    free(m);

    s_pct = -1;
    s_status = ok ? UP_SUCCESSFUL : UP_FAILED;
    esp_err_t r = send_json(req, ok ? 200 : 500, ok ? "ok" : "failed");
    if (ok) {
        schedule_reboot();
    } else {
        s_status = UP_NONE;  // stay probeable for a retry
    }
    return r;
}

static esp_err_t updatefw_get(httpd_req_t *req)
{
    bool busy = update_blocked();
    return send_json(req, busy ? 409 : 200, busy ? "busy" : "ready");
}

// ------------------------------------------------------------------- public

esp_err_t ota_init(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // The default 4 KB is not enough once the multipart parser and esp_ota sit
    // on it together.
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    // A 1.6 MB upload over WiFi is long; the default 5 s recv timeout aborts it
    // on any stall.
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t post = {
        .uri = "/updatefw", .method = HTTP_POST, .handler = updatefw_post};
    const httpd_uri_t get = {
        .uri = "/updatefw", .method = HTTP_GET, .handler = updatefw_get};
    httpd_register_uri_handler(s_server, &post);
    httpd_register_uri_handler(s_server, &get);

    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "update server up on :%d (fw=%s mcu=%s, running from %s)",
             cfg.server_port, ota_version(), CONFIG_IDF_TARGET,
             run != NULL ? run->label : "?");
    return ESP_OK;
}

// ------------------------------------------------------ pulling an update

static ota_state_t s_pull_state = OTA_IDLE;
static char s_latest[32];
static char s_err[96];
static ota_state_cb_t s_state_cb;
static volatile bool s_pull_busy;

ota_state_t ota_state(void)
{
    return s_pull_state;
}

const char *ota_latest_version(void)
{
    return s_latest;
}

const char *ota_error(void)
{
    return s_err;
}

void ota_set_state_cb(ota_state_cb_t cb)
{
    s_state_cb = cb;
}

static void set_state(ota_state_t st, const char *err)
{
    s_pull_state = st;
    if (err != NULL) {
        strlcpy(s_err, err, sizeof(s_err));
    } else if (st != OTA_ERROR) {
        s_err[0] = '\0';
    }
    if (s_state_cb != NULL) {
        s_state_cb();
    }
}

// "v0.1.2" vs "0.1.1" -> compares 0.1.2 to 0.1.1. Numeric per component, so
// 0.1.10 sorts above 0.1.9 (a strcmp would get that backwards).
static int version_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') {
        a++;
    }
    if (*b == 'v' || *b == 'V') {
        b++;
    }
    for (;;) {
        long na = strtol(a, (char **)&a, 10);
        long nb = strtol(b, (char **)&b, 10);
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
        // Stop at the first non-version suffix ("-rc1"): a release candidate is
        // not newer than the release it precedes, and treating it as such would
        // offer testers a downgrade.
        if (*a != '.' || *b != '.') {
            return 0;
        }
        a++;
        b++;
    }
}

// Reads the whole response into `buf`. The release JSON is a few KB; anything
// past the cap is dropped, which is fine because tag_name is near the front.
static esp_err_t http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "User-Agent", GH_USER_AGENT);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    int n = esp_http_client_read(c, buf, (int)cap - 1);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) {
        ESP_LOGE(TAG, "%s -> HTTP %d", url, status);
        return ESP_FAIL;
    }
    if (n <= 0) {
        return ESP_FAIL;
    }
    buf[n] = '\0';
    return ESP_OK;
}

static void check_job(void *arg)
{
    (void)arg;
    set_state(OTA_CHECKING, NULL);

    char *buf = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(6144);
    }
    if (buf == NULL) {
        set_state(OTA_ERROR, "Out of memory");
        s_pull_busy = false;
        return;
    }
    esp_err_t err = http_get(GH_LATEST_URL, buf, 6144);
    if (err != ESP_OK) {
        free(buf);
        set_state(OTA_ERROR, "Could not reach GitHub");
        s_pull_busy = false;
        return;
    }
    // Deliberately not cJSON: the release payload is ~5 KB of mostly asset
    // metadata and parsing it into a tree costs far more than the one field.
    const char *p = strstr(buf, "\"tag_name\"");
    if (p != NULL) {
        p = strchr(p + 10, '"');
    }
    if (p == NULL) {
        free(buf);
        set_state(OTA_ERROR, "No release found");
        s_pull_busy = false;
        return;
    }
    p++;
    size_t i = 0;
    while (p[i] != '"' && p[i] != '\0' && i < sizeof(s_latest) - 1) {
        s_latest[i] = p[i];
        i++;
    }
    s_latest[i] = '\0';
    free(buf);

    bool newer = version_cmp(ota_version(), s_latest) < 0;
    ESP_LOGI(TAG, "latest release %s, running %s -> %s", s_latest, ota_version(),
             newer ? "update available" : "up to date");
    set_state(newer ? OTA_AVAILABLE : OTA_UP_TO_DATE, NULL);
    s_pull_busy = false;
}

static void update_job(void *arg)
{
    (void)arg;
    char url[192];
    snprintf(url, sizeof(url), GH_IMAGE_FMT, s_latest);
    ESP_LOGI(TAG, "pulling %s", url);

    s_pct = 0;
    set_state(OTA_DOWNLOADING, NULL);

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || h == NULL) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        s_pct = -1;
        set_state(OTA_ERROR, "Could not start download");
        s_pull_busy = false;
        return;
    }
    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) {
            int pct = esp_https_ota_get_image_len_read(h) * 100 / total;
            if (pct != s_pct) {
                s_pct = pct;
                if (pct % 5 == 0) {
                    ESP_LOGI(TAG, "download %d%%", pct);
                }
                if (s_state_cb != NULL) {
                    s_state_cb();
                }
            }
        }
    }
    bool complete = esp_https_ota_is_complete_data_received(h);
    if (err != ESP_OK || !complete) {
        ESP_LOGE(TAG, "download failed: %s (complete=%d)", esp_err_to_name(err), (int)complete);
        esp_https_ota_abort(h);
        s_pct = -1;
        set_state(OTA_ERROR, complete ? "Download failed" : "Connection dropped");
        s_pull_busy = false;
        return;
    }
    err = esp_https_ota_finish(h);
    s_pct = -1;
    if (err != ESP_OK) {
        // ESP_ERR_OTA_VALIDATE_FAILED lands here: the image streamed fine but
        // is not a valid app for this chip.
        ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
        set_state(OTA_ERROR, "Downloaded image was rejected");
        s_pull_busy = false;
        return;
    }
    ESP_LOGI(TAG, "installed %s, rebooting", s_latest);
    set_state(OTA_INSTALLED, NULL);
    s_pull_busy = false;
    schedule_reboot();
}

void ota_check_async(void)
{
    if (s_pull_busy || s_pull_state == OTA_DOWNLOADING) {
        return;
    }
    s_pull_busy = true;
    if (jobs_submit(check_job, NULL) != ESP_OK) {
        s_pull_busy = false;
        set_state(OTA_ERROR, "Busy, try again");
    }
}

void ota_update_async(void)
{
    if (s_pull_busy || s_pull_state != OTA_AVAILABLE || s_latest[0] == '\0') {
        return;
    }
    s_pull_busy = true;
    if (jobs_submit(update_job, NULL) != ESP_OK) {
        s_pull_busy = false;
        set_state(OTA_ERROR, "Busy, try again");
    }
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (run == NULL || esp_ota_get_state_partition(run, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;  // not the first boot after an update
    }
    // Reaching here means init completed, so this image is good enough to keep.
    // Without this the bootloader reverts to the previous slot on the next
    // reset, which is exactly what should happen to an image that panics first.
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "first boot after update: %s",
             err == ESP_OK ? "image marked valid" : esp_err_to_name(err));
}
