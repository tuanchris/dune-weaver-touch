// Elecrow CrowPanel Advance 5.0-HMI board support. Compiled only for
// -DBOARD_CROWPANEL_ADV_5; board.c holds the Waveshare equivalent and is
// #if'd out for this env (one file per board rather than nested ifdefs,
// because the two expanders have nothing in common).
//
// Verified against Elecrow's own factory code
// (Elecrow-RD/CrowPanel-Advance-HMI-ESP32-AI-Display, 5.0/factory_code).
//
// The differences from the Waveshare boards that matter here:
//   * The IO expander is a TCA9534 with a normal register model, NOT the
//     CH422G's "every function is its own I2C address" scheme.
//   * TP_INT is a real GPIO (1), not a shared expander line.
//   * There is no USB-Serial-JTAG: GPIO19/20 are the I2S microphone, so the
//     console goes out UART0 to the onboard CH340K (see platformio.ini's
//     custom_sdkconfig for this env).
//   * The SD card's chip select is not wired to anything the ESP can drive,
//     same as the Waveshare boards -- see sdcard.c.
#include "board.h"

#if defined(BOARD_CROWPANEL_ADV_5)

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// ---- IO expander ----------------------------------------------------------
// Two revisions exist and Elecrow does not make it easy to tell them apart
// from the outside, so probe rather than guess:
//   v1.0-ish  TCA9534 @ 0x18   -- plain 8-bit expander, backlight is ON/OFF
//   v1.1+     STC8H1K28 @ 0x30 -- MCU, accepts a BRIGHTNESS byte
// The distinction is not cosmetic: on the STC8 the backlight can be DIMMED
// instead of switched, which is the one thing that avoids the white-halo
// mechanism that plagues the Waveshare 5B (backlight boost power-cycling).
#define TCA9534_ADDR 0x18
#define TCA9534_REG_OUTPUT 0x01
#define TCA9534_REG_CONFIG 0x03

#define STC8_ADDR 0x30

// TCA9534 bit assignments, from Elecrow's setup(): pin 2 is pulsed low/high
// with GPIO1 held low around it (the classic GT911 address latch), and pin 4
// is toggled around audio playback. Pins 1 and 3 are set once and left.
#define EXP_BIT_BL (1u << 1)    // backlight enable (set high at boot)
#define EXP_BIT_TP_RST (1u << 2)  // touch reset, active low
#define EXP_BIT_LCD (1u << 3)   // driven low at boot and left there
#define EXP_BIT_AMP (1u << 4)   // audio amp; high = muted/idle

// Backlight on, touch out of reset, amp idle. Bit 3 low to match Elecrow.
#define EXP_RUN_DEFAULT (EXP_BIT_BL | EXP_BIT_TP_RST | EXP_BIT_AMP)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exp;   // TCA9534, if present
static i2c_master_dev_handle_t s_stc8;  // STC8 dimmer, if present
static uint8_t s_exp_out = EXP_RUN_DEFAULT;

static esp_err_t exp_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 100);
}

static esp_err_t exp_apply(void)
{
    return s_exp != NULL ? exp_write(TCA9534_REG_OUTPUT, s_exp_out) : ESP_OK;
}

static esp_err_t exp_update(uint8_t set, uint8_t clear)
{
    s_exp_out = (uint8_t)((s_exp_out | set) & ~clear);
    return exp_apply();
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out);
}

esp_err_t board_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_GPIO_SDA,
        .scl_io_num = BOARD_I2C_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    // Which backlight controller is fitted? Probe both; absence is not fatal
    // because the panel is readable either way, only dimming is lost.
    const bool has_stc8 = i2c_master_probe(s_bus, STC8_ADDR, 100) == ESP_OK;
    const bool has_tca = i2c_master_probe(s_bus, TCA9534_ADDR, 100) == ESP_OK;
    ESP_LOGI(TAG, "expander probe: TCA9534@0x18 %s, STC8@0x30 %s",
             has_tca ? "yes" : "no", has_stc8 ? "yes" : "no");

    if (has_stc8) {
        ESP_RETURN_ON_ERROR(add_dev(STC8_ADDR, &s_stc8), TAG, "stc8 dev");
    }
    if (has_tca) {
        ESP_RETURN_ON_ERROR(add_dev(TCA9534_ADDR, &s_exp), TAG, "tca dev");
        // Outputs on the four lines we drive; the rest stay inputs.
        ESP_RETURN_ON_ERROR(
            exp_write(TCA9534_REG_CONFIG,
                      (uint8_t) ~(EXP_BIT_BL | EXP_BIT_TP_RST | EXP_BIT_LCD | EXP_BIT_AMP)),
            TAG, "tca config");
    }
    if (!has_tca && !has_stc8) {
        ESP_LOGE(TAG, "no IO expander found on I2C %d/%d - check the board revision",
                 BOARD_I2C_GPIO_SDA, BOARD_I2C_GPIO_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    // GT911 address latch: INT low across the reset pulse selects 0x5D.
    // The panel's backlight stays OFF here; app_main lights it once the first
    // frame exists, so nobody sees the boot garbage.
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_TP_IRQ,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "tp int");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_GPIO_TP_IRQ, 0), TAG, "tp int low");

    s_exp_out = (uint8_t)(EXP_RUN_DEFAULT & ~EXP_BIT_BL);  // backlight off
    ESP_RETURN_ON_ERROR(exp_update(0, EXP_BIT_TP_RST), TAG, "tp rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(exp_update(EXP_BIT_TP_RST, 0), TAG, "tp rst high");
    vTaskDelay(pdMS_TO_TICKS(50));

    // Release INT so the touch driver can read it as an input.
    const gpio_config_t int_in = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_TP_IRQ,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_in), TAG, "tp int in");

    ESP_LOGI(TAG, "board init done (I2C on %d/%d)", BOARD_I2C_GPIO_SDA,
             BOARD_I2C_GPIO_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

// On the STC8 revision this is a real brightness control, so "on" means full.
// screen_sleep can dim instead of switching, which is what keeps the backlight
// converter from power-cycling -- the white-halo trigger on the Waveshare 5B.
//
// The STC8 takes a brightness byte, and the board revisions define it OPPOSITE
// ways (Elecrow's own 5.0 wiki, re-checked 2026-09-03):
//   v1.1    0x05 is OFF, and up from there is brighter (see the ladder note).
//   v1.2+   0..245, where 0 is BRIGHTEST, 244 dimmest and 245 OFF.
// So v1.2's "on" (0) is out of range on v1.1, and a firmware built for v1.2
// NEVER LIGHTS A v1.1 PANEL: the board boots, LVGL runs, touch works, and the
// glass stays black -- which reads as a dead board, and did, to a customer on
// 2026-09-03. That is the whole of the v1.1 incompatibility; nothing else
// about this board is revision-dependent (Elecrow: v1.2 changed the buttons,
// v1.3 an FPC package; I/O pins unchanged throughout).
//
// Which revision a board is shows in the boot log's `expander probe:` line
// only for v1.0 (TCA9534/PCA9557 @ 0x18, no STC8); v1.1 and v1.2+ both answer
// at 0x30 and are indistinguishable over I2C -- so the code never asks, and
// nothing downstream (release spec, installer, settings) carries a revision.
//
// v1.1's own step list is ambiguous and BOTH readings have to be satisfied,
// because the one board we have to get right is a v1.1 we cannot test on.
// Elecrow writes the steps as "0x05, 0x06, 0x07, 0x08, 0x09 and 0x10" -- which
// skips 0x0A..0x0F, so either they mean hex and max is 22 decimal, or (far more
// likely) they counted 5,6,7,8,9,10 in decimal and prefixed every one with 0x.
// Six steps is what the third-party teardowns report too, which fits 5..10.
// So the legal set is {5,6,7,8,9,16} under one reading and {5,6,7,8,9,10}
// under the other, and their only shared lit values are 6..9.
//
// Hence LADDERS rather than single bytes, in both directions. Write ascending
// (or descending) through the candidates: the last write the firmware ACCEPTS
// wins, and one it does not recognise is ignored, so the panel lands on the
// right end under either encoding from ONE image. That is what keeps board
// revision out of the release spec, out of the installer, and out of settings.
//
//   ON   9 -> 10 -> 16   v1.1: 9, then max under whichever reading is real
//                        (9 -> 10 -> ignored, or 9 -> ignored -> 16), so never
//                        below 9. v1.2+: all legal, lands on 16, ~94% duty --
//                        invisible, and the price of one image.
//   OFF  5 -> 245        v1.1: 5 is OFF, 245 is out of range and ignored.
//                        v1.2+: 5 is ~98% bright for the ~1 ms until 245 lands
//                        and blanks it -- and screen_sleep has already drawn
//                        its black shield by then, so the flash is of a black
//                        frame. Order matters: 245 then 5 would end BRIGHT on
//                        v1.2+.
//
// Both ladders assume an unrecognised byte is ignored rather than acted on.
// The one place that could bite is OFF: v1.1 changed the buzzer/speaker
// commands and their values are undocumented, so if 245 collides with one, a
// v1.1 panel beeps when it sleeps. Harmless and obvious -- if a v1.1 owner
// reports that, split board_backlight into the two encodings behind a stored
// revision flag rather than weakening the ladders.
//   v1.1    0x05 off .. max per the reading above
//   v1.2+   0 brightest .. 244 dimmest, 245 off; 246/247 buzzer, 248/249 spk
static const uint8_t STC8_BL_ON_LADDER[] = {9, 10, 16};
static const uint8_t STC8_BL_OFF_LADDER[] = {5, 245};

static esp_err_t stc8_ladder(const uint8_t *bytes, size_t n)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < n; i++) {
        // Keep going on error: a NACK here is the likely signature of an
        // out-of-range byte, which is exactly the case the ladder exists for.
        const esp_err_t one = i2c_master_transmit(s_stc8, &bytes[i], 1, 100);
        if (one != ESP_OK) {
            err = one;
        }
    }
    return err;
}

esp_err_t board_backlight(bool on)
{
    if (s_stc8 != NULL) {
        return on ? stc8_ladder(STC8_BL_ON_LADDER, sizeof(STC8_BL_ON_LADDER))
                  : stc8_ladder(STC8_BL_OFF_LADDER, sizeof(STC8_BL_OFF_LADDER));
    }
    return exp_update(on ? EXP_BIT_BL : 0, on ? 0 : EXP_BIT_BL);
}

// Brightness, 0..255 where 0 is off. Only the STC8 revision can do this; on
// the TCA9534 it degrades to on/off at the midpoint. This is what lets sleep
// DIM rather than cut the backlight converter -- the mechanism behind the
// Waveshare 5B's white-halo artifact.
//
// Unlike on/off, DIMMING cannot be made revision-proof: the two encodings run
// opposite directions, and the bytes that are lit under both (6..9) are all
// ~97% brightness on v1.2+, so there is no shared range to dim across. This
// is therefore the v1.2+ scheme, and on a v1.1 board every level but the
// endpoints is out of range and ignored, leaving the backlight where
// board_backlight left it. Nothing calls this yet; wire it up only together
// with a stored revision flag.
esp_err_t board_backlight_level(uint8_t level)
{
    if (s_stc8 == NULL) {
        return board_backlight(level >= 128);
    }
    if (level == 0) {
        return board_backlight(false);
    }
    // Never brighter than the ON ladder's top, so a v1.1 board still gets a
    // byte it can act on at full level rather than a dark screen.
    const uint8_t val = (uint8_t)(245 - ((uint32_t)level * (245 - 16)) / 255);
    return i2c_master_transmit(s_stc8, &val, 1, 100);
}

// No panel reset line is broken out on this board -- Elecrow's LovyanGFX
// config leaves the RGB panel with no reset pin at all. A dumb RGB panel has
// no register state to lose, so this is a no-op rather than an error, and
// screen_sleep's UI_DEBUG_RGB_STOP experiment simply does nothing here.
esp_err_t board_lcd_reset(bool asserted)
{
    (void)asserted;
    return ESP_OK;
}

// Chip select is not wired to the ESP on this board (Elecrow's factory code
// passes a dummy). sdcard.c configures the sdspi device with no CS pin.
esp_err_t board_sd_select(bool selected)
{
    (void)selected;
    return ESP_OK;
}

#endif  // BOARD_CROWPANEL_ADV_5
