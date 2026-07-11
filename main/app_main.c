#include "bt_dualsense_raw_hidp.h"
#include "bt_ds5_tx_scheduler.h"
#include "esp32_dual_chip_response_scheduler.h"
#include "esp32_dual_chip_spi.h"
#include "led_status.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "ds5_main";

static void log_runtime_health(void)
{
    ds5_sched_metrics_t metrics;
    ds5_sched_metrics_t response_metrics;
    esp32_dual_chip_spi_stats_t stats;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    bt_ds5_tx_scheduler_get_metrics(&metrics);
    esp32_dual_response_get_metrics(&response_metrics);
    esp32_dual_chip_spi_get_stats(&stats);
    ESP_LOGI(TAG,
             "runtime heap_free=%u heap_min=%u heap_largest=%u "
             "spi_rx=%u spi_tx=%u crc=%u frame=%u driver=%u seq=%u dup=%u "
             "short=%u queue_drop=%u "
             "rt_pending=%llu rt_tx=%llu rt_replace=%llu rt_stale=%llu "
             "rt_fail=%llu gap_max_us=%llu gap25=%llu gap40=%llu "
             "input_pending=%llu input_tx=%llu input_replace=%llu "
             "mic_pending=%llu control_pending=%llu",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)stats.spi_rx_frames,
             (unsigned)stats.spi_tx_frames,
             (unsigned)stats.spi_crc_errors,
             (unsigned)stats.spi_frame_errors,
             (unsigned)stats.spi_driver_errors,
             (unsigned)stats.spi_sequence_errors,
             (unsigned)stats.spi_duplicates,
             (unsigned)stats.spi_short_transactions,
             (unsigned)stats.spi_queue_drops,
             (unsigned long long)
                 metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].pending,
             (unsigned long long)
                 metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].transmitted,
             (unsigned long long)
                 metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].replaced,
             (unsigned long long)
                 metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].stale,
             (unsigned long long)
                 metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].transport_failed,
             (unsigned long long)metrics.realtime.gap_max_us,
             (unsigned long long)metrics.realtime.gap_over_25ms,
             (unsigned long long)metrics.realtime.gap_over_40ms,
             (unsigned long long)
                 response_metrics.classes[DS5_SCHED_INPUT_REALTIME - 1U].pending,
             (unsigned long long)
                 response_metrics.classes[DS5_SCHED_INPUT_REALTIME - 1U].transmitted,
             (unsigned long long)
                 response_metrics.classes[DS5_SCHED_INPUT_REALTIME - 1U].replaced,
             (unsigned long long)
                 response_metrics.classes[DS5_SCHED_MIC_STREAM - 1U].pending,
             (unsigned long long)
                 response_metrics.classes[DS5_SCHED_RELIABLE_CONTROL - 1U].pending);
}

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
        vTaskDelay(pdMS_TO_TICKS(30000));
        log_runtime_health();
    }
}
