// .thr -> circular RGB565 preview tiles (PORTING_NOTES.md §6).
// Fetches pattern files from the board (slow: 30-60 KB/s), renders them with a
// tiny software rasterizer (scanline discs + Bresenham polyline with a 2x2
// brush), and caches the raw pixel buffers on the LittleFS "storage"
// partition. A small PSRAM LRU keeps the most recent descriptors alive for
// lv_image_set_src. Everything here BLOCKS: jobs task only.
#include "render/thr_preview.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "net/fw_client.h"

static const char *TAG = "thr_preview";

#define PV_PARTITION "storage"
#define PV_MOUNT "/storage"
#define PV_DIR PV_MOUNT "/pv"
#define PV_TMP_PATH PV_DIR "/wr.tmp"
#define PV_RAM_SLOTS 24
#define PV_MIN_SIZE 16
#define PV_MAX_SIZE 640
// LittleFS wants headroom; below this free space we stop adding cache files.
#define PV_FS_SLACK (64 * 1024)
// Decimation: skip points closer than 0.75 px to the last drawn point.
#define PV_MIN_DIST_SQ (0.75f * 0.75f)

// PORTING_NOTES §6 look: background, dish fill, dish ring, sand stroke.
#define PV_RGB(hex) \
    ((uint16_t)((((hex) >> 19 & 0x1f) << 11) | (((hex) >> 10 & 0x3f) << 5) | ((hex) >> 3 & 0x1f)))
#define PV_COLOR_BG PV_RGB(0x171310)
#define PV_COLOR_DISH PV_RGB(0x1b1712)
#define PV_COLOR_RING PV_RGB(0x3e362c)
#define PV_COLOR_SAND PV_RGB(0xd8b578)

typedef struct {
    char *rel;              // heap copy of the pattern rel path; NULL = free slot
    int size_px;
    uint32_t stamp;         // LRU clock at last use
    int refs;               // live thr_preview_get handouts; pinned while > 0
    bool zombie;            // clear_ram hit a pinned slot: free fully on last release
    uint8_t *pixels;        // PSRAM, size_px*size_px*2
    lv_image_dsc_t dsc;
} pv_slot_t;

// Overflow handouts when every slot is pinned: uncached, freed on release.
typedef struct pv_orphan {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
    struct pv_orphan *next;
} pv_orphan_t;

static pv_slot_t s_slots[PV_RAM_SLOTS];
static pv_orphan_t *s_orphans;
static uint32_t s_stamp;
static SemaphoreHandle_t s_lock;
static bool s_mounted;

// ---------------------------------------------------------------- utilities

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    while (*s) {
        h = h * 33u + (uint8_t)*s++;
    }
    return h;
}

static char *pv_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

// Board sends "a.thr" / "sub/x.thr"; tolerate a stray leading slash.
static const char *canonical_rel(const char *rel_path)
{
    while (*rel_path == '/') {
        rel_path++;
    }
    return rel_path;
}

static void cache_file_path(char *out, size_t out_len, const char *rel, int size_px)
{
    snprintf(out, out_len, PV_DIR "/%08lx_%d.raw", (unsigned long)djb2(rel), size_px);
}

// ------------------------------------------------------- local SD previews

// SD cards ship 300 px tiles (tools/make_pattern_sd.py); other sizes fall
// through to the render pipeline.
#define PV_SD_SIZE_PX 300

// Preview key, dune-weaver-mobile semantics: basename only (cross-table —
// the same pattern filed under another folder still finds its image),
// lowercased (export casing varies). "sub/Star.thr" -> "star.thr".
static void sd_preview_path(char *out, size_t out_len, const char *rel)
{
    const char *base = strrchr(rel, '/');
    base = (base != NULL) ? base + 1 : rel;
    int n = snprintf(out, out_len, "/sdcard/previews/%s.bin", base);
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return;
    }
    for (char *p = out; *p != '\0'; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p + ('a' - 'A'));
        }
    }
}

// Pre-rendered tile from the panel's TF card: exact-size raw RGB565 read,
// no render, no network. Wrong-size/missing files just miss.
static bool sd_preview_load(const char *rel, uint8_t *pixels, int size_px,
                            size_t data_size)
{
    if (size_px != PV_SD_SIZE_PX) {
        return false;
    }
    char path[192];
    sd_preview_path(path, sizeof(path), rel);
    if (path[0] == '\0') {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != data_size) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t rd = fread(pixels, 1, data_size, f);
    fclose(f);
    return rd == data_size;
}

// ---------------------------------------------------------------- RAM LRU

// Call with s_lock held.
static pv_slot_t *lru_find_locked(const char *rel, int size_px)
{
    for (int i = 0; i < PV_RAM_SLOTS; i++) {
        pv_slot_t *slot = &s_slots[i];
        if (slot->rel != NULL && !slot->zombie && slot->size_px == size_px &&
            strcmp(slot->rel, rel) == 0) {
            slot->stamp = ++s_stamp;
            return slot;
        }
    }
    return NULL;
}

static void slot_free_locked(pv_slot_t *slot)
{
    free(slot->rel);
    free(slot->pixels);
    memset(slot, 0, sizeof(*slot));
}

// Takes ownership of pixels on success; frees it on failure. Lock held.
// Only slots with refs == 0 are eviction candidates: a pinned slot's dsc is
// referenced by a live widget, and freeing it under the widget is the
// use-after-free this refcounting exists to prevent. Returns NULL when every
// slot is pinned (caller falls back to an orphan handout).
static pv_slot_t *lru_insert_locked(const char *rel, int size_px, uint8_t *pixels)
{
    pv_slot_t *victim = NULL;
    for (int i = 0; i < PV_RAM_SLOTS; i++) {
        pv_slot_t *slot = &s_slots[i];
        if (slot->rel == NULL) {
            victim = slot;
            break;
        }
        if (slot->refs > 0) {
            continue;
        }
        if (victim == NULL || slot->stamp < victim->stamp) {
            victim = slot;
        }
    }
    if (victim == NULL) {
        return NULL;  // everything pinned
    }
    if (victim->rel != NULL) {
        slot_free_locked(victim);
    }
    victim->rel = pv_strdup(rel);
    if (victim->rel == NULL) {
        free(pixels);
        return NULL;
    }
    victim->size_px = size_px;
    victim->stamp = ++s_stamp;
    victim->refs = 0;
    victim->zombie = false;
    victim->pixels = pixels;
    memset(&victim->dsc, 0, sizeof(victim->dsc));
    victim->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    victim->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    victim->dsc.header.w = (uint32_t)size_px;
    victim->dsc.header.h = (uint32_t)size_px;
    victim->dsc.header.stride = (uint32_t)size_px * 2;
    victim->dsc.data_size = (uint32_t)size_px * (uint32_t)size_px * 2;
    victim->dsc.data = victim->pixels;
    return victim;
}

// ---------------------------------------------------------------- FS cache

static bool fs_cache_load(const char *path, uint8_t *pixels, size_t data_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if ((size_t)st.st_size != data_size) {
        ESP_LOGW(TAG, "cache %s has stale size %ld, dropping", path, (long)st.st_size);
        unlink(path);
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t rd = fread(pixels, 1, data_size, f);
    fclose(f);
    if (rd != data_size) {
        ESP_LOGW(TAG, "cache %s short read (%u/%u), dropping", path, (unsigned)rd, (unsigned)data_size);
        unlink(path);
        return false;
    }
    return true;
}

static void fs_cache_store(const char *path, const uint8_t *pixels, size_t data_size)
{
    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info(PV_PARTITION, &total, &used) == ESP_OK &&
        total - used < data_size + PV_FS_SLACK) {
        ESP_LOGW(TAG, "storage nearly full (%u B free), not caching %s",
                 (unsigned)(total - used), path);
        return;
    }
    FILE *f = fopen(PV_TMP_PATH, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cache tmp open failed: errno=%d", errno);
        return;
    }
    size_t wr = fwrite(pixels, 1, data_size, f);
    fclose(f);
    if (wr != data_size) {
        ESP_LOGW(TAG, "cache write failed (%u/%u): errno=%d", (unsigned)wr, (unsigned)data_size, errno);
        unlink(PV_TMP_PATH);
        return;
    }
    if (rename(PV_TMP_PATH, path) != 0) {
        ESP_LOGW(TAG, "cache rename to %s failed: errno=%d", path, errno);
        unlink(PV_TMP_PATH);
    }
}

// ---------------------------------------------------------------- rasterizer

static inline void plot_brush(uint16_t *px, int size, int x, int y, uint16_t c)
{
    // 2x2 block = the ~2 px stroke width from the reference renderer.
    for (int dy = 0; dy < 2; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= size) {
            continue;
        }
        uint16_t *row = px + (size_t)yy * size;
        for (int dx = 0; dx < 2; dx++) {
            int xx = x + dx;
            if (xx >= 0 && xx < size) {
                row[xx] = c;
            }
        }
    }
}

static void draw_thick_line(uint16_t *px, int size, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot_brush(px, size, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fill_disc(uint16_t *px, int size, float cx, float cy, float radius, uint16_t c)
{
    for (int iy = 0; iy < size; iy++) {
        float dy = (float)iy + 0.5f - cy;
        float d2 = radius * radius - dy * dy;
        if (d2 <= 0.0f) {
            continue;
        }
        float dx = sqrtf(d2);
        int x0 = (int)lrintf(cx - dx);
        int x1 = (int)lrintf(cx + dx);
        if (x0 < 0) {
            x0 = 0;
        }
        if (x1 > size) {
            x1 = size;
        }
        uint16_t *row = px + (size_t)iy * size;
        for (int x = x0; x < x1; x++) {
            row[x] = c;
        }
    }
}

// Streams "theta rho" pairs out of the NUL-terminated text and draws the
// polyline (x = cx + rho*R*cos(theta), y = cy + rho*R*sin(theta)) directly
// into the pixel buffer. Returns the number of points used.
static int draw_pattern(uint16_t *px, int size, float cx, float cy, float radius, const char *text)
{
    int points = 0;
    bool have_prev = false;
    float prev_x = 0.0f;
    float prev_y = 0.0f;
    int prev_ix = 0;
    int prev_iy = 0;
    const char *p = text;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '\n') {
            p++;
            continue;
        }
        if (*p == '#') {
            while (*p != '\0' && *p != '\n') {
                p++;
            }
            continue;
        }
        char *end = NULL;
        float theta = strtof(p, &end);
        if (end == p) {  // junk line: skip it whole
            while (*p != '\0' && *p != '\n') {
                p++;
            }
            continue;
        }
        p = end;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r') {
            p++;
        }
        // Guard before strtof: it would skip a newline and pair theta with the
        // NEXT line's first number, misaligning the whole stream.
        if (*p == '\n' || *p == '\0' || *p == '#') {
            continue;  // line with a single number: skip it
        }
        float rho = strtof(p, &end);
        if (end == p) {  // second column isn't a number: skip the line
            while (*p != '\0' && *p != '\n') {
                p++;
            }
            continue;
        }
        p = end;
        while (*p != '\0' && *p != '\n') {  // ignore trailing columns
            p++;
        }

        if (!isfinite(theta) || !isfinite(rho)) {  // "nan"/"inf" parse as floats
            continue;
        }
        if (rho < 0.0f) {
            rho = 0.0f;
        } else if (rho > 1.0f) {
            rho = 1.0f;
        }
        float x = cx + rho * radius * cosf(theta);
        float y = cy + rho * radius * sinf(theta);
        points++;

        if (have_prev) {
            float ddx = x - prev_x;
            float ddy = y - prev_y;
            if (ddx * ddx + ddy * ddy < PV_MIN_DIST_SQ) {
                continue;  // decimate; prev stays the last DRAWN point
            }
        }
        int ix = (int)lrintf(x);
        int iy = (int)lrintf(y);
        if (have_prev) {
            draw_thick_line(px, size, prev_ix, prev_iy, ix, iy, PV_COLOR_SAND);
        } else {
            plot_brush(px, size, ix, iy, PV_COLOR_SAND);
        }
        prev_x = x;
        prev_y = y;
        prev_ix = ix;
        prev_iy = iy;
        have_prev = true;
    }
    return points;
}

static int render_tile(uint16_t *px, int size, const char *text)
{
    int margin = size * 12 / 512;  // dish margin, scaled from the 512 px reference
    float cx = (float)size * 0.5f;
    float cy = cx;
    float radius = cx - (float)margin;

    size_t n = (size_t)size * size;
    for (size_t i = 0; i < n; i++) {
        px[i] = PV_COLOR_BG;
    }
    fill_disc(px, size, cx, cy, radius, PV_COLOR_RING);
    fill_disc(px, size, cx, cy, radius - 2.0f, PV_COLOR_DISH);
    return draw_pattern(px, size, cx, cy, radius, text);
}

// Fetch the .thr from the board and render it into pixels. A fetch failure is
// TRANSIENT (returned as-is, nothing cached); an empty/unparseable file is
// definitively unrenderable and renders as the bare dish (which the caller
// then caches — the "cache the failure" rule).
static esp_err_t fetch_and_render(const char *rel, uint16_t *px, int size_px)
{
    // fw_fetch_sd prepends "/sd" itself — pass the bare board path.
    char sd_path[192];
    int n = snprintf(sd_path, sizeof(sd_path), "/patterns/%s", rel);
    if (n < 0 || n >= (int)sizeof(sd_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *buf = NULL;
    size_t len = 0;
    esp_err_t err = fw_fetch_sd(sd_path, &buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fetch %s failed: 0x%x", sd_path, (unsigned)err);
        return err;  // transient: do NOT cache
    }

    // strtof needs NUL termination; fw_fetch_sd's buffer has none guaranteed.
    char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (text == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(text, buf, len);
    text[len] = '\0';
    free(buf);

    int points = render_tile(px, size_px, text);
    free(text);
    if (points == 0) {
        ESP_LOGW(TAG, "%s has no drawable points — caching empty dish", rel);
    } else {
        ESP_LOGI(TAG, "rendered %s: %d points at %d px", rel, points, size_px);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- public API

esp_err_t thr_preview_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = PV_MOUNT,
        .partition_label = PV_PARTITION,
        .format_if_mount_failed = true,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf), TAG, "littlefs mount");
    s_mounted = true;

    if (mkdir(PV_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", PV_DIR, errno);
    }
    unlink(PV_TMP_PATH);  // clear any half-written cache file from a reset

    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info(PV_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "storage mounted at %s: %u/%u KB used", PV_MOUNT,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    return ESP_OK;
}

esp_err_t thr_preview_get(const char *rel_path, int size_px, const lv_image_dsc_t **out)
{
    if (rel_path == NULL || out == NULL || size_px < PV_MIN_SIZE || size_px > PV_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = NULL;
    const char *rel = canonical_rel(rel_path);
    if (*rel == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // 1) RAM LRU
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pv_slot_t *slot = lru_find_locked(rel, size_px);
    if (slot != NULL) {
        slot->refs++;
        *out = &slot->dsc;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);

    size_t data_size = (size_t)size_px * (size_t)size_px * 2;
    uint8_t *pixels = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (pixels == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // 2) local SD card tile, else 3) FS cache, else 4) fetch + render (+ store).
    // SD hits skip the LittleFS store — the card IS the durable copy.
    char path[64];
    cache_file_path(path, sizeof(path), rel, size_px);
    if (!sd_preview_load(rel, pixels, size_px, data_size) &&
        !fs_cache_load(path, pixels, data_size)) {
        esp_err_t err = fetch_and_render(rel, (uint16_t *)pixels, size_px);
        if (err != ESP_OK) {
            free(pixels);
            return err;
        }
        fs_cache_store(path, pixels, data_size);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot = lru_insert_locked(rel, size_px, pixels);
    if (slot != NULL) {
        slot->refs = 1;
        *out = &slot->dsc;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    // Every slot pinned: hand out an uncached orphan (freed on release).
    pv_orphan_t *orph = heap_caps_malloc(sizeof(*orph), MALLOC_CAP_SPIRAM);
    if (orph == NULL) {
        xSemaphoreGive(s_lock);
        free(pixels);
        return ESP_ERR_NO_MEM;
    }
    memset(orph, 0, sizeof(*orph));
    orph->pixels = pixels;
    orph->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    orph->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    orph->dsc.header.w = (uint32_t)size_px;
    orph->dsc.header.h = (uint32_t)size_px;
    orph->dsc.header.stride = (uint32_t)size_px * 2;
    orph->dsc.data_size = (uint32_t)size_px * (uint32_t)size_px * 2;
    orph->dsc.data = orph->pixels;
    orph->next = s_orphans;
    s_orphans = orph;
    *out = &orph->dsc;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "all %d preview slots pinned - handing out an uncached copy", PV_RAM_SLOTS);
    return ESP_OK;
}

void thr_preview_release(const lv_image_dsc_t *dsc)
{
    if (dsc == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PV_RAM_SLOTS; i++) {
        pv_slot_t *slot = &s_slots[i];
        if (slot->rel != NULL && &slot->dsc == dsc) {
            if (slot->refs > 0) {
                slot->refs--;
            }
            if (slot->refs == 0 && slot->zombie) {
                slot_free_locked(slot);
            }
            xSemaphoreGive(s_lock);
            return;
        }
    }
    for (pv_orphan_t **pp = &s_orphans; *pp != NULL; pp = &(*pp)->next) {
        if (&(*pp)->dsc == dsc) {
            pv_orphan_t *orph = *pp;
            *pp = orph->next;
            free(orph->pixels);
            free(orph);
            xSemaphoreGive(s_lock);
            return;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "release of unknown preview descriptor");
}

void thr_preview_clear_ram(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PV_RAM_SLOTS; i++) {
        pv_slot_t *slot = &s_slots[i];
        if (slot->rel == NULL) {
            continue;
        }
        if (slot->refs > 0) {
            slot->zombie = true;  // widget still holds it; freed on last release
        } else {
            slot_free_locked(slot);
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "RAM preview cache cleared");
}
