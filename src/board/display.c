#include "display.h"

#include "board.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "display";

// 10 lines per bounce buffer keeps PSRAM DMA from starving the panel. Two are
// allocated (1024 px * 2 B each), so this is 40 KB of internal DMA RAM.
// Doubling to 20 was tried 2026-08-27 to buy headroom for a 32 MHz pixel clock
// and did NOT stop the drift, while costing 41 KB internal free and halving the
// largest contiguous block (119,723/63,488 -> 78,675/34,816). Reverted: the
// pixel-clock ceiling is not bounce-buffer slack. See board.h BOARD_LCD_PCLK_HZ.
#define BOUNCE_BUFFER_LINES 10

static lv_display_t *s_disp;

// ---------------------------------------------------------- buffered touch
// esp_lvgl_port's stock read callback samples the GT911 from inside the LVGL
// task, so while a long render blocks that task every intermediate touch
// position is lost. A quick flick then collapses to "press at A, release at
// A" — no movement ever seen, which LVGL classifies as a CLICK on whatever
// sat under the finger: the ghost-select-while-scrolling bug. Instead a
// dedicated task samples the controller every TOUCH_POLL_MS into a queue and
// the read callback drains the backlog via continue_reading, so LVGL always
// classifies the full gesture, however late it processes it. Samples carry
// their capture tick so scroll-throw velocity stays correct in a burst.

#define TOUCH_POLL_MS 10
#define TOUCH_QUEUE_LEN 64
#define TOUCH_TASK_STACK 3072
#define TOUCH_TASK_PRIO 6  // above the LVGL task: sampling must not miss

typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
    uint32_t tick;  // lv_tick at capture
} touch_sample_t;

static esp_lcd_touch_handle_t s_tp;
static QueueHandle_t s_touch_queue;
static touch_sample_t s_touch_reported;  // LVGL task only: last state handed over

// Polls the GT911 (i2c_master serializes the bus, so this is safe alongside
// the CH422G writes) and queues state/position CHANGES only — a resting
// finger queues nothing and the read callback just repeats the last state.
static void touch_poll_task(void *arg)
{
    (void)arg;
    touch_sample_t last = {0};
    for (;;) {
        esp_lcd_touch_read_data(s_tp);
        uint16_t x = 0;
        uint16_t y = 0;
        uint8_t cnt = 0;
        bool pressed = esp_lcd_touch_get_coordinates(s_tp, &x, &y, NULL, &cnt, 1) && cnt > 0;
        touch_sample_t s = {
            .x = pressed ? (int16_t)x : last.x,
            .y = pressed ? (int16_t)y : last.y,
            .pressed = pressed,
            .tick = lv_tick_get(),
        };
        if (s.pressed != last.pressed || s.x != last.x || s.y != last.y) {
            if (xQueueSend(s_touch_queue, &s, 0) != pdTRUE) {
                // Full: drop the OLDEST sample. The newest must never be lost
                // — dropping a release would leave LVGL stuck pressed.
                touch_sample_t scrap;
                (void)xQueueReceive(s_touch_queue, &scrap, 0);
                (void)xQueueSend(s_touch_queue, &s, 0);
            }
            last = s;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

// LVGL indev read: hand over one queued sample per call; continue_reading
// makes lv_indev_read loop until the backlog is drained. Empty queue = repeat
// the current state (keeps long-press timing alive via the default tick).
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    touch_sample_t s;
    if (xQueueReceive(s_touch_queue, &s, 0) == pdTRUE) {
        s_touch_reported = s;
        data->timestamp = s.tick;
        data->continue_reading = uxQueueMessagesWaiting(s_touch_queue) > 0;
    }
    data->point.x = s_touch_reported.x;
    data->point.y = s_touch_reported.y;
    data->state = s_touch_reported.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t display_init(void)
{
    const esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_back_porch = BOARD_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH,
            .hsync_pulse_width = BOARD_LCD_HSYNC_PULSE_WIDTH,
            .vsync_back_porch = BOARD_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH,
            .vsync_pulse_width = BOARD_LCD_VSYNC_PULSE_WIDTH,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * BOUNCE_BUFFER_LINES,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = BOARD_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BOARD_LCD_GPIO_VSYNC,
        .de_gpio_num = BOARD_LCD_GPIO_DE,
        .pclk_gpio_num = BOARD_LCD_GPIO_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            BOARD_LCD_GPIO_DATA0, BOARD_LCD_GPIO_DATA1, BOARD_LCD_GPIO_DATA2,
            BOARD_LCD_GPIO_DATA3, BOARD_LCD_GPIO_DATA4, BOARD_LCD_GPIO_DATA5,
            BOARD_LCD_GPIO_DATA6, BOARD_LCD_GPIO_DATA7, BOARD_LCD_GPIO_DATA8,
            BOARD_LCD_GPIO_DATA9, BOARD_LCD_GPIO_DATA10, BOARD_LCD_GPIO_DATA11,
            BOARD_LCD_GPIO_DATA12, BOARD_LCD_GPIO_DATA13, BOARD_LCD_GPIO_DATA14,
            BOARD_LCD_GPIO_DATA15,
        },
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_cfg, &panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    // GT911 over the shared I2C bus; INT/RST already handled in board_init()
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
#if defined(BOARD_TOUCH_I2C_HZ)
    tp_io_cfg.scl_speed_hz = BOARD_TOUCH_I2C_HZ;
#else
    tp_io_cfg.scl_speed_hz = 400000;
#endif
#if defined(BOARD_TOUCH_ADDR_PROBE)
    // No INT/RST latch on this board (board.h), so the GT911 answers at
    // whichever address it chose for itself at power-on. Default first, then
    // the backup; an unanswered probe leaves the default and the driver's own
    // error says so.
    if (i2c_master_probe(board_i2c_bus(), (uint16_t)tp_io_cfg.dev_addr, 50) != ESP_OK &&
        i2c_master_probe(board_i2c_bus(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50) == ESP_OK) {
        tp_io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X", (unsigned)tp_io_cfg.dev_addr);
#endif
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(board_i2c_bus(), &tp_io_cfg, &tp_io), TAG, "tp io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp), TAG, "gt911");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // The default 7168-byte stack overflows rendering the deeper pages
    // (nested flex + LONG_MODE_DOTS labels + opa layers) — symptoms are
    // corrupted backtraces full of 0xa5a5a5a5 and StoreProhibited in
    // scheduler assembly.
    port_cfg.task_stack = 16384;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .full_refresh = true,
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "add disp");

    // Buffered touch instead of lvgl_port_add_touch — see the block comment
    // at touch_poll_task.
    s_tp = tp;
    s_touch_queue = xQueueCreate(TOUCH_QUEUE_LEN, sizeof(touch_sample_t));
    ESP_RETURN_ON_FALSE(s_touch_queue != NULL, ESP_ERR_NO_MEM, TAG, "touch queue");
    lvgl_port_lock(0);
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, s_disp);
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "touch indev");
    ESP_RETURN_ON_FALSE(
        xTaskCreate(touch_poll_task, "touch_poll", TOUCH_TASK_STACK, NULL, TOUCH_TASK_PRIO,
                    NULL) == pdPASS,
        ESP_FAIL, TAG, "touch task");

    ESP_LOGI(TAG, "display up: %dx%d @ %d MHz pclk", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

lv_display_t *display_lvgl_disp(void)
{
    return s_disp;
}
