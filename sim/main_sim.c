// Desktop simulator entry point: SDL window standing in for the 1024x600
// panel, then the same bring-up sequence as src/main.c minus board/display.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"  // also pulls the SDL driver API (LV_USE_SDL)

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "app/jobs.h"
#include "app/state.h"
#include "net/fw_client.h"
#include "net/settings.h"
#include "net/wifi.h"
#include "render/thr_preview.h"
#include "ui/theme.h"
#include "ui/ui.h"

static const char *TAG = "sim";

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Self-screenshot: `touch <dir>/shot.req` (default dir "sim") and the next
// loop iteration dumps screen + top/sys layers as ARGB8888 raws and writes
// shot.done; sim/shot.py composites them into a PNG. Avoids macOS screen-
// recording permissions entirely.
static void maybe_snapshot(void)
{
    const char *dir = getenv("DWT_SIM_SHOT_DIR");
    if (dir == NULL) {
        dir = "sim";
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/shot.req", dir);
    if (access(path, F_OK) != 0) {
        return;
    }
    unlink(path);

    lv_obj_t *layers[] = { lv_screen_active(), lv_layer_top(), lv_layer_sys() };
    for (int i = 0; i < 3; i++) {
        lv_draw_buf_t *buf = lv_snapshot_take(layers[i], LV_COLOR_FORMAT_ARGB8888);
        if (buf == NULL) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/shot_layer%d.raw", dir, i);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            uint32_t hdr[3] = { buf->header.w, buf->header.h, buf->header.stride };
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite(buf->data, 1, (size_t)buf->header.stride * buf->header.h, f);
            fclose(f);
        }
        lv_draw_buf_destroy(buf);
    }
    snprintf(path, sizeof(path), "%s/shot.done", dir);
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fclose(f);
    }
}

// DWT_SIM_PREVIEW_SELFTEST=<pattern.thr>: pull that pattern's tile at every
// size the UI asks for, dump each as a raw RGB565 next to the shot files, and
// exit. Exists because the 300 px path (Now Playing, the Browse detail
// overlay) is only reachable by tapping, so it would otherwise go unverified
// on a card-format change — and a format change is exactly when you want to
// see both sizes decoded. sim/check_tiles.py renders the dumps.
static int preview_selftest(const char *rel)
{
    if (thr_preview_init() != ESP_OK) {
        ESP_LOGE(TAG, "selftest: thr_preview_init failed");
        return 1;
    }
    const int sizes[] = { 160, 300 };  // CARD_PREVIEW_PX, DETAIL_SRC_PX
    const char *dir = getenv("DWT_SIM_SHOT_DIR");
    if (dir == NULL) {
        dir = "sim";
    }
    int rc = 0;
    for (int i = 0; i < 2; i++) {
        const lv_image_dsc_t *dsc = NULL;
        // Same corner each page uses: the grid sits on a card, the big
        // previews on the page background.
        uint16_t corner = ui_rgb565(sizes[i] == 160 ? th.surface : th.bg);
        esp_err_t err = thr_preview_get(rel, sizes[i], corner, &dsc);
        if (err != ESP_OK || dsc == NULL) {
            ESP_LOGE(TAG, "selftest: %s at %d -> %s", rel, sizes[i],
                     esp_err_to_name(err));
            rc = 1;
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/selftest_%d.raw", dir, sizes[i]);
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(dsc->data, 1, dsc->data_size, f);
            fclose(f);
        }
        ESP_LOGI(TAG, "selftest: %s at %d -> %ux%u, %u B -> %s", rel, sizes[i],
                 (unsigned)dsc->header.w, (unsigned)dsc->header.h,
                 (unsigned)dsc->data_size, path);
        thr_preview_release(dsc);
    }
    return rc;
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_sdl_window_create(1024, 600);
    lv_sdl_window_set_title(disp, "dune-weaver-touch sim");
    const char *zoom = getenv("DWT_SIM_ZOOM");
    if (zoom != NULL) {
        lv_sdl_window_set_zoom(disp, (uint8_t)atoi(zoom));
    }
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    lv_sdl_keyboard_create();

    // Same order as app_main() in src/main.c.
    ESP_ERROR_CHECK(settings_init());

    lvgl_port_lock(0);
    theme_init();
    theme_set_dark(settings_get()->dark_mode);
    const char *selftest = getenv("DWT_SIM_PREVIEW_SELFTEST");
    if (selftest != NULL) {
        int rc = preview_selftest(selftest);  // needs the theme, not the UI
        lvgl_port_unlock();
        return rc;
    }
    ui_init();
    lvgl_port_unlock();

    ESP_ERROR_CHECK(jobs_init());
    ESP_ERROR_CHECK(fw_client_init());
    if (thr_preview_init() != ESP_OK) {
        ESP_LOGW(TAG, "preview cache unavailable");
    }
    ESP_ERROR_CHECK(wifi_init());
    state_init();

    ESP_LOGI(TAG, "dune-weaver-touch sim up (tables via DWT_SIM_TABLES, "
                  "default DWSIM=http://127.0.0.1:8080)");

    while (true) {
        lvgl_port_lock(0);
        uint32_t wait_ms = lv_timer_handler();
        maybe_snapshot();
        lvgl_port_unlock();
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > 20) {
            wait_ms = 20;  // stay responsive to input + background publishes
        }
        usleep(wait_ms * 1000);
    }
    return 0;
}
