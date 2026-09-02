#include "board.h"

#if !defined(BOARD_CROWPANEL_ADV_5)  // board_crowpanel5.c covers that board

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// CH422G has no register pointer: each function is its own I2C address.
#define CH422G_ADDR_MODE 0x24  // system parameter register (0x01 = all EXIO output)
#define CH422G_ADDR_EXIO 0x38  // EXIO0..7 output latch

// EXIO bits: 1=TP_RST, 2=DISP (backlight), 3=LCD_RST, 4=SD_CS (active low)
#define EXIO_BIT_TP_RST 0x02
#define EXIO_BIT_DISP 0x04
#define EXIO_BIT_LCD_RST 0x08
#define EXIO_BIT_SD_CS 0x10
// Run state: everything released, SD deselected, backlight OFF — app_main
// lights it once the first frame is built.
#define EXIO_RUN_DEFAULT (EXIO_BIT_TP_RST | EXIO_BIT_LCD_RST | EXIO_BIT_SD_CS)
// Waveshare's demo values (0x2C/0x2E) with DISP cleared. R2 (4.7K to 5V) holds
// the AP3032's CTRL high from the instant power is applied, so the panel is lit
// through the whole un-driven window: RGB timing does not start until
// display_init, and an unaddressed cell under the backlight is a violet wash
// with every non-uniformity it owns on show. Setting DISP here made app_main's
// "first frame is built; light the panel" a no-op and put ~3 s of that wash on
// every boot and reset — it is now bounded by the time to the first I2C write.
#define EXIO_TP_RESET_LOW 0x28
#define EXIO_TP_RESET_HIGH 0x2A

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_ch422g_mode;
static i2c_master_dev_handle_t s_ch422g_exio;
// Current run-state latch value; mutated only through exio_update() so the
// backlight and SD_CS bits don't clobber each other.
static uint8_t s_exio = EXIO_RUN_DEFAULT;

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, 1000);
}

static esp_err_t ch422g_set_exio(uint8_t value)
{
    esp_err_t err = ch422g_write(s_ch422g_mode, 0x01);
    if (err != ESP_OK) {
        return err;
    }
    return ch422g_write(s_ch422g_exio, value);
}

// GT911 samples its INT line at reset release to pick its I2C address; holding
// INT low selects 0x5D, which is what esp_lcd_touch_gt911 expects by default.
static esp_err_t touch_reset(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_GPIO_TP_IRQ,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "tp irq gpio");

    ESP_RETURN_ON_ERROR(ch422g_set_exio(EXIO_TP_RESET_LOW), TAG, "tp rst low");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_GPIO_TP_IRQ, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(ch422g_set_exio(EXIO_TP_RESET_HIGH), TAG, "tp rst high");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
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

    const i2c_device_config_t mode_cfg = {
        .device_address = CH422G_ADDR_MODE,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &mode_cfg, &s_ch422g_mode), TAG, "ch422g mode dev");
    const i2c_device_config_t exio_cfg = {
        .device_address = CH422G_ADDR_EXIO,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &exio_cfg, &s_ch422g_exio), TAG, "ch422g exio dev");

    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "touch reset");
    ESP_LOGI(TAG, "board init done (I2C on %d/%d, CH422G ok)", BOARD_I2C_GPIO_SDA, BOARD_I2C_GPIO_SCL);
    return ESP_OK;
}

static esp_err_t exio_update(uint8_t set, uint8_t clear)
{
    s_exio = (uint8_t)((s_exio | set) & ~clear);
    return ch422g_set_exio(s_exio);
}

// Panel reset (EXIO3, active low). Asserting it holds the LCD's own timing
// controller and its source/gate drivers in reset, so the liquid crystal sees
// NO drive — the RGB peripheral keeps clocking pixels at a panel that ignores
// them, harmlessly. This is the closest software equivalent of pulling power,
// which is so far the only thing measured to clear retention on this panel:
// actively scanning black is not the same as unpowered, because an imperfect
// Vcom trim leaves a small DC offset on every frame.
esp_err_t board_lcd_reset(bool asserted)
{
    return exio_update(asserted ? 0 : EXIO_BIT_LCD_RST, asserted ? EXIO_BIT_LCD_RST : 0);
}

esp_err_t board_backlight(bool on)
{
    return exio_update(on ? EXIO_BIT_DISP : 0, on ? 0 : EXIO_BIT_DISP);
}

esp_err_t board_sd_select(bool selected)
{
    // SD is alone on its SPI bus, so CS can stay asserted for the whole
    // session (the sdspi driver runs with no CS pin of its own).
    return exio_update(selected ? 0 : EXIO_BIT_SD_CS, selected ? EXIO_BIT_SD_CS : 0);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

#endif  // !BOARD_CROWPANEL_ADV_5
