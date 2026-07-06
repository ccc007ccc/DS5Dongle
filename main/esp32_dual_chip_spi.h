#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t spi_rx_frames;
    uint32_t spi_tx_frames;
    uint32_t spi_crc_errors;
    uint32_t spi_queue_drops;
    uint32_t hidp_tx_31;
    uint32_t hidp_tx_32;
    uint32_t hidp_tx_36;
    uint32_t hidp_tx_feature_get;
    uint32_t hidp_tx_feature_set;
    uint32_t hidp_tx_errors;
    int hidp_last_err;
    uint32_t hidp_rx_input;
    uint32_t hidp_rx_mic_opus;
    uint32_t hidp_rx_feature_report;
    uint32_t bt_state_tx;
    uint32_t bt_state_drops;
    uint32_t hello_rx;
    uint32_t hello_tx;
    uint32_t time_sync_rx;
    uint32_t time_sync_tx;
    uint32_t bt_connect_rx;
    uint32_t bt_disconnect_rx;
    uint32_t stats_rx;
    uint32_t stats_tx;
    uint32_t stats_drops;
    uint8_t peer_role;
    uint8_t peer_protocol_version;
    uint16_t peer_max_payload;
    uint16_t peer_spi_mtu;
    uint8_t peer_tx_queue_depth;
    uint32_t peer_capabilities;
    uint32_t flow_credit_tx;
    uint32_t ack_tx;
    uint32_t ack_drops;
    uint32_t deadline_miss_36;
} esp32_dual_chip_spi_stats_t;

esp_err_t esp32_dual_chip_spi_start(void);
esp_err_t esp32_dual_chip_spi_handle_frame(const uint8_t *frame, size_t frame_len);
void esp32_dual_chip_spi_get_stats(esp32_dual_chip_spi_stats_t *stats);
void esp32_dual_chip_spi_reset_stats(void);

#ifdef __cplusplus
}
#endif
