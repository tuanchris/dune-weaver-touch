// Elecrow CrowPanel 7.0-HMI board support -- the ORIGINAL 7" (DIS07070H),
// not the Advance series and not the Waveshare 7. Compiled only for
// -DBOARD_CROWPANEL_7; board.c (Waveshare) and board_crowpanel5.c (Advance)
// are #if'd out for this env.
//
// Sources: Elecrow's wiki page for the board and its
// CrowPanel-7.0-HMI-ESP32-Display-800x480 examples for the V1.0, V2.0 and
// V3.0 revisions, which agree on everything used here.
//
// What is different from every other board in this tree:
//   * There is NO IO expander. Backlight is GPIO2 straight into the boost
//     driver's enable, so it is PWM'd by LEDC and can genuinely dim.
//   * GT911 INT and RST reach nothing (all three revisions' examples pass -1
//     for both), so there is no address latch: display.c probes 0x5D/0x14.
//   * I2C lives on GPIO19/20, the S3's USB pins. The USB-Serial-JTAG
//     peripheral is disabled for this env (platformio.ini), which is what
//     makes IDF release the USB pad at startup; the console is UART0 through
//     the onboard CH340.
//   * The TF card's chip select IS a GPIO (10). sdcard.c hands it to the
//     sdspi driver, and board_sd_select has nothing to do.
//   * 4 MB flash (N4R8 module), hence partitions-4mb.csv.
#include "board.h"

#if defined(BOARD_CROWPANEL_7)

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

// LEDC for the backlight enable. 1 kHz rather than Elecrow's 300 Hz: still
// trivially inside any boost driver's PWM-dimming range, and further from the
// ~33 Hz refresh and its harmonics when sleep dims the panel. 10-bit so the
// 0..255 API maps without visible steps; duty 1024 is a solid high.
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MODE LEDC_LOW_SPEED_MODE
#define BL_FREQ_HZ 1000
#define BL_RES LEDC_TIMER_10_BIT
#define BL_DUTY_MAX (1u << 10)

static i2c_master_bus_handle_t s_bus;

static esp_err_t bl_set_duty(uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_MODE, BL_CHANNEL, duty), TAG, "bl duty");
    return ledc_update_duty(BL_MODE, BL_CHANNEL);
}

esp_err_t board_init(void)
{
    // Backlight first and OFF: app_main lights it once the first frame exists,
    // so nobody sees the un-driven panel. The pin is high-Z out of reset and
    // the boost stays disabled until something drives it, so unlike the
    // Waveshare 5B there is no lit window to shorten here.
    const ledc_timer_config_t tcfg = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "bl timer");
    const ledc_channel_config_t ccfg = {
        .gpio_num = BOARD_GPIO_BACKLIGHT,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "bl channel");

    // Touch controller bus. Internal pull-ups on top of the board's own: the
    // USB pad is already released by startup (see the file comment), so
    // nothing else loads these two pins.
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_GPIO_SDA,
        .scl_io_num = BOARD_I2C_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    // No touch reset to run: neither line is wired. Report what answers so a
    // boot log tells the revision story without a scope.
    const bool at_5d = i2c_master_probe(s_bus, 0x5D, 50) == ESP_OK;
    const bool at_14 = !at_5d && i2c_master_probe(s_bus, 0x14, 50) == ESP_OK;
    ESP_LOGI(TAG, "board init done (I2C on %d/%d, GT911 %s, backlight LEDC on %d)",
             BOARD_I2C_GPIO_SDA, BOARD_I2C_GPIO_SCL,
             at_5d ? "at 0x5D" : at_14 ? "at 0x14" : "NOT FOUND",
             BOARD_GPIO_BACKLIGHT);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

#ifdef UI_DEBUG_SLEEP_CYCLE
// Read the pad back: with the input buffer enabled the level the LEDC output
// actually drives is visible, so this is a measurement, not a register echo.
static void bl_sample(const char *what)
{
    gpio_input_enable(BOARD_GPIO_BACKLIGHT);
    int high = 0;
    for (int i = 0; i < 20; i++) {
        high += gpio_get_level(BOARD_GPIO_BACKLIGHT);
        esp_rom_delay_us(100);  // 2 ms of samples spans two 1 kHz periods
    }
    ESP_LOGI(TAG, "DEBUG: backlight %s -> GPIO%d high %d/20, ledc duty %u",
             what, BOARD_GPIO_BACKLIGHT, high, (unsigned)ledc_get_duty(BL_MODE, BL_CHANNEL));
}
#endif

esp_err_t board_backlight(bool on)
{
    esp_err_t err = bl_set_duty(on ? BL_DUTY_MAX : 0);
#ifdef UI_DEBUG_SLEEP_CYCLE
    bl_sample(on ? "ON" : "OFF");
#endif
    return err;
}

// Brightness, 0..255 where 0 is off and 255 is a solid high. A real dimmer,
// so sleep can dim instead of cutting the converter -- the structural answer
// to the Waveshare 5B's white-halo mechanism, same as the Advance's STC8.
esp_err_t board_backlight_level(uint8_t level)
{
    return bl_set_duty(((uint32_t)level * BL_DUTY_MAX) / 255);
}

// No panel reset line is broken out; a dumb RGB panel has no register state
// to lose, so this is a no-op rather than an error (screen_sleep's
// UI_DEBUG_RGB_STOP experiment does nothing here).
esp_err_t board_lcd_reset(bool asserted)
{
    (void)asserted;
    return ESP_OK;
}

// CS is a GPIO the sdspi driver owns (BOARD_SD_GPIO_CS); nothing to do.
esp_err_t board_sd_select(bool selected)
{
    (void)selected;
    return ESP_OK;
}

#endif  // BOARD_CROWPANEL_7
