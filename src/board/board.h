// Waveshare ESP32-S3-Touch-LCD board support, for two panel variants:
//   5B (1024x600, default) and the 5 / 7 (800x480,
//   -DBOARD_PANEL_800X480). BOARD_WAVESHARE_7 is a separate axis: it says
//   only that the glass has non-square pixels (theme.h), not its size.
//
// The two boards are the same base design. Every GPIO below — RGB bus, I2C,
// touch IRQ — and the CH422G sequences are IDENTICAL between them; only the
// resolution and RGB timings differ, which is why the split is a handful of
// #defines and not a second board file. Verified against Waveshare's own
// demos: ESP-IDF/08_lvgl_Porting (5B) and
// ESP-IDF/09_lvgl_v9_demo/components/waveshare_rgb_lcd_port.[ch] (7).
//
// The Elecrow boards are NOT derivatives and get their own files --
// board_crowpanel5.c (Advance 5.0) and board_crowpanel7.c (7.0-HMI) -- but
// their pin maps and timings live here too, under BOARD_CROWPANEL_*.
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#if defined(BOARD_CROWPANEL_ADV_5)

// ---- Elecrow CrowPanel Advance 5.0-HMI: 5" 800x480 -------------------------
// From Elecrow's own factory code (CrowPanel-Advance-HMI-ESP32-AI-Display,
// 5.0/factory_code/LovyanGFX_Driver.h), not inferred.
//
// 16 MHz, NOT the 21 Elecrow's own LovyanGFX demo uses. Measured here
// 2026-09-01: at 21 MHz the image DRIFTS -- jumps and loses centring, the
// RGB DMA underrun signature. Their demo gets away with it because LovyanGFX
// drives the panel differently; esp_lvgl_port refills bounce buffers during
// BLANKING, and these timings have almost none: H total is 820 for 800
// active, i.e. 20 clocks of horizontal blanking against the 5B's 345. Same
// 42 MB/s peak as the 5B, a fraction of the recovery window.
//
// 16 MHz is the Waveshare 7's proven value at IDENTICAL timings (820 x 500),
// giving 32 MB/s and ~39 Hz -- still far clear of the 8-15 Hz flicker band.
// If you want the extra refresh back, buy it with PORCHES (more blanking),
// not with PCLK.
//
// LovyanGFX's pclk_idle_high = 1 is the same latching choice as the
// Waveshare boards' pclk_active_neg = 1.
#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480
#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_BACK_PORCH 8
#define BOARD_LCD_HSYNC_FRONT_PORCH 8
#define BOARD_LCD_HSYNC_PULSE_WIDTH 4
#define BOARD_LCD_VSYNC_BACK_PORCH 8
#define BOARD_LCD_VSYNC_FRONT_PORCH 8
#define BOARD_LCD_VSYNC_PULSE_WIDTH 4

#elif defined(BOARD_CROWPANEL_7)

// ---- Elecrow CrowPanel 7.0-HMI (original series): 7" 800x480 ---------------
// Elecrow's LovyanGFX config for this board (wiki, and the
// CrowPanel-7.0-HMI-ESP32-Display-800x480 examples, identical across the
// V1.0/V2.0/V3.0 revisions): 40/48/40 horizontal, 1/31/13 vertical,
// pclk_active_neg = 1. That is 928 x 525 = 487,200 clocks/frame, so Elecrow's
// 15 MHz is ~30.8 Hz with a ~15.4 Hz frame-inversion beat -- the top edge of
// the 8-15 Hz band that makes the 5B flicker. 16 MHz (32.8 Hz, 16.4 Hz beat)
// is used instead: the same peak DMA as the Advance 5 at 16 MHz (32 MB/s),
// and this panel gives the bounce buffers 128 clocks of horizontal blanking
// to refill in, seven times the Advance's 20. UNMEASURED on this glass as of
// 2026-09-03: if the dark theme beats, more PCLK is the lever (the EK9716
// controller is rated well above this), but check for drift first -- the 5B
// history below is the reason.
#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480
#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_BACK_PORCH 40
#define BOARD_LCD_HSYNC_FRONT_PORCH 40
#define BOARD_LCD_HSYNC_PULSE_WIDTH 48
#define BOARD_LCD_VSYNC_BACK_PORCH 13
#define BOARD_LCD_VSYNC_FRONT_PORCH 1
#define BOARD_LCD_VSYNC_PULSE_WIDTH 31

#elif defined(BOARD_PANEL_800X480)

// ---- ESP32-S3-Touch-LCD-7: 800x480 ----------------------------------------
// Waveshare's own values (09_lvgl_v9_demo/waveshare_rgb_lcd_port.[ch]).
//
// 820 x 500 = 410,000 clocks/frame, so 16 MHz is ~39 Hz — against the 5B's
// 24 Hz below. A frame-inverted panel beats at half the refresh, which puts
// this one near 19.5 Hz, clear of the 8-15 Hz band that makes the 5B flicker
// in mid-greys. CONFIRMED on hardware 2026-08-29: no flicker at 39 Hz, so the
// light-theme constraint the 5B forces does NOT apply here — settings.c
// therefore defaults this board to the dark theme.
// Peak framebuffer DMA is lower too (16 MHz x 2 B = 32 MB/s vs 42) and the
// framebuffer smaller (768 KB vs 1.2 MB), so the sustained-PSRAM-bandwidth
// ceiling that blocked every 5B fix is further away here.
#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480
#define BOARD_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BOARD_LCD_HSYNC_BACK_PORCH 8
#define BOARD_LCD_HSYNC_FRONT_PORCH 8
#define BOARD_LCD_HSYNC_PULSE_WIDTH 4
#define BOARD_LCD_VSYNC_BACK_PORCH 8
#define BOARD_LCD_VSYNC_FRONT_PORCH 8
#define BOARD_LCD_VSYNC_PULSE_WIDTH 4

#else

// ---- ESP32-S3-Touch-LCD-5B: 1024x600 (default) -----------------------------
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

#endif  // BOARD_PANEL_800X480

#if defined(BOARD_CROWPANEL_ADV_5)

// ---- Elecrow CrowPanel Advance 5.0 GPIO map --------------------------------
// Nothing here is shared with the Waveshare boards. LovyanGFX orders its
// pin_d0..d15 as B0..B4, G0..G5, R0..R4, which is the same low-to-high bit
// order esp_lcd's data_gpio_nums[] wants, so the list transfers 1:1.
#define BOARD_LCD_GPIO_VSYNC 41
#define BOARD_LCD_GPIO_HSYNC 40
#define BOARD_LCD_GPIO_DE 42
#define BOARD_LCD_GPIO_PCLK 39
// DATA0..15 = B0..B4, G0..G5, R0..R4
#define BOARD_LCD_GPIO_DATA0 21
#define BOARD_LCD_GPIO_DATA1 47
#define BOARD_LCD_GPIO_DATA2 48
#define BOARD_LCD_GPIO_DATA3 45
#define BOARD_LCD_GPIO_DATA4 38
#define BOARD_LCD_GPIO_DATA5 9
#define BOARD_LCD_GPIO_DATA6 10
#define BOARD_LCD_GPIO_DATA7 11
#define BOARD_LCD_GPIO_DATA8 12
#define BOARD_LCD_GPIO_DATA9 13
#define BOARD_LCD_GPIO_DATA10 14
#define BOARD_LCD_GPIO_DATA11 7
#define BOARD_LCD_GPIO_DATA12 17
#define BOARD_LCD_GPIO_DATA13 18
#define BOARD_LCD_GPIO_DATA14 3
#define BOARD_LCD_GPIO_DATA15 46

// Touch (GT911) shares I2C with the TCA9534 expander and the BM8563 RTC.
#define BOARD_I2C_GPIO_SDA 15
#define BOARD_I2C_GPIO_SCL 16
// TP_INT is a REAL GPIO here (the Waveshare boards use 4). Held low through
// the touch reset so the GT911 latches address 0x5D rather than 0x14.
#define BOARD_GPIO_TP_IRQ 1

// TF card, SPI. The Function Select DIP must be in the TF Card position:
// these three pins are shared with the I2S speaker. Chip select is NOT wired
// to a GPIO (Elecrow's own code says so), so sdcard.c runs with
// gpio_cs = GPIO_NUM_NC, exactly as on the Waveshare boards.
#define BOARD_SD_GPIO_MISO 4
#define BOARD_SD_GPIO_SCK 5
#define BOARD_SD_GPIO_MOSI 6

#elif defined(BOARD_CROWPANEL_7)

// ---- Elecrow CrowPanel 7.0-HMI GPIO map ------------------------------------
// From Elecrow's LovyanGFX config (wiki). pin_d0..d15 = B0..B4, G0..G5,
// R0..R4 is esp_lcd's data_gpio_nums[] order, so it transfers 1:1. PCLK sits
// on GPIO0, a strapping pin -- Elecrow's own choice, harmless once booted.
#define BOARD_LCD_GPIO_VSYNC 40
#define BOARD_LCD_GPIO_HSYNC 39
#define BOARD_LCD_GPIO_DE 41
#define BOARD_LCD_GPIO_PCLK 0
// DATA0..15 = B0..B4, G0..G5, R0..R4
#define BOARD_LCD_GPIO_DATA0 15
#define BOARD_LCD_GPIO_DATA1 7
#define BOARD_LCD_GPIO_DATA2 6
#define BOARD_LCD_GPIO_DATA3 5
#define BOARD_LCD_GPIO_DATA4 4
#define BOARD_LCD_GPIO_DATA5 9
#define BOARD_LCD_GPIO_DATA6 46
#define BOARD_LCD_GPIO_DATA7 3
#define BOARD_LCD_GPIO_DATA8 8
#define BOARD_LCD_GPIO_DATA9 16
#define BOARD_LCD_GPIO_DATA10 1
#define BOARD_LCD_GPIO_DATA11 14
#define BOARD_LCD_GPIO_DATA12 21
#define BOARD_LCD_GPIO_DATA13 47
#define BOARD_LCD_GPIO_DATA14 48
#define BOARD_LCD_GPIO_DATA15 45

// Backlight: a plain enable into the boost driver, PWM'd by LEDC in
// board_crowpanel7.c (Elecrow's examples do the same, at 300 Hz).
#define BOARD_GPIO_BACKLIGHT 2

// Touch (GT911) on the S3's USB pins. Neither INT nor RST reaches the ESP on
// any revision (every Elecrow example passes -1 for both), so the address it
// latched at its own power-on is whatever it is: display.c probes 0x5D, then
// 0x14, instead of assuming.
#define BOARD_I2C_GPIO_SDA 19
#define BOARD_I2C_GPIO_SCL 20
#define BOARD_TOUCH_ADDR_PROBE 1
// 100 kHz, not the 400 the Waveshare boards run: at 400 every GT911 register
// read came back 0xCF (product ID AND config version), the signature of
// edges too slow for the clock on this board's pull-ups. Measured 2026-09-03.
#define BOARD_TOUCH_I2C_HZ 100000

// TF card, SPI, with a REAL chip select for once: the sdspi driver drives it
// (sdcard.c) and board_sd_select is a no-op.
#define BOARD_SD_GPIO_MOSI 11
#define BOARD_SD_GPIO_MISO 13
#define BOARD_SD_GPIO_SCK 12
#define BOARD_SD_GPIO_CS 10

#else

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

// TF card, SPI (CS is CH422G EXIO4 -- see sdcard.c).
#define BOARD_SD_GPIO_MOSI 11
#define BOARD_SD_GPIO_SCK 12
#define BOARD_SD_GPIO_MISO 13

#endif  // board GPIO map

// Initializes the shared I2C bus, puts the CH422G in output mode, resets the
// touch controller (GT911 latches I2C address 0x5D while INT is held low).
esp_err_t board_init(void);

// On/off only, no PWM. EXIO2 is called DISP in Waveshare's netlist but reaches
// only the AP3032 boost driver's CTRL pin; the panel's own DISP input is tied
// to 3V3 through R30, so this never stops the panel refreshing. Off at boot —
// app_main lights it once the first frame exists.
esp_err_t board_backlight(bool on);

// Panel reset (CH422G EXIO3, active low) — see board.c. Asserted, the LC is
// undriven; released, a dumb RGB panel resumes from the live pixel stream
// with no re-init, since there is no register state to restore.
esp_err_t board_lcd_reset(bool asserted);

// TF-card chip select (CH422G EXIO4, active low). sdcard.c holds it selected
// for the whole session; the sdspi driver is configured with no CS GPIO.
esp_err_t board_sd_select(bool selected);

i2c_master_bus_handle_t board_i2c_bus(void);
