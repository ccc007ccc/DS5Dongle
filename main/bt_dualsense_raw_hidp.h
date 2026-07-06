#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bt_dualsense_raw_hidp_rx_cb_t)(const uint8_t *data,
                                              size_t len,
                                              int64_t timestamp_us,
                                              void *ctx);

typedef struct {
    uint32_t flags;
    int32_t last_error;
    int8_t rssi;
    uint8_t bringup_attempts;
    uint8_t reconnect_failures;
    uint16_t control_mtu;
    uint16_t interrupt_mtu;
    uint8_t bda[6];
    uint16_t state_seq;
} bt_dualsense_raw_hidp_state_t;

typedef void (*bt_dualsense_raw_hidp_state_cb_t)(
    const bt_dualsense_raw_hidp_state_t *state,
    int64_t timestamp_us,
    void *ctx);

esp_err_t bt_dualsense_raw_hidp_start(void);
bool bt_dualsense_raw_hidp_ready(void);
void bt_dualsense_raw_hidp_get_state(bt_dualsense_raw_hidp_state_t *state);
int bt_dualsense_raw_hidp_connect(const uint8_t *bda, size_t len, uint8_t mode);
int bt_dualsense_raw_hidp_disconnect(bool allow_reconnect);
int bt_dualsense_raw_hidp_forget(uint8_t flags);
int bt_dualsense_raw_hidp_send_report(const uint8_t *report,
                                      size_t report_len,
                                      bool realtime);
int bt_dualsense_raw_hidp_get_feature(uint8_t report_id);
int bt_dualsense_raw_hidp_set_feature(uint8_t report_id,
                                      const uint8_t *data,
                                      size_t len);
void bt_dualsense_raw_hidp_set_rx_callback(bt_dualsense_raw_hidp_rx_cb_t cb,
                                           void *ctx);
void bt_dualsense_raw_hidp_set_state_callback(bt_dualsense_raw_hidp_state_cb_t cb,
                                              void *ctx);

#ifdef __cplusplus
}
#endif
