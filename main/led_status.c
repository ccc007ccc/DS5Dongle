#include "led_status.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_DS5_LED_GREEN_GPIO
#define CONFIG_DS5_LED_GREEN_GPIO -1
#endif

#ifndef CONFIG_DS5_LED_BLUE_GPIO
#define CONFIG_DS5_LED_BLUE_GPIO 2
#endif

#ifndef CONFIG_DS5_LED_RED_GPIO
#define CONFIG_DS5_LED_RED_GPIO -1
#endif

#ifdef CONFIG_DS5_LED_ACTIVE_LOW
#define DS5_LED_ACTIVE_LEVEL 0
#else
#define DS5_LED_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_DS5_LED_BLINK_MS
#define CONFIG_DS5_LED_BLINK_MS 250
#endif

#define DS5_LED_DISABLED_GPIO (-1)

static const char *TAG = "ds5_led";

static volatile ds5_led_state_t s_led_state = DS5_LED_STATE_BOOT_OK;
static bool s_led_task_started;

static bool gpio_enabled(int gpio)
{
    return gpio >= 0;
}

static void write_led(int gpio, bool on)
{
    if (!gpio_enabled(gpio)) {
        return;
    }

    int level = on ? DS5_LED_ACTIVE_LEVEL : (1 - DS5_LED_ACTIVE_LEVEL);
    gpio_set_level((gpio_num_t) gpio, level);
}

static esp_err_t configure_led_gpio(int gpio, const char *name)
{
    if (!gpio_enabled(gpio)) {
        ESP_LOGI(TAG, "%s LED disabled; set its GPIO in menuconfig if the board has one", name);
        return ESP_OK;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure %s LED GPIO%d: %s",
                 name, gpio, esp_err_to_name(err));
        return err;
    }

    write_led(gpio, false);
    ESP_LOGI(TAG, "%s LED GPIO%d active_%s", name, gpio,
             DS5_LED_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

static void apply_led_state(ds5_led_state_t state, bool blink_on)
{
    switch (state) {
    case DS5_LED_STATE_BOOT_OK:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, true);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, false);
        break;
    case DS5_LED_STATE_BT_CONNECTING:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, false);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, blink_on);
        break;
    case DS5_LED_STATE_BT_CONNECTED:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, false);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, true);
        break;
    case DS5_LED_STATE_WIRE_TESTING:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, false);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, blink_on);
        break;
    case DS5_LED_STATE_WIRE_TEST_PASS:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, true);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, true);
        break;
    case DS5_LED_STATE_WIRE_TEST_FAIL:
        write_led(CONFIG_DS5_LED_RED_GPIO, true);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, false);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, blink_on);
        break;
    default:
        write_led(CONFIG_DS5_LED_RED_GPIO, false);
        write_led(CONFIG_DS5_LED_GREEN_GPIO, false);
        write_led(CONFIG_DS5_LED_BLUE_GPIO, false);
        break;
    }
}

static void led_status_task(void *arg)
{
    (void) arg;

    bool blink_on = true;
    while (true) {
        ds5_led_state_t state = s_led_state;
        apply_led_state(state, blink_on);
        if (state == DS5_LED_STATE_BT_CONNECTING ||
            state == DS5_LED_STATE_WIRE_TESTING ||
            state == DS5_LED_STATE_WIRE_TEST_FAIL) {
            blink_on = !blink_on;
        } else {
            blink_on = true;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DS5_LED_BLINK_MS));
    }
}

esp_err_t led_status_init(void)
{
    esp_err_t err = configure_led_gpio(CONFIG_DS5_LED_RED_GPIO, "red");
    if (err != ESP_OK) {
        return err;
    }

    err = configure_led_gpio(CONFIG_DS5_LED_GREEN_GPIO, "green");
    if (err != ESP_OK) {
        return err;
    }

    err = configure_led_gpio(CONFIG_DS5_LED_BLUE_GPIO, "blue");
    if (err != ESP_OK) {
        return err;
    }

    if (!gpio_enabled(CONFIG_DS5_LED_GREEN_GPIO)) {
        ESP_LOGW(TAG, "Boot-ok green LED is not visible because DS5_LED_GREEN_GPIO is disabled");
    }
    if (!gpio_enabled(CONFIG_DS5_LED_RED_GPIO)) {
        ESP_LOGI(TAG, "Red LED is not MCU-controlled; if it is a power LED it will stay on");
    }

    apply_led_state(s_led_state, true);

    if (!s_led_task_started &&
        (gpio_enabled(CONFIG_DS5_LED_RED_GPIO) ||
         gpio_enabled(CONFIG_DS5_LED_GREEN_GPIO) ||
         gpio_enabled(CONFIG_DS5_LED_BLUE_GPIO))) {
        BaseType_t ok = xTaskCreate(led_status_task, "ds5_led", 2048, NULL,
                                    tskIDLE_PRIORITY + 1, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create LED task");
            return ESP_ERR_NO_MEM;
        }
        s_led_task_started = true;
    }

    return ESP_OK;
}

void led_status_set(ds5_led_state_t state)
{
    s_led_state = state;
    apply_led_state(state, true);
}
