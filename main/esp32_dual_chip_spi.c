#include "esp32_dual_chip_spi.h"

#include "bt_dualsense_raw_hidp.h"
#include "dual_chip_spi_proto.h"
#include "led_status.h"
#include "dualsense_parser.h"

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
#define CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR 0
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_TX_QUEUE_DEPTH
#define CONFIG_DS5_DUAL_CHIP_TX_QUEUE_DEPTH 4
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH
#define CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH 4
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_HOST
#define CONFIG_DS5_DUAL_CHIP_SPI_HOST 2
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO
#define CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO -1
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO
#define CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO -1
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO
#define CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO -1
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO
#define CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO -1
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO
#define CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO -1
#endif

#ifndef CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO
#define CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO -1
#endif

#define DS5_DUAL_CHIP_TASK_STACK 4096
#define DS5_DUAL_CHIP_SPI_TASK_STACK 4096
#define DS5_DUAL_CHIP_SPI_TRANSACTION_LEN \
    (DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD)

typedef struct {
    uint8_t type;
    uint16_t flags;
    uint8_t channel;
    uint8_t priority;
    uint32_t deadline_us;
    uint16_t len;
    uint8_t payload[DS5_DUAL_SPI_MAX_PAYLOAD];
} dual_chip_tx_item_t;

typedef struct {
    uint8_t type;
    uint16_t flags;
    size_t len;
    uint8_t frame[DS5_DUAL_CHIP_SPI_TRANSACTION_LEN];
} dual_chip_response_item_t;

static const char *TAG = "ds5_dual_spi";
static QueueHandle_t s_tx_queue;
#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
static TaskHandle_t s_tx_task;
static TaskHandle_t s_spi_task;
static SemaphoreHandle_t s_response_lock;
static dual_chip_response_item_t s_response_queue[CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH];
static uint8_t s_response_head;
static uint8_t s_response_count;
static uint32_t s_reject_log_count;
static uint32_t s_transaction_log_count;
static uint64_t s_output_gpio_configured_mask;
DMA_ATTR static uint8_t s_spi_rx_buf[DS5_DUAL_CHIP_SPI_TRANSACTION_LEN];
DMA_ATTR static uint8_t s_spi_tx_buf[DS5_DUAL_CHIP_SPI_TRANSACTION_LEN];
#endif
static esp32_dual_chip_spi_stats_t s_stats;

#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
static void note_hidp_report(uint8_t report_id)
{
    switch (report_id) {
    case 0x31:
        s_stats.hidp_tx_31++;
        break;
    case 0x32:
        s_stats.hidp_tx_32++;
        break;
    case 0x36:
        s_stats.hidp_tx_36++;
        break;
    default:
        break;
    }
}
#endif

static bool enqueue_latest(const dual_chip_tx_item_t *item)
{
    dual_chip_tx_item_t dropped;

    if (s_tx_queue == NULL || item == NULL) {
        return false;
    }

    if (xQueueSend(s_tx_queue, item, 0) == pdPASS) {
        return true;
    }

    if ((item->flags & DS5_DUAL_FLAG_LATEST) != 0 &&
        xQueueReceive(s_tx_queue, &dropped, 0) == pdPASS) {
        s_stats.spi_queue_drops++;
        return xQueueSend(s_tx_queue, item, 0) == pdPASS;
    }

    s_stats.spi_queue_drops++;
    return false;
}

#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
static bool spi_pins_configured(void)
{
    return CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO >= 0 &&
           CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO >= 0 &&
           CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO >= 0 &&
           CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO >= 0;
}

static void set_optional_gpio_level(int gpio_num, int level)
{
    if (gpio_num < 0) {
        return;
    }

    uint64_t gpio_mask = 1ULL << (uint32_t)gpio_num;
    if ((s_output_gpio_configured_mask & gpio_mask) == 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = gpio_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        s_output_gpio_configured_mask |= gpio_mask;
    }
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)gpio_num, level));
}

static uint16_t load_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t load_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint32_t diag_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
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

static uint32_t diag_frame_crc32(const uint8_t *frame, uint16_t payload_len)
{
    uint32_t crc = diag_crc32_update(0xFFFFFFFFU, frame, 16);

    crc = diag_crc32_update(crc,
                            frame + DS5_DUAL_SPI_HEADER_LEN,
                            payload_len);
    return ~crc;
}

static void log_rejected_frame(const uint8_t *frame, size_t rx_len)
{
    uint32_t log_count = ++s_reject_log_count;
    size_t dump_len = rx_len < 32U ? rx_len : 32U;

    if (log_count > 12U && (log_count % 64U) != 0U) {
        return;
    }
    if (frame == NULL || rx_len < DS5_DUAL_SPI_HEADER_LEN) {
        ESP_LOGW(TAG,
                 "SPI reject diag #%u short rx_len=%u",
                 (unsigned)log_count,
                 (unsigned)rx_len);
        if (frame != NULL && dump_len != 0U) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, dump_len, ESP_LOG_WARN);
        }
        return;
    }

    uint16_t magic = load_u16_le(frame + 0);
    uint16_t flags = load_u16_le(frame + 4);
    uint16_t seq = load_u16_le(frame + 8);
    uint16_t payload_len = load_u16_le(frame + 14);
    uint32_t crc_rx = load_u32_le(frame + 16);
    bool len_ok = payload_len <= DS5_DUAL_SPI_MAX_PAYLOAD &&
                  rx_len >= ds5_dual_spi_frame_len(payload_len);
    uint32_t crc_calc = len_ok ? diag_frame_crc32(frame, payload_len) : 0U;

    ESP_LOGW(TAG,
             "SPI reject diag #%u magic=0x%04x ver=%u type=%u flags=0x%04x chan=%u prio=%u seq=%u len=%u len_ok=%d crc_rx=0x%08x crc_calc=0x%08x",
             (unsigned)log_count,
             (unsigned)magic,
             (unsigned)frame[2],
             (unsigned)frame[3],
             (unsigned)flags,
             (unsigned)frame[6],
             (unsigned)frame[7],
             (unsigned)seq,
             (unsigned)payload_len,
             len_ok ? 1 : 0,
             (unsigned)crc_rx,
             (unsigned)crc_calc);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, dump_len, ESP_LOG_WARN);
}

static void log_transaction_diag(const uint8_t *frame,
                                 size_t rx_len,
                                 size_t trans_bits,
                                 bool had_response)
{
    uint32_t log_count = ++s_transaction_log_count;
    size_t dump_len = rx_len < 32U ? rx_len : 32U;

    if (log_count > 24U && (log_count % 64U) != 0U) {
        return;
    }

    ESP_LOGI(TAG,
             "SPI trans diag #%u bits=%u rx_len=%u had_response=%d first=%02x",
             (unsigned)log_count,
             (unsigned)trans_bits,
             (unsigned)rx_len,
             had_response ? 1 : 0,
             (frame != NULL && rx_len != 0U) ? frame[0] : 0U);
    if (frame != NULL && dump_len != 0U) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, dump_len, ESP_LOG_INFO);
    }
}

static uint16_t response_seq_for_type(uint8_t type)
{
    static uint16_t input_seq;
    static uint16_t mic_seq;
    static uint16_t status_seq;

    switch (type) {
    case DS5_DUAL_MSG_BT_RX_INPUT:
        return input_seq++;
    case DS5_DUAL_MSG_BT_RX_MIC_OPUS:
        return mic_seq++;
    case DS5_DUAL_MSG_BT_RX_FEATURE_REPORT:
        return status_seq++;
    default:
            return status_seq++;
    }
}

static uint8_t response_queue_index(uint8_t offset)
{
    return (uint8_t)((s_response_head + offset) %
                     CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH);
}

static void response_queue_drop_tail_locked(void)
{
    if (s_response_count == 0) {
        return;
    }

    s_response_count--;
    s_stats.spi_queue_drops++;
}

static bool response_queue_replace_matching_locked(const dual_chip_response_item_t *item)
{
    if (item == NULL) {
        return false;
    }

    for (uint8_t i = 0; i < s_response_count; i++) {
        uint8_t idx = response_queue_index(i);

        if (s_response_queue[idx].type == item->type &&
            (s_response_queue[idx].flags & DS5_DUAL_FLAG_ACK) == 0) {
            s_response_queue[idx] = *item;
            s_stats.spi_queue_drops++;
            return true;
        }
    }

    return false;
}

static bool response_queue_push_back_locked(const dual_chip_response_item_t *item,
                                            bool replace_existing)
{
    uint8_t idx;

    if (item == NULL) {
        return false;
    }
    if (replace_existing && response_queue_replace_matching_locked(item)) {
        return true;
    }
    if (s_response_count >= CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH) {
        if (!replace_existing) {
            return false;
        }
        response_queue_drop_tail_locked();
    }

    idx = response_queue_index(s_response_count);
    s_response_queue[idx] = *item;
    s_response_count++;
    return true;
}

static bool response_queue_push_front_locked(const dual_chip_response_item_t *item)
{
    if (item == NULL) {
        return false;
    }
    if (s_response_count >= CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH) {
        response_queue_drop_tail_locked();
    }

    s_response_head = s_response_head == 0 ?
        (CONFIG_DS5_DUAL_CHIP_RESPONSE_QUEUE_DEPTH - 1) :
        (s_response_head - 1);
    s_response_queue[s_response_head] = *item;
    s_response_count++;
    return true;
}

static bool response_queue_peek_front_locked(dual_chip_response_item_t *item)
{
    if (item == NULL || s_response_count == 0) {
        return false;
    }

    *item = s_response_queue[s_response_head];
    return true;
}

static void response_queue_pop_front_locked(void)
{
    if (s_response_count == 0) {
        return;
    }

    s_response_head = response_queue_index(1);
    s_response_count--;
}

static bool response_queue_consume_matching_front_locked(const dual_chip_response_item_t *item)
{
    const dual_chip_response_item_t *head;

    if (item == NULL || s_response_count == 0) {
        return false;
    }

    head = &s_response_queue[s_response_head];
    if (head->type != item->type ||
        head->flags != item->flags ||
        head->len != item->len ||
        memcmp(head->frame, item->frame, item->len) != 0) {
        return false;
    }

    response_queue_pop_front_locked();
    return true;
}

static bool set_pending_response_with_flags(uint8_t type,
                                            uint16_t flags,
                                            uint8_t channel,
                                            uint8_t priority,
                                            uint32_t timestamp_us,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            bool replace_existing)
{
    ds5_dual_spi_header_t header;
    dual_chip_response_item_t item;
    bool queued;

    if (payload == NULL || payload_len == 0 || payload_len > DS5_DUAL_SPI_MAX_PAYLOAD) {
        return false;
    }

    memset(&item, 0, sizeof(item));
    item.type = type;
    item.flags = flags;
    item.len = ds5_dual_spi_frame_len((uint16_t)payload_len);
    ds5_dual_spi_header_init(&header,
                             type,
                             flags,
                             channel,
                             priority,
                             response_seq_for_type(type),
                             timestamp_us,
                             (uint16_t)payload_len);
    if (!ds5_dual_spi_finalize_frame(item.frame, sizeof(item.frame), &header, payload)) {
        s_stats.spi_crc_errors++;
        return false;
    }

    if (s_response_lock != NULL) {
        xSemaphoreTake(s_response_lock, portMAX_DELAY);
    }
    queued = ((flags & DS5_DUAL_FLAG_ACK) != 0 ||
              type == DS5_DUAL_MSG_HELLO ||
              type == DS5_DUAL_MSG_TIME_SYNC ||
              type == DS5_DUAL_MSG_STATS ||
              type == DS5_DUAL_MSG_FLOW_CREDIT) ?
        response_queue_push_front_locked(&item) :
        response_queue_push_back_locked(&item, replace_existing);
    if (s_response_lock != NULL) {
        xSemaphoreGive(s_response_lock);
    }

    if (!queued) {
        return false;
    }
    set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO, 1);
    return true;
}

static bool set_pending_response(uint8_t type,
                                 uint8_t channel,
                                 uint8_t priority,
                                 uint32_t timestamp_us,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 bool replace_existing)
{
    return set_pending_response_with_flags(type,
                                           DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK,
                                           channel,
                                           priority,
                                           timestamp_us,
                                           payload,
                                           payload_len,
                                           replace_existing);
}

static void set_pending_ack(const ds5_dual_spi_header_t *request, int32_t status)
{
    ds5_dual_ack_t ack;
    uint8_t payload[DS5_DUAL_ACK_PAYLOAD_LEN];

    if (request == NULL) {
        return;
    }

    ack.seq = request->seq;
    ack.type = request->type;
    ack.status = status;
    if (!ds5_dual_ack_encode(&ack, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response_with_flags(request->type,
                                        DS5_DUAL_FLAG_ACK,
                                        request->channel,
                                        DS5_DUAL_PRIORITY_CONTROL,
                                        (uint32_t)esp_timer_get_time(),
                                        payload,
                                        sizeof(payload),
                                        false)) {
        s_stats.ack_tx++;
    } else {
        s_stats.ack_drops++;
    }
}

static void set_pending_flow_credit(void)
{
    ds5_dual_flow_credit_t credit = {
        .tx_queue_free = s_tx_queue != NULL ?
            (uint8_t)uxQueueSpacesAvailable(s_tx_queue) :
            0,
        .tx_queue_depth = CONFIG_DS5_DUAL_CHIP_TX_QUEUE_DEPTH,
        .bt_ready = bt_dualsense_raw_hidp_ready(),
        .spi_queue_drops = s_stats.spi_queue_drops,
        .hidp_tx_errors = s_stats.hidp_tx_errors,
        .hidp_last_err = s_stats.hidp_last_err,
    };
    uint8_t payload[DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN];

    if (!ds5_dual_flow_credit_encode(&credit, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response(DS5_DUAL_MSG_FLOW_CREDIT,
                             DS5_DUAL_CHANNEL_STATUS,
                             DS5_DUAL_PRIORITY_CONTROL,
                             (uint32_t)esp_timer_get_time(),
                             payload,
                             sizeof(payload),
                             false)) {
        s_stats.flow_credit_tx++;
    }
}

static void set_pending_stats(void)
{
    ds5_dual_stats_t stats = {
        .protocol_version = DS5_DUAL_SPI_VERSION,
        .role = DS5_DUAL_ROLE_ESP32_BT,
        .uptime_us = (uint32_t)esp_timer_get_time(),
        .spi_rx_frames = s_stats.spi_rx_frames,
        .spi_tx_frames = s_stats.spi_tx_frames,
        .spi_crc_errors = s_stats.spi_crc_errors,
        .spi_queue_drops = s_stats.spi_queue_drops,
        .hidp_tx_31 = s_stats.hidp_tx_31,
        .hidp_tx_32 = s_stats.hidp_tx_32,
        .hidp_tx_36 = s_stats.hidp_tx_36,
        .hidp_tx_feature_get = s_stats.hidp_tx_feature_get,
        .hidp_tx_feature_set = s_stats.hidp_tx_feature_set,
        .hidp_tx_errors = s_stats.hidp_tx_errors,
        .deadline_miss_36 = s_stats.deadline_miss_36,
        .hidp_rx_input = s_stats.hidp_rx_input,
        .hidp_rx_mic_opus = s_stats.hidp_rx_mic_opus,
        .hidp_rx_feature_report = s_stats.hidp_rx_feature_report,
        .ack_tx = s_stats.ack_tx,
        .ack_drops = s_stats.ack_drops,
        .flow_credit_tx = s_stats.flow_credit_tx,
        .bt_connect_rx = s_stats.bt_connect_rx,
        .bt_disconnect_rx = s_stats.bt_disconnect_rx,
    };
    uint8_t payload[DS5_DUAL_STATS_PAYLOAD_LEN];

    if (!ds5_dual_stats_encode(&stats, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response(DS5_DUAL_MSG_STATS,
                             DS5_DUAL_CHANNEL_STATUS,
                             DS5_DUAL_PRIORITY_LOW,
                             stats.uptime_us,
                             payload,
                             sizeof(payload),
                             true)) {
        s_stats.stats_tx++;
    } else {
        s_stats.stats_drops++;
    }
}

static void set_pending_hello(void)
{
    ds5_dual_hello_t hello = {
        .protocol_version = DS5_DUAL_SPI_VERSION,
        .role = DS5_DUAL_ROLE_ESP32_BT,
        .header_len = DS5_DUAL_SPI_HEADER_LEN,
        .max_payload = DS5_DUAL_SPI_MAX_PAYLOAD,
        .spi_mtu = DS5_DUAL_CHIP_SPI_TRANSACTION_LEN,
        .tx_queue_depth = CONFIG_DS5_DUAL_CHIP_TX_QUEUE_DEPTH,
        .capabilities = DS5_DUAL_CAP_BT_HIDP_RAW |
                        DS5_DUAL_CAP_AUDIO_RT |
                        DS5_DUAL_CAP_FEATURE_REPORTS |
                        DS5_DUAL_CAP_FLOW_CREDIT |
                        DS5_DUAL_CAP_RELIABLE_ACK,
    };
    uint8_t payload[DS5_DUAL_HELLO_PAYLOAD_LEN];

    if (!ds5_dual_hello_encode(&hello, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response(DS5_DUAL_MSG_HELLO,
                             DS5_DUAL_CHANNEL_CTRL,
                             DS5_DUAL_PRIORITY_CONTROL,
                             (uint32_t)esp_timer_get_time(),
                             payload,
                             sizeof(payload),
                             false)) {
        s_stats.hello_tx++;
    }
}

static void set_pending_time_sync(const ds5_dual_time_sync_t *request)
{
    ds5_dual_time_sync_t sync;
    uint8_t payload[DS5_DUAL_TIME_SYNC_PAYLOAD_LEN];

    if (request == NULL) {
        return;
    }

    sync = *request;
    sync.esp_time_us = (uint32_t)esp_timer_get_time();
    sync.roundtrip_us = 0;
    sync.offset_us = 0;
    if (!ds5_dual_time_sync_encode(&sync, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response(DS5_DUAL_MSG_TIME_SYNC,
                             DS5_DUAL_CHANNEL_CTRL,
                             DS5_DUAL_PRIORITY_CONTROL,
                             sync.esp_time_us,
                             payload,
                             sizeof(payload),
                             false)) {
        s_stats.time_sync_tx++;
    }
}

static void set_pending_bt_state(const bt_dualsense_raw_hidp_state_t *raw_state,
                                 int64_t timestamp_us)
{
    ds5_dual_bt_state_t state;
    uint8_t payload[DS5_DUAL_BT_STATE_PAYLOAD_LEN];

    if (raw_state == NULL) {
        return;
    }

    memset(&state, 0, sizeof(state));
    state.flags = raw_state->flags;
    state.last_error = raw_state->last_error;
    state.rssi = raw_state->rssi;
    state.bringup_attempts = raw_state->bringup_attempts;
    state.reconnect_failures = raw_state->reconnect_failures;
    state.control_mtu = raw_state->control_mtu;
    state.interrupt_mtu = raw_state->interrupt_mtu;
    memcpy(state.bda, raw_state->bda, sizeof(state.bda));
    state.state_seq = raw_state->state_seq;
    if (!ds5_dual_bt_state_encode(&state, payload, sizeof(payload))) {
        s_stats.spi_crc_errors++;
        return;
    }

    if (set_pending_response(DS5_DUAL_MSG_BT_STATE,
                             DS5_DUAL_CHANNEL_STATUS,
                             DS5_DUAL_PRIORITY_CONTROL,
                             (uint32_t)timestamp_us,
                             payload,
                             sizeof(payload),
                             true)) {
        s_stats.bt_state_tx++;
    } else {
        s_stats.bt_state_drops++;
    }
}

static bool stage_response_for_transaction(dual_chip_response_item_t *item,
                                          size_t *response_len)
{
    bool valid = false;
    bool dropped = false;
    uint8_t queued_after = 0;

    /* Keep the queued item in place until the slave transaction succeeds. */
    memset(s_spi_tx_buf, 0, sizeof(s_spi_tx_buf));
    if (s_response_lock != NULL) {
        xSemaphoreTake(s_response_lock, portMAX_DELAY);
    }
    valid = response_queue_peek_front_locked(item);
    if (valid && item->len <= sizeof(s_spi_tx_buf)) {
        memcpy(s_spi_tx_buf, item->frame, item->len);
        if (response_len != NULL) {
            *response_len = item->len;
        }
    } else if (valid) {
        response_queue_pop_front_locked();
        s_stats.spi_queue_drops++;
        queued_after = s_response_count;
        dropped = true;
        valid = false;
    }
    if (s_response_lock != NULL) {
        xSemaphoreGive(s_response_lock);
    }

    if (!valid && response_len != NULL) {
        *response_len = 0;
    }
    if (dropped) {
        set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO,
                                queued_after > 0 ? 1 : 0);
    }
    return valid;
}

static void finish_response_transaction(const dual_chip_response_item_t *item, bool success)
{
    uint8_t queued_after;

    if (s_response_lock != NULL) {
        xSemaphoreTake(s_response_lock, portMAX_DELAY);
    }
    if (success) {
        (void)response_queue_consume_matching_front_locked(item);
    }
    queued_after = s_response_count;
    if (s_response_lock != NULL) {
        xSemaphoreGive(s_response_lock);
    }

    set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO,
                            queued_after > 0 ? 1 : 0);
}

static void rx_forward_cb(const uint8_t *data, size_t len, int64_t timestamp_us, void *ctx)
{
    dualsense_state_t state;
    dualsense_parse_result_t parse = {0};

    (void)ctx;

    if (data != NULL && len >= 3 && data[0] == 0xA3) {
        s_stats.hidp_rx_feature_report++;
        set_pending_response(DS5_DUAL_MSG_BT_RX_FEATURE_REPORT,
                             DS5_DUAL_CHANNEL_CTRL,
                             DS5_DUAL_PRIORITY_CONTROL,
                             (uint32_t)timestamp_us,
                             data + 1,
                             len - 1U,
                             true);
        return;
    }

    if (dualsense_parse_report(data, len, &state, &parse)) {
        s_stats.hidp_rx_input++;
        set_pending_response(DS5_DUAL_MSG_BT_RX_INPUT,
                             DS5_DUAL_CHANNEL_INPUT,
                             DS5_DUAL_PRIORITY_RT,
                             (uint32_t)timestamp_us,
                             data,
                             len,
                             true);
    } else if (parse.kind == DS5_REPORT_MIC_AUDIO) {
        s_stats.hidp_rx_mic_opus++;
        if (parse.payload_len > 1U && parse.payload_offset + parse.payload_len <= len) {
            set_pending_response(DS5_DUAL_MSG_BT_RX_MIC_OPUS,
                                 DS5_DUAL_CHANNEL_AUDIO,
                                 DS5_DUAL_PRIORITY_AUDIO,
                                 (uint32_t)timestamp_us,
                                 data + parse.payload_offset + 1U,
                                 parse.payload_len - 1U,
                                 true);
        }
    }
}

static void state_forward_cb(const bt_dualsense_raw_hidp_state_t *state,
                             int64_t timestamp_us,
                             void *ctx)
{
    (void)ctx;
    set_pending_bt_state(state, timestamp_us);
}

static void tx_task(void *arg)
{
    dual_chip_tx_item_t item;

    (void)arg;

    while (true) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdPASS) {
            continue;
        }

        if (item.deadline_us != 0 &&
            esp_timer_get_time() > (int64_t)item.deadline_us) {
            if (item.type == DS5_DUAL_MSG_BT_TX_AUDIO_RT ||
                (item.len > 0 && item.payload[0] == 0x36)) {
                s_stats.deadline_miss_36++;
            }
            continue;
        }

        int err;
        if (item.type == DS5_DUAL_MSG_BT_TX_FEATURE_GET) {
            if (item.len < 1U) {
                err = -EINVAL;
            } else {
                err = bt_dualsense_raw_hidp_get_feature(item.payload[0]);
                if (err == 0) {
                    s_stats.hidp_tx_feature_get++;
                }
            }
        } else if (item.type == DS5_DUAL_MSG_BT_TX_FEATURE_SET) {
            if (item.len < 1U) {
                err = -EINVAL;
            } else {
                err = bt_dualsense_raw_hidp_set_feature(item.payload[0],
                                                        item.payload + 1,
                                                        item.len - 1U);
                if (err == 0) {
                    s_stats.hidp_tx_feature_set++;
                }
            }
        } else {
            err = bt_dualsense_raw_hidp_send_report(
                item.payload,
                item.len,
                item.type == DS5_DUAL_MSG_BT_TX_AUDIO_RT);
        }
        if (err) {
            s_stats.hidp_tx_errors++;
            s_stats.hidp_last_err = err;
            ESP_LOGW(TAG, "HIDP tx from SPI failed type=%s len=%u err=%d",
                     ds5_dual_spi_type_name(item.type),
                     (unsigned)item.len,
                     err);
        } else if ((item.type == DS5_DUAL_MSG_BT_TX_REPORT ||
                    item.type == DS5_DUAL_MSG_BT_TX_AUDIO_RT) &&
                   item.len > 0) {
            s_stats.hidp_last_err = 0;
            note_hidp_report(item.payload[0]);
        } else {
            s_stats.hidp_last_err = 0;
        }
    }
}

static esp_err_t init_spi_slave(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DS5_DUAL_CHIP_SPI_TRANSACTION_LEN,
    };
    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO,
        .flags = 0,
        .queue_size = 3,
        .mode = 0,
    };

    return spi_slave_initialize((spi_host_device_t)CONFIG_DS5_DUAL_CHIP_SPI_HOST,
                                &buscfg,
                                &slvcfg,
                                SPI_DMA_CH_AUTO);
}

static void spi_task(void *arg)
{
    (void)arg;

    esp_err_t err = init_spi_slave();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI slave init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO, 0);
    set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO, 0);
    ESP_LOGI(TAG,
             "SPI slave ready host=%d sclk=%d mosi=%d miso=%d cs=%d ready=%d irq=%d mtu=%u",
             CONFIG_DS5_DUAL_CHIP_SPI_HOST,
             CONFIG_DS5_DUAL_CHIP_SPI_SCLK_GPIO,
             CONFIG_DS5_DUAL_CHIP_SPI_MOSI_GPIO,
             CONFIG_DS5_DUAL_CHIP_SPI_MISO_GPIO,
             CONFIG_DS5_DUAL_CHIP_SPI_CS_GPIO,
             CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO,
             CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO,
             (unsigned)DS5_DUAL_CHIP_SPI_TRANSACTION_LEN);

    set_pending_hello();

    while (true) {
        spi_slave_transaction_t trans = {0};
        dual_chip_response_item_t response_item = {0};
        size_t response_len = 0;
        bool had_response = stage_response_for_transaction(&response_item, &response_len);

        memset(s_spi_rx_buf, 0, sizeof(s_spi_rx_buf));
        trans.length = sizeof(s_spi_rx_buf) * 8U;
        trans.tx_buffer = s_spi_tx_buf;
        trans.rx_buffer = s_spi_rx_buf;

        set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO, 1);
        err = spi_slave_transmit((spi_host_device_t)CONFIG_DS5_DUAL_CHIP_SPI_HOST,
                                 &trans,
                                 portMAX_DELAY);
        set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_READY_GPIO, 0);
        if (err != ESP_OK) {
            s_stats.spi_crc_errors++;
            ESP_LOGW(TAG, "SPI slave transaction failed: %s", esp_err_to_name(err));
            continue;
        }

        if (had_response) {
            s_stats.spi_tx_frames++;
            finish_response_transaction(&response_item, true);
            (void)response_len;
        }

        size_t rx_len = trans.trans_len > 0 ?
            (size_t)(trans.trans_len / 8U) :
            sizeof(s_spi_rx_buf);
        log_transaction_diag(s_spi_rx_buf, rx_len, trans.trans_len, had_response);
        if (rx_len >= DS5_DUAL_SPI_HEADER_LEN &&
            s_spi_rx_buf[0] != 0 &&
            esp32_dual_chip_spi_handle_frame(s_spi_rx_buf, rx_len) != ESP_OK) {
            log_rejected_frame(s_spi_rx_buf, rx_len);
        }
    }
}
#else
static void set_pending_ack(const ds5_dual_spi_header_t *request, int32_t status)
{
    (void)request;
    (void)status;
}

static void set_pending_flow_credit(void)
{
}

static void set_pending_stats(void)
{
}

static void set_pending_time_sync(const ds5_dual_time_sync_t *request)
{
    (void)request;
}

#endif

esp_err_t esp32_dual_chip_spi_start(void)
{
#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
    if (s_tx_queue == NULL) {
        s_tx_queue = xQueueCreate(CONFIG_DS5_DUAL_CHIP_TX_QUEUE_DEPTH,
                                  sizeof(dual_chip_tx_item_t));
        if (s_tx_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_response_lock == NULL) {
        s_response_lock = xSemaphoreCreateMutex();
        if (s_response_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_tx_task == NULL) {
        BaseType_t ok = xTaskCreatePinnedToCore(tx_task,
                                                "ds5_spi_tx",
                                                DS5_DUAL_CHIP_TASK_STACK,
                                                NULL,
                                                6,
                                                &s_tx_task,
                                                1);
        if (ok != pdPASS) {
            s_tx_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    bt_dualsense_raw_hidp_set_rx_callback(rx_forward_cb, NULL);
    bt_dualsense_raw_hidp_set_state_callback(state_forward_cb, NULL);
    if (spi_pins_configured()) {
        if (s_spi_task == NULL) {
            BaseType_t ok = xTaskCreatePinnedToCore(spi_task,
                                                    "ds5_spi_slave",
                                                    DS5_DUAL_CHIP_SPI_TASK_STACK,
                                                    NULL,
                                                    7,
                                                    &s_spi_task,
                                                    1);
            if (ok != pdPASS) {
                s_spi_task = NULL;
                return ESP_ERR_NO_MEM;
            }
        }
    } else {
        ESP_LOGW(TAG,
                 "Dual-chip SPI queue enabled but hardware pins are unset; configure SCLK/MOSI/MISO/CS GPIOs before wiring test");
    }
#else
    ESP_LOGI(TAG, "Dual-chip SPI coprocessor disabled by config");
#endif
    return ESP_OK;
}

esp_err_t esp32_dual_chip_spi_handle_frame(const uint8_t *frame, size_t frame_len)
{
    ds5_dual_spi_header_t header;
    dual_chip_tx_item_t item;
    const uint8_t *payload;
    bool wants_ack;

    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ds5_dual_spi_validate_frame(frame, frame_len, &header)) {
        s_stats.spi_crc_errors++;
        return ESP_ERR_INVALID_CRC;
    }

    s_stats.spi_rx_frames++;
    ESP_LOGI(TAG,
             "SPI rx valid type=%s seq=%u len=%u flags=0x%04x",
             ds5_dual_spi_type_name(header.type),
             (unsigned)header.seq,
             (unsigned)header.length,
             (unsigned)header.flags);
    payload = frame + DS5_DUAL_SPI_HEADER_LEN;
    wants_ack = (header.flags & DS5_DUAL_FLAG_RELIABLE) != 0;

    if (header.type == DS5_DUAL_MSG_HELLO) {
        ds5_dual_hello_t hello;

        if (!ds5_dual_hello_decode(payload, header.length, &hello)) {
            if (wants_ack) {
                set_pending_ack(&header, -EINVAL);
            }
            return ESP_ERR_INVALID_ARG;
        }
        s_stats.hello_rx++;
        s_stats.peer_role = hello.role;
        s_stats.peer_protocol_version = hello.protocol_version;
        s_stats.peer_max_payload = hello.max_payload;
        s_stats.peer_spi_mtu = hello.spi_mtu;
        s_stats.peer_tx_queue_depth = hello.tx_queue_depth;
        s_stats.peer_capabilities = hello.capabilities;
        if (wants_ack) {
            set_pending_ack(&header, 0);
        }
        set_pending_hello();
        return ESP_OK;
    }
    if (header.type == DS5_DUAL_MSG_TIME_SYNC) {
        ds5_dual_time_sync_t sync;

        if (!ds5_dual_time_sync_decode(payload, header.length, &sync)) {
            if (wants_ack) {
                set_pending_ack(&header, -EINVAL);
            }
            return ESP_ERR_INVALID_ARG;
        }
        s_stats.time_sync_rx++;
        set_pending_time_sync(&sync);
        if (wants_ack) {
            set_pending_ack(&header, 0);
        }
        return ESP_OK;
    }
    if (header.type == DS5_DUAL_MSG_RESET_STATS) {
        esp32_dual_chip_spi_reset_stats();
        if (wants_ack) {
            set_pending_ack(&header, 0);
        }
        return ESP_OK;
    }
    if (header.type == DS5_DUAL_MSG_STATS) {
        if (header.length != 0) {
            if (wants_ack) {
                set_pending_ack(&header, -EINVAL);
            }
            return ESP_ERR_INVALID_SIZE;
        }
        s_stats.stats_rx++;
        set_pending_stats();
        if (wants_ack) {
            set_pending_ack(&header, 0);
        }
        return ESP_OK;
    }
    if (header.type == DS5_DUAL_MSG_WIRE_TEST) {
        int status = 0;

        if (header.length < 1U) {
            status = -EINVAL;
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_START) {
            led_status_set(DS5_LED_STATE_WIRE_TESTING);
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_PASS) {
            led_status_set(DS5_LED_STATE_WIRE_TEST_PASS);
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_FAIL) {
            led_status_set(DS5_LED_STATE_WIRE_TEST_FAIL);
        } else {
            status = -EINVAL;
        }
        if (wants_ack) {
            set_pending_ack(&header, status);
        }
        return status == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (header.type == DS5_DUAL_MSG_BT_CONNECT) {
        int status;

        if (header.length == 0) {
            status = bt_dualsense_raw_hidp_connect(NULL, 0, DS5_DUAL_BT_CONNECT_AUTO);
        } else if (header.length == 1U) {
            status = bt_dualsense_raw_hidp_connect(NULL, 0, payload[0]);
        } else if (header.length == 6U) {
            status = bt_dualsense_raw_hidp_connect(payload, header.length,
                                                   DS5_DUAL_BT_CONNECT_AUTO);
        } else {
            status = -EINVAL;
        }
        if ((status == 0 || status == -EAGAIN) &&
            (header.length == 0 || header.length == 1U || header.length == 6U)) {
            s_stats.bt_connect_rx++;
        }
        if (wants_ack) {
            set_pending_ack(&header, status);
        }
        set_pending_flow_credit();
        return status == 0 || status == -EAGAIN ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (header.type == DS5_DUAL_MSG_BT_DISCONNECT) {
        bool allow_reconnect = header.length > 0 && payload[0] != 0;
        int status = header.length > 1U ?
            -EINVAL :
            bt_dualsense_raw_hidp_disconnect(allow_reconnect);

        if (status == 0) {
            s_stats.bt_disconnect_rx++;
        }
        if (wants_ack) {
            set_pending_ack(&header, status);
        }
        set_pending_flow_credit();
        return status == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (header.type == DS5_DUAL_MSG_BT_FORGET) {
        int status = header.length == 1U ?
            bt_dualsense_raw_hidp_forget(payload[0]) :
            -EINVAL;

        if (wants_ack) {
            set_pending_ack(&header, status);
        }
        set_pending_flow_credit();
        return status == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (header.type != DS5_DUAL_MSG_BT_TX_REPORT &&
        header.type != DS5_DUAL_MSG_BT_TX_AUDIO_RT &&
        header.type != DS5_DUAL_MSG_BT_TX_FEATURE_GET &&
        header.type != DS5_DUAL_MSG_BT_TX_FEATURE_SET) {
        if (wants_ack) {
            set_pending_ack(&header, -ENOTSUP);
        }
        return ESP_OK;
    }
    if (header.length == 0 || header.length > sizeof(item.payload)) {
        if (wants_ack) {
            set_pending_ack(&header, -EMSGSIZE);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&item, 0, sizeof(item));
    item.type = header.type;
    item.flags = header.flags;
    item.channel = header.channel;
    item.priority = header.priority;
    item.deadline_us = header.deadline;
    item.len = header.length;
    memcpy(item.payload, payload, header.length);

    if (!enqueue_latest(&item)) {
        if (wants_ack) {
            set_pending_ack(&header, -ENOMEM);
        }
        set_pending_flow_credit();
        return ESP_ERR_NO_MEM;
    }
    if (wants_ack) {
        set_pending_ack(&header, 0);
    }
    set_pending_flow_credit();
    return ESP_OK;
}

void esp32_dual_chip_spi_get_stats(esp32_dual_chip_spi_stats_t *stats)
{
    if (stats != NULL) {
        *stats = s_stats;
    }
}

void esp32_dual_chip_spi_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}
