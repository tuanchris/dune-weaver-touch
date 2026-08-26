#include "display.h"

#include "board.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

// 10 lines per bounce buffer keeps PSRAM DMA from starving the panel
#define BOUNCE_BUFFER_LINES 10

static lv_display_t *s_disp;

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
    tp_io_cfg.scl_speed_hz = 400000;
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

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = tp,
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg) != NULL, ESP_FAIL, TAG, "add touch");

    ESP_LOGI(TAG, "display up: %dx%d @ %d MHz pclk", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

lv_display_t *display_lvgl_disp(void)
{
    return s_disp;
}
