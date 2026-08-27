// Waveshare ESP32-S3-Touch-LCD-5B board support (1024x600 variant).
// Pin map and CH422G sequences verified against the Waveshare wiki and the
// ESP32-S3-Touch-LCD-5 demo bundle (ESP-IDF/08_lvgl_Porting).
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600
// 21 MHz, Waveshare's value. This IS the ceiling — measured 2026-08-27, do not
// retry without new information.
//
// Why it matters: 1369x637 = 872,053 clocks/frame, so this is ~24 Hz, and a
// frame-inverted panel beats at half the refresh. That puts an 11-13 Hz flicker
// on the glass, right at peak eye sensitivity. It is invisible at white, black
// and saturated primaries (flat parts of the V-T curve — both polarities land
// at the same luminance) and plainly visible on greys, which is the whole of
// the "flickering on low contrast colours" symptom. PCLK is the only lever:
// peak framebuffer DMA demand is set by PCLK alone, so trimming the porches
// raises refresh while REMOVING the blanking slack the bounce buffer refills
// in, which is strictly worse.
//
// What was tried on hardware, all of it drifting (image jumps and loses
// centring — RGB DMA underrun, Waveshare's "screen drifting"):
//   40 MHz (160/4, 2.0x)  -> unusable in seconds
//   32 MHz (160/5, 1.6x)  -> jumped about once a second
//   32 MHz + bounce buffers doubled to 20 lines (80 KB internal) -> no better,
//       which is the useful result: the binding constraint is NOT bounce-buffer
//       slack, so buying it with internal RAM is a dead end.
// Untried rungs are 160/7 (22.86 MHz, 13.1 Hz beat) and 160/6 (26.67 MHz,
// 15.3 Hz beat) — both still inside the 8-15 Hz band, so neither solves it.
// Treat the flicker as a property of this panel + SoC and mitigate in the
// theme instead: keep large fills out of the mid-grey band where the V-T curve
// is steep and the beat is visible.
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

// On/off only, no PWM. EXIO2 is called DISP in Waveshare's netlist but reaches
// only the AP3032 boost driver's CTRL pin; the panel's own DISP input is tied
// to 3V3 through R30, so this never stops the panel refreshing. Off at boot —
// app_main lights it once the first frame exists.
esp_err_t board_backlight(bool on);

// TF-card chip select (CH422G EXIO4, active low). sdcard.c holds it selected
// for the whole session; the sdspi driver is configured with no CS GPIO.
esp_err_t board_sd_select(bool selected);

i2c_master_bus_handle_t board_i2c_bus(void);
