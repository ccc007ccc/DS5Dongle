#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "dual_chip_spi_proto.h"
#include "dualsense_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t tx_audio_rt;
    uint32_t tx_reports;
    uint32_t tx_feature_get;
    uint32_t tx_feature_set;
    uint32_t tx_bt_connect;
    uint32_t tx_bt_disconnect;
    uint32_t tx_reset_stats;
    uint32_t tx_hello;
    uint32_t tx_time_sync;
    uint32_t time_sync_failures;
    uint32_t recovery_attempts;
    uint32_t recovery_successes;
    uint32_t recovery_failures;
    uint32_t recovery_skipped_no_reset;
    uint32_t recovery_suppressed;
    uint32_t recovery_consecutive_errors;
    uint8_t last_recovery_reason;
    uint32_t last_recovery_local_us;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t rx_hello;
    uint32_t rx_time_sync;
    uint32_t rx_bt_state;
    uint32_t rx_flow_credit;
    uint32_t rx_stats;
    uint32_t tx_stats_request;
    uint32_t rx_ack;
    uint32_t tx_ack_polls;
    uint32_t ack_retries;
    uint32_t ack_failures;
    uint32_t ack_miss;
    uint32_t ack_status_errors;
    uint16_t last_ack_seq;
    uint8_t last_ack_type;
    int last_ack_status;
    uint8_t last_credit_free;
    uint8_t last_credit_depth;
    uint8_t last_credit_bt_ready;
    uint8_t peer_role;
    uint8_t peer_protocol_version;
    uint16_t peer_max_payload;
    uint16_t peer_spi_mtu;
    uint8_t peer_tx_queue_depth;
    uint32_t peer_capabilities;
    uint8_t time_sync_valid;
    uint32_t last_time_sync_rtt_us;
    uint32_t last_time_sync_local_us;
    int32_t esp_time_offset_us;
    uint32_t peer_bt_flags;
    int32_t peer_bt_last_error;
    int8_t peer_bt_rssi;
    uint8_t peer_bt_bringup_attempts;
    uint8_t peer_bt_reconnect_failures;
    uint16_t peer_bt_control_mtu;
    uint16_t peer_bt_interrupt_mtu;
    uint8_t peer_bt_bda[6];
    uint16_t peer_bt_state_seq;
    uint32_t peer_queue_drops;
    uint32_t peer_hidp_tx_errors;
    int peer_hidp_last_err;
    uint8_t peer_stats_role;
    uint8_t peer_stats_version;
    uint32_t peer_stats_uptime_us;
    uint32_t peer_stats_spi_rx_frames;
    uint32_t peer_stats_spi_tx_frames;
    uint32_t peer_stats_spi_crc_errors;
    uint32_t peer_stats_spi_queue_drops;
    uint32_t peer_stats_hidp_tx_31;
    uint32_t peer_stats_hidp_tx_32;
    uint32_t peer_stats_hidp_tx_36;
    uint32_t peer_stats_hidp_tx_feature_get;
    uint32_t peer_stats_hidp_tx_feature_set;
    uint32_t peer_stats_hidp_tx_errors;
    uint32_t peer_stats_deadline_miss_36;
    uint32_t peer_stats_hidp_rx_input;
    uint32_t peer_stats_hidp_rx_mic_opus;
    uint32_t peer_stats_hidp_rx_feature_report;
    uint32_t peer_stats_ack_tx;
    uint32_t peer_stats_ack_drops;
    uint32_t peer_stats_flow_credit_tx;
    uint32_t peer_stats_bt_connect_rx;
    uint32_t peer_stats_bt_disconnect_rx;
    uint32_t frame_errors;
    uint32_t crc_errors;
    uint32_t deadline_miss;
    uint32_t queue_drop_old;
    uint32_t not_ready;
    uint16_t last_seq;
    int last_error;
} m61_esp32_transport_stats_t;

typedef void (*m61_esp32_transport_input_cb_t)(
    const uint8_t *payload,
    size_t payload_len,
    const dualsense_state_t *state,
    const dualsense_parse_result_t *parse,
    void *ctx);
typedef void (*m61_esp32_transport_feature_cb_t)(uint8_t report_id,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 void *ctx);

void m61_esp32_transport_init(void);
bool m61_esp32_transport_ready(void);
bool m61_esp32_transport_bt_ready(void);
void m61_esp32_transport_set_input_callback(m61_esp32_transport_input_cb_t cb,
                                            void *ctx);
void m61_esp32_transport_set_feature_callback(m61_esp32_transport_feature_cb_t cb,
                                              void *ctx);
int m61_esp32_transport_send_bt_report(const uint8_t *report,
                                       size_t report_len,
                                       TickType_t deadline_tick,
                                       bool realtime);
int m61_esp32_transport_request_feature(uint8_t report_id, uint32_t requested_len);
int m61_esp32_transport_set_feature(uint8_t report_id, const uint8_t *data, size_t len);
int m61_esp32_transport_connect(const uint8_t *bda, size_t len);
int m61_esp32_transport_connect_mode(uint8_t mode);
int m61_esp32_transport_disconnect(bool allow_reconnect);
int m61_esp32_transport_forget(uint8_t flags);
int m61_esp32_transport_wire_test(void);
int m61_esp32_transport_request_stats(void);
void m61_esp32_transport_get_stats(m61_esp32_transport_stats_t *stats);
void m61_esp32_transport_reset_stats(void);
void m61_esp32_transport_print_stats(void);
void m61_esp32_transport_print_events(bool clear_after_print);

#ifdef __cplusplus
}
#endif
