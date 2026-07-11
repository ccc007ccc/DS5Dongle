#include "esp32_dual_chip_spi.h"

#include "bt_dualsense_raw_hidp.h"
#include "bt_ds5_tx_scheduler.h"
#include "esp32_dual_chip_response_scheduler.h"
#include "dual_chip_spi_proto.h"
#include "led_status.h"
#include "dualsense_parser.h"

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#ifndef CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
#define CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR 0
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

#define DS5_DUAL_CHIP_SPI_TASK_STACK 4096
#define DS5_DUAL_CHIP_SPI_TRANSACTION_LEN \
    (DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD)

static const char *TAG = "ds5_dual_spi";
#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
static TaskHandle_t s_spi_task;
static uint32_t s_reject_log_count;
static uint32_t s_transaction_log_count;
static uint32_t s_frame_log_count;
static uint32_t s_zero_transaction_count;
static uint32_t s_short_response_count;
static uint16_t s_rx_last_seq[DS5_DUAL_CHANNEL_LOG + 1U];
static bool s_rx_seq_seen[DS5_DUAL_CHANNEL_LOG + 1U];
static uint64_t s_output_gpio_configured_mask;
DMA_ATTR static uint8_t s_spi_rx_buf[DS5_DUAL_CHIP_SPI_TRANSACTION_LEN];
DMA_ATTR static uint8_t s_spi_tx_buf[DS5_DUAL_CHIP_SPI_TRANSACTION_LEN];
#endif
static esp32_dual_chip_spi_stats_t s_stats;

#if CONFIG_DS5_DUAL_CHIP_SPI_COPROCESSOR
static void set_pending_flow_credit(void);

static void note_hidp_report(uint8_t report_id)
{
    switch (report_id) {
    case 0x31:
        s_stats.hidp_tx_31++;
        break;
    case 0x32:
        s_stats.hidp_tx_32++;
        break;
    case 0x39:
        s_stats.hidp_tx_36++;
        break;
    default:
        break;
    }
}

void bt_dualsense_raw_hidp_note_tx(const uint8_t *data, size_t len, int status)
{
    if (status != 0) {
        s_stats.hidp_tx_errors++;
        s_stats.hidp_last_err = status;
        return;
    }

    s_stats.hidp_last_err = 0;
    if (data == NULL || len == 0) {
        return;
    }

    if (data[0] == (uint8_t)(0x40 | 0x03)) {
        s_stats.hidp_tx_feature_get++;
    } else if (data[0] == (uint8_t)(0x50 | 0x03)) {
        s_stats.hidp_tx_feature_set++;
    } else if (data[0] == 0xA2 && len > 1) {
        note_hidp_report(data[1]);
    } else {
        note_hidp_report(data[0]);
    }
}
#endif

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

static void response_irq_set(bool asserted, void *ctx)
{
    (void)ctx;
    if (CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO >= 0) {
        (void)gpio_set_level(
            (gpio_num_t)CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO,
            asserted ? 1U : 0U);
    }
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

    if (trans_bits == 0U) {
        uint32_t zero_count = ++s_zero_transaction_count;

        if (zero_count > 4U && (zero_count % 4096U) != 0U) {
            return;
        }
    }
    if (log_count > 4U && (log_count & (log_count - 1U)) != 0U) {
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

static bool set_pending_response_with_flags(uint8_t type,
                                            uint16_t flags,
                                            uint8_t channel,
                                            uint8_t priority,
                                            uint32_t timestamp_us,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            bool replace_existing)
{
    (void)replace_existing;
    return esp32_dual_response_publish(type,
                                       flags,
                                       channel,
                                       priority,
                                       response_seq_for_type(type),
                                       timestamp_us,
                                       payload,
                                       payload_len);
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

static void set_pending_ack_fields(uint16_t seq,
                                   uint8_t type,
                                   uint8_t channel,
                                   int32_t status)
{
    ds5_dual_ack_t ack;
    uint8_t payload[DS5_DUAL_ACK_PAYLOAD_LEN];

    ack.seq = seq;
    ack.type = type;
    ack.status = status;
    if (!ds5_dual_ack_encode(&ack, payload, sizeof(payload))) {
        s_stats.spi_frame_errors++;
        return;
    }

    if (set_pending_response_with_flags(type,
                                        DS5_DUAL_FLAG_ACK,
                                        channel,
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

static void set_pending_ack(const ds5_dual_spi_header_t *request, int32_t status)
{
    if (request == NULL) {
        return;
    }

    set_pending_ack_fields(request->seq, request->type, request->channel, status);
}

static void control_complete_cb(uint16_t seq,
                                uint8_t type,
                                uint8_t channel,
                                int status,
                                void *ctx)
{
    (void)ctx;
    set_pending_ack_fields(seq, type, channel, status);
    set_pending_flow_credit();
}

static void set_pending_flow_credit(void)
{
    ds5_dual_flow_credit_t credit = {
        .tx_queue_free = bt_ds5_tx_scheduler_free_count(),
        .tx_queue_depth = DS5_SCHED_ESP_RT_REPORT_CAPACITY,
        .bt_ready = bt_dualsense_raw_hidp_ready(),
        .spi_queue_drops = s_stats.spi_queue_drops,
        .hidp_tx_errors = s_stats.hidp_tx_errors,
        .hidp_last_err = s_stats.hidp_last_err,
    };
    uint8_t payload[DS5_DUAL_FLOW_CREDIT_PAYLOAD_LEN];

    if (!ds5_dual_flow_credit_encode(&credit, payload, sizeof(payload))) {
        s_stats.spi_frame_errors++;
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
        s_stats.spi_frame_errors++;
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
        .tx_queue_depth = DS5_SCHED_ESP_RT_REPORT_CAPACITY,
        .capabilities = DS5_DUAL_CAP_BT_HIDP_RAW |
                        DS5_DUAL_CAP_AUDIO_RT |
                        DS5_DUAL_CAP_FEATURE_REPORTS |
                        DS5_DUAL_CAP_FLOW_CREDIT |
                        DS5_DUAL_CAP_RELIABLE_ACK |
                        DS5_DUAL_CAP_BT_CONNECT_MODES,
    };
    uint8_t payload[DS5_DUAL_HELLO_PAYLOAD_LEN];

    if (!ds5_dual_hello_encode(&hello, payload, sizeof(payload))) {
        s_stats.spi_frame_errors++;
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
        s_stats.spi_frame_errors++;
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
        s_stats.spi_frame_errors++;
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

static bool stage_response_for_transaction(esp32_dual_response_token_t *token,
                                           size_t *response_len)
{
    memset(s_spi_tx_buf, 0, sizeof(s_spi_tx_buf));
    return esp32_dual_response_stage(s_spi_tx_buf,
                                     sizeof(s_spi_tx_buf),
                                     response_len,
                                     token);
}

static void finish_response_transaction(const esp32_dual_response_token_t *token,
                                        bool success)
{
    esp32_dual_response_finish(token, success);
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
    esp32_dual_response_reset_generation(
        bt_ds5_tx_scheduler_generation());
    set_pending_bt_state(state, timestamp_us);
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
        esp32_dual_response_token_t response_token = {0};
        size_t response_len = 0;
        bool had_response =
            stage_response_for_transaction(&response_token, &response_len);

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
            s_stats.spi_driver_errors++;
            if (had_response) {
                finish_response_transaction(&response_token, false);
            }
            ESP_LOGW(TAG, "SPI slave transaction failed: %s", esp_err_to_name(err));
            continue;
        }

        size_t rx_len = (size_t)(trans.trans_len / 8U);
        bool response_complete =
            !had_response || trans.trans_len >= (response_len * 8U);
        if (had_response) {
            if (response_complete) {
                s_stats.spi_tx_frames++;
            } else {
                uint32_t short_count = ++s_short_response_count;
                s_stats.spi_short_transactions++;

                if (short_count <= 4U || (short_count % 256U) == 0U) {
                    ESP_LOGW(TAG,
                             "SPI response retained: clocked_bits=%u needed_bits=%u count=%u",
                             (unsigned)trans.trans_len,
                             (unsigned)(response_len * 8U),
                             (unsigned)short_count);
                }
            }
            finish_response_transaction(&response_token, response_complete);
        }

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

static void set_pending_hello(void)
{
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
    /* The scheduler synchronizes IRQ state while holding its portMUX. GPIO
     * configuration can log and take newlib locks, so it must happen first. */
    set_optional_gpio_level(CONFIG_DS5_DUAL_CHIP_ESP_IRQ_GPIO, 0);
    esp32_dual_response_scheduler_init(response_irq_set, NULL);

    bt_dualsense_raw_hidp_set_rx_callback(rx_forward_cb, NULL);
    bt_dualsense_raw_hidp_set_state_callback(state_forward_cb, NULL);
    bt_dualsense_raw_hidp_set_control_complete_callback(control_complete_cb,
                                                        NULL);
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
    const uint8_t *payload;
    bool wants_ack;

    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ds5_dual_spi_decode_header(frame, frame_len, &header) ||
        frame_len < ds5_dual_spi_frame_len(header.length)) {
        s_stats.spi_frame_errors++;
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ds5_dual_spi_validate_frame(frame, frame_len, &header)) {
        s_stats.spi_crc_errors++;
        return ESP_ERR_INVALID_CRC;
    }

    s_stats.spi_rx_frames++;
    if (header.channel <= DS5_DUAL_CHANNEL_LOG) {
        uint8_t channel = header.channel;
        if (s_rx_seq_seen[channel]) {
            uint16_t expected = (uint16_t)(s_rx_last_seq[channel] + 1U);
            if (header.seq == s_rx_last_seq[channel]) {
                s_stats.spi_duplicates++;
            } else if (header.seq != expected) {
                s_stats.spi_sequence_errors++;
            }
        }
        if (!s_rx_seq_seen[channel] || header.seq != s_rx_last_seq[channel]) {
            s_rx_last_seq[channel] = header.seq;
            s_rx_seq_seen[channel] = true;
        }
    }
    s_frame_log_count++;
    if (s_frame_log_count <= 4U ||
        (s_frame_log_count & (s_frame_log_count - 1U)) == 0U) {
        ESP_LOGI(TAG,
                 "SPI rx sampled count=%u type=%s seq=%u len=%u flags=0x%04x",
                 (unsigned)s_frame_log_count,
                 ds5_dual_spi_type_name(header.type),
                 (unsigned)header.seq,
                 (unsigned)header.length,
                 (unsigned)header.flags);
    }
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
        if (header.length != 0U) {
            if (wants_ack) {
                set_pending_ack(&header, -EINVAL);
            }
            return ESP_ERR_INVALID_SIZE;
        }
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

        if (header.length != DS5_DUAL_WIRE_TEST_PAYLOAD_LEN) {
            status = -EINVAL;
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_START) {
            led_status_set_transient(DS5_LED_STATE_WIRE_TESTING, 10000);
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_PASS) {
            led_status_set_transient(DS5_LED_STATE_WIRE_TEST_PASS, 3000);
        } else if (payload[0] == DS5_DUAL_WIRE_TEST_FAIL) {
            led_status_set_transient(DS5_LED_STATE_WIRE_TEST_FAIL, 10000);
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
    if (header.length == 0 || header.length > DS5_DUAL_SPI_MAX_PAYLOAD) {
        if (wants_ack) {
            set_pending_ack(&header, -EMSGSIZE);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    int status;
    bool ack_deferred = false;
    if (header.type == DS5_DUAL_MSG_BT_TX_FEATURE_GET) {
        if (header.length != DS5_DUAL_FEATURE_GET_PAYLOAD_LEN) {
            status = -EINVAL;
        } else if (wants_ack) {
            status = bt_dualsense_raw_hidp_get_feature_tracked(
                payload[0], header.seq, header.type, header.channel);
            ack_deferred = status == 0;
        } else {
            status = bt_dualsense_raw_hidp_get_feature(payload[0]);
        }
    } else if (header.type == DS5_DUAL_MSG_BT_TX_FEATURE_SET) {
        if (header.length < 2U) {
            status = -EINVAL;
        } else if (wants_ack) {
            status = bt_dualsense_raw_hidp_set_feature_tracked(
                payload[0],
                payload + 1,
                header.length - 1U,
                header.seq,
                header.type,
                header.channel);
            ack_deferred = status == 0;
        } else {
            status = bt_dualsense_raw_hidp_set_feature(payload[0],
                                                       payload + 1,
                                                       header.length - 1U);
        }
    } else {
        status = bt_dualsense_raw_hidp_send_report_at(
            payload,
            header.length,
            header.type == DS5_DUAL_MSG_BT_TX_AUDIO_RT,
            (uint64_t)esp_timer_get_time(),
            bt_ds5_tx_scheduler_generation());
    }
    if (status != 0) {
        s_stats.hidp_tx_errors++;
        s_stats.hidp_last_err = status;
        if (status == -ENOBUFS || status == -EAGAIN) {
            s_stats.spi_queue_drops++;
        }
    } else {
        s_stats.hidp_last_err = 0;
    }
    if (wants_ack && !ack_deferred) {
        set_pending_ack(&header, status);
    }
    set_pending_flow_credit();
    return status == 0 ? ESP_OK : ESP_FAIL;
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
    memset(s_rx_last_seq, 0, sizeof(s_rx_last_seq));
    memset(s_rx_seq_seen, 0, sizeof(s_rx_seq_seen));
    bt_ds5_tx_scheduler_reset_metrics();
    esp32_dual_response_reset_metrics();
}
