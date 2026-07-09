#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_LED_STATE_BOOT_OK = 0,
    DS5_LED_STATE_BT_CONNECTING,
    DS5_LED_STATE_BT_CONNECTED,
    DS5_LED_STATE_WIRE_TESTING,
    DS5_LED_STATE_WIRE_TEST_PASS,
    DS5_LED_STATE_WIRE_TEST_FAIL,
} ds5_led_state_t;

esp_err_t led_status_init(void);
void led_status_set(ds5_led_state_t state);
/* Show `state` for `timeout_ms`, then fall back to the last led_status_set()
 * state. Used for wire-test results so they don't mask the BT status forever. */
void led_status_set_transient(ds5_led_state_t state, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
