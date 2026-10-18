#include "led.h"
#include "board_pins.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "led";
static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

void led_init(void)
{
    // RMT TX channel at 10 MHz (100 ns per tick)
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = PIN_LED_STRIP,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_chan));

    // WS2812 bit timings:
    //   bit 0: 0.3 us high, 0.9 us low  -> 3 ticks / 9 ticks
    //   bit 1: 0.9 us high, 0.3 us low  -> 9 ticks / 3 ticks
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0 = 1, .duration0 = 3,
            .level1 = 0, .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1, .duration0 = 9,
            .level1 = 0, .duration1 = 3,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_chan));
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d", PIN_LED_STRIP);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_chan) return;
    uint8_t grb[3] = { g, r, b };  // WS2812 expects GRB order
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transmit failed: %s", esp_err_to_name(err));
    }
    // Fire-and-forget: no need to block waiting for a single status LED.
}
