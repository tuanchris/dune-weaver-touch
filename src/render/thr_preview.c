// Circular preview tiles, read from the pattern TF card ONLY (PORTING_NOTES
// §7a). ONE folder, ONE size: the card ships /previews/<key>.bin, a 300x300
// 4-bit ALPHA MASK (45,000 B) carrying the pattern's stroke coverage and
// nothing else. This module reads it, resamples the mask to whatever size the
// widget displays at, composites it over a dish built from the CURRENT theme,
// and keeps a small pinned PSRAM LRU alive for lv_image_set_src.
//
// Why a mask and not RGB565 (2026-08-26):
//   * 45,000 B beats 180,000 for the same 300 px picture, on a bus where the
//     read is nearly the whole cost — and beats even a 160 px RGB565 tile.
//   * Resampling ONE 8-bit channel to reach 160 costs ~3 ms; resampling three
//     packed RGB565 channels cost ~20.
//   * Tiles carry no colour, so night/day retheming needs no card re-prep.
//     The dish/ring/sand colours are theme tokens (th.preview_*).
// Measured lossless enough: 4-bit alpha lands within 6/255 of the 8-bit
// composite, under the RGB565 step of 8 this panel quantises to anyway.
//
// The panel deliberately does NOT render previews and does NOT cache them in
// flash (removed 2026-08-26): rasterizing meant fetching every .thr over the
// wire at 30-60 KB/s, and the LittleFS tile cache burned 10 MB of flash
// re-storing what the card already holds. No card => no previews, and Browse
// nags for one. SD reads BLOCK: jobs task only, which is also what makes the
// shared scratch buffers below safe.
#include "render/thr_preview.h"

#include <math.h>  // sqrtf, in the once-per-size dish base only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/sd_catalog.h"
#include "ui/theme.h"  // th.preview_*: the dish look lives in the theme now
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "thr_preview";

#define PV_RAM_SLOTS 24
#define PV_MIN_SIZE 16
#define PV_MAX_SIZE 640

// The card's ONLY tile size (tools/make_pattern_sd.py --size). Every widget
// size is resampled from it, so the card stays the single source. Change this
// and --size together, or every tile fails its length check.
#define PV_SD_SIZE_PX 300
#define PV_MASK_BYTES ((size_t)PV_SD_SIZE_PX * PV_SD_SIZE_PX / 2)  // 4 bits/px

// Dish geometry, in step with make_pattern_sd.py: the mask's rho=1 lands on
// the dish edge at radius (size/2 - margin), and the ring is a 2 px band just
// inside it. Integer division, exactly as the tool does it.
#define PV_MARGIN(size) ((size) * 12 / 512)
#define PV_RING_PX 2.0f

// Width of the antialiased band at the dish rim and the ring's inner edge, in
// px. The tool used to supersample x4 and LANCZOS down, so a hard-edged
// circle here reads as a visibly jagged dish against the old tiles — measured
// at up to 32/255 along the whole circumference.
#define PV_EDGE_PX 1.0f

typedef struct {
    char *rel;              // heap copy of the pattern rel path; NULL = free slot
    int size_px;
    uint16_t corner;        // colour painted outside the dish (part of the key)
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

// Tiles live in PSRAM, but reading straight into PSRAM is a trap: the S3 does
// not set SOC_SDMMC_PSRAM_DMA_CAPABLE, so sdmmc_read_sectors sees an external
// pointer and falls back to SINGLE-BLOCK reads through a 512 B temp buffer
// (esp-idf sdmmc_cmd.c). Measured on the board: 180 KB took ~400 ms, about
// 450 KB/s on a bus good for 2.5 MB/s. Reading through an internal
// DMA-capable bounce in big chunks restores multi-block transfers; the
// memcpy to PSRAM is free by comparison.
#define PV_BOUNCE_BYTES (32 * 1024)
static uint8_t *s_bounce;

// Scratch, allocated on first use and kept: the packed mask straight off the
// card, and the same mask expanded to 8 bits so it can be resampled. Shared
// rather than per-call because every caller runs on the single jobs task
// (see the header) and a 135 KB malloc/free per tile is pure churn.
static uint8_t *s_mask;   // PV_MASK_BYTES
static uint8_t *s_alpha;  // PV_SD_SIZE_PX^2

// The bare dish a tile is composited onto: background/ring/dish, antialiased,
// already in RGB565. Identical for every pattern, so it is built once per
// (size, corner) and reused — two entries covers the grid (160 on th.surface)
// and the detail/Now Playing disc (300 on th.bg) without thrashing.
typedef struct {
    int size_px;
    uint16_t corner;
    uint16_t *px;
} pv_base_t;
static pv_base_t s_bases[2];

// ---------------------------------------------------------------- utilities

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

// ------------------------------------------------------- local SD previews

// Preview key, dune-weaver-mobile semantics: basename only (cross-table —
// the same pattern filed under another folder still finds its image),
// lowercased (export casing varies). "sub/Star.thr" -> "star.thr".
// ONE folder: /previews/. There are no per-size directories any more.
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

// Read one packed 4-bit mask off the card. ESP_ERR_NOT_FOUND = the card has
// no usable tile for this pattern, a PERMANENT miss for this card, so callers
// must not retry it. ESP_ERR_INVALID_STATE = no card in the slot, which a
// later hot-insert can fix.
static esp_err_t sd_preview_load(const char *rel, uint8_t *mask)
{
    int64_t t0 = esp_timer_get_time();
    if (!sd_catalog_present()) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t t1 = esp_timer_get_time();
    size_t data_size = PV_MASK_BYTES;
    char path[192];
    sd_preview_path(path, sizeof(path), rel);
    if (path[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    int64_t t2 = esp_timer_get_time();
    // POSIX, not stdio: newlib's fread refills through the FILE's own small
    // buffer, so FATFS only ever sees ~1 KB reads no matter how much you ask
    // for. read() passes the length straight down.
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != data_size) {
        // The length IS the format check — there is no header. A card written
        // for an older firmware (300px RGB565 = 180,000 B) lands here on every
        // tile, and silently showing 1200 empty dishes is a rotten way to
        // report "re-run make_pattern_sd.py", so say it once.
        static bool warned;
        if (!warned && st.st_size > 0) {
            warned = true;
            ESP_LOGW(TAG, "%s is %lld B, expected %u — card written for a "
                          "different firmware? re-run tools/make_pattern_sd.py",
                     path, (long long)st.st_size, (unsigned)data_size);
        }
        close(fd);
        return ESP_ERR_NOT_FOUND;  // fstat: no second directory lookup
    }
    int64_t t3 = esp_timer_get_time();
    size_t rd = 0;
    while (rd < data_size) {
        size_t want = data_size - rd;
        uint8_t *dst = mask + rd;
        ssize_t n;
        if (s_bounce != NULL) {
            if (want > PV_BOUNCE_BYTES) {
                want = PV_BOUNCE_BYTES;
            }
            n = read(fd, s_bounce, want);
            if (n > 0) {
                memcpy(dst, s_bounce, (size_t)n);
            }
        } else {
            n = read(fd, dst, want);  // straight to PSRAM: single-block path
        }
        if (n <= 0) {
            break;
        }
        rd += (size_t)n;
    }
    int64_t t4 = esp_timer_get_time();
    close(fd);
    ESP_LOGD(TAG, "mask: present %lldms path %lldms open %lldms read %lldms (%uB)",
             (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000,
             (t4 - t3) / 1000, (unsigned)data_size);
    return (rd == data_size) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ------------------------------------------------------- mask -> RGB565

// Unpack the 4-bit mask to one byte per pixel, scaled to 0..255 so the
// resampler has headroom. Low nibble is the even pixel (the card format note
// in make_pattern_sd.py); x17 maps 15 -> 255 exactly, so a native-size tile
// round-trips with no loss at all.
static void expand_mask(const uint8_t *packed, uint8_t *alpha, int size_px)
{
    size_t pairs = (size_t)size_px * (size_t)size_px / 2;
    for (size_t i = 0; i < pairs; i++) {
        uint8_t b = packed[i];
        alpha[2 * i] = (uint8_t)((b & 0x0F) * 17);
        alpha[2 * i + 1] = (uint8_t)((b >> 4) * 17);
    }
}

// Integer bilinear resample of ONE 8-bit channel (8-bit fixed-point weights).
// Quality matters: these are fine hairlines and nearest-neighbour would alias
// them to a flicker of 1-2 px. This replaced a three-channel RGB565 version —
// identical maths over one channel instead of three unpacked ones, which is
// most of why the grid's 300 -> 160 costs ~3 ms rather than ~20.
static void scale_a8_bilinear(const uint8_t *src, int src_px, uint8_t *dst, int dst_px)
{
    // Map destination pixel centers into source space; 16.16 fixed point.
    uint32_t step = ((uint32_t)src_px << 16) / (uint32_t)dst_px;
    int32_t start = (int32_t)(step >> 1) - (1 << 15);
    for (int dy = 0; dy < dst_px; dy++) {
        int32_t sy_fp = start + (int32_t)(step * (uint32_t)dy);
        if (sy_fp < 0) {
            sy_fp = 0;
        }
        int sy = sy_fp >> 16;
        uint32_t wy = (sy_fp >> 8) & 0xFF;
        if (sy >= src_px - 1) {
            sy = src_px - 2;
            wy = 255;
        }
        const uint8_t *r0 = src + (size_t)sy * src_px;
        const uint8_t *r1 = r0 + src_px;
        uint8_t *out = dst + (size_t)dy * dst_px;
        for (int dx = 0; dx < dst_px; dx++) {
            int32_t sx_fp = start + (int32_t)(step * (uint32_t)dx);
            if (sx_fp < 0) {
                sx_fp = 0;
            }
            int sx = sx_fp >> 16;
            uint32_t wx = (sx_fp >> 8) & 0xFF;
            if (sx >= src_px - 1) {
                sx = src_px - 2;
                wx = 255;
            }
            uint32_t top = r0[sx] * (256 - wx) + r0[sx + 1] * wx;
            uint32_t bot = r1[sx] * (256 - wx) + r1[sx + 1] * wx;
            out[dx] = (uint8_t)((top * (256 - wy) + bot * wy) >> 16);
        }
    }
}

// The bare dish, antialiased, in RGB565: background outside, a PV_RING_PX
// band at the rim, dish inside. Pure geometry + theme, so it is the same for
// every pattern and gets built once per (size, corner) and kept. sqrtf runs
// here, once per pixel per size — never in the per-tile path.
static const uint16_t *base_tile_get(int size_px, uint16_t corner)
{
    for (int i = 0; i < 2; i++) {
        if (s_bases[i].px != NULL && s_bases[i].size_px == size_px &&
            s_bases[i].corner == corner) {
            return s_bases[i].px;
        }
    }
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (s_bases[i].px == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {  // only ever two in play (grid, and detail/Now Playing)
        slot = 0;
        free(s_bases[0].px);
        s_bases[0].px = NULL;
    }
    uint16_t *px = heap_caps_malloc((size_t)size_px * size_px * 2, MALLOC_CAP_SPIRAM);
    if (px == NULL) {
        return NULL;
    }
    const uint8_t out_r = (uint8_t)(((corner >> 11) & 0x1F) << 3);
    const uint8_t out_g = (uint8_t)(((corner >> 5) & 0x3F) << 2);
    const uint8_t out_b = (uint8_t)((corner & 0x1F) << 3);
    float cx = (float)size_px * 0.5f;
    float r = cx - (float)PV_MARGIN(size_px);
    float r_in = r - PV_RING_PX;
    for (int y = 0; y < size_px; y++) {
        float dy = (float)y + 0.5f - cx;
        uint16_t *row = px + (size_t)y * size_px;
        for (int x = 0; x < size_px; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            // Coverage of "inside the rim" and "inside the ring", each a 1 px
            // ramp. dish = inner, ring = outer - inner, background = the rest.
            float c_out = (r - d) / PV_EDGE_PX + 0.5f;
            float c_in = (r_in - d) / PV_EDGE_PX + 0.5f;
            c_out = (c_out < 0.0f) ? 0.0f : ((c_out > 1.0f) ? 1.0f : c_out);
            c_in = (c_in < 0.0f) ? 0.0f : ((c_in > 1.0f) ? 1.0f : c_in);
            float w_ring = c_out - c_in;
            float w_bg = 1.0f - c_out;
            float fr = out_r * w_bg + th.preview_ring.red * w_ring + th.preview_dish.red * c_in;
            float fg = out_g * w_bg + th.preview_ring.green * w_ring + th.preview_dish.green * c_in;
            float fb = out_b * w_bg + th.preview_ring.blue * w_ring + th.preview_dish.blue * c_in;
            row[x] = (uint16_t)((((uint32_t)fr >> 3) << 11) |
                                (((uint32_t)fg >> 2) << 5) | ((uint32_t)fb >> 3));
        }
    }
    s_bases[slot].size_px = size_px;
    s_bases[slot].corner = corner;
    s_bases[slot].px = px;
    return px;
}

// Blend the base toward the sand colour by coverage. Most of a tile is bare
// dish, so the a == 0 shortcut carries the majority of pixels at one load and
// one store. Colours are read live from the theme, which is what makes a
// retheme a cache flush rather than a card re-prep.
static void composite_tile(const uint8_t *alpha, const uint16_t *base, uint16_t *out,
                           int size_px)
{
    const uint32_t sr = th.preview_sand.red;
    const uint32_t sg = th.preview_sand.green;
    const uint32_t sb = th.preview_sand.blue;
    size_t n = (size_t)size_px * (size_t)size_px;
    for (size_t i = 0; i < n; i++) {
        uint32_t a = alpha[i];
        if (a == 0) {
            out[i] = base[i];
            continue;
        }
        uint32_t b = base[i];
        uint32_t inv = 255 - a;
        uint32_t r = ((((b >> 11) & 0x1F) << 3) * inv + sr * a) / 255;
        uint32_t g = ((((b >> 5) & 0x3F) << 2) * inv + sg * a) / 255;
        uint32_t bl = (((b & 0x1F) << 3) * inv + sb * a) / 255;
        out[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

// Allocated on first use, then kept: a tile costs 135 KB of scratch and
// malloc/free per tile is pure churn. Only the jobs task gets here.
static esp_err_t ensure_scratch(void)
{
    if (s_mask == NULL) {
        s_mask = heap_caps_malloc(PV_MASK_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_alpha == NULL) {
        s_alpha = heap_caps_malloc((size_t)PV_SD_SIZE_PX * PV_SD_SIZE_PX, MALLOC_CAP_SPIRAM);
    }
    return (s_mask != NULL && s_alpha != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

// Fill `pixels` with this pattern's tile at size_px: read the card's one
// 300 px mask, resample it to the size the widget displays at, and composite
// it over a dish built from the current theme. There is no per-size tile on
// the card and no fallback path any more — one folder, one size, and the
// resample is cheap because it runs on a single 8-bit channel.
static esp_err_t load_tile(const char *rel, uint8_t *pixels, int size_px, uint16_t corner)
{
    esp_err_t err = ensure_scratch();
    if (err != ESP_OK) {
        return err;
    }
    int64_t ta = esp_timer_get_time();
    err = sd_preview_load(rel, s_mask);
    if (err != ESP_OK) {
        return err;
    }
    int64_t tb = esp_timer_get_time();

    expand_mask(s_mask, s_alpha, PV_SD_SIZE_PX);
    int64_t tx = esp_timer_get_time();
    const uint8_t *alpha = s_alpha;
    uint8_t *resampled = NULL;
    if (size_px != PV_SD_SIZE_PX) {
        resampled = heap_caps_malloc((size_t)size_px * (size_t)size_px, MALLOC_CAP_SPIRAM);
        if (resampled == NULL) {
            return ESP_ERR_NO_MEM;
        }
        scale_a8_bilinear(s_alpha, PV_SD_SIZE_PX, resampled, size_px);
        alpha = resampled;
    }
    int64_t tc = esp_timer_get_time();

    const uint16_t *base = base_tile_get(size_px, corner);
    if (base == NULL) {
        free(resampled);
        return ESP_ERR_NO_MEM;
    }
    int64_t tbase = esp_timer_get_time();
    composite_tile(alpha, base, (uint16_t *)pixels, size_px);
    free(resampled);
    int64_t td = esp_timer_get_time();
    // -DPV_DEBUG_TIMING promotes this to INFO. ESP_LOGD is compiled OUT on
    // device (CONFIG_LOG_MAXIMUM_LEVEL=3), so the debug form is sim-only —
    // which is exactly how a 375 ms/tile regression went unnoticed.
#ifdef PV_DEBUG_TIMING
    ESP_LOGI(TAG,
#else
    ESP_LOGD(TAG,
#endif
             "%dpx: read %lldus expand %lldus resample %lldus base %lldus "
             "composite %lldus | total %lldus",
             size_px, tb - ta, tx - tb, tc - tx, tbase - tc, td - tbase, td - ta);
    return ESP_OK;
}

// ---------------------------------------------------------------- RAM LRU

// Call with s_lock held.
static pv_slot_t *lru_find_locked(const char *rel, int size_px, uint16_t corner)
{
    for (int i = 0; i < PV_RAM_SLOTS; i++) {
        pv_slot_t *slot = &s_slots[i];
        if (slot->rel != NULL && !slot->zombie && slot->size_px == size_px &&
            slot->corner == corner && strcmp(slot->rel, rel) == 0) {
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
static pv_slot_t *lru_insert_locked(const char *rel, int size_px, uint16_t corner,
                                    uint8_t *pixels)
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
    victim->corner = corner;
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

// ---------------------------------------------------------------- public API

esp_err_t thr_preview_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_bounce == NULL) {
        s_bounce = heap_caps_malloc(PV_BOUNCE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_bounce == NULL) {
            ESP_LOGW(TAG, "no DMA bounce buffer - tile reads will be ~5x slower");
        }
    }
    ESP_LOGI(TAG, "previews: pattern card only (no render, no flash cache)");
    return ESP_OK;
}

esp_err_t thr_preview_get(const char *rel_path, int size_px, uint16_t corner,
                          const lv_image_dsc_t **out)
{
    if (rel_path == NULL || out == NULL || size_px < PV_MIN_SIZE || size_px > PV_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = NULL;
    const char *rel = canonical_rel(rel_path);
    if (*rel == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // 1) RAM LRU
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pv_slot_t *slot = lru_find_locked(rel, size_px, corner);
    if (slot != NULL) {
        slot->refs++;
        *out = &slot->dsc;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);

    // 2) the card
    size_t data_size = (size_t)size_px * (size_t)size_px * 2;
    uint8_t *pixels = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (pixels == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = load_tile(rel, pixels, size_px, corner);
    if (err != ESP_OK) {
        free(pixels);
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot = lru_insert_locked(rel, size_px, corner, pixels);
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
    // The dish bases bake theme colours, so they go too. This is what makes
    // this function the retheme hook: call it after theme_set_dark() and every
    // tile is recomposited in the new palette off the same card.
    for (int i = 0; i < 2; i++) {
        free(s_bases[i].px);
        s_bases[i].px = NULL;
        s_bases[i].size_px = 0;
        s_bases[i].corner = 0;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "RAM preview cache cleared");
}
