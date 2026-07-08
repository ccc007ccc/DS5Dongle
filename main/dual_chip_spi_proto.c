#include "dual_chip_spi_proto.h"

#include <string.h>

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

uint32_t ds5_dual_spi_crc32(const uint8_t *data, size_t len)
{
    if (data == NULL && len != 0) {
        return 0;
    }
    return ~crc32_update(0xFFFFFFFFU, data, len);
}

void ds5_dual_spi_header_init(ds5_dual_spi_header_t *header,
                              uint8_t type,
                              uint16_t flags,
                              uint8_t channel,
                              uint8_t priority,
                              uint16_t seq,
                              uint32_t deadline,
                              uint16_t length)
{
    if (header == NULL) {
        return;
    }

    memset(header, 0, sizeof(*header));
    header->type = type;
    header->flags = flags;
    header->channel = channel;
    header->priority = priority;
    header->seq = seq;
    header->deadline = deadline;
    header->length = length;
}

bool ds5_dual_spi_encode_header(const ds5_dual_spi_header_t *header,
                                uint8_t *out,
                                size_t out_len)
{
    if (header == NULL || out == NULL || out_len < DS5_DUAL_SPI_HEADER_LEN ||
        header->length > DS5_DUAL_SPI_MAX_PAYLOAD) {
        return false;
    }

    put_u16_le(out + 0, DS5_DUAL_SPI_MAGIC);
    out[2] = DS5_DUAL_SPI_VERSION;
    out[3] = header->type;
    put_u16_le(out + 4, header->flags);
    out[6] = header->channel;
    out[7] = header->priority;
    put_u16_le(out + 8, header->seq);
    put_u32_le(out + 10, header->deadline);
    put_u16_le(out + 14, header->length);
    put_u32_le(out + 16, header->crc32);
    return true;
}

bool ds5_dual_spi_decode_header(const uint8_t *data,
                                size_t len,
                                ds5_dual_spi_header_t *header)
{
    if (data == NULL || header == NULL || len < DS5_DUAL_SPI_HEADER_LEN) {
        return false;
    }
    if (get_u16_le(data + 0) != DS5_DUAL_SPI_MAGIC ||
        data[2] != DS5_DUAL_SPI_VERSION) {
        return false;
    }

    header->type = data[3];
    header->flags = get_u16_le(data + 4);
    header->channel = data[6];
    header->priority = data[7];
    header->seq = get_u16_le(data + 8);
    header->deadline = get_u32_le(data + 10);
    header->length = get_u16_le(data + 14);
    header->crc32 = get_u32_le(data + 16);
    return header->length <= DS5_DUAL_SPI_MAX_PAYLOAD;
}

bool ds5_dual_spi_finalize_frame(uint8_t *frame,
                                 size_t frame_len,
                                 const ds5_dual_spi_header_t *header,
                                 const uint8_t *payload)
{
    ds5_dual_spi_header_t tmp;
    uint32_t crc;

    if (frame == NULL || header == NULL ||
        frame_len < ds5_dual_spi_frame_len(header->length) ||
        (payload == NULL && header->length != 0)) {
        return false;
    }

    tmp = *header;
    tmp.crc32 = 0;
    if (!ds5_dual_spi_encode_header(&tmp, frame, frame_len)) {
        return false;
    }
    if (header->length != 0) {
        memcpy(frame + DS5_DUAL_SPI_HEADER_LEN, payload, header->length);
    }

    crc = ~crc32_update(0xFFFFFFFFU, frame, 16);
    crc = ~crc32_update(~crc, frame + DS5_DUAL_SPI_HEADER_LEN, header->length);
    tmp.crc32 = crc;
    return ds5_dual_spi_encode_header(&tmp, frame, frame_len);
}

bool ds5_dual_spi_validate_frame(const uint8_t *frame,
                                 size_t frame_len,
                                 ds5_dual_spi_header_t *header)
{
    ds5_dual_spi_header_t parsed;
    uint32_t crc;

    if (!ds5_dual_spi_decode_header(frame, frame_len, &parsed)) {
        return false;
    }
    if (frame_len < ds5_dual_spi_frame_len(parsed.length)) {
        return false;
    }

    crc = ~crc32_update(0xFFFFFFFFU, frame, 16);
    crc = ~crc32_update(~crc, frame + DS5_DUAL_SPI_HEADER_LEN, parsed.length);
    if (crc != parsed.crc32) {
        return false;
    }

    if (header != NULL) {
        *header = parsed;
    }
    return true;
}

size_t ds5_dual_spi_frame_len(uint16_t payload_len)
{
    return DS5_DUAL_SPI_HEADER_LEN + (size_t)payload_len;
}

bool ds5_dual_spi_report_is_audio_rt(const uint8_t *report, size_t len)
{
    return report != NULL && len >= 1 && report[0] == 0x39;
}

bool ds5_dual_hello_encode(const ds5_dual_hello_t *hello,
                           uint8_t *out,
                           size_t out_len)
{
    if (hello == NULL || out == NULL || out_len < DS5_DUAL_HELLO_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_HELLO_PAYLOAD_LEN);
    out[0] = hello->protocol_version;
    out[1] = hello->role;
    put_u16_le(out + 2, hello->header_len);
    put_u16_le(out + 4, hello->max_payload);
    put_u16_le(out + 6, hello->spi_mtu);
    out[8] = hello->tx_queue_depth;
    put_u32_le(out + 10, hello->capabilities);
    return true;
}

bool ds5_dual_hello_decode(const uint8_t *data,
                           size_t len,
                           ds5_dual_hello_t *hello)
{
    if (data == NULL || hello == NULL || len < DS5_DUAL_HELLO_PAYLOAD_LEN) {
        return false;
    }

    memset(hello, 0, sizeof(*hello));
    hello->protocol_version = data[0];
    hello->role = data[1];
    hello->header_len = get_u16_le(data + 2);
    hello->max_payload = get_u16_le(data + 4);
    hello->spi_mtu = get_u16_le(data + 6);
    hello->tx_queue_depth = data[8];
    hello->capabilities = get_u32_le(data + 10);
    return true;
}

bool ds5_dual_time_sync_encode(const ds5_dual_time_sync_t *sync,
                               uint8_t *out,
                               size_t out_len)
{
    if (sync == NULL || out == NULL || out_len < DS5_DUAL_TIME_SYNC_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_TIME_SYNC_PAYLOAD_LEN);
    put_u32_le(out + 0, sync->m61_time_us);
    put_u32_le(out + 4, sync->esp_time_us);
    put_u32_le(out + 8, sync->roundtrip_us);
    put_u32_le(out + 12, (uint32_t)sync->offset_us);
    return true;
}

bool ds5_dual_time_sync_decode(const uint8_t *data,
                               size_t len,
                               ds5_dual_time_sync_t *sync)
{
    if (data == NULL || sync == NULL || len < DS5_DUAL_TIME_SYNC_PAYLOAD_LEN) {
        return false;
    }

    memset(sync, 0, sizeof(*sync));
    sync->m61_time_us = get_u32_le(data + 0);
    sync->esp_time_us = get_u32_le(data + 4);
    sync->roundtrip_us = get_u32_le(data + 8);
    sync->offset_us = (int32_t)get_u32_le(data + 12);
    return true;
}

bool ds5_dual_bt_state_encode(const ds5_dual_bt_state_t *state,
                              uint8_t *out,
                              size_t out_len)
{
    if (state == NULL || out == NULL || out_len < DS5_DUAL_BT_STATE_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_BT_STATE_PAYLOAD_LEN);
    put_u32_le(out + 0, state->flags);
    put_u32_le(out + 4, (uint32_t)state->last_error);
    out[8] = (uint8_t)state->rssi;
    out[9] = state->bringup_attempts;
    out[10] = state->reconnect_failures;
    put_u16_le(out + 12, state->control_mtu);
    put_u16_le(out + 14, state->interrupt_mtu);
    memcpy(out + 16, state->bda, sizeof(state->bda));
    put_u16_le(out + 22, state->state_seq);
    return true;
}

bool ds5_dual_bt_state_decode(const uint8_t *data,
                              size_t len,
                              ds5_dual_bt_state_t *state)
{
    if (data == NULL || state == NULL || len < DS5_DUAL_BT_STATE_PAYLOAD_LEN) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->flags = get_u32_le(data + 0);
    state->last_error = (int32_t)get_u32_le(data + 4);
    state->rssi = (int8_t)data[8];
    state->bringup_attempts = data[9];
    state->reconnect_failures = data[10];
    state->control_mtu = get_u16_le(data + 12);
    state->interrupt_mtu = get_u16_le(data + 14);
    memcpy(state->bda, data + 16, sizeof(state->bda));
    state->state_seq = get_u16_le(data + 22);
    return true;
}

bool ds5_dual_flow_credit_encode(const ds5_dual_flow_credit_t *credit,
                                  uint8_t *out,
                                  size_t out_len)
{
    if (credit == NULL || out == NULL ||
        out_len < DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN);
    out[0] = credit->tx_queue_free;
    out[1] = credit->tx_queue_depth;
    out[2] = credit->bt_ready ? 1U : 0U;
    put_u32_le(out + 4, credit->spi_queue_drops);
    put_u32_le(out + 8, credit->hidp_tx_errors);
    put_u32_le(out + 12, (uint32_t)credit->hidp_last_err);
    return true;
}

bool ds5_dual_flow_credit_decode(const uint8_t *data,
                                  size_t len,
                                  ds5_dual_flow_credit_t *credit)
{
    if (data == NULL || credit == NULL ||
        len < DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN) {
        return false;
    }

    memset(credit, 0, sizeof(*credit));
    credit->tx_queue_free = data[0];
    credit->tx_queue_depth = data[1];
    credit->bt_ready = data[2] != 0;
    credit->spi_queue_drops = get_u32_le(data + 4);
    credit->hidp_tx_errors = get_u32_le(data + 8);
    credit->hidp_last_err = (int32_t)get_u32_le(data + 12);
    return true;
}

bool ds5_dual_ack_encode(const ds5_dual_ack_t *ack,
                         uint8_t *out,
                         size_t out_len)
{
    if (ack == NULL || out == NULL || out_len < DS5_DUAL_ACK_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_ACK_PAYLOAD_LEN);
    put_u16_le(out + 0, ack->seq);
    out[2] = ack->type;
    put_u32_le(out + 4, (uint32_t)ack->status);
    return true;
}

bool ds5_dual_ack_decode(const uint8_t *data,
                         size_t len,
                         ds5_dual_ack_t *ack)
{
    if (data == NULL || ack == NULL || len < DS5_DUAL_ACK_PAYLOAD_LEN) {
        return false;
    }

    memset(ack, 0, sizeof(*ack));
    ack->seq = get_u16_le(data + 0);
    ack->type = data[2];
    ack->status = (int32_t)get_u32_le(data + 4);
    return true;
}

bool ds5_dual_stats_encode(const ds5_dual_stats_t *stats,
                           uint8_t *out,
                           size_t out_len)
{
    if (stats == NULL || out == NULL || out_len < DS5_DUAL_STATS_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, DS5_DUAL_STATS_PAYLOAD_LEN);
    out[0] = stats->protocol_version;
    out[1] = stats->role;
    put_u32_le(out + 4, stats->uptime_us);
    put_u32_le(out + 8, stats->spi_rx_frames);
    put_u32_le(out + 12, stats->spi_tx_frames);
    put_u32_le(out + 16, stats->spi_crc_errors);
    put_u32_le(out + 20, stats->spi_queue_drops);
    put_u32_le(out + 24, stats->hidp_tx_31);
    put_u32_le(out + 28, stats->hidp_tx_32);
    put_u32_le(out + 32, stats->hidp_tx_36);
    put_u32_le(out + 36, stats->hidp_tx_feature_get);
    put_u32_le(out + 40, stats->hidp_tx_feature_set);
    put_u32_le(out + 44, stats->hidp_tx_errors);
    put_u32_le(out + 48, stats->deadline_miss_36);
    put_u32_le(out + 52, stats->hidp_rx_input);
    put_u32_le(out + 56, stats->hidp_rx_mic_opus);
    put_u32_le(out + 60, stats->hidp_rx_feature_report);
    put_u32_le(out + 64, stats->ack_tx);
    put_u32_le(out + 68, stats->ack_drops);
    put_u32_le(out + 72, stats->flow_credit_tx);
    put_u32_le(out + 76, stats->bt_connect_rx);
    put_u32_le(out + 80, stats->bt_disconnect_rx);
    return true;
}

bool ds5_dual_stats_decode(const uint8_t *data,
                           size_t len,
                           ds5_dual_stats_t *stats)
{
    if (data == NULL || stats == NULL || len < DS5_DUAL_STATS_PAYLOAD_LEN) {
        return false;
    }

    memset(stats, 0, sizeof(*stats));
    stats->protocol_version = data[0];
    stats->role = data[1];
    stats->uptime_us = get_u32_le(data + 4);
    stats->spi_rx_frames = get_u32_le(data + 8);
    stats->spi_tx_frames = get_u32_le(data + 12);
    stats->spi_crc_errors = get_u32_le(data + 16);
    stats->spi_queue_drops = get_u32_le(data + 20);
    stats->hidp_tx_31 = get_u32_le(data + 24);
    stats->hidp_tx_32 = get_u32_le(data + 28);
    stats->hidp_tx_36 = get_u32_le(data + 32);
    stats->hidp_tx_feature_get = get_u32_le(data + 36);
    stats->hidp_tx_feature_set = get_u32_le(data + 40);
    stats->hidp_tx_errors = get_u32_le(data + 44);
    stats->deadline_miss_36 = get_u32_le(data + 48);
    stats->hidp_rx_input = get_u32_le(data + 52);
    stats->hidp_rx_mic_opus = get_u32_le(data + 56);
    stats->hidp_rx_feature_report = get_u32_le(data + 60);
    stats->ack_tx = get_u32_le(data + 64);
    stats->ack_drops = get_u32_le(data + 68);
    stats->flow_credit_tx = get_u32_le(data + 72);
    stats->bt_connect_rx = get_u32_le(data + 76);
    stats->bt_disconnect_rx = get_u32_le(data + 80);
    return true;
}

const char *ds5_dual_spi_type_name(uint8_t type)
{
    switch (type) {
    case DS5_DUAL_MSG_HELLO:
        return "HELLO";
    case DS5_DUAL_MSG_TIME_SYNC:
        return "TIME_SYNC";
    case DS5_DUAL_MSG_BT_CONNECT:
        return "BT_CONNECT";
    case DS5_DUAL_MSG_BT_DISCONNECT:
        return "BT_DISCONNECT";
    case DS5_DUAL_MSG_BT_STATE:
        return "BT_STATE";
    case DS5_DUAL_MSG_BT_RX_INPUT:
        return "BT_RX_INPUT";
    case DS5_DUAL_MSG_BT_RX_MIC_OPUS:
        return "BT_RX_MIC_OPUS";
    case DS5_DUAL_MSG_BT_TX_REPORT:
        return "BT_TX_REPORT";
    case DS5_DUAL_MSG_BT_TX_AUDIO_RT:
        return "BT_TX_AUDIO_RT";
    case DS5_DUAL_MSG_FLOW_CREDIT:
        return "FLOW_CREDIT";
    case DS5_DUAL_MSG_STATS:
        return "STATS";
    case DS5_DUAL_MSG_RESET_STATS:
        return "RESET_STATS";
    case DS5_DUAL_MSG_BT_TX_FEATURE_GET:
        return "BT_TX_FEATURE_GET";
    case DS5_DUAL_MSG_BT_TX_FEATURE_SET:
        return "BT_TX_FEATURE_SET";
    case DS5_DUAL_MSG_BT_RX_FEATURE_REPORT:
        return "BT_RX_FEATURE_REPORT";
    case DS5_DUAL_MSG_WIRE_TEST:
        return "WIRE_TEST";
    case DS5_DUAL_MSG_BT_FORGET:
        return "BT_FORGET";
    default:
        return "?";
    }
}
