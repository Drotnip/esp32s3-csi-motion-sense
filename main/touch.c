#include "touch.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;

void touch_init(void)
{
    // FT6336G reset pulse: HIGH 20 ms -> LOW 20 ms -> HIGH, then wait 500 ms.
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&gc);
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &s_dev));

    uint8_t reg = 0x02, val = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 50) == ESP_OK) {
        ESP_LOGI(TAG, "FT6336G touch controller found");
    } else {
        ESP_LOGW(TAG, "touch controller probe failed - touch button disabled");
    }
}

bool touch_read(int *x, int *y)
{
    uint8_t reg = 0x02, cnt = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &cnt, 1, 50) != ESP_OK) return false;
    if ((cnt & 0x0F) == 0) return false;
    uint8_t r = 0x03, d[4];
    if (i2c_master_transmit_receive(s_dev, &r, 1, d, 4, 50) != ESP_OK) return false;
    *x = ((d[0] & 0x0F) << 8) | d[1];
    *y = ((d[2] & 0x0F) << 8) | d[3];
    return true;
}
