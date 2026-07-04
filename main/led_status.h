#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_LED_STATE_BOOT_OK = 0,
    DS5_LED_STATE_BT_CONNECTING,
    DS5_LED_STATE_BT_CONNECTED,
} ds5_led_state_t;

esp_err_t led_status_init(void);
void led_status_set(ds5_led_state_t state);

#ifdef __cplusplus
}
#endif
