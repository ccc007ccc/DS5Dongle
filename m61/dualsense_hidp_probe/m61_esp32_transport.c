#include "m61_esp32_transport.h"

#include "dual_chip_spi_proto.h"
#include "dualsense_parser.h"
#include "m61_usb_gamepad.h"

#include "bflb_core.h"
#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "bflb_spi.h"
#include "semphr.h"
#include "task.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_M61_ESP32_SPI_ENABLE
#define CONFIG_M61_ESP32_SPI_ENABLE 0
#endif

#ifndef CONFIG_M61_ESP32_SPI_READY
#define CONFIG_M61_ESP32_SPI_READY 0
#endif

#ifndef CONFIG_M61_ESP32_SPI_SCLK_PIN
#define CONFIG_M61_ESP32_SPI_SCLK_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_MOSI_PIN
#define CONFIG_M61_ESP32_SPI_MOSI_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_MISO_PIN
#define CONFIG_M61_ESP32_SPI_MISO_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_CS_PIN
#define CONFIG_M61_ESP32_SPI_CS_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_SPI_FREQ_HZ
#define CONFIG_M61_ESP32_SPI_FREQ_HZ 8000000
#endif

#ifndef CONFIG_M61_ESP32_READY_PIN
#define CONFIG_M61_ESP32_READY_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_IRQ_PIN
#define CONFIG_M61_ESP32_IRQ_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_RESET_PIN
#define CONFIG_M61_ESP32_RESET_PIN 255
#endif

#ifndef CONFIG_M61_ESP32_RX_POLL_ENABLE
#define CONFIG_M61_ESP32_RX_POLL_ENABLE 0
#endif

#ifndef CONFIG_M61_ESP32_RX_POLL_INTERVAL_MS
#define CONFIG_M61_ESP32_RX_POLL_INTERVAL_MS 1
#endif

#ifndef CONFIG_M61_ESP32_ACK_POLL_COUNT
#define CONFIG_M61_ESP32_ACK_POLL_COUNT 1
#endif

#ifndef CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT
#define CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT 1
#endif

#ifndef CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS
#define CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS 1000
#endif

#ifndef CONFIG_M61_ESP32_RESET_PULSE_MS
#define CONFIG_M61_ESP32_RESET_PULSE_MS 50
#endif

#ifndef CONFIG_M61_ESP32_RESET_BOOT_MS
#define CONFIG_M61_ESP32_RESET_BOOT_MS 750
#endif

#ifndef CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD
#define CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD 8
#endif

#ifndef CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS
#define CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS 5000
#endif

#define M61_ESP32_FEATURE_PAYLOAD_MAX 64U
#define M61_ESP32_SPI_TRANSACTION_LEN \
    (DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD)
#define M61_ESP32_WIRE_TEST_READY_WAIT_MS 10000U
#define M61_ESP32_SPI_READY_WAIT_MS 20U
#define M61_ESP32_EVENT_LOG_DEPTH 64U

enum {
    M61_ESP32_RECOVERY_REASON_TX = 1,
    M61_ESP32_RECOVERY_REASON_TIME_SYNC = 2,
};

typedef enum {
    M61_ESP32_EVENT_INIT = 1,
    M61_ESP32_EVENT_HELLO_RX,
    M61_ESP32_EVENT_TIME_SYNC_RX,
    M61_ESP32_EVENT_BT_STATE_RX,
    M61_ESP32_EVENT_FLOW_CREDIT_RX,
    M61_ESP32_EVENT_ACK_ERROR,
    M61_ESP32_EVENT_TX_ERROR,
    M61_ESP32_EVENT_RECOVERY,
    M61_ESP32_EVENT_WIRE_TEST,
    M61_ESP32_EVENT_RX_REJECT,
    M61_ESP32_EVENT_BT_NOT_READY,
} m61_esp32_event_type_t;

typedef struct {
    uint32_t time_us;
    uint8_t type;
    uint8_t data0;
    int16_t err;
    uint32_t data1;
    uint32_t data2;
} m61_esp32_event_t;

typedef struct {
    uint16_t seq_ctrl;
    uint16_t seq_output;
    uint16_t seq_audio;
    bool ready;
    struct bflb_device_s *gpio;
    struct bflb_device_s *spi;
    SemaphoreHandle_t lock;
    TaskHandle_t bringup_task;
    TaskHandle_t rx_poll_task;
    TaskHandle_t time_sync_task;
    bool recovering;
    bool time_sync_valid;
    int32_t esp_time_offset_us;
    m61_esp32_transport_input_cb_t input_cb;
    void *input_cb_ctx;
    m61_esp32_transport_feature_cb_t feature_cb;
    void *feature_cb_ctx;
    m61_esp32_transport_stats_t stats;
} m61_esp32_transport_state_t;

static m61_esp32_transport_state_t s_transport;
static uint8_t s_spi_tx_frame[M61_ESP32_SPI_TRANSACTION_LEN];
static uint8_t s_spi_rx_frame[M61_ESP32_SPI_TRANSACTION_LEN];
static m61_esp32_event_t s_event_log[M61_ESP32_EVENT_LOG_DEPTH];
static uint32_t s_event_seq;
static uint32_t s_diag_tx_log_count;
static uint32_t s_diag_exchange_log_count;
static uint32_t s_diag_rx_reject_count;

static bool pin_configured(int pin)
{
    return pin >= 0 && pin < GPIO_PIN_MAX;
}

static uint16_t next_seq(uint8_t channel)
{
    switch (channel) {
    case DS5_DUAL_CHANNEL_AUDIO:
        return s_transport.seq_audio++;
    case DS5_DUAL_CHANNEL_OUTPUT:
        return s_transport.seq_output++;
    default:
        return s_transport.seq_ctrl++;
    }
}

static bool spi_pins_configured(void)
{
    return pin_configured(CONFIG_M61_ESP32_SPI_SCLK_PIN) &&
           pin_configured(CONFIG_M61_ESP32_SPI_MOSI_PIN) &&
           pin_configured(CONFIG_M61_ESP32_SPI_MISO_PIN) &&
           pin_configured(CONFIG_M61_ESP32_SPI_CS_PIN);
}

static uint32_t local_time_us(void)
{
    return (uint32_t)bflb_mtimer_get_time_us();
}

static bool event_log_interesting_counter(uint32_t value)
{
    return value < 8U || (value & (value - 1U)) == 0U;
}

static void note_event(m61_esp32_event_type_t type,
                       uint8_t data0,
                       int err,
                       uint32_t data1,
                       uint32_t data2)
{
    uint32_t seq = s_event_seq++;
    m61_esp32_event_t *event = &s_event_log[seq % M61_ESP32_EVENT_LOG_DEPTH];

    event->time_us = local_time_us();
    event->type = (uint8_t)type;
    event->data0 = data0;
    event->err = (int16_t)err;
    event->data1 = data1;
    event->data2 = data2;
}

static const char *event_name(uint8_t type)
{
    switch ((m61_esp32_event_type_t)type) {
    case M61_ESP32_EVENT_INIT:
        return "init";
    case M61_ESP32_EVENT_HELLO_RX:
        return "hello_rx";
    case M61_ESP32_EVENT_TIME_SYNC_RX:
        return "time_sync_rx";
    case M61_ESP32_EVENT_BT_STATE_RX:
        return "bt_state_rx";
    case M61_ESP32_EVENT_FLOW_CREDIT_RX:
        return "flow_credit_rx";
    case M61_ESP32_EVENT_ACK_ERROR:
        return "ack_error";
    case M61_ESP32_EVENT_TX_ERROR:
        return "tx_error";
    case M61_ESP32_EVENT_RECOVERY:
        return "recovery";
    case M61_ESP32_EVENT_WIRE_TEST:
        return "wire_test";
    case M61_ESP32_EVENT_RX_REJECT:
        return "rx_reject";
    case M61_ESP32_EVENT_BT_NOT_READY:
        return "bt_not_ready";
    default:
        return "unknown";
    }
}

static uint16_t diag_load_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t diag_load_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void diag_print_hex_prefix(const uint8_t *data, size_t len)
{
    size_t dump_len = len < 32U ? len : 32U;

    for (size_t i = 0; i < dump_len; i++) {
        printf("%s%02x", i == 0 ? "" : " ", data[i]);
    }
}

static void diag_print_frame_header(const char *prefix,
                                    uint32_t count,
                                    const uint8_t *frame,
                                    size_t frame_len)
{
    if (frame == NULL || frame_len < DS5_DUAL_SPI_HEADER_LEN) {
        printf("%s #%lu short len=%lu bytes=",
               prefix,
               (unsigned long)count,
               (unsigned long)frame_len);
        if (frame != NULL) {
            diag_print_hex_prefix(frame, frame_len);
        }
        printf("\r\n");
        return;
    }

    printf("%s #%lu magic=0x%04x ver=%u type=%u flags=0x%04x chan=%u prio=%u seq=%u len=%u crc=0x%08lx bytes=",
           prefix,
           (unsigned long)count,
           (unsigned int)diag_load_u16_le(frame + 0),
           (unsigned int)frame[2],
           (unsigned int)frame[3],
           (unsigned int)diag_load_u16_le(frame + 4),
           (unsigned int)frame[6],
           (unsigned int)frame[7],
           (unsigned int)diag_load_u16_le(frame + 8),
           (unsigned int)diag_load_u16_le(frame + 14),
           (unsigned long)diag_load_u32_le(frame + 16));
    diag_print_hex_prefix(frame, frame_len);
    printf("\r\n");
}

static void diag_log_tx_frame(const uint8_t *frame, size_t frame_len)
{
    uint32_t count = ++s_diag_tx_log_count;

    if (count <= 12U || (count % 64U) == 0U) {
        diag_print_frame_header("esp32_spi_tx_diag", count, frame, frame_len);
    }
}

static void diag_log_rx_reject(const char *reason, const uint8_t *frame, size_t frame_len)
{
    uint32_t count = ++s_diag_rx_reject_count;
    char prefix[48];

    note_event(M61_ESP32_EVENT_RX_REJECT,
               frame != NULL && frame_len > 3U ? frame[3] : 0U,
               0,
               count,
               frame_len);

    if (count > 12U && (count % 64U) != 0U) {
        return;
    }

    snprintf(prefix, sizeof(prefix), "esp32_spi_rx_reject_%s", reason);
    diag_print_frame_header(prefix, count, frame, frame_len);
}

static void diag_log_exchange_rx(const uint8_t *frame, size_t frame_len)
{
    uint32_t count = ++s_diag_exchange_log_count;

    if (frame == NULL || frame_len == 0U || frame[0] == 0U) {
        return;
    }
    if (count <= 24U || (count % 64U) == 0U) {
        diag_print_frame_header("esp32_spi_rx_diag", count, frame, frame_len);
    }
}

static uint32_t tick_delta_us(TickType_t from_tick, TickType_t to_tick)
{
    TickType_t delta_ticks = to_tick - from_tick;

    return (uint32_t)delta_ticks * (uint32_t)portTICK_PERIOD_MS * 1000U;
}

static int send_time_sync(void);
static int send_hello(void);

static bool reset_pin_configured(void)
{
    return pin_configured(CONFIG_M61_ESP32_RESET_PIN);
}

static void mark_transport_success(void)
{
    s_transport.stats.recovery_consecutive_errors = 0;
}

static bool recovery_cooldown_elapsed(uint32_t now_us)
{
    uint32_t cooldown_us = (CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS > 0 ?
                            CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS :
                            0) * 1000U;

    if (cooldown_us == 0 ||
        s_transport.stats.last_recovery_local_us == 0) {
        return true;
    }

    return (now_us - s_transport.stats.last_recovery_local_us) >= cooldown_us;
}

static void perform_recovery(uint8_t reason)
{
    int hello_err;
    int sync_err;
    uint32_t now_us = local_time_us();

    if (CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD <= 0 ||
        s_transport.recovering) {
        return;
    }
    if (!recovery_cooldown_elapsed(now_us)) {
        s_transport.stats.recovery_suppressed++;
        return;
    }

    s_transport.stats.last_recovery_reason = reason;
    s_transport.stats.last_recovery_local_us = now_us;
    s_transport.stats.recovery_consecutive_errors = 0;
    note_event(M61_ESP32_EVENT_RECOVERY, reason, 0, now_us, s_transport.stats.recovery_attempts);

    if (s_transport.gpio == NULL || !reset_pin_configured()) {
        s_transport.stats.recovery_skipped_no_reset++;
        return;
    }

    s_transport.recovering = true;
    s_transport.ready = false;
    s_transport.time_sync_valid = false;
    s_transport.stats.time_sync_valid = 0;
    s_transport.stats.recovery_attempts++;

    if (s_transport.lock != NULL &&
        xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_transport.ready = true;
        s_transport.recovering = false;
        s_transport.stats.recovery_failures++;
        s_transport.stats.last_error = -EBUSY;
        return;
    }

    bflb_gpio_reset(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_M61_ESP32_RESET_PULSE_MS > 0 ?
                             CONFIG_M61_ESP32_RESET_PULSE_MS : 50));
    bflb_gpio_set(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_RESET_PIN);

    if (s_transport.lock != NULL) {
        xSemaphoreGive(s_transport.lock);
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_M61_ESP32_RESET_BOOT_MS > 0 ?
                             CONFIG_M61_ESP32_RESET_BOOT_MS : 750));
    s_transport.ready = true;

    hello_err = send_hello();
    sync_err = send_time_sync();
    if (hello_err == 0 && sync_err == 0) {
        s_transport.stats.recovery_successes++;
        s_transport.stats.last_error = 0;
        mark_transport_success();
    } else {
        s_transport.stats.recovery_failures++;
        s_transport.stats.last_error = hello_err < 0 ? hello_err : sync_err;
    }

    s_transport.recovering = false;
}

static void mark_transport_error(int err, uint8_t reason)
{
    (void)err;

    if (CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD <= 0 ||
        s_transport.recovering) {
        return;
    }

    s_transport.stats.recovery_consecutive_errors++;
    if (s_transport.stats.recovery_consecutive_errors >=
        (uint32_t)CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD) {
        perform_recovery(reason);
    }
}

static void init_optional_input_gpio(int pin, uint32_t pull)
{
    if (s_transport.gpio == NULL || !pin_configured(pin)) {
        return;
    }

    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)pin,
                   GPIO_INPUT | pull | GPIO_SMT_EN | GPIO_DRV_0);
}

static void init_optional_reset_gpio(void)
{
    if (s_transport.gpio == NULL || !pin_configured(CONFIG_M61_ESP32_RESET_PIN)) {
        return;
    }

    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)CONFIG_M61_ESP32_RESET_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_set(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_RESET_PIN);
}

static int init_spi_master(void)
{
    struct bflb_spi_config_s spi_cfg = {
        .freq = CONFIG_M61_ESP32_SPI_FREQ_HZ,
        .role = SPI_ROLE_MASTER,
        .mode = SPI_MODE0,
        .data_width = SPI_DATA_WIDTH_8BIT,
        .bit_order = SPI_BIT_MSB,
        .byte_order = SPI_BYTE_LSB,
        .tx_fifo_threshold = 0,
        .rx_fifo_threshold = 0,
    };

    if (!CONFIG_M61_ESP32_SPI_ENABLE || !CONFIG_M61_ESP32_SPI_READY) {
        return -ENOTCONN;
    }
    if (!spi_pins_configured()) {
        return -EINVAL;
    }

    s_transport.gpio = bflb_device_get_by_name("gpio");
    s_transport.spi = bflb_device_get_by_name("spi0");
    if (s_transport.gpio == NULL || s_transport.spi == NULL) {
        return -ENODEV;
    }

    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)CONFIG_M61_ESP32_SPI_SCLK_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)CONFIG_M61_ESP32_SPI_MOSI_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)CONFIG_M61_ESP32_SPI_MISO_PIN,
                   GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(s_transport.gpio,
                   (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_set(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);
    init_optional_input_gpio(CONFIG_M61_ESP32_READY_PIN, GPIO_PULLDOWN);
    init_optional_input_gpio(CONFIG_M61_ESP32_IRQ_PIN, GPIO_PULLDOWN);
    init_optional_reset_gpio();

    bflb_spi_init(s_transport.spi, &spi_cfg);
    bflb_spi_feature_control(s_transport.spi, SPI_CMD_SET_CS_INTERVAL, 0);
    return 0;
}

static bool optional_ready_pin_active(void)
{
    if (s_transport.gpio == NULL || !pin_configured(CONFIG_M61_ESP32_READY_PIN)) {
        return true;
    }

    return bflb_gpio_read(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_READY_PIN);
}

static bool wait_ready_for_exchange(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    do {
        if (optional_ready_pin_active()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    return optional_ready_pin_active();
}

static bool optional_irq_pin_active(void)
{
    if (s_transport.gpio == NULL || !pin_configured(CONFIG_M61_ESP32_IRQ_PIN)) {
        return false;
    }

    return bflb_gpio_read(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_IRQ_PIN);
}

static void spi_cs_select(void)
{
    if (s_transport.gpio == NULL || !pin_configured(CONFIG_M61_ESP32_SPI_CS_PIN)) {
        return;
    }

    bflb_gpio_reset(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);
    bflb_mtimer_delay_us(2);
}

static void spi_cs_deselect(void)
{
    if (s_transport.gpio == NULL || !pin_configured(CONFIG_M61_ESP32_SPI_CS_PIN)) {
        return;
    }

    bflb_mtimer_delay_us(2);
    bflb_gpio_set(s_transport.gpio, (uint8_t)CONFIG_M61_ESP32_SPI_CS_PIN);
}

static bool peer_link_ready(void)
{
    return s_transport.stats.rx_hello > 0 &&
           s_transport.stats.peer_role == DS5_DUAL_ROLE_ESP32_BT &&
           s_transport.stats.peer_protocol_version == DS5_DUAL_SPI_VERSION;
}

static bool peer_supports_bt_connect_modes(void)
{
    return (s_transport.stats.peer_capabilities & DS5_DUAL_CAP_BT_CONNECT_MODES) != 0U;
}

static int require_peer_link_ready(void)
{
    if (peer_link_ready()) {
        return 0;
    }

    s_transport.stats.not_ready++;
    s_transport.stats.last_error = -ENOTCONN;
    if (event_log_interesting_counter(s_transport.stats.not_ready)) {
        note_event(M61_ESP32_EVENT_BT_NOT_READY,
                   0,
                   -ENOTCONN,
                   s_transport.stats.rx_hello,
                   ((uint32_t)s_transport.stats.peer_role << 8) |
                       s_transport.stats.peer_protocol_version);
    }
    return -ENOTCONN;
}

static void handle_rx_input(const uint8_t *payload, size_t payload_len)
{
    dualsense_state_t state = { 0 };
    dualsense_parse_result_t parse = { 0 };

    if (payload == NULL || payload_len == 0) {
        s_transport.stats.frame_errors++;
        return;
    }

    if (dualsense_parse_report(payload, payload_len, &state, &parse)) {
        if (s_transport.input_cb != NULL) {
            s_transport.input_cb(payload,
                                 payload_len,
                                 &state,
                                 &parse,
                                 s_transport.input_cb_ctx);
        } else {
            if (state.is_full_report &&
                parse.payload_len >= M61_DS5_USB_INPUT_PAYLOAD_LEN &&
                parse.payload_offset + M61_DS5_USB_INPUT_PAYLOAD_LEN <= payload_len) {
                m61_usb_gamepad_send_report01(payload + parse.payload_offset,
                                              M61_DS5_USB_INPUT_PAYLOAD_LEN);
            } else {
                m61_usb_gamepad_send_state(&state);
            }
        }
    } else if (parse.kind == DS5_REPORT_MIC_AUDIO) {
        if (parse.payload_len > 1U &&
            parse.payload_offset + parse.payload_len <= payload_len) {
            m61_usb_gamepad_submit_mic_opus(payload + parse.payload_offset + 1U,
                                            parse.payload_len - 1U);
        }
    } else {
        s_transport.stats.frame_errors++;
    }
}

static void handle_rx_hello(const uint8_t *payload, size_t payload_len)
{
    ds5_dual_hello_t hello;

    if (!ds5_dual_hello_decode(payload, payload_len, &hello)) {
        s_transport.stats.frame_errors++;
        return;
    }

    s_transport.stats.rx_hello++;
    s_transport.stats.peer_role = hello.role;
    s_transport.stats.peer_protocol_version = hello.protocol_version;
    s_transport.stats.peer_max_payload = hello.max_payload;
    s_transport.stats.peer_spi_mtu = hello.spi_mtu;
    s_transport.stats.peer_tx_queue_depth = hello.tx_queue_depth;
    s_transport.stats.peer_capabilities = hello.capabilities;
    note_event(M61_ESP32_EVENT_HELLO_RX,
               hello.role,
               0,
               ((uint32_t)hello.protocol_version << 16) | hello.spi_mtu,
               hello.capabilities);
}

static void handle_rx_time_sync(const uint8_t *payload, size_t payload_len)
{
    ds5_dual_time_sync_t sync;
    uint32_t now_us;
    uint32_t rtt_us;
    uint32_t midpoint_us;
    int32_t offset_us;

    if (!ds5_dual_time_sync_decode(payload, payload_len, &sync) ||
        sync.esp_time_us == 0) {
        s_transport.stats.frame_errors++;
        return;
    }

    now_us = local_time_us();
    rtt_us = now_us - sync.m61_time_us;
    midpoint_us = sync.m61_time_us + (rtt_us / 2U);
    offset_us = (int32_t)(sync.esp_time_us - midpoint_us);

    s_transport.time_sync_valid = true;
    s_transport.esp_time_offset_us = offset_us;
    s_transport.stats.time_sync_valid = 1U;
    s_transport.stats.rx_time_sync++;
    s_transport.stats.last_time_sync_rtt_us = rtt_us;
    s_transport.stats.last_time_sync_local_us = now_us;
    s_transport.stats.esp_time_offset_us = offset_us;
    note_event(M61_ESP32_EVENT_TIME_SYNC_RX,
               0,
               0,
               rtt_us,
               (uint32_t)offset_us);
}

static void handle_rx_bt_state(const uint8_t *payload, size_t payload_len)
{
    ds5_dual_bt_state_t state;

    if (!ds5_dual_bt_state_decode(payload, payload_len, &state)) {
        s_transport.stats.frame_errors++;
        return;
    }

    s_transport.stats.rx_bt_state++;
    s_transport.stats.peer_bt_flags = state.flags;
    s_transport.stats.peer_bt_last_error = state.last_error;
    s_transport.stats.peer_bt_rssi = state.rssi;
    s_transport.stats.peer_bt_bringup_attempts = state.bringup_attempts;
    s_transport.stats.peer_bt_reconnect_failures = state.reconnect_failures;
    s_transport.stats.peer_bt_control_mtu = state.control_mtu;
    s_transport.stats.peer_bt_interrupt_mtu = state.interrupt_mtu;
    memcpy(s_transport.stats.peer_bt_bda, state.bda, sizeof(state.bda));
    s_transport.stats.peer_bt_state_seq = state.state_seq;
    note_event(M61_ESP32_EVENT_BT_STATE_RX,
               state.reconnect_failures,
               state.last_error,
               state.flags,
               ((uint32_t)state.control_mtu << 16) | state.interrupt_mtu);
}

static void handle_rx_stats(const uint8_t *payload, size_t payload_len)
{
    ds5_dual_stats_t stats;

    if (!ds5_dual_stats_decode(payload, payload_len, &stats)) {
        s_transport.stats.frame_errors++;
        return;
    }

    s_transport.stats.rx_stats++;
    s_transport.stats.peer_stats_role = stats.role;
    s_transport.stats.peer_stats_version = stats.protocol_version;
    s_transport.stats.peer_stats_uptime_us = stats.uptime_us;
    s_transport.stats.peer_stats_spi_rx_frames = stats.spi_rx_frames;
    s_transport.stats.peer_stats_spi_tx_frames = stats.spi_tx_frames;
    s_transport.stats.peer_stats_spi_crc_errors = stats.spi_crc_errors;
    s_transport.stats.peer_stats_spi_queue_drops = stats.spi_queue_drops;
    s_transport.stats.peer_stats_hidp_tx_31 = stats.hidp_tx_31;
    s_transport.stats.peer_stats_hidp_tx_32 = stats.hidp_tx_32;
    s_transport.stats.peer_stats_hidp_tx_36 = stats.hidp_tx_36;
    s_transport.stats.peer_stats_hidp_tx_feature_get = stats.hidp_tx_feature_get;
    s_transport.stats.peer_stats_hidp_tx_feature_set = stats.hidp_tx_feature_set;
    s_transport.stats.peer_stats_hidp_tx_errors = stats.hidp_tx_errors;
    s_transport.stats.peer_stats_deadline_miss_36 = stats.deadline_miss_36;
    s_transport.stats.peer_stats_hidp_rx_input = stats.hidp_rx_input;
    s_transport.stats.peer_stats_hidp_rx_mic_opus = stats.hidp_rx_mic_opus;
    s_transport.stats.peer_stats_hidp_rx_feature_report = stats.hidp_rx_feature_report;
    s_transport.stats.peer_stats_ack_tx = stats.ack_tx;
    s_transport.stats.peer_stats_ack_drops = stats.ack_drops;
    s_transport.stats.peer_stats_flow_credit_tx = stats.flow_credit_tx;
    s_transport.stats.peer_stats_bt_connect_rx = stats.bt_connect_rx;
    s_transport.stats.peer_stats_bt_disconnect_rx = stats.bt_disconnect_rx;
}

static void process_rx_frame(const uint8_t *frame, size_t frame_len)
{
    ds5_dual_spi_header_t header;
    const uint8_t *payload;

    if (frame == NULL || frame_len < DS5_DUAL_SPI_HEADER_LEN || frame[0] == 0) {
        return;
    }
    if (!ds5_dual_spi_decode_header(frame, frame_len, &header)) {
        s_transport.stats.frame_errors++;
        diag_log_rx_reject("header", frame, frame_len);
        return;
    }
    if (!ds5_dual_spi_validate_frame(frame, frame_len, &header)) {
        s_transport.stats.crc_errors++;
        diag_log_rx_reject("crc", frame, frame_len);
        return;
    }

    s_transport.stats.rx_frames++;
    s_transport.stats.rx_bytes += (uint32_t)ds5_dual_spi_frame_len(header.length);
    payload = frame + DS5_DUAL_SPI_HEADER_LEN;

    if ((header.flags & DS5_DUAL_FLAG_ACK) != 0) {
        ds5_dual_ack_t ack;

        if (ds5_dual_ack_decode(payload, header.length, &ack)) {
            s_transport.stats.rx_ack++;
            s_transport.stats.last_ack_seq = ack.seq;
            s_transport.stats.last_ack_type = ack.type;
            s_transport.stats.last_ack_status = ack.status;
            if (ack.status < 0) {
                note_event(M61_ESP32_EVENT_ACK_ERROR,
                           ack.type,
                           ack.status,
                           ack.seq,
                           s_transport.stats.rx_ack);
            }
        } else {
            s_transport.stats.frame_errors++;
        }
        return;
    }

    switch (header.type) {
    case DS5_DUAL_MSG_HELLO:
        handle_rx_hello(payload, header.length);
        break;
    case DS5_DUAL_MSG_TIME_SYNC:
        handle_rx_time_sync(payload, header.length);
        break;
    case DS5_DUAL_MSG_BT_STATE:
        handle_rx_bt_state(payload, header.length);
        break;
    case DS5_DUAL_MSG_BT_RX_INPUT:
        handle_rx_input(payload, header.length);
        break;
    case DS5_DUAL_MSG_BT_RX_MIC_OPUS:
        if (header.length > 0) {
            m61_usb_gamepad_submit_mic_opus(payload, header.length);
        }
        break;
    case DS5_DUAL_MSG_BT_RX_FEATURE_REPORT:
        if (header.length >= 1U) {
            m61_usb_gamepad_store_feature_report(payload[0],
                                                 payload + 1,
                                                 header.length - 1U);
            if (s_transport.feature_cb != NULL) {
                s_transport.feature_cb(payload[0],
                                       payload + 1,
                                       header.length - 1U,
                                       s_transport.feature_cb_ctx);
            }
        }
        break;
    case DS5_DUAL_MSG_FLOW_CREDIT: {
        ds5_dual_flow_credit_t credit;

        if (ds5_dual_flow_credit_decode(payload, header.length, &credit)) {
            uint8_t prev_free = s_transport.stats.last_credit_free;
            uint8_t prev_depth = s_transport.stats.last_credit_depth;
            uint8_t prev_ready = s_transport.stats.last_credit_bt_ready;
            uint32_t prev_errors = s_transport.stats.peer_hidp_tx_errors;
            int prev_last_err = s_transport.stats.peer_hidp_last_err;

            s_transport.stats.rx_flow_credit++;
            s_transport.stats.last_credit_free = credit.tx_queue_free;
            s_transport.stats.last_credit_depth = credit.tx_queue_depth;
            s_transport.stats.last_credit_bt_ready = credit.bt_ready ? 1U : 0U;
            s_transport.stats.peer_queue_drops = credit.spi_queue_drops;
            s_transport.stats.peer_hidp_tx_errors = credit.hidp_tx_errors;
            s_transport.stats.peer_hidp_last_err = credit.hidp_last_err;
            if (s_transport.stats.rx_flow_credit == 1U ||
                prev_ready != s_transport.stats.last_credit_bt_ready ||
                prev_free != s_transport.stats.last_credit_free ||
                prev_depth != s_transport.stats.last_credit_depth ||
                prev_errors != s_transport.stats.peer_hidp_tx_errors ||
                prev_last_err != s_transport.stats.peer_hidp_last_err) {
                note_event(M61_ESP32_EVENT_FLOW_CREDIT_RX,
                           credit.bt_ready ? 1U : 0U,
                           credit.hidp_last_err,
                           ((uint32_t)credit.tx_queue_depth << 8) | credit.tx_queue_free,
                           credit.hidp_tx_errors);
            }
        } else {
            s_transport.stats.frame_errors++;
        }
        break;
    }
    case DS5_DUAL_MSG_STATS:
        handle_rx_stats(payload, header.length);
        break;
    default:
        break;
    }
}

static int exchange_frame(uint8_t *frame, size_t frame_len)
{
    int err;

    if (frame == NULL || frame_len > M61_ESP32_SPI_TRANSACTION_LEN) {
        return -EINVAL;
    }
    if (s_transport.spi == NULL) {
        return -ENODEV;
    }
    if (!wait_ready_for_exchange(M61_ESP32_SPI_READY_WAIT_MS)) {
        s_transport.stats.not_ready++;
        return -EAGAIN;
    }

    memset(s_spi_tx_frame, 0, sizeof(s_spi_tx_frame));
    memset(s_spi_rx_frame, 0, sizeof(s_spi_rx_frame));
    memcpy(s_spi_tx_frame, frame, frame_len);
    diag_log_tx_frame(s_spi_tx_frame, frame_len);

    spi_cs_select();
    err = bflb_spi_poll_exchange(s_transport.spi,
                                 s_spi_tx_frame,
                                 s_spi_rx_frame,
                                 sizeof(s_spi_tx_frame));
    spi_cs_deselect();
    if (err < 0) {
        return err;
    }

    diag_log_exchange_rx(s_spi_rx_frame, sizeof(s_spi_rx_frame));
    process_rx_frame(s_spi_rx_frame, sizeof(s_spi_rx_frame));
    return 0;
}

static int poll_rx_response(void)
{
    int err;

    if (s_transport.spi == NULL) {
        return -ENODEV;
    }
    if (!wait_ready_for_exchange(M61_ESP32_SPI_READY_WAIT_MS)) {
        s_transport.stats.not_ready++;
        return -EAGAIN;
    }

    memset(s_spi_tx_frame, 0, sizeof(s_spi_tx_frame));
    memset(s_spi_rx_frame, 0, sizeof(s_spi_rx_frame));
    spi_cs_select();
    err = bflb_spi_poll_exchange(s_transport.spi,
                                 s_spi_tx_frame,
                                 s_spi_rx_frame,
                                 sizeof(s_spi_tx_frame));
    spi_cs_deselect();
    if (err < 0) {
        return err;
    }

    diag_log_exchange_rx(s_spi_rx_frame, sizeof(s_spi_rx_frame));
    process_rx_frame(s_spi_rx_frame, sizeof(s_spi_rx_frame));
    return 0;
}

static int poll_reliable_ack(uint16_t seq, uint8_t type)
{
    uint32_t start_ack = s_transport.stats.rx_ack;
    uint32_t poll_count = CONFIG_M61_ESP32_ACK_POLL_COUNT > 0 ?
        CONFIG_M61_ESP32_ACK_POLL_COUNT :
        0;

    if (poll_count == 0) {
        return -ETIMEDOUT;
    }

    for (uint32_t i = 0; i < poll_count; i++) {
        int err;

        vTaskDelay(pdMS_TO_TICKS(1));
        err = poll_rx_response();
        s_transport.stats.tx_ack_polls++;
        if (err < 0) {
            if (err == -EAGAIN || err == -EBUSY) {
                continue;
            }
            s_transport.stats.last_error = err;
            return err;
        }
        if (s_transport.stats.rx_ack != start_ack &&
            s_transport.stats.last_ack_seq == seq &&
            s_transport.stats.last_ack_type == type) {
            if (s_transport.stats.last_ack_status < 0) {
                s_transport.stats.ack_status_errors++;
            }
            return s_transport.stats.last_ack_status;
        }
    }

    s_transport.stats.ack_miss++;
    return -ETIMEDOUT;
}

static bool ack_result_should_retry(int ack_result)
{
    return ack_result == -ETIMEDOUT ||
           ack_result == -ENOMEM ||
           ack_result == -EAGAIN ||
           ack_result == -EBUSY;
}

static void rx_poll_task(void *arg)
{
    const TickType_t delay_ticks =
        pdMS_TO_TICKS(CONFIG_M61_ESP32_RX_POLL_INTERVAL_MS > 0 ?
                      CONFIG_M61_ESP32_RX_POLL_INTERVAL_MS : 1);
    bool irq_configured = pin_configured(CONFIG_M61_ESP32_IRQ_PIN);

    (void)arg;

    while (1) {
        if (s_transport.ready &&
            (!irq_configured || optional_irq_pin_active())) {
            int err;

            if (s_transport.lock != NULL &&
                xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
                s_transport.stats.queue_drop_old++;
            } else {
                err = poll_rx_response();
                if (err < 0) {
                    s_transport.stats.last_error = err;
                } else {
                    s_transport.stats.last_error = 0;
                }
                if (s_transport.lock != NULL) {
                    xSemaphoreGive(s_transport.lock);
                }
            }
        }
        vTaskDelay(delay_ticks);
    }
}

static void start_rx_poll_task(void)
{
    if (!CONFIG_M61_ESP32_RX_POLL_ENABLE || s_transport.rx_poll_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreate(rx_poll_task,
                                "m61_esp32_rx",
                                1024,
                                NULL,
                                6,
                                &s_transport.rx_poll_task);
    if (ok != pdPASS) {
        s_transport.rx_poll_task = NULL;
        s_transport.stats.last_error = -ENOMEM;
    }
}

static void time_sync_task(void *arg)
{
    const TickType_t delay_ticks =
        pdMS_TO_TICKS(CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS > 0 ?
                      CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS : 1000);

    (void)arg;

    while (1) {
        vTaskDelay(delay_ticks);
        if (!s_transport.ready) {
            continue;
        }

        int err = send_time_sync();
        if (err < 0) {
            s_transport.stats.last_error = err;
        }
    }
}

static void start_time_sync_task(void)
{
    if (CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS <= 0 ||
        s_transport.time_sync_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreate(time_sync_task,
                                "m61_esp32_ts",
                                1024,
                                NULL,
                                4,
                                &s_transport.time_sync_task);
    if (ok != pdPASS) {
        s_transport.time_sync_task = NULL;
        s_transport.stats.last_error = -ENOMEM;
    }
}

static void bringup_task(void *arg)
{
    int err;

    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(50));

    err = send_hello();
    if (err < 0) {
        s_transport.stats.last_error = err;
    }
    err = send_time_sync();
    if (err < 0) {
        s_transport.stats.last_error = err;
    }

    start_rx_poll_task();
    start_time_sync_task();

    printf("M61 ESP32 dual-chip SPI bringup hello=%lu time_sync=%lu sync_fail=%lu sync_valid=%u rtt_us=%lu offset_us=%ld peer_role=%u peer_ver=%u peer_mtu=%u peer_caps=0x%08lx err=%d\r\n",
           (unsigned long)s_transport.stats.rx_hello,
           (unsigned long)s_transport.stats.rx_time_sync,
           (unsigned long)s_transport.stats.time_sync_failures,
           (unsigned int)s_transport.stats.time_sync_valid,
           (unsigned long)s_transport.stats.last_time_sync_rtt_us,
           (long)s_transport.stats.esp_time_offset_us,
           (unsigned int)s_transport.stats.peer_role,
           (unsigned int)s_transport.stats.peer_protocol_version,
           (unsigned int)s_transport.stats.peer_spi_mtu,
           (unsigned long)s_transport.stats.peer_capabilities,
           err);

    s_transport.bringup_task = NULL;
    vTaskDelete(NULL);
}

static void start_bringup_task(void)
{
    if (s_transport.bringup_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreate(bringup_task,
                                "m61_esp32_up",
                                1536,
                                NULL,
                                5,
                                &s_transport.bringup_task);
    if (ok != pdPASS) {
        s_transport.bringup_task = NULL;
        s_transport.stats.last_error = -ENOMEM;
    }
}

static int send_payload(uint8_t type,
                        uint16_t flags,
                        uint8_t channel,
                        uint8_t priority,
                        TickType_t deadline_tick,
                        const uint8_t *payload,
                        size_t payload_len)
{
    ds5_dual_spi_header_t header;
    uint8_t frame[DS5_DUAL_SPI_HEADER_LEN + DS5_DUAL_SPI_MAX_PAYLOAD];
    size_t frame_len;
    uint16_t seq;
    uint32_t deadline = 0;
    bool reliable = (flags & DS5_DUAL_FLAG_RELIABLE) != 0;
    uint32_t max_attempts = reliable ?
        (1U + (CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT > 0 ?
               CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT :
               0U)) :
        1U;
    bool ack_failure = false;
    bool peer_ack_status_error = false;
    int err;

    if ((payload == NULL && payload_len != 0) ||
        payload_len > DS5_DUAL_SPI_MAX_PAYLOAD) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }
    if (!s_transport.ready) {
        s_transport.stats.not_ready++;
        s_transport.stats.last_error = -ENOTCONN;
        if (event_log_interesting_counter(s_transport.stats.not_ready)) {
            note_event(M61_ESP32_EVENT_TX_ERROR,
                       type,
                       -ENOTCONN,
                       flags,
                       payload_len);
        }
        return -ENOTCONN;
    }
    if (s_transport.stats.rx_flow_credit > 0 &&
        (flags & DS5_DUAL_FLAG_DROP_OK) != 0) {
        if (!s_transport.stats.last_credit_bt_ready) {
            s_transport.stats.not_ready++;
            s_transport.stats.last_error = -ENOTCONN;
            if (event_log_interesting_counter(s_transport.stats.not_ready)) {
                note_event(M61_ESP32_EVENT_BT_NOT_READY,
                           type,
                           -ENOTCONN,
                           s_transport.stats.peer_bt_flags,
                           s_transport.stats.rx_flow_credit);
            }
            return -ENOTCONN;
        }
        if (s_transport.stats.last_credit_free == 0) {
            s_transport.stats.queue_drop_old++;
            s_transport.stats.last_error = -EAGAIN;
            return -EAGAIN;
        }
    }
    if (deadline_tick != 0 && s_transport.time_sync_valid) {
        TickType_t now_tick = xTaskGetTickCount();
        int32_t delta_ticks = (int32_t)(deadline_tick - now_tick);
        uint32_t local_deadline_us;

        if (delta_ticks <= 0) {
            s_transport.stats.deadline_miss++;
            local_deadline_us = local_time_us();
        } else {
            local_deadline_us = local_time_us() + tick_delta_us(now_tick, deadline_tick);
        }
        deadline = (uint32_t)((int32_t)local_deadline_us + s_transport.esp_time_offset_us);
    }

    frame_len = ds5_dual_spi_frame_len((uint16_t)payload_len);
    seq = next_seq(channel);
    ds5_dual_spi_header_init(&header,
                             type,
                             flags,
                             channel,
                             priority,
                             seq,
                             deadline,
                             (uint16_t)payload_len);
    if (!ds5_dual_spi_finalize_frame(frame, sizeof(frame), &header, payload)) {
        s_transport.stats.frame_errors++;
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }

    if (s_transport.lock != NULL &&
        xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_transport.stats.queue_drop_old++;
        s_transport.stats.last_error = -EBUSY;
        return -EBUSY;
    }
    for (uint32_t attempt = 0; attempt < max_attempts; attempt++) {
        peer_ack_status_error = false;
        err = exchange_frame(frame, frame_len);
        if (err < 0) {
            break;
        }

        s_transport.stats.tx_frames++;
        s_transport.stats.tx_bytes += (uint32_t)frame_len;
        if (!reliable) {
            break;
        }

        uint32_t ack_rx_before = s_transport.stats.rx_ack;
        int ack_result = poll_reliable_ack(seq, type);
        bool got_peer_ack = s_transport.stats.rx_ack != ack_rx_before &&
                            s_transport.stats.last_ack_seq == seq &&
                            s_transport.stats.last_ack_type == type;
        if (ack_result >= 0) {
            err = 0;
            peer_ack_status_error = false;
            break;
        }
        err = ack_result;
        ack_failure = true;
        peer_ack_status_error = got_peer_ack;
        if (attempt + 1U < max_attempts &&
            ack_result_should_retry(ack_result)) {
            s_transport.stats.ack_retries++;
            continue;
        }
        s_transport.stats.ack_failures++;
        break;
    }
    if (s_transport.lock != NULL) {
        xSemaphoreGive(s_transport.lock);
    }
    if (err < 0) {
        s_transport.stats.last_error = err;
        note_event(ack_failure ? M61_ESP32_EVENT_ACK_ERROR : M61_ESP32_EVENT_TX_ERROR,
                   type,
                   err,
                   ((uint32_t)seq << 16) | flags,
                   payload_len);
        if (!peer_ack_status_error) {
            mark_transport_error(err, M61_ESP32_RECOVERY_REASON_TX);
        }
        return err;
    }

    s_transport.stats.last_seq = seq;
    s_transport.stats.last_error = 0;
    mark_transport_success();
    return 0;
}

static int send_hello(void)
{
    ds5_dual_hello_t hello = {
        .protocol_version = DS5_DUAL_SPI_VERSION,
        .role = DS5_DUAL_ROLE_M61_USB,
        .header_len = DS5_DUAL_SPI_HEADER_LEN,
        .max_payload = DS5_DUAL_SPI_MAX_PAYLOAD,
        .spi_mtu = M61_ESP32_SPI_TRANSACTION_LEN,
        .tx_queue_depth = 1,
        .capabilities = DS5_DUAL_CAP_USB_DS5_GADGET |
                        DS5_DUAL_CAP_AUDIO_RT |
                        DS5_DUAL_CAP_FEATURE_REPORTS |
                        DS5_DUAL_CAP_FLOW_CREDIT |
                        DS5_DUAL_CAP_RELIABLE_ACK |
                        DS5_DUAL_CAP_BT_CONNECT_MODES,
    };
    uint8_t payload[DS5_DUAL_HELLO_PAYLOAD_LEN];
    int err;

    if (!ds5_dual_hello_encode(&hello, payload, sizeof(payload))) {
        s_transport.stats.frame_errors++;
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }

    err = send_payload(DS5_DUAL_MSG_HELLO,
                       DS5_DUAL_FLAG_RELIABLE,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       payload,
                       sizeof(payload));
    if (err == 0) {
        s_transport.stats.tx_hello++;
    }
    return err;
}

static int send_time_sync(void)
{
    ds5_dual_time_sync_t sync = {
        .m61_time_us = local_time_us(),
    };
    uint8_t payload[DS5_DUAL_TIME_SYNC_PAYLOAD_LEN];
    uint32_t start_rx_time_sync = s_transport.stats.rx_time_sync;
    int err;

    if (!ds5_dual_time_sync_encode(&sync, payload, sizeof(payload))) {
        s_transport.stats.frame_errors++;
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }

    err = send_payload(DS5_DUAL_MSG_TIME_SYNC,
                       0,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       payload,
                       sizeof(payload));
    if (err < 0) {
        s_transport.stats.time_sync_failures++;
        return err;
    }
    s_transport.stats.tx_time_sync++;

    for (uint32_t i = 0; i < 2U; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (s_transport.lock != NULL &&
            xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
            s_transport.stats.queue_drop_old++;
            s_transport.stats.last_error = -EBUSY;
            s_transport.stats.time_sync_failures++;
            return -EBUSY;
        }
        err = poll_rx_response();
        if (s_transport.lock != NULL) {
            xSemaphoreGive(s_transport.lock);
        }
        if (err < 0) {
            s_transport.stats.last_error = err;
            s_transport.stats.time_sync_failures++;
            mark_transport_error(err, M61_ESP32_RECOVERY_REASON_TIME_SYNC);
            return err;
        }
        if (s_transport.stats.rx_time_sync != start_rx_time_sync) {
            s_transport.stats.last_error = 0;
            return 0;
        }
    }

    s_transport.stats.last_error = -ETIMEDOUT;
    s_transport.stats.time_sync_failures++;
    mark_transport_error(-ETIMEDOUT, M61_ESP32_RECOVERY_REASON_TIME_SYNC);
    return -ETIMEDOUT;
}

void m61_esp32_transport_init(void)
{
    int err;

    memset(&s_transport, 0, sizeof(s_transport));
    s_transport.lock = xSemaphoreCreateMutex();
    err = init_spi_master();
    s_transport.ready = (err == 0);
    if (!s_transport.ready) {
        s_transport.stats.last_error = err;
    }
    if (s_transport.ready) {
        start_bringup_task();
    }
    note_event(M61_ESP32_EVENT_INIT,
               s_transport.ready ? 1U : 0U,
               err,
               CONFIG_M61_ESP32_SPI_FREQ_HZ,
               (uint32_t)CONFIG_M61_ESP32_SPI_CS_PIN);
    printf("M61 ESP32 dual-chip SPI transport %s enable=%u pins=%d/%d/%d/%d ready_pin=%d irq_pin=%d reset_pin=%d rx_poll=%u tsync_ms=%d recov_threshold=%d recov_cooldown_ms=%d err=%d hello=%lu time_sync=%lu sync_fail=%lu sync_valid=%u rtt_us=%lu offset_us=%ld peer_role=%u peer_ver=%u peer_mtu=%u peer_caps=0x%08lx\r\n",
           s_transport.ready ? "ready" : "not ready",
           CONFIG_M61_ESP32_SPI_ENABLE ? 1U : 0U,
           CONFIG_M61_ESP32_SPI_SCLK_PIN,
           CONFIG_M61_ESP32_SPI_MOSI_PIN,
           CONFIG_M61_ESP32_SPI_MISO_PIN,
           CONFIG_M61_ESP32_SPI_CS_PIN,
           CONFIG_M61_ESP32_READY_PIN,
           CONFIG_M61_ESP32_IRQ_PIN,
           CONFIG_M61_ESP32_RESET_PIN,
           CONFIG_M61_ESP32_RX_POLL_ENABLE ? 1U : 0U,
           CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS,
           CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD,
           CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS,
           err,
           (unsigned long)s_transport.stats.rx_hello,
           (unsigned long)s_transport.stats.rx_time_sync,
           (unsigned long)s_transport.stats.time_sync_failures,
           (unsigned int)s_transport.stats.time_sync_valid,
           (unsigned long)s_transport.stats.last_time_sync_rtt_us,
           (long)s_transport.stats.esp_time_offset_us,
           (unsigned int)s_transport.stats.peer_role,
           (unsigned int)s_transport.stats.peer_protocol_version,
           (unsigned int)s_transport.stats.peer_spi_mtu,
           (unsigned long)s_transport.stats.peer_capabilities);
}

bool m61_esp32_transport_ready(void)
{
    return s_transport.ready;
}

bool m61_esp32_transport_bt_ready(void)
{
    if (!s_transport.ready || !peer_link_ready()) {
        return false;
    }
    if ((s_transport.stats.peer_bt_flags & DS5_DUAL_BT_STATE_READY) != 0U) {
        return true;
    }
    return s_transport.stats.rx_flow_credit > 0 &&
           s_transport.stats.last_credit_bt_ready != 0U;
}

void m61_esp32_transport_set_input_callback(m61_esp32_transport_input_cb_t cb,
                                            void *ctx)
{
    s_transport.input_cb = cb;
    s_transport.input_cb_ctx = ctx;
}

void m61_esp32_transport_set_feature_callback(m61_esp32_transport_feature_cb_t cb,
                                              void *ctx)
{
    s_transport.feature_cb = cb;
    s_transport.feature_cb_ctx = ctx;
}

int m61_esp32_transport_send_bt_report(const uint8_t *report,
                                       size_t report_len,
                                       TickType_t deadline_tick,
                                       bool realtime)
{
    uint8_t type = realtime ? DS5_DUAL_MSG_BT_TX_AUDIO_RT : DS5_DUAL_MSG_BT_TX_REPORT;
    uint8_t channel = realtime ? DS5_DUAL_CHANNEL_AUDIO : DS5_DUAL_CHANNEL_OUTPUT;
    uint8_t priority = realtime ? DS5_DUAL_PRIORITY_RT : DS5_DUAL_PRIORITY_HID;
    uint16_t flags = realtime ?
        (DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK) :
        (DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_RELIABLE);
    int err;

    if (report == NULL || report_len == 0) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }
    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }
    if (ds5_dual_spi_report_is_audio_rt(report, report_len)) {
        type = DS5_DUAL_MSG_BT_TX_AUDIO_RT;
        channel = DS5_DUAL_CHANNEL_AUDIO;
        priority = DS5_DUAL_PRIORITY_RT;
        flags = DS5_DUAL_FLAG_LATEST | DS5_DUAL_FLAG_DROP_OK;
    }
    if (!m61_esp32_transport_bt_ready()) {
        s_transport.stats.not_ready++;
        s_transport.stats.last_error = -ENOTCONN;
        if (event_log_interesting_counter(s_transport.stats.not_ready)) {
            note_event(M61_ESP32_EVENT_BT_NOT_READY,
                       type,
                       -ENOTCONN,
                       s_transport.stats.peer_bt_flags,
                       ((uint32_t)s_transport.stats.last_credit_bt_ready << 24) |
                           ((uint32_t)s_transport.stats.last_credit_depth << 8) |
                           s_transport.stats.last_credit_free);
        }
        return -ENOTCONN;
    }

    err = send_payload(type, flags, channel, priority, deadline_tick, report, report_len);
    if (err == 0) {
        if (type == DS5_DUAL_MSG_BT_TX_AUDIO_RT) {
            s_transport.stats.tx_audio_rt++;
        } else {
            s_transport.stats.tx_reports++;
        }
    }
    return err;
}

int m61_esp32_transport_request_feature(uint8_t report_id, uint32_t requested_len)
{
    uint8_t payload[5];
    int err;

    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }

    payload[0] = report_id;
    payload[1] = (uint8_t)(requested_len & 0xFFU);
    payload[2] = (uint8_t)((requested_len >> 8) & 0xFFU);
    payload[3] = (uint8_t)((requested_len >> 16) & 0xFFU);
    payload[4] = (uint8_t)((requested_len >> 24) & 0xFFU);

    err = send_payload(DS5_DUAL_MSG_BT_TX_FEATURE_GET,
                       DS5_DUAL_FLAG_RELIABLE | DS5_DUAL_FLAG_HIDP_CONTROL,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       payload,
                       sizeof(payload));
    if (err == 0) {
        s_transport.stats.tx_feature_get++;
    }
    return err;
}

int m61_esp32_transport_set_feature(uint8_t report_id, const uint8_t *data, size_t len)
{
    uint8_t payload[1 + M61_ESP32_FEATURE_PAYLOAD_MAX];
    int err;

    if (data == NULL || len > M61_ESP32_FEATURE_PAYLOAD_MAX) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }
    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }

    payload[0] = report_id;
    memcpy(payload + 1, data, len);
    err = send_payload(DS5_DUAL_MSG_BT_TX_FEATURE_SET,
                       DS5_DUAL_FLAG_RELIABLE | DS5_DUAL_FLAG_HIDP_CONTROL,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       payload,
                       len + 1);
    if (err == 0) {
        s_transport.stats.tx_feature_set++;
    }
    return err;
}

static int send_bt_connect_request(const uint8_t *payload, size_t len)
{
    int err;

    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }

    err = send_payload(DS5_DUAL_MSG_BT_CONNECT,
                       DS5_DUAL_FLAG_RELIABLE,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       payload,
                       len);
    if (err == 0) {
        s_transport.stats.tx_bt_connect++;
    }
    return err;
}

int m61_esp32_transport_connect(const uint8_t *bda, size_t len)
{
    if (bda != NULL && len != 6U) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }
    if (bda == NULL) {
        len = 0;
    }

    return send_bt_connect_request(bda, len);
}

int m61_esp32_transport_connect_mode(uint8_t mode)
{
    if (mode == DS5_DUAL_BT_CONNECT_AUTO) {
        return m61_esp32_transport_connect(NULL, 0);
    }
    if (mode != DS5_DUAL_BT_CONNECT_SCAN_ONLY &&
        mode != DS5_DUAL_BT_CONNECT_SAVED_ONLY) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }
    if (!peer_link_ready()) {
        s_transport.stats.not_ready++;
        s_transport.stats.last_error = -ENOTCONN;
        return -ENOTCONN;
    }
    if (!peer_supports_bt_connect_modes()) {
        s_transport.stats.last_error = -ENOTSUP;
        return -ENOTSUP;
    }

    return send_bt_connect_request(&mode, sizeof(mode));
}

int m61_esp32_transport_disconnect(bool allow_reconnect)
{
    uint8_t payload = allow_reconnect ? 1U : 0U;
    int err;

    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }

    err = send_payload(DS5_DUAL_MSG_BT_DISCONNECT,
                       DS5_DUAL_FLAG_RELIABLE,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       &payload,
                       sizeof(payload));
    if (err == 0) {
        s_transport.stats.tx_bt_disconnect++;
    }
    return err;
}

int m61_esp32_transport_forget(uint8_t flags)
{
    int err;

    if ((flags & ~(DS5_DUAL_FORGET_SAVED_ADDR | DS5_DUAL_FORGET_BONDS)) != 0 ||
        flags == 0) {
        s_transport.stats.last_error = -EINVAL;
        return -EINVAL;
    }

    err = require_peer_link_ready();
    if (err < 0) {
        return err;
    }

    err = send_payload(DS5_DUAL_MSG_BT_FORGET,
                       DS5_DUAL_FLAG_RELIABLE,
                       DS5_DUAL_CHANNEL_CTRL,
                       DS5_DUAL_PRIORITY_CONTROL,
                       0,
                       &flags,
                       sizeof(flags));
    return err;
}

static int poll_rx_response_locked(void)
{
    int err;

    if (s_transport.lock != NULL &&
        xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_transport.stats.queue_drop_old++;
        s_transport.stats.last_error = -EBUSY;
        return -EBUSY;
    }
    err = poll_rx_response();
    if (s_transport.lock != NULL) {
        xSemaphoreGive(s_transport.lock);
    }
    return err;
}

static int send_wire_test_marker(uint8_t marker)
{
    return send_payload(DS5_DUAL_MSG_WIRE_TEST,
                        DS5_DUAL_FLAG_RELIABLE,
                        DS5_DUAL_CHANNEL_STATUS,
                        DS5_DUAL_PRIORITY_CONTROL,
                        0,
                        &marker,
                        sizeof(marker));
}

static void print_wire_test_check(const char *name, bool ok, const char *detail, int value)
{
    printf("esp32_wire_test %s %s", ok ? "PASS" : "FAIL", name);
    if (detail != NULL) {
        printf(" %s=%d", detail, value);
    }
    printf("\r\n");
}

static void print_wire_test_warn(const char *name, const char *detail, int value)
{
    printf("esp32_wire_test WARN %s", name);
    if (detail != NULL) {
        printf(" %s=%d", detail, value);
    }
    printf("\r\n");
}

static void print_wire_test_hint(const char *hint)
{
    printf("esp32_wire_test HINT %s\r\n", hint);
}

static bool wait_ready_pin_for_wire_test(void)
{
    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(M61_ESP32_WIRE_TEST_READY_WAIT_MS);

    if (optional_ready_pin_active()) {
        return true;
    }

    printf("esp32_wire_test wait ready_pin timeout_ms=%lu hint=reset ESP32 during this window\r\n",
           (unsigned long)M61_ESP32_WIRE_TEST_READY_WAIT_MS);
    do {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (optional_ready_pin_active()) {
            return true;
        }
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    return optional_ready_pin_active();
}

static void poll_wire_test_responses(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    do {
        (void)poll_rx_response_locked();
        vTaskDelay(pdMS_TO_TICKS(2));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
}

static void poll_until_wire_test_stats(uint32_t start_stats, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    do {
        if (s_transport.stats.rx_stats > start_stats &&
            s_transport.stats.peer_stats_role == DS5_DUAL_ROLE_ESP32_BT) {
            return;
        }
        (void)poll_rx_response_locked();
        vTaskDelay(pdMS_TO_TICKS(2));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
}

int m61_esp32_transport_wire_test(void)
{
    uint32_t start_hello;
    uint32_t start_time_sync;
    uint32_t start_stats;
    uint32_t start_ack;
    uint32_t start_ack_failures;
    uint32_t start_crc;
    uint32_t start_frame_errors;
    bool ready_pin_ok;
    bool start_ack_ok;
    bool hello_ok = false;
    bool time_sync_ok = false;
    bool irq_seen = false;
    bool stats_ok = false;
    bool peer_ok;
    bool error_ok;
    bool all_ok = true;
    int err;

    printf("esp32_wire_test start reset_pin=not-used pins=%d/%d/%d/%d ready_pin=%d irq_pin=%d\r\n",
           CONFIG_M61_ESP32_SPI_SCLK_PIN,
           CONFIG_M61_ESP32_SPI_MOSI_PIN,
           CONFIG_M61_ESP32_SPI_MISO_PIN,
           CONFIG_M61_ESP32_SPI_CS_PIN,
           CONFIG_M61_ESP32_READY_PIN,
           CONFIG_M61_ESP32_IRQ_PIN);
    note_event(M61_ESP32_EVENT_WIRE_TEST,
               DS5_DUAL_WIRE_TEST_START,
               0,
               s_transport.ready ? 1U : 0U,
               s_transport.stats.rx_hello);

    if (!s_transport.ready) {
        print_wire_test_check("transport_ready", false, "err", s_transport.stats.last_error);
        note_event(M61_ESP32_EVENT_WIRE_TEST,
                   DS5_DUAL_WIRE_TEST_FAIL,
                   -ENOTCONN,
                   0,
                   s_transport.stats.last_error);
        return -ENOTCONN;
    }

    ready_pin_ok = wait_ready_pin_for_wire_test();
    print_wire_test_check("ready_pin", ready_pin_ok, "level", ready_pin_ok ? 1 : 0);
    if (!ready_pin_ok) {
        print_wire_test_hint("check ESP_READY GPIO32->M61 IO16 first, then common GND and ESP32 left-profile firmware");
    }
    all_ok = all_ok && ready_pin_ok;

    m61_esp32_transport_reset_stats();
    poll_wire_test_responses(50);

    start_hello = s_transport.stats.rx_hello;
    start_time_sync = s_transport.stats.rx_time_sync;
    start_stats = s_transport.stats.rx_stats;
    start_ack = s_transport.stats.rx_ack;
    start_ack_failures = s_transport.stats.ack_failures;
    start_crc = s_transport.stats.crc_errors;
    start_frame_errors = s_transport.stats.frame_errors;

    err = send_wire_test_marker(DS5_DUAL_WIRE_TEST_START);
    if (err != 0 || s_transport.stats.rx_ack == start_ack) {
        poll_wire_test_responses(40);
    }
    start_ack_ok = err == 0 || s_transport.stats.rx_ack > start_ack;
    print_wire_test_check("wire_test_start_ack", start_ack_ok, "err", err);
    if (!start_ack_ok) {
        print_wire_test_hint("if ESP32 LED did not blink, check SCLK/MOSI/CS and ESP32 SPI slave firmware");
    }
    all_ok = all_ok && start_ack_ok;

    err = send_hello();
    if (s_transport.stats.rx_hello == start_hello ||
        s_transport.stats.peer_role != DS5_DUAL_ROLE_ESP32_BT) {
        poll_wire_test_responses(250);
    }
    hello_ok = (err == 0 || s_transport.stats.rx_hello > start_hello) &&
               s_transport.stats.rx_hello > 0 &&
               s_transport.stats.peer_role == DS5_DUAL_ROLE_ESP32_BT &&
               s_transport.stats.peer_protocol_version == DS5_DUAL_SPI_VERSION;
    print_wire_test_check("hello", hello_ok, "err", err);
    if (!hello_ok) {
        print_wire_test_hint("check MISO/readback path and that ESP32 is flashed with dual-chip firmware");
    }
    all_ok = all_ok && hello_ok;

    err = send_time_sync();
    if (s_transport.stats.rx_time_sync == start_time_sync) {
        poll_wire_test_responses(1000);
    }
    time_sync_ok = s_transport.stats.rx_time_sync > start_time_sync &&
                   s_transport.stats.time_sync_valid != 0;
    if (time_sync_ok) {
        print_wire_test_check("time_sync", true, "err", err);
    } else {
        print_wire_test_warn("time_sync", "err", err);
    }
    if (!time_sync_ok) {
        print_wire_test_hint("TIME_SYNC response is delayed; verify ds5 status tsync_rx/sync before changing wires");
    }

    err = send_payload(DS5_DUAL_MSG_STATS,
                       0,
                       DS5_DUAL_CHANNEL_STATUS,
                       DS5_DUAL_PRIORITY_LOW,
                       0,
                       NULL,
                       0);
    if (err == 0) {
        s_transport.stats.tx_stats_request++;
        irq_seen = optional_irq_pin_active();
        poll_until_wire_test_stats(start_stats, 2000);
    }
    stats_ok = err == 0 &&
               s_transport.stats.rx_stats > start_stats &&
               s_transport.stats.peer_stats_role == DS5_DUAL_ROLE_ESP32_BT;
    if (irq_seen) {
        print_wire_test_check("irq_pending", true, "level", 1);
    } else {
        print_wire_test_warn("irq_pending", "level", 0);
    }
    if (stats_ok) {
        print_wire_test_check("stats", true, "err", err);
    } else {
        print_wire_test_warn("stats", "err", err);
    }
    if (!stats_ok) {
        print_wire_test_hint("STATS response is delayed; verify ds5 status stats_rx/esp32_peer_stats before changing wires");
    }

    peer_ok = s_transport.stats.peer_role == DS5_DUAL_ROLE_ESP32_BT &&
              s_transport.stats.peer_protocol_version == DS5_DUAL_SPI_VERSION;
    print_wire_test_check("peer_role", peer_ok, "role", s_transport.stats.peer_role);
    if (!peer_ok) {
        print_wire_test_hint("ESP32 replied with an unexpected role; rebuild and flash the dual-chip profile");
    }
    all_ok = all_ok && peer_ok;

    error_ok = s_transport.stats.ack_failures == start_ack_failures &&
               s_transport.stats.crc_errors == start_crc &&
               s_transport.stats.frame_errors == start_frame_errors;
    print_wire_test_check("crc_frame_errors", error_ok, "crc", (int)(s_transport.stats.crc_errors - start_crc));
    if (!error_ok) {
        print_wire_test_hint("CRC/frame errors usually mean long wires, weak ground, or SPI clock too high");
    }
    all_ok = all_ok && error_ok;

    (void)send_wire_test_marker(all_ok ? DS5_DUAL_WIRE_TEST_PASS : DS5_DUAL_WIRE_TEST_FAIL);
    printf("esp32_wire_test result=%s ready=%u hello=%lu tsync=%lu stats=%lu ack=%lu irq=%u peer_role=%u peer_stats_role=%u\r\n",
           all_ok ? "PASS" : "FAIL",
           s_transport.ready ? 1U : 0U,
           (unsigned long)s_transport.stats.rx_hello,
           (unsigned long)s_transport.stats.rx_time_sync,
           (unsigned long)s_transport.stats.rx_stats,
           (unsigned long)s_transport.stats.rx_ack,
           irq_seen ? 1U : 0U,
           (unsigned int)s_transport.stats.peer_role,
           (unsigned int)s_transport.stats.peer_stats_role);
    note_event(M61_ESP32_EVENT_WIRE_TEST,
               all_ok ? DS5_DUAL_WIRE_TEST_PASS : DS5_DUAL_WIRE_TEST_FAIL,
               all_ok ? 0 : -EIO,
               ((uint32_t)s_transport.stats.rx_ack << 16) |
                   ((uint32_t)s_transport.stats.rx_time_sync & 0xFFFFU),
               ((uint32_t)s_transport.stats.rx_stats << 16) |
                   (uint32_t)s_transport.stats.peer_role);

    return all_ok ? 0 : -EIO;
}

int m61_esp32_transport_request_stats(void)
{
    uint32_t start_rx_stats = s_transport.stats.rx_stats;
    int err;

    err = send_payload(DS5_DUAL_MSG_STATS,
                       0,
                       DS5_DUAL_CHANNEL_STATUS,
                       DS5_DUAL_PRIORITY_LOW,
                       0,
                       NULL,
                       0);
    if (err < 0) {
        return err;
    }
    s_transport.stats.tx_stats_request++;

    for (uint32_t i = 0; i < 2U; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (s_transport.lock != NULL &&
            xSemaphoreTake(s_transport.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
            s_transport.stats.queue_drop_old++;
            s_transport.stats.last_error = -EBUSY;
            return -EBUSY;
        }
        err = poll_rx_response();
        if (s_transport.lock != NULL) {
            xSemaphoreGive(s_transport.lock);
        }
        if (err < 0) {
            s_transport.stats.last_error = err;
            return err;
        }
        if (s_transport.stats.rx_stats != start_rx_stats) {
            s_transport.stats.last_error = 0;
            return 0;
        }
    }

    s_transport.stats.last_error = -ETIMEDOUT;
    return -ETIMEDOUT;
}

void m61_esp32_transport_get_stats(m61_esp32_transport_stats_t *stats)
{
    if (stats != NULL) {
        *stats = s_transport.stats;
    }
}

void m61_esp32_transport_reset_stats(void)
{
    if (s_transport.ready) {
        if (send_payload(DS5_DUAL_MSG_RESET_STATS,
                         DS5_DUAL_FLAG_RELIABLE,
                         DS5_DUAL_CHANNEL_CTRL,
                         DS5_DUAL_PRIORITY_CONTROL,
                         0,
                         NULL,
                         0) == 0) {
            s_transport.stats.tx_reset_stats++;
        }
    }
    memset(&s_transport.stats, 0, sizeof(s_transport.stats));
}

static const char *bt_flags_to_str(uint32_t flags, char *buf, size_t buf_len)
{
    typedef struct {
        uint32_t bit;
        const char *name;
    } bt_flag_name_t;

    static const bt_flag_name_t flag_names[] = {
        { DS5_DUAL_BT_STATE_READY, "ready" },
        { DS5_DUAL_BT_STATE_L2CAP_READY, "l2cap" },
        { DS5_DUAL_BT_STATE_SDP_READY, "sdp" },
        { DS5_DUAL_BT_STATE_CONTROL_OPEN, "ctl" },
        { DS5_DUAL_BT_STATE_INTERRUPT_OPEN, "intr" },
        { DS5_DUAL_BT_STATE_FULL_REPORT, "full" },
        { DS5_DUAL_BT_STATE_CONNECTING, "connecting" },
        { DS5_DUAL_BT_STATE_SDP_SEARCHING, "sdp_search" },
        { DS5_DUAL_BT_STATE_TARGET_FOUND, "target" },
        { DS5_DUAL_BT_STATE_HAVE_SAVED, "saved" },
        { DS5_DUAL_BT_STATE_FROM_SAVED, "from_saved" },
    };
    size_t used = 0;

    if (buf == NULL || buf_len == 0U) {
        return "";
    }

    buf[0] = '\0';
    if (flags == 0U) {
        snprintf(buf, buf_len, "-");
        return buf;
    }

    for (size_t i = 0; i < sizeof(flag_names) / sizeof(flag_names[0]); i++) {
        if ((flags & flag_names[i].bit) == 0U) {
            continue;
        }
        int written = snprintf(buf + used,
                               buf_len - used,
                               "%s%s",
                               used == 0U ? "" : "|",
                               flag_names[i].name);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= buf_len - used) {
            used = buf_len - 1U;
            break;
        }
        used += (size_t)written;
    }

    return buf;
}

void m61_esp32_transport_print_stats(void)
{
    char bt_flags_buf[96];
    uint32_t time_sync_age_ms = s_transport.stats.time_sync_valid ?
        ((local_time_us() - s_transport.stats.last_time_sync_local_us) / 1000U) :
        0U;

    printf("esp32_spi ready=%u tx=%lu bytes=%lu hello_tx=%lu tsync_tx=%lu tsync_fail=%lu tsync_age_ms=%lu recov=%lu recov_ok=%lu recov_fail=%lu recov_skip=%lu recov_suppress=%lu recov_consec=%lu recov_reason=%u audio_rt=%lu reports=%lu fget=%lu fset=%lu bt_conn=%lu bt_disc=%lu rst=%lu stats_req=%lu rx=%lu rx_bytes=%lu hello_rx=%lu tsync_rx=%lu stats_rx=%lu sync=%u rtt_us=%lu offset_us=%ld ack=%lu ack_poll=%lu ack_retry=%lu ack_fail=%lu ack_miss=%lu ack_err=%lu ack_seq=%u ack_type=%u ack_status=%d credit=%lu free=%u/%u bt=%u peer_role=%u peer_ver=%u peer_mtu=%u peer_payload=%u peer_q=%u peer_caps=0x%08lx peer_drop=%lu peer_txerr=%lu peer_last=%d ferr=%lu crc=%lu deadline=%lu drop_old=%lu not_ready=%lu seq=%u last_err=%d\r\n",
           s_transport.ready ? 1U : 0U,
           (unsigned long)s_transport.stats.tx_frames,
           (unsigned long)s_transport.stats.tx_bytes,
           (unsigned long)s_transport.stats.tx_hello,
           (unsigned long)s_transport.stats.tx_time_sync,
           (unsigned long)s_transport.stats.time_sync_failures,
           (unsigned long)time_sync_age_ms,
           (unsigned long)s_transport.stats.recovery_attempts,
           (unsigned long)s_transport.stats.recovery_successes,
           (unsigned long)s_transport.stats.recovery_failures,
           (unsigned long)s_transport.stats.recovery_skipped_no_reset,
           (unsigned long)s_transport.stats.recovery_suppressed,
           (unsigned long)s_transport.stats.recovery_consecutive_errors,
           (unsigned int)s_transport.stats.last_recovery_reason,
           (unsigned long)s_transport.stats.tx_audio_rt,
           (unsigned long)s_transport.stats.tx_reports,
           (unsigned long)s_transport.stats.tx_feature_get,
           (unsigned long)s_transport.stats.tx_feature_set,
           (unsigned long)s_transport.stats.tx_bt_connect,
           (unsigned long)s_transport.stats.tx_bt_disconnect,
           (unsigned long)s_transport.stats.tx_reset_stats,
           (unsigned long)s_transport.stats.tx_stats_request,
           (unsigned long)s_transport.stats.rx_frames,
           (unsigned long)s_transport.stats.rx_bytes,
           (unsigned long)s_transport.stats.rx_hello,
           (unsigned long)s_transport.stats.rx_time_sync,
           (unsigned long)s_transport.stats.rx_stats,
           (unsigned int)s_transport.stats.time_sync_valid,
           (unsigned long)s_transport.stats.last_time_sync_rtt_us,
           (long)s_transport.stats.esp_time_offset_us,
           (unsigned long)s_transport.stats.rx_ack,
           (unsigned long)s_transport.stats.tx_ack_polls,
           (unsigned long)s_transport.stats.ack_retries,
           (unsigned long)s_transport.stats.ack_failures,
           (unsigned long)s_transport.stats.ack_miss,
           (unsigned long)s_transport.stats.ack_status_errors,
           (unsigned int)s_transport.stats.last_ack_seq,
           (unsigned int)s_transport.stats.last_ack_type,
           s_transport.stats.last_ack_status,
           (unsigned long)s_transport.stats.rx_flow_credit,
           (unsigned int)s_transport.stats.last_credit_free,
           (unsigned int)s_transport.stats.last_credit_depth,
           (unsigned int)s_transport.stats.last_credit_bt_ready,
           (unsigned int)s_transport.stats.peer_role,
           (unsigned int)s_transport.stats.peer_protocol_version,
           (unsigned int)s_transport.stats.peer_spi_mtu,
           (unsigned int)s_transport.stats.peer_max_payload,
           (unsigned int)s_transport.stats.peer_tx_queue_depth,
           (unsigned long)s_transport.stats.peer_capabilities,
           (unsigned long)s_transport.stats.peer_queue_drops,
           (unsigned long)s_transport.stats.peer_hidp_tx_errors,
           s_transport.stats.peer_hidp_last_err,
           (unsigned long)s_transport.stats.frame_errors,
           (unsigned long)s_transport.stats.crc_errors,
           (unsigned long)s_transport.stats.deadline_miss,
           (unsigned long)s_transport.stats.queue_drop_old,
           (unsigned long)s_transport.stats.not_ready,
           (unsigned int)s_transport.stats.last_seq,
           s_transport.stats.last_error);
    printf("esp32_bt state_rx=%lu flags=0x%08lx(%s) seq=%u err=%ld rssi=%d bringup=%u reconnect_fail=%u mtu=%u/%u bda=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
           (unsigned long)s_transport.stats.rx_bt_state,
           (unsigned long)s_transport.stats.peer_bt_flags,
           bt_flags_to_str(s_transport.stats.peer_bt_flags,
                           bt_flags_buf,
                           sizeof(bt_flags_buf)),
           (unsigned int)s_transport.stats.peer_bt_state_seq,
           (long)s_transport.stats.peer_bt_last_error,
           (int)s_transport.stats.peer_bt_rssi,
           (unsigned int)s_transport.stats.peer_bt_bringup_attempts,
           (unsigned int)s_transport.stats.peer_bt_reconnect_failures,
           (unsigned int)s_transport.stats.peer_bt_control_mtu,
           (unsigned int)s_transport.stats.peer_bt_interrupt_mtu,
           (unsigned int)s_transport.stats.peer_bt_bda[0],
           (unsigned int)s_transport.stats.peer_bt_bda[1],
           (unsigned int)s_transport.stats.peer_bt_bda[2],
           (unsigned int)s_transport.stats.peer_bt_bda[3],
           (unsigned int)s_transport.stats.peer_bt_bda[4],
           (unsigned int)s_transport.stats.peer_bt_bda[5]);
    printf("esp32_peer_stats role=%u ver=%u uptime_us=%lu spi_rx=%lu spi_tx=%lu spi_crc=%lu spi_drop=%lu tx31=%lu tx32=%lu tx36=%lu fget=%lu fset=%lu txerr=%lu deadline36=%lu rx_input=%lu rx_mic=%lu rx_feature=%lu ack_tx=%lu ack_drop=%lu credit_tx=%lu bt_conn_rx=%lu bt_disc_rx=%lu\r\n",
           (unsigned int)s_transport.stats.peer_stats_role,
           (unsigned int)s_transport.stats.peer_stats_version,
           (unsigned long)s_transport.stats.peer_stats_uptime_us,
           (unsigned long)s_transport.stats.peer_stats_spi_rx_frames,
           (unsigned long)s_transport.stats.peer_stats_spi_tx_frames,
           (unsigned long)s_transport.stats.peer_stats_spi_crc_errors,
           (unsigned long)s_transport.stats.peer_stats_spi_queue_drops,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_31,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_32,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_36,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_feature_get,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_feature_set,
           (unsigned long)s_transport.stats.peer_stats_hidp_tx_errors,
           (unsigned long)s_transport.stats.peer_stats_deadline_miss_36,
           (unsigned long)s_transport.stats.peer_stats_hidp_rx_input,
           (unsigned long)s_transport.stats.peer_stats_hidp_rx_mic_opus,
           (unsigned long)s_transport.stats.peer_stats_hidp_rx_feature_report,
           (unsigned long)s_transport.stats.peer_stats_ack_tx,
           (unsigned long)s_transport.stats.peer_stats_ack_drops,
           (unsigned long)s_transport.stats.peer_stats_flow_credit_tx,
           (unsigned long)s_transport.stats.peer_stats_bt_connect_rx,
           (unsigned long)s_transport.stats.peer_stats_bt_disconnect_rx);
}

void m61_esp32_transport_print_events(bool clear_after_print)
{
    uint32_t total = s_event_seq;
    uint32_t start = total > M61_ESP32_EVENT_LOG_DEPTH ?
        total - M61_ESP32_EVENT_LOG_DEPTH :
        0U;

    printf("esp32_event_log count=%lu total=%lu clear_after=%u\r\n",
           (unsigned long)(total - start),
           (unsigned long)total,
           clear_after_print ? 1U : 0U);
    for (uint32_t seq = start; seq < total; seq++) {
        const m61_esp32_event_t *event =
            &s_event_log[seq % M61_ESP32_EVENT_LOG_DEPTH];

        if (event->type == 0U) {
            continue;
        }
        printf("esp32_event seq=%lu t_us=%lu name=%s type=%u d0=%u err=%d d1=%lu d2=%lu\r\n",
               (unsigned long)seq,
               (unsigned long)event->time_us,
               event_name(event->type),
               (unsigned int)event->type,
               (unsigned int)event->data0,
               (int)event->err,
               (unsigned long)event->data1,
               (unsigned long)event->data2);
    }

    if (clear_after_print) {
        memset(s_event_log, 0, sizeof(s_event_log));
        s_event_seq = 0;
        printf("esp32_event_log cleared\r\n");
    }
}
