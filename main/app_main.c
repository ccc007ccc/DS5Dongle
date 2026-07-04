#include "bt_dualsense_host.h"
#include "bt_dualsense_raw_hidp.h"
#include "led_status.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ds5_stage1";

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
    ESP_LOGI(TAG, "Stage 1 firmware: ESP32 Classic Bluetooth host for DualSense");
    ESP_LOGI(TAG, "Scope guard: BL618 USB and ESP32 UART forwarding are intentionally not built in this stage");

    ESP_ERROR_CHECK(led_status_init());
    led_status_set(DS5_LED_STATE_BOOT_OK);

    init_nvs();
#if CONFIG_DS5_BT_BACKEND_RAW_HIDP
    ESP_LOGI(TAG, "Bluetooth backend: raw Classic L2CAP HIDP");
    ESP_ERROR_CHECK(bt_dualsense_raw_hidp_start());
#else
    ESP_LOGI(TAG, "Bluetooth backend: ESP-IDF HID Host API");
    ESP_ERROR_CHECK(bt_dualsense_host_start());
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
