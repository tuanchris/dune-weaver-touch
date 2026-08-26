// Waveshare ESP32-S3-Touch-LCD-5B board support (1024x600 variant).
// Pin map and CH422G sequences verified against the Waveshare wiki and the
// ESP32-S3-Touch-LCD-5 demo bundle (ESP-IDF/08_lvgl_Porting).
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600
#define BOARD_LCD_PCLK_HZ (21 * 1000 * 1000)

// RGB timings for the 1024x600 panel
#define BOARD_LCD_HSYNC_BACK_PORCH 145
#define BOARD_LCD_HSYNC_FRONT_PORCH 170
#define BOARD_LCD_HSYNC_PULSE_WIDTH 30
#define BOARD_LCD_VSYNC_BACK_PORCH 23
#define BOARD_LCD_VSYNC_FRONT_PORCH 12
#define BOARD_LCD_VSYNC_PULSE_WIDTH 2

// RGB signal GPIOs
#define BOARD_LCD_GPIO_VSYNC 3
#define BOARD_LCD_GPIO_HSYNC 46
#define BOARD_LCD_GPIO_DE 5
#define BOARD_LCD_GPIO_PCLK 7
// DATA0..15 = B3..B7, G2..G7, R3..R7
#define BOARD_LCD_GPIO_DATA0 14
#define BOARD_LCD_GPIO_DATA1 38
#define BOARD_LCD_GPIO_DATA2 18
#define BOARD_LCD_GPIO_DATA3 17
#define BOARD_LCD_GPIO_DATA4 10
#define BOARD_LCD_GPIO_DATA5 39
#define BOARD_LCD_GPIO_DATA6 0
#define BOARD_LCD_GPIO_DATA7 45
#define BOARD_LCD_GPIO_DATA8 48
#define BOARD_LCD_GPIO_DATA9 47
#define BOARD_LCD_GPIO_DATA10 21
#define BOARD_LCD_GPIO_DATA11 1
#define BOARD_LCD_GPIO_DATA12 2
#define BOARD_LCD_GPIO_DATA13 42
#define BOARD_LCD_GPIO_DATA14 41
#define BOARD_LCD_GPIO_DATA15 40

// Touch (GT911) + IO expander share I2C0
#define BOARD_I2C_GPIO_SDA 8
#define BOARD_I2C_GPIO_SCL 9
#define BOARD_GPIO_TP_IRQ 4

// Initializes the shared I2C bus, puts the CH422G in output mode, resets the
// touch controller (GT911 latches I2C address 0x5D while INT is held low).
esp_err_t board_init(void);

// Backlight is the CH422G DISP line (EXIO2) — on/off only, no PWM.
esp_err_t board_backlight(bool on);

// TF-card chip select (CH422G EXIO4, active low). sdcard.c holds it selected
// for the whole session; the sdspi driver is configured with no CS GPIO.
esp_err_t board_sd_select(bool selected);

i2c_master_bus_handle_t board_i2c_bus(void);
