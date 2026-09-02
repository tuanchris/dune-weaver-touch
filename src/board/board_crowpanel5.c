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
// The STC8 takes a brightness byte, and the two board revisions define it
// OPPOSITE ways (Elecrow's wiki):
//   v1.1    0x05..0x10, where 0x05 is OFF and 0x10 is maximum
//   v1.2+   0..245,     where 0 is BRIGHTEST and 245 is OFF
// Getting this backwards does not fail loudly -- it just never turns the
// backlight off, which looks like "the screensaver leaves the screen on".
// Default is v1.2+; flip STC8_BL_V11 if the panel does not go dark on sleep.
#define STC8_BL_V11 0

#if STC8_BL_V11
#define STC8_BL_OFF 0x05
#define STC8_BL_MAX 0x10
#else
#define STC8_BL_OFF 245
#define STC8_BL_MAX 0
#endif

esp_err_t board_backlight(bool on)
{
    if (s_stc8 != NULL) {
        const uint8_t val = on ? STC8_BL_MAX : STC8_BL_OFF;
        return i2c_master_transmit(s_stc8, &val, 1, 100);
    }
    return exp_update(on ? EXP_BIT_BL : 0, on ? 0 : EXP_BIT_BL);
}

// Brightness, 0..255 where 0 is off. Only the STC8 revision can do this; on
// the TCA9534 it degrades to on/off at the midpoint. This is what lets sleep
// DIM rather than cut the backlight converter -- the mechanism behind the
// Waveshare 5B's white-halo artifact.
esp_err_t board_backlight_level(uint8_t level)
{
    if (s_stc8 == NULL) {
        return board_backlight(level >= 128);
    }
#if STC8_BL_V11
    const uint8_t val = level == 0 ? 0x05 : (uint8_t)(0x05 + (level * 11) / 255);
#else
    const uint8_t val = (uint8_t)(245 - (level * 245) / 255);
#endif
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
