#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_DUAL_SPI_MAGIC 0x3544U
#define DS5_DUAL_SPI_VERSION 1U
#define DS5_DUAL_SPI_HEADER_LEN 20U
#define DS5_DUAL_SPI_MAX_PAYLOAD 512U
#define DS5_DUAL_HELLO_PAYLOAD_LEN 16U
#define DS5_DUAL_TIME_SYNC_PAYLOAD_LEN 16U
#define DS5_DUAL_BT_STATE_PAYLOAD_LEN 24U
#define DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN 16U
#define DS5_DUAL_ACK_PAYLOAD_LEN 8U
#define DS5_DUAL_STATS_PAYLOAD_LEN 84U

#define DS5_DUAL_ROLE_M61_USB 1U
#define DS5_DUAL_ROLE_ESP32_BT 2U

#define DS5_DUAL_CAP_USB_DS5_GADGET 0x00000001UL
#define DS5_DUAL_CAP_BT_HIDP_RAW 0x00000002UL
#define DS5_DUAL_CAP_AUDIO_RT 0x00000004UL
#define DS5_DUAL_CAP_FEATURE_REPORTS 0x00000008UL
#define DS5_DUAL_CAP_FLOW_CREDIT 0x00000010UL
#define DS5_DUAL_CAP_RELIABLE_ACK 0x00000020UL
#define DS5_DUAL_CAP_BT_CONNECT_MODES 0x00000040UL

#define DS5_DUAL_BT_STATE_READY            0x00000001UL
#define DS5_DUAL_BT_STATE_L2CAP_READY      0x00000002UL
#define DS5_DUAL_BT_STATE_SDP_READY        0x00000004UL
#define DS5_DUAL_BT_STATE_CONTROL_OPEN     0x00000008UL
#define DS5_DUAL_BT_STATE_INTERRUPT_OPEN   0x00000010UL
#define DS5_DUAL_BT_STATE_FULL_REPORT      0x00000020UL
#define DS5_DUAL_BT_STATE_CONNECTING       0x00000040UL
#define DS5_DUAL_BT_STATE_SDP_SEARCHING    0x00000080UL
#define DS5_DUAL_BT_STATE_TARGET_FOUND     0x00000100UL
#define DS5_DUAL_BT_STATE_HAVE_SAVED       0x00000200UL
#define DS5_DUAL_BT_STATE_FROM_SAVED       0x00000400UL

typedef enum {
    DS5_DUAL_MSG_HELLO = 1,
    DS5_DUAL_MSG_TIME_SYNC = 2,
    DS5_DUAL_MSG_BT_CONNECT = 3,
    DS5_DUAL_MSG_BT_DISCONNECT = 4,
    DS5_DUAL_MSG_BT_STATE = 5,
    DS5_DUAL_MSG_BT_RX_INPUT = 6,
    DS5_DUAL_MSG_BT_RX_MIC_OPUS = 7,
    DS5_DUAL_MSG_BT_TX_REPORT = 8,
    DS5_DUAL_MSG_BT_TX_AUDIO_RT = 9,
    DS5_DUAL_MSG_FLOW_CREDIT = 10,
    DS5_DUAL_MSG_STATS = 11,
    DS5_DUAL_MSG_RESET_STATS = 12,
    DS5_DUAL_MSG_BT_TX_FEATURE_GET = 13,
    DS5_DUAL_MSG_BT_TX_FEATURE_SET = 14,
    DS5_DUAL_MSG_BT_RX_FEATURE_REPORT = 15,
    DS5_DUAL_MSG_WIRE_TEST = 16,
    DS5_DUAL_MSG_BT_FORGET = 17,
} ds5_dual_msg_type_t;

typedef enum {
    DS5_DUAL_CHANNEL_CTRL = 1,
    DS5_DUAL_CHANNEL_INPUT = 2,
    DS5_DUAL_CHANNEL_OUTPUT = 3,
    DS5_DUAL_CHANNEL_AUDIO = 4,
    DS5_DUAL_CHANNEL_STATUS = 5,
    DS5_DUAL_CHANNEL_LOG = 6,
} ds5_dual_channel_t;

typedef enum {
    DS5_DUAL_PRIORITY_RT = 1,
    DS5_DUAL_PRIORITY_AUDIO = 2,
    DS5_DUAL_PRIORITY_HID = 3,
    DS5_DUAL_PRIORITY_CONTROL = 4,
    DS5_DUAL_PRIORITY_LOW = 5,
} ds5_dual_priority_t;

#define DS5_DUAL_FLAG_RELIABLE 0x0001U
#define DS5_DUAL_FLAG_LATEST   0x0002U
#define DS5_DUAL_FLAG_DROP_OK  0x0004U
#define DS5_DUAL_FLAG_ACK      0x0008U
#define DS5_DUAL_FLAG_HIDP_CONTROL 0x0010U

#define DS5_DUAL_WIRE_TEST_START 1U
#define DS5_DUAL_WIRE_TEST_PASS 2U
#define DS5_DUAL_WIRE_TEST_FAIL 3U

#define DS5_DUAL_FORGET_SAVED_ADDR 0x01U
#define DS5_DUAL_FORGET_BONDS      0x02U

#define DS5_DUAL_BT_CONNECT_AUTO       0x00U
#define DS5_DUAL_BT_CONNECT_SCAN_ONLY  0x01U
#define DS5_DUAL_BT_CONNECT_SAVED_ONLY 0x02U

typedef struct {
    uint8_t type;
    uint16_t flags;
    uint8_t channel;
    uint8_t priority;
    uint16_t seq;
    uint32_t deadline;
    uint16_t length;
    uint32_t crc32;
} ds5_dual_spi_header_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t role;
    uint16_t header_len;
    uint16_t max_payload;
    uint16_t spi_mtu;
    uint8_t tx_queue_depth;
    uint32_t capabilities;
} ds5_dual_hello_t;

typedef struct {
    uint32_t m61_time_us;
    uint32_t esp_time_us;
    uint32_t roundtrip_us;
    int32_t offset_us;
} ds5_dual_time_sync_t;

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
} ds5_dual_bt_state_t;

typedef struct {
    uint8_t tx_queue_free;
    uint8_t tx_queue_depth;
    bool bt_ready;
    uint32_t spi_queue_drops;
    uint32_t hidp_tx_errors;
    int32_t hidp_last_err;
} ds5_dual_flow_credit_t;

typedef struct {
    uint16_t seq;
    uint8_t type;
    int32_t status;
} ds5_dual_ack_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t role;
    uint32_t uptime_us;
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
    uint32_t deadline_miss_36;
    uint32_t hidp_rx_input;
    uint32_t hidp_rx_mic_opus;
    uint32_t hidp_rx_feature_report;
    uint32_t ack_tx;
    uint32_t ack_drops;
    uint32_t flow_credit_tx;
    uint32_t bt_connect_rx;
    uint32_t bt_disconnect_rx;
} ds5_dual_stats_t;

uint32_t ds5_dual_spi_crc32(const uint8_t *data, size_t len);
void ds5_dual_spi_header_init(ds5_dual_spi_header_t *header,
                              uint8_t type,
                              uint16_t flags,
                              uint8_t channel,
                              uint8_t priority,
                              uint16_t seq,
                              uint32_t deadline,
                              uint16_t length);
bool ds5_dual_spi_encode_header(const ds5_dual_spi_header_t *header,
                                uint8_t *out,
                                size_t out_len);
bool ds5_dual_spi_decode_header(const uint8_t *data,
                                size_t len,
                                ds5_dual_spi_header_t *header);
bool ds5_dual_spi_finalize_frame(uint8_t *frame,
                                 size_t frame_len,
                                 const ds5_dual_spi_header_t *header,
                                 const uint8_t *payload);
bool ds5_dual_spi_validate_frame(const uint8_t *frame,
                                 size_t frame_len,
                                 ds5_dual_spi_header_t *header);
size_t ds5_dual_spi_frame_len(uint16_t payload_len);
bool ds5_dual_spi_report_is_audio_rt(const uint8_t *report, size_t len);
bool ds5_dual_hello_encode(const ds5_dual_hello_t *hello,
                           uint8_t *out,
                           size_t out_len);
bool ds5_dual_hello_decode(const uint8_t *data,
                           size_t len,
                           ds5_dual_hello_t *hello);
bool ds5_dual_time_sync_encode(const ds5_dual_time_sync_t *sync,
                               uint8_t *out,
                               size_t out_len);
bool ds5_dual_time_sync_decode(const uint8_t *data,
                               size_t len,
                               ds5_dual_time_sync_t *sync);
bool ds5_dual_bt_state_encode(const ds5_dual_bt_state_t *state,
                              uint8_t *out,
                              size_t out_len);
bool ds5_dual_bt_state_decode(const uint8_t *data,
                              size_t len,
                              ds5_dual_bt_state_t *state);
bool ds5_dual_flow_credit_encode(const ds5_dual_flow_credit_t *credit,
                                  uint8_t *out,
                                  size_t out_len);
bool ds5_dual_flow_credit_decode(const uint8_t *data,
                                  size_t len,
                                  ds5_dual_flow_credit_t *credit);
bool ds5_dual_ack_encode(const ds5_dual_ack_t *ack,
                         uint8_t *out,
                         size_t out_len);
bool ds5_dual_ack_decode(const uint8_t *data,
                         size_t len,
                         ds5_dual_ack_t *ack);
bool ds5_dual_stats_encode(const ds5_dual_stats_t *stats,
                           uint8_t *out,
                           size_t out_len);
bool ds5_dual_stats_decode(const uint8_t *data,
                           size_t len,
                           ds5_dual_stats_t *stats);
const char *ds5_dual_spi_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
