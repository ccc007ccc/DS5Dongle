#include "bt_dualsense_raw_hidp.h"
#include "esp32_dual_chip_spi.h"
#include "led_status.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ds5_main";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "DS5Dongle ESP32: BTstack DualSense host + SPI coprocessor");

    ESP_ERROR_CHECK(led_status_init());
    led_status_set(DS5_LED_STATE_BOOT_OK);

    init_nvs();
    ESP_ERROR_CHECK(bt_dualsense_raw_hidp_start());
    ESP_ERROR_CHECK(esp32_dual_chip_spi_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
