#include "esp_log.h"
#include "nvs_flash.h"
#include "display.h"
#include "touch.h"
#include "led.h"
#include "net.h"
#include "csi_sense.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    display_init();
    display_splash("CSI MOTION SENSE", "BOOTING...");
    led_init();
    touch_init();

    display_splash("CSI MOTION SENSE", "WIFI CONNECTING...");
    net_init();          // Wi-Fi STA init + start (connect happens async)
    csi_sense_init();    // sense task + queue + NVS calibration load
    csi_sense_enable();  // enable CSI now that Wi-Fi is initialized

    display_start_ui();  // 10 fps UI task: screen + touch + status LED
    ESP_LOGI(TAG, "up and running");
}
