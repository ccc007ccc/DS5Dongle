/*
 * bt_ds5_btstack.c — DualSense Classic Bluetooth host on BTstack (ESP32 VHCI).
 *
 * Port of the reference implementation awalol/DS5Dongle src/bt.cpp (Pico W +
 * BTstack) to the ESP32 dual-chip firmware. Implements the existing
 * bt_dualsense_raw_hidp.h API so the SPI coprocessor bridge keeps working.
 *
 * Threading: everything Bluetooth runs on the BTstack FreeRTOS run-loop task.
 * Public API calls coming from SPI tasks are marshalled through a command
 * queue drained via btstack_run_loop_execute_on_main_thread().
 */

#include "bt_dualsense_raw_hidp.h"

#include "sdkconfig.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_port_esp32.h"
#include "classic/sdp_server.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"

#include "dual_chip_spi_proto.h"
#include "led_status.h"

#define MTU_CONTROL 672
#define MTU_INTERRUPT 672

#define DS5_HIDP_DATA_INPUT 0xA1
#define DS5_HIDP_DATA_OUTPUT 0xA2
#define DS5_HIDP_DATA_FEATURE 0xA3
#define DS5_HIDP_GET_REPORT_FEATURE 0x43
#define DS5_HIDP_SET_REPORT_FEATURE 0x53

#define DS5_SEND_FIFO_DEPTH 10
#define DS5_CMD_QUEUE_DEPTH 12
#define DS5_RSSI_POLL_MS 2000
#define DS5_ACL_CREATE_RETRY_MS 20

#ifndef CONFIG_DS5_SCAN_SECONDS
#define CONFIG_DS5_SCAN_SECONDS 30
#endif

#ifndef CONFIG_DS5_RAW_HIDP_AUTO_CONNECT
#define CONFIG_DS5_RAW_HIDP_AUTO_CONNECT 0
#endif

static const char *TAG = "ds5_bt";

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);

/* ---------------------------------------------------------------- CRC32 --
 * Matches awalol/DS5Dongle src/utils.h: standard reflected CRC-32
 * (poly 0xEDB88320). The seeds are the CRC state after hashing the single
 * HIDP prefix byte (0xA2 for output reports, 0x53 for SET_REPORT feature).
 */
#define DS5_CRC_SEED_OUTPUT 0xEADA2D49UL  /* seed equivalent to prefix 0xA2 */
#define DS5_CRC_SEED_FEATURE 0x2060EFC3UL /* seed equivalent to prefix 0x53 */

static uint32_t s_crc_table[256];

static void crc32_table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
        }
        s_crc_table[i] = c;
    }
}

static uint32_t crc32_seeded(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t crc = ~seed;
    while (len--) {
        crc = (crc >> 8) ^ s_crc_table[(crc ^ *data++) & 0xFF];
    }
    return ~crc;
}

/* CRC over the report bytes (after the 0xA2 prefix); stored little-endian in
 * the last 4 bytes of the report, exactly like fill_output_report_checksum. */
static void fill_output_report_checksum(uint8_t *report, size_t len)
{
    uint32_t crc = crc32_seeded(report, len - 4, DS5_CRC_SEED_OUTPUT);
    report[len - 4] = (uint8_t)(crc >> 0);
    report[len - 3] = (uint8_t)(crc >> 8);
    report[len - 2] = (uint8_t)(crc >> 16);
    report[len - 1] = (uint8_t)(crc >> 24);
}

static void fill_feature_report_checksum(uint8_t *data, size_t len)
{
    uint32_t crc = crc32_seeded(data, len - 4, DS5_CRC_SEED_FEATURE);
    data[len - 4] = (uint8_t)(crc >> 0);
    data[len - 3] = (uint8_t)(crc >> 8);
    data[len - 2] = (uint8_t)(crc >> 16);
    data[len - 1] = (uint8_t)(crc >> 24);
}

/* ------------------------------------------------------------- BT state -- */

typedef struct {
    size_t len;
    uint8_t data[MTU_INTERRUPT + 1];
} send_element_t;

typedef enum {
    BT_CMD_CONNECT = 1,
    BT_CMD_DISCONNECT,
    BT_CMD_FORGET,
    BT_CMD_SEND_REPORT,
    BT_CMD_FEATURE_GET,
    BT_CMD_FEATURE_SET,
} bt_cmd_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t mode;      /* CONNECT: DS5_DUAL_BT_CONNECT_*  FORGET: flags */
    bool flag;         /* DISCONNECT: allow_reconnect */
    bool has_bda;
    uint8_t bda[6];
    uint16_t len;      /* SEND/FEATURE_SET payload length */
    uint8_t report_id; /* FEATURE_GET / FEATURE_SET */
    uint8_t data[MTU_INTERRUPT];
} bt_cmd_t;

static btstack_packet_callback_registration_t s_hci_cb_reg;
static btstack_packet_callback_registration_t s_l2cap_cb_reg;
static btstack_context_callback_registration_t s_cmd_drain_reg;
static btstack_timer_source_t s_rssi_timer;
static btstack_timer_source_t s_acl_create_retry_timer;
static btstack_timer_source_t s_acl_cancel_retry_timer;
static btstack_timer_source_t s_acl_disconnect_retry_timer;
static btstack_timer_source_t s_acl_accept_retry_timer;

static QueueHandle_t s_cmd_queue;
static volatile bool s_cmd_drain_pending;

static bd_addr_t s_current_addr;
static bd_addr_t s_saved_addr;
static bool s_have_saved;
static bool s_device_found;
static bool s_new_pair; /* we initiated: create L2CAP channels ourselves */
static bool s_inquiring;
static bool s_acl_pending;
static bool s_acl_outgoing_pending;
static bool s_connect_from_saved;
static bool s_reconnect_allowed = true;
static bool s_discovery_enabled;
static bool s_abort_link;
static bool s_hci_disconnect_pending;
static bool s_forget_pending;
static bool s_acl_create_retry_scheduled;
static bool s_acl_cancel_requested;
static bool s_acl_cancel_pending;
static bool s_acl_cancel_retry_scheduled;
static bool s_acl_disconnect_requested;
static bool s_acl_disconnect_retry_scheduled;
static bool s_acl_accept_requested;
static bool s_acl_accept_pending;
static bool s_acl_accept_retry_scheduled;
static uint8_t s_acl_disconnect_reason;
static hci_con_handle_t s_acl_handle = HCI_CON_HANDLE_INVALID;
static uint16_t s_control_cid;
static uint16_t s_interrupt_cid;
static bool s_stack_ready;

/* send FIFO for the interrupt channel, drained on CAN_SEND_NOW */
static send_element_t s_send_fifo[DS5_SEND_FIFO_DEPTH];
static uint8_t s_send_head;
static uint8_t s_send_count;

/* state snapshot shared with SPI tasks */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bt_dualsense_raw_hidp_state_t s_state;

static bt_dualsense_raw_hidp_rx_cb_t s_rx_cb;
static void *s_rx_cb_ctx;
static bt_dualsense_raw_hidp_state_cb_t s_state_cb;
static void *s_state_cb_ctx;

void bt_dualsense_raw_hidp_note_tx(const uint8_t *data, size_t len, int status);

/* ------------------------------------------------------------ NVS store -- */

#define DS5_NVS_NAMESPACE "ds5bt"
#define DS5_NVS_KEY_ADDR "saved_bda"

static void saved_addr_load(void)
{
    nvs_handle_t h;
    if (nvs_open(DS5_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved_addr);
    if (nvs_get_blob(h, DS5_NVS_KEY_ADDR, s_saved_addr, &len) == ESP_OK &&
        len == sizeof(s_saved_addr)) {
        s_have_saved = true;
        ESP_LOGI(TAG, "Saved controller: %s", bd_addr_to_str(s_saved_addr));
    }
    nvs_close(h);
}

static void saved_addr_store(const bd_addr_t addr)
{
    if (s_have_saved && bd_addr_cmp(addr, s_saved_addr) == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(DS5_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed, controller address not saved");
        return;
    }
    esp_err_t err =
        nvs_set_blob(h, DS5_NVS_KEY_ADDR, addr, sizeof(bd_addr_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        bd_addr_copy(s_saved_addr, addr);
        s_have_saved = true;
        ESP_LOGI(TAG, "Controller address saved: %s", bd_addr_to_str(addr));
    } else {
        ESP_LOGE(TAG, "Controller address save failed: %s",
                 esp_err_to_name(err));
    }
    nvs_close(h);
}

static void saved_addr_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(DS5_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_erase_key(h, DS5_NVS_KEY_ADDR);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Controller address erase failed: %s",
                     esp_err_to_name(err));
        }
        nvs_close(h);
    }
    s_have_saved = false;
    memset(s_saved_addr, 0, sizeof(s_saved_addr));
    ESP_LOGI(TAG, "Saved controller address erased");
}

/* -------------------------------------------------------- state reports -- */

static void state_publish_locked_fields(uint32_t set, uint32_t clear, int32_t err)
{
    bt_dualsense_raw_hidp_state_t snapshot;
    bt_dualsense_raw_hidp_state_cb_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_state_mux);
    s_state.flags = (s_state.flags | set) & ~clear;
    if (s_have_saved) {
        s_state.flags |= DS5_DUAL_BT_STATE_HAVE_SAVED;
    } else {
        s_state.flags &= ~DS5_DUAL_BT_STATE_HAVE_SAVED;
    }
    if (err != 0) {
        s_state.last_error = err;
    }
    s_state.control_mtu = s_control_cid ? MTU_CONTROL : 0;
    s_state.interrupt_mtu = s_interrupt_cid ? MTU_INTERRUPT : 0;
    memcpy(s_state.bda, s_current_addr, sizeof(s_state.bda));
    s_state.state_seq++;
    snapshot = s_state;
    callback = s_state_cb;
    callback_ctx = s_state_cb_ctx;
    portEXIT_CRITICAL(&s_state_mux);

    if (callback != NULL) {
        callback(&snapshot, esp_timer_get_time(), callback_ctx);
    }
}

#define STATE_SET(bits) state_publish_locked_fields((bits), 0, 0)
#define STATE_CLEAR(bits) state_publish_locked_fields(0, (bits), 0)

/* ------------------------------------------------------------- TX paths -- */

static bool send_fifo_push(const uint8_t *data, size_t len)
{
    if (s_send_count >= DS5_SEND_FIFO_DEPTH || len > sizeof(s_send_fifo[0].data)) {
        return false;
    }
    send_element_t *slot =
        &s_send_fifo[(s_send_head + s_send_count) % DS5_SEND_FIFO_DEPTH];
    memcpy(slot->data, data, len);
    slot->len = len;
    s_send_count++;
    return true;
}

static bool send_fifo_pop(send_element_t *out)
{
    if (s_send_count == 0) {
        return false;
    }
    *out = s_send_fifo[s_send_head];
    s_send_head = (s_send_head + 1) % DS5_SEND_FIFO_DEPTH;
    s_send_count--;
    return true;
}

static void send_fifo_clear(void)
{
    s_send_head = 0;
    s_send_count = 0;
}

/* bt_write() from the reference: 0xA2 prefix + report + CRC32 in the last
 * four bytes of the report, queued for CAN_SEND_NOW on the interrupt cid. */
static int bt_write_report(const uint8_t *report, size_t len)
{
    if (s_interrupt_cid == 0) {
        return -ENOTCONN;
    }
    if (len < 5 || len + 1 > sizeof(s_send_fifo[0].data)) {
        return -EINVAL;
    }

    uint8_t packet[MTU_INTERRUPT + 1];
    packet[0] = DS5_HIDP_DATA_OUTPUT;
    memcpy(packet + 1, report, len);
    fill_output_report_checksum(packet + 1, len);

    if (!send_fifo_push(packet, len + 1)) {
        ESP_LOGW(TAG, "send FIFO full, output report dropped");
        return -ENOBUFS;
    }
    if (s_send_count == 1) {
        l2cap_request_can_send_now_event(s_interrupt_cid);
    }
    return 0;
}

static int bt_feature_get(uint8_t report_id)
{
    if (s_control_cid == 0) {
        return -ENOTCONN;
    }
    uint8_t frame[2] = { DS5_HIDP_GET_REPORT_FEATURE, report_id };
    uint8_t status = l2cap_send(s_control_cid, frame, sizeof(frame));
    return status == ERROR_CODE_SUCCESS ? 0 : -EIO;
}

static int bt_feature_set(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (s_control_cid == 0) {
        return -ENOTCONN;
    }
    if (len < 4 || len + 2 > MTU_CONTROL) {
        return -EINVAL;
    }
    uint8_t frame[MTU_CONTROL];
    frame[0] = DS5_HIDP_SET_REPORT_FEATURE;
    frame[1] = report_id;
    memcpy(frame + 2, data, len);
    fill_feature_report_checksum(frame + 1, len + 1);
    uint8_t status = l2cap_send(s_control_cid, frame, len + 2);
    return status == ERROR_CODE_SUCCESS ? 0 : -EIO;
}

/* init_feature() from the reference: prefetch calibration/version/pairing
 * feature reports so the USB side can answer host GET_FEATURE quickly, and
 * probe 0x70 to detect a DualSense Edge. Responses (0xA3 ...) flow to the
 * M61 through the normal control-channel RX path. */
static void init_feature_prefetch(void)
{
    static const uint8_t ids[] = { 0x09, 0x20, 0x22, 0x05, 0x70 };
    for (size_t i = 0; i < sizeof(ids); i++) {
        (void)bt_feature_get(ids[i]);
    }
}

/* update_state() from the reference: enable the lightbar right after the
 * HID channels open (FadeOut animation, bright, DS5Dongle gold). */
static void send_connect_led_state(void)
{
    uint8_t pkt[142] = {0};
    pkt[0] = 0x32;
    pkt[1] = 0x10;
    pkt[2] = 0x90;
    pkt[3] = 0x3f;
    /* SetStateData at offset 4 (see reference utils.h):
     * byte1 bit2 AllowLedColor, byte38 bits0-1 brightness/fade-allow,
     * byte41 LightFadeAnimation=FadeOut, byte42 LightBrightness=Bright,
     * bytes 44..46 RGB. */
    pkt[4 + 1] = 0x04;
    pkt[4 + 38] = 0x03;
    pkt[4 + 41] = 2;
    pkt[4 + 42] = 0;
    pkt[4 + 44] = 0xFF;
    pkt[4 + 45] = 0xD7;
    pkt[4 + 46] = 0x00;
    (void)bt_write_report(pkt, sizeof(pkt));
}

/* --------------------------------------------------- connect state flow -- */

static void apply_reconnect_visibility(void)
{
    const int visible = s_reconnect_allowed && !s_abort_link &&
                        !s_forget_pending &&
                        s_acl_handle == HCI_CON_HANDLE_INVALID &&
                        !s_acl_pending;

    gap_connectable_control(visible);
    gap_discoverable_control(visible);
}

static void set_reconnect_policy(bool allow_reconnect, bool enable_discovery)
{
    s_reconnect_allowed = allow_reconnect;
    s_discovery_enabled = allow_reconnect && enable_discovery;
    if (!s_discovery_enabled) {
        s_device_found = false;
    }
    apply_reconnect_visibility();
}

static void request_inquiry_stop(const char *reason)
{
    const int status = gap_inquiry_stop();

    if (status != ERROR_CODE_SUCCESS &&
        status != ERROR_CODE_COMMAND_DISALLOWED) {
        ESP_LOGW(TAG, "Inquiry stop failed reason=%s status=0x%02X",
                 reason != NULL ? reason : "policy", status);
    }
}

static void stop_inquiry_for_policy(void)
{
    s_device_found = false;
    if (s_inquiring) {
        request_inquiry_stop("policy");
    }
}

static void try_send_acl_cancel(void);
static void try_send_acl_disconnect(void);
static void try_send_acl_accept(void);
static void handle_disconnected(uint8_t reason);

static void request_acl_disconnect(uint8_t reason)
{
    if (s_acl_handle == HCI_CON_HANDLE_INVALID ||
        s_hci_disconnect_pending || s_acl_disconnect_requested) {
        return;
    }

    s_acl_disconnect_requested = true;
    s_acl_disconnect_reason = reason;
    try_send_acl_disconnect();
}

static void cancel_scheduled_acl_create(void)
{
    if (!s_acl_create_retry_scheduled) {
        return;
    }

    btstack_run_loop_remove_timer(&s_acl_create_retry_timer);
    s_acl_create_retry_scheduled = false;
}

static void cancel_scheduled_acl_cancel(void)
{
    if (!s_acl_cancel_retry_scheduled) {
        return;
    }

    btstack_run_loop_remove_timer(&s_acl_cancel_retry_timer);
    s_acl_cancel_retry_scheduled = false;
}

static void cancel_scheduled_acl_disconnect(void)
{
    if (!s_acl_disconnect_retry_scheduled) {
        return;
    }

    btstack_run_loop_remove_timer(&s_acl_disconnect_retry_timer);
    s_acl_disconnect_retry_scheduled = false;
}

static void cancel_scheduled_acl_accept(void)
{
    if (!s_acl_accept_retry_scheduled) {
        return;
    }

    btstack_run_loop_remove_timer(&s_acl_accept_retry_timer);
    s_acl_accept_retry_scheduled = false;
}

static void cancel_pending_outgoing_acl(void)
{
    if (!s_acl_pending || !s_new_pair) {
        return;
    }

    if (!s_acl_outgoing_pending) {
        cancel_scheduled_acl_create();
        s_acl_cancel_requested = false;
        s_acl_pending = false;
        s_new_pair = false;
        return;
    }

    if (s_acl_cancel_pending) {
        return;
    }

    s_acl_cancel_requested = true;
    try_send_acl_cancel();
}

static void cancel_unsent_incoming_acl(void)
{
    if (!s_acl_pending || s_new_pair || !s_acl_accept_requested ||
        s_acl_accept_pending) {
        return;
    }

    cancel_scheduled_acl_accept();
    s_acl_accept_requested = false;
    s_acl_pending = false;
}

static void start_inquiry(void)
{
    if (!s_reconnect_allowed || !s_discovery_enabled || s_abort_link ||
        s_forget_pending || s_inquiring || s_acl_pending ||
        s_acl_handle != HCI_CON_HANDLE_INVALID) {
        return;
    }

    const int status = gap_inquiry_start(CONFIG_DS5_SCAN_SECONDS);
    if (status != ERROR_CODE_SUCCESS) {
        ESP_LOGW(TAG, "Start inquiry rejected status=0x%02X", status);
        state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING,
                                    -status);
        return;
    }

    ESP_LOGI(TAG, "Start inquiry (%u x 1.28s)",
             (unsigned)CONFIG_DS5_SCAN_SECONDS);
    s_device_found = false;
    s_connect_from_saved = false;
    s_inquiring = true;
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    state_publish_locked_fields(DS5_DUAL_BT_STATE_CONNECTING,
                                DS5_DUAL_BT_STATE_TARGET_FOUND |
                                    DS5_DUAL_BT_STATE_FROM_SAVED,
                                0);
}

static void wait_for_controller_page(const char *reason)
{
    if (s_acl_pending || s_acl_handle != HCI_CON_HANDLE_INVALID) {
        return;
    }

    s_abort_link = false;
    s_forget_pending = false;
    set_reconnect_policy(true, false);
    stop_inquiry_for_policy();
    if (s_have_saved) {
        bd_addr_copy(s_current_addr, s_saved_addr);
    } else {
        memset(s_current_addr, 0, sizeof(s_current_addr));
    }
    s_new_pair = false;
    s_connect_from_saved = s_have_saved;
    apply_reconnect_visibility();
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "Waiting for controller page reason=%s saved=%d",
             reason != NULL ? reason : "auto",
             s_have_saved ? 1 : 0);
    STATE_SET(DS5_DUAL_BT_STATE_CONNECTING |
              (s_have_saved ? (DS5_DUAL_BT_STATE_TARGET_FOUND |
                               DS5_DUAL_BT_STATE_FROM_SAVED) : 0));
}

static void start_auto_connect(const char *reason)
{
    s_abort_link = false;
    s_forget_pending = false;
    set_reconnect_policy(true, true);
    if (s_have_saved) {
        bd_addr_copy(s_current_addr, s_saved_addr);
    }
    ESP_LOGI(TAG, "Auto connect: inquiry + incoming page reason=%s saved=%d",
             reason != NULL ? reason : "auto", s_have_saved ? 1 : 0);
    start_inquiry();
}

static void try_send_acl_create(void);

static void acl_create_retry_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    s_acl_create_retry_scheduled = false;
    try_send_acl_create();
}

static void schedule_acl_create_retry(void)
{
    if (s_acl_create_retry_scheduled || !s_acl_pending ||
        s_acl_outgoing_pending) {
        return;
    }

    s_acl_create_retry_scheduled = true;
    btstack_run_loop_set_timer_handler(&s_acl_create_retry_timer,
                                       acl_create_retry_handler);
    btstack_run_loop_set_timer(&s_acl_create_retry_timer,
                               DS5_ACL_CREATE_RETRY_MS);
    btstack_run_loop_add_timer(&s_acl_create_retry_timer);
}

static void acl_cancel_retry_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    s_acl_cancel_retry_scheduled = false;
    try_send_acl_cancel();
}

static void schedule_acl_cancel_retry(void)
{
    if (s_acl_cancel_retry_scheduled || !s_acl_cancel_requested) {
        return;
    }

    s_acl_cancel_retry_scheduled = true;
    btstack_run_loop_set_timer_handler(&s_acl_cancel_retry_timer,
                                       acl_cancel_retry_handler);
    btstack_run_loop_set_timer(&s_acl_cancel_retry_timer,
                               DS5_ACL_CREATE_RETRY_MS);
    btstack_run_loop_add_timer(&s_acl_cancel_retry_timer);
}

static void try_send_acl_cancel(void)
{
    if (!s_acl_cancel_requested || s_acl_cancel_pending || !s_acl_pending ||
        !s_new_pair || !s_acl_outgoing_pending) {
        s_acl_cancel_requested = false;
        return;
    }

    if (!hci_can_send_command_packet_now()) {
        schedule_acl_cancel_retry();
        return;
    }

    const uint8_t status =
        hci_send_cmd(&hci_create_connection_cancel, s_current_addr);
    if (!s_acl_cancel_requested || !s_acl_pending || !s_new_pair) {
        s_acl_cancel_requested = false;
        return;
    }
    if (status == ERROR_CODE_SUCCESS) {
        s_acl_cancel_requested = false;
        s_acl_cancel_pending = true;
        ESP_LOGI(TAG, "ACL create cancel command sent");
        return;
    }
    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        schedule_acl_cancel_retry();
        return;
    }

    s_acl_cancel_requested = false;
    ESP_LOGW(TAG, "ACL create cancel submit failed status=0x%02X", status);
    state_publish_locked_fields(0, 0, -(int32_t)status);
}

static void acl_disconnect_retry_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    s_acl_disconnect_retry_scheduled = false;
    try_send_acl_disconnect();
}

static void schedule_acl_disconnect_retry(void)
{
    if (s_acl_disconnect_retry_scheduled || !s_acl_disconnect_requested) {
        return;
    }

    s_acl_disconnect_retry_scheduled = true;
    btstack_run_loop_set_timer_handler(&s_acl_disconnect_retry_timer,
                                       acl_disconnect_retry_handler);
    btstack_run_loop_set_timer(&s_acl_disconnect_retry_timer,
                               DS5_ACL_CREATE_RETRY_MS);
    btstack_run_loop_add_timer(&s_acl_disconnect_retry_timer);
}

static void try_send_acl_disconnect(void)
{
    if (!s_acl_disconnect_requested || s_hci_disconnect_pending ||
        s_acl_handle == HCI_CON_HANDLE_INVALID) {
        s_acl_disconnect_requested = false;
        return;
    }

    if (!hci_can_send_command_packet_now()) {
        schedule_acl_disconnect_retry();
        return;
    }

    const hci_con_handle_t handle = s_acl_handle;
    const uint8_t status =
        hci_send_cmd(&hci_disconnect, handle, s_acl_disconnect_reason);
    if (!s_acl_disconnect_requested || s_acl_handle != handle ||
        s_acl_handle == HCI_CON_HANDLE_INVALID) {
        s_acl_disconnect_requested = false;
        return;
    }
    if (status == ERROR_CODE_SUCCESS) {
        s_acl_disconnect_requested = false;
        s_hci_disconnect_pending = true;
        ESP_LOGI(TAG, "ACL disconnect command sent handle=0x%04X", handle);
        return;
    }
    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        schedule_acl_disconnect_retry();
        return;
    }

    s_acl_disconnect_requested = false;
    ESP_LOGW(TAG, "ACL disconnect submit failed status=0x%02X", status);
    state_publish_locked_fields(0, 0, -(int32_t)status);
}

static void try_send_acl_create(void)
{
    if (!s_acl_pending || !s_new_pair || s_acl_outgoing_pending ||
        s_acl_handle != HCI_CON_HANDLE_INVALID) {
        return;
    }
    if (s_abort_link || s_forget_pending || !s_reconnect_allowed) {
        s_acl_pending = false;
        s_new_pair = false;
        return;
    }

    if (!hci_can_send_command_packet_now()) {
        ESP_LOGI(TAG, "ACL create waits for HCI command buffer");
        schedule_acl_create_retry();
        return;
    }

    const uint8_t status =
        hci_send_cmd(&hci_create_connection, s_current_addr,
                     hci_usable_acl_packet_types(), 0, 0, 0, 1);
    if (!s_acl_pending || !s_new_pair ||
        s_acl_handle != HCI_CON_HANDLE_INVALID) {
        return;
    }
    if (status == ERROR_CODE_SUCCESS) {
        s_acl_outgoing_pending = true;
        ESP_LOGI(TAG, "ACL create command sent to %s",
                 bd_addr_to_str(s_current_addr));
        return;
    }
    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        ESP_LOGI(TAG, "ACL create waits for HCI command buffer");
        schedule_acl_create_retry();
        return;
    }

    ESP_LOGW(TAG, "ACL create submit failed status=0x%02X", status);
    s_device_found = false;
    s_new_pair = false;
    s_acl_pending = false;
    s_acl_outgoing_pending = false;
    state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING, -status);
    start_auto_connect("create-submit-failed");
}

static void acl_accept_retry_handler(btstack_timer_source_t *ts)
{
    (void)ts;
    s_acl_accept_retry_scheduled = false;
    try_send_acl_accept();
}

static void schedule_acl_accept_retry(void)
{
    if (s_acl_accept_retry_scheduled || !s_acl_accept_requested ||
        s_acl_accept_pending) {
        return;
    }

    s_acl_accept_retry_scheduled = true;
    btstack_run_loop_set_timer_handler(&s_acl_accept_retry_timer,
                                       acl_accept_retry_handler);
    btstack_run_loop_set_timer(&s_acl_accept_retry_timer,
                               DS5_ACL_CREATE_RETRY_MS);
    btstack_run_loop_add_timer(&s_acl_accept_retry_timer);
}

static void try_send_acl_accept(void)
{
    if (!s_acl_accept_requested || s_acl_accept_pending || !s_acl_pending ||
        s_new_pair || s_acl_handle != HCI_CON_HANDLE_INVALID) {
        s_acl_accept_requested = false;
        return;
    }
    if (s_abort_link || s_forget_pending || !s_reconnect_allowed) {
        s_acl_accept_requested = false;
        s_acl_pending = false;
        return;
    }
    if (!hci_can_send_command_packet_now()) {
        schedule_acl_accept_retry();
        return;
    }

    const uint8_t status =
        hci_send_cmd(&hci_accept_connection_request, s_current_addr, 0x01);
    if (!s_acl_accept_requested || !s_acl_pending ||
        s_acl_handle != HCI_CON_HANDLE_INVALID) {
        s_acl_accept_requested = false;
        return;
    }
    if (status == ERROR_CODE_SUCCESS) {
        s_acl_accept_requested = false;
        s_acl_accept_pending = true;
        ESP_LOGI(TAG, "ACL accept command sent to %s",
                 bd_addr_to_str(s_current_addr));
        return;
    }
    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        schedule_acl_accept_retry();
        return;
    }

    ESP_LOGW(TAG, "ACL accept submit failed status=0x%02X", status);
    s_acl_accept_requested = false;
    s_acl_pending = false;
    state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING, -status);
    handle_disconnected(status);
}

static void create_connection_to(const bd_addr_t addr, bool from_saved)
{
    s_abort_link = false;
    s_forget_pending = false;
    set_reconnect_policy(true, false);
    bd_addr_copy(s_current_addr, addr);
    s_new_pair = true; /* we initiate → we create the L2CAP channels */
    s_acl_pending = true;
    s_acl_outgoing_pending = false;
    s_connect_from_saved = from_saved;
    ESP_LOGI(TAG, "Connecting to %s%s", bd_addr_to_str(addr),
             from_saved ? " (saved)" : "");
    STATE_SET(DS5_DUAL_BT_STATE_CONNECTING |
              (from_saved ? DS5_DUAL_BT_STATE_FROM_SAVED : 0));
    try_send_acl_create();
}

static bool decode_inquiry_result(uint8_t event_type, const uint8_t *packet,
                                  bd_addr_t addr, uint32_t *cod)
{
    switch (event_type) {
    case GAP_EVENT_INQUIRY_RESULT:
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        *cod = gap_event_inquiry_result_get_class_of_device(packet);
        return true;
    case HCI_EVENT_INQUIRY_RESULT:
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        *cod = hci_event_inquiry_result_get_class_of_device(packet);
        return true;
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
        *cod =
            hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
        return true;
    case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
        hci_event_extended_inquiry_response_get_bd_addr(packet, addr);
        *cod =
            hci_event_extended_inquiry_response_get_class_of_device(packet);
        return true;
    default:
        return false;
    }
}

static void handle_inquiry_result(uint8_t event_type, const uint8_t *packet)
{
    bd_addr_t addr;
    uint32_t cod;

    if (!s_reconnect_allowed || !s_discovery_enabled || s_abort_link ||
        s_forget_pending || !s_inquiring || s_device_found || s_acl_pending ||
        !decode_inquiry_result(event_type, packet, addr, &cod)) {
        return;
    }

    ESP_LOGI(TAG, "Inquiry result event=0x%02X addr=%s CoD=0x%06X",
             event_type, bd_addr_to_str(addr), (unsigned)cod);
    if ((cod & 0x000F00) != 0x000500) {
        return;
    }

    ESP_LOGI(TAG, "Gamepad found: %s (CoD 0x%06X)",
             bd_addr_to_str(addr), (unsigned)cod);
    bd_addr_copy(s_current_addr, addr);
    s_device_found = true;
    STATE_SET(DS5_DUAL_BT_STATE_TARGET_FOUND);
    request_inquiry_stop("target-found");
}

static void rssi_poll_handler(btstack_timer_source_t *ts)
{
    if (s_acl_handle != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(s_acl_handle);
    }
    btstack_run_loop_set_timer(ts, DS5_RSSI_POLL_MS);
    btstack_run_loop_add_timer(ts);
}

static void open_hid_channels(const char *reason)
{
    if (s_acl_handle == HCI_CON_HANDLE_INVALID || s_control_cid != 0) {
        return;
    }

    ESP_LOGI(TAG, "Open HID L2CAP channels reason=%s addr=%s",
             reason != NULL ? reason : "?",
             bd_addr_to_str(s_current_addr));
    l2cap_create_channel(l2cap_packet_handler, s_current_addr,
                         PSM_HID_CONTROL, MTU_CONTROL, &s_control_cid);
}

static void handle_disconnected(uint8_t reason)
{
    ESP_LOGI(TAG, "Disconnected reason=0x%02X", reason);
    cancel_scheduled_acl_create();
    cancel_scheduled_acl_cancel();
    cancel_scheduled_acl_disconnect();
    cancel_scheduled_acl_accept();
    s_acl_cancel_requested = false;
    s_acl_cancel_pending = false;
    s_acl_disconnect_requested = false;
    s_acl_accept_requested = false;
    s_acl_accept_pending = false;
    s_device_found = false;
    s_new_pair = false;
    s_acl_pending = false;
    s_acl_outgoing_pending = false;
    s_abort_link = false;
    s_hci_disconnect_pending = false;
    s_forget_pending = false;
    s_acl_handle = HCI_CON_HANDLE_INVALID;
    s_control_cid = 0;
    s_interrupt_cid = 0;
    send_fifo_clear();
    apply_reconnect_visibility();
    led_status_set(s_reconnect_allowed ? DS5_LED_STATE_BT_CONNECTING
                                       : DS5_LED_STATE_BOOT_OK);
    portENTER_CRITICAL(&s_state_mux);
    s_state.rssi = 0;
    portEXIT_CRITICAL(&s_state_mux);
    state_publish_locked_fields(0,
                                DS5_DUAL_BT_STATE_CONTROL_OPEN |
                                DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                                DS5_DUAL_BT_STATE_FULL_REPORT |
                                DS5_DUAL_BT_STATE_CONNECTING |
                                DS5_DUAL_BT_STATE_TARGET_FOUND |
                                DS5_DUAL_BT_STATE_FROM_SAVED,
                                0);
    if (s_reconnect_allowed && s_discovery_enabled) {
        start_inquiry();
    }
}

static void recover_security_failure(hci_con_handle_t handle,
                                     uint8_t status,
                                     const char *stage)
{
    const bool retry_discovery = s_new_pair;
    const int32_t error = status != ERROR_CODE_SUCCESS ?
                          -(int32_t)status : -EACCES;

    ESP_LOGW(TAG, "%s failed status=0x%02X; drop stale key and retry",
             stage, status);
    gap_drop_link_key_for_bd_addr(s_current_addr);
    s_device_found = false;
    s_new_pair = false;
    s_abort_link = true;
    s_forget_pending = false;
    set_reconnect_policy(true, retry_discovery);
    state_publish_locked_fields(0,
                                DS5_DUAL_BT_STATE_CONTROL_OPEN |
                                DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                                DS5_DUAL_BT_STATE_FULL_REPORT,
                                error);

    if (handle != HCI_CON_HANDLE_INVALID) {
        s_acl_handle = handle;
    }
    if (s_acl_handle != HCI_CON_HANDLE_INVALID) {
        request_acl_disconnect(ERROR_CODE_AUTHENTICATION_FAILURE);
    } else if (!s_acl_pending) {
        handle_disconnected(ERROR_CODE_AUTHENTICATION_FAILURE);
    }
}

/* -------------------------------------------------------- HCI handler ---- */

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    const uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
    case BTSTACK_EVENT_STATE: {
        const uint8_t state = btstack_event_state_get_state(packet);
        if (state != HCI_STATE_WORKING) {
            break;
        }
        bd_addr_t local;
        gap_local_bd_addr(local);
        ESP_LOGI(TAG, "BTstack up at %s", bd_addr_to_str(local));
        s_stack_ready = true;
        STATE_SET(DS5_DUAL_BT_STATE_READY |
                  DS5_DUAL_BT_STATE_L2CAP_READY |
                  DS5_DUAL_BT_STATE_SDP_READY);

        btstack_run_loop_set_timer(&s_rssi_timer, DS5_RSSI_POLL_MS);
        btstack_run_loop_set_timer_handler(&s_rssi_timer, rssi_poll_handler);
        btstack_run_loop_add_timer(&s_rssi_timer);

#if CONFIG_DS5_RAW_HIDP_AUTO_CONNECT
        start_auto_connect("boot-auto");
#endif
        break;
    }

    /* awalol/DS5Dongle handles the controller's raw inquiry events. Keep the
     * GAP event as a fallback because BTstack normally emits it first, but do
     * not depend on that synthesis: some controller/transport combinations
     * only expose the raw event reliably. handle_inquiry_result() is
     * idempotent across both forms. */
    case GAP_EVENT_INQUIRY_RESULT:
    case HCI_EVENT_INQUIRY_RESULT:
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
    case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
        handle_inquiry_result(event_type, packet);
        break;

    case GAP_EVENT_INQUIRY_COMPLETE:
    case HCI_EVENT_INQUIRY_COMPLETE:
        /* ESP32 VHCI can deliver only the raw HCI completion after
         * gap_inquiry_stop(). BTstack can also synthesize the GAP event, so
         * s_inquiring is the one-shot guard against a duplicate create. */
        if (!s_inquiring) {
            break;
        }
        ESP_LOGI(TAG, "Inquiry complete event=0x%02X", event_type);
        s_inquiring = false;
        if (!s_reconnect_allowed || !s_discovery_enabled || s_abort_link ||
            s_forget_pending) {
            s_device_found = false;
            apply_reconnect_visibility();
            break;
        }
        if (s_acl_pending || s_acl_handle != HCI_CON_HANDLE_INVALID) {
            s_device_found = false;
            break;
        }
        if (s_device_found) {
            s_device_found = false;
            create_connection_to(s_current_addr, false);
            break;
        }
        /* Match awalol/DS5Dongle: an empty inquiry ends the dongle-initiated
         * pairing window. Page scan stays enabled for PS-only controller-
         * initiated reconnects; a fresh PS+Create pairing window is started
         * explicitly with BT_CONNECT scan/auto (M61 `ds5 scan`). */
        apply_reconnect_visibility();
        break;

    case HCI_EVENT_COMMAND_STATUS: {
        const uint8_t status = hci_event_command_status_get_status(packet);
        const uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
        if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION &&
            status != ERROR_CODE_SUCCESS) {
            ESP_LOGW(TAG, "Create connection rejected status=0x%02X", status);
            cancel_scheduled_acl_create();
            s_device_found = false;
            s_new_pair = false;
            s_acl_pending = false;
            s_acl_outgoing_pending = false;
            state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING, -status);
            if (s_abort_link || !s_reconnect_allowed) {
                handle_disconnected(status);
            } else {
                start_auto_connect("create-rejected");
            }
        }
        if (opcode == HCI_OPCODE_HCI_DISCONNECT &&
            status != ERROR_CODE_SUCCESS) {
            ESP_LOGW(TAG, "Disconnect rejected status=0x%02X", status);
            cancel_scheduled_acl_disconnect();
            s_acl_disconnect_requested = false;
            s_hci_disconnect_pending = false;
            state_publish_locked_fields(0, 0, -(int32_t)status);
        }
        if (opcode == HCI_OPCODE_HCI_ACCEPT_CONNECTION_REQUEST &&
            status != ERROR_CODE_SUCCESS) {
            ESP_LOGW(TAG, "Accept connection rejected status=0x%02X", status);
            cancel_scheduled_acl_accept();
            s_acl_accept_requested = false;
            s_acl_accept_pending = false;
            s_acl_pending = false;
            state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING,
                                        -(int32_t)status);
            handle_disconnected(status);
        }
        break;
    }

    case HCI_EVENT_COMMAND_COMPLETE: {
        const uint16_t opcode =
            hci_event_command_complete_get_command_opcode(packet);
        if (opcode == HCI_OPCODE_HCI_CREATE_CONNECTION_CANCEL) {
            const uint8_t *return_params =
                hci_event_command_complete_get_return_parameters(packet);
            const uint8_t status = return_params[0];

            cancel_scheduled_acl_cancel();
            s_acl_cancel_requested = false;
            s_acl_cancel_pending = false;

            if (status == ERROR_CODE_SUCCESS) {
                ESP_LOGI(TAG, "Pending ACL create cancelled");
                s_acl_pending = false;
                s_acl_outgoing_pending = false;
                s_new_pair = false;
                if (s_abort_link || !s_reconnect_allowed) {
                    handle_disconnected(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
                }
            } else {
                ESP_LOGW(TAG, "ACL create cancel failed status=0x%02X", status);
                state_publish_locked_fields(0, 0, -(int32_t)status);
            }
        }
        break;
    }

    case HCI_EVENT_CONNECTION_COMPLETE: {
        const uint8_t status = hci_event_connection_complete_get_status(packet);
        const bool connection_was_pending = s_acl_pending;
        cancel_scheduled_acl_create();
        cancel_scheduled_acl_cancel();
        cancel_scheduled_acl_accept();
        s_acl_cancel_requested = false;
        s_acl_cancel_pending = false;
        s_acl_accept_requested = false;
        s_acl_accept_pending = false;
        s_acl_pending = false;
        s_acl_outgoing_pending = false;
        if (status == ERROR_CODE_SUCCESS) {
            s_acl_handle =
                hci_event_connection_complete_get_connection_handle(packet);
            hci_event_connection_complete_get_bd_addr(packet, s_current_addr);
            ESP_LOGI(TAG, "ACL connected %s handle=0x%04X",
                     bd_addr_to_str(s_current_addr), s_acl_handle);
            portENTER_CRITICAL(&s_state_mux);
            s_state.rssi = 0;
            portEXIT_CRITICAL(&s_state_mux);
            if (!connection_was_pending || s_abort_link || s_forget_pending ||
                !s_reconnect_allowed) {
                ESP_LOGI(TAG, "ACL completed after shutdown policy; disconnect");
                s_abort_link = true;
                request_acl_disconnect(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
            } else {
                gap_request_security_level(s_acl_handle, LEVEL_2);
            }
        } else {
            if (!connection_was_pending) {
                ESP_LOGI(TAG, "Ignore late cancelled ACL completion status=0x%02X",
                         status);
                break;
            }
            ESP_LOGW(TAG, "ACL connect failed status=0x%02X", status);
            s_device_found = false;
            s_new_pair = false;
            state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING, -status);
            if (s_abort_link || !s_reconnect_allowed) {
                handle_disconnected(status);
            } else {
                start_auto_connect("acl-failed");
            }
        }
        break;
    }

    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        ESP_LOGI(TAG, "SSP user confirmation from %s: accept",
                 bd_addr_to_str(addr));
        hci_send_cmd(&hci_user_confirmation_request_reply, addr);
        break;
    }

    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        ESP_LOGI(TAG, "Legacy PIN request from %s: reply 0000",
                 bd_addr_to_str(addr));
        gap_pin_code_response(addr, "0000");
        break;
    }

    case HCI_EVENT_AUTHENTICATION_COMPLETE: {
        const uint8_t status =
            hci_event_authentication_complete_get_status(packet);
        const hci_con_handle_t handle =
            hci_event_authentication_complete_get_connection_handle(packet);
        ESP_LOGI(TAG, "Authentication complete status=0x%02X", status);
        if (status != ERROR_CODE_SUCCESS && !s_abort_link) {
            recover_security_failure(handle, status, "authentication");
        }
        break;
    }

    case HCI_EVENT_ENCRYPTION_CHANGE: {
        const uint8_t status = hci_event_encryption_change_get_status(packet);
        const uint8_t enabled =
            hci_event_encryption_change_get_encryption_enabled(packet);
        ESP_LOGI(TAG, "Encryption change status=0x%02X enabled=%u",
                 status, enabled);
        if ((status != ERROR_CODE_SUCCESS || !enabled) && !s_abort_link) {
            recover_security_failure(
                hci_event_encryption_change_get_connection_handle(packet),
                status,
                "encryption");
        }
        break;
    }

    case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
        const uint8_t status =
            hci_event_encryption_change_v2_get_status(packet);
        const uint8_t enabled =
            hci_event_encryption_change_v2_get_encryption_enabled(packet);
        const uint8_t key_size =
            hci_event_encryption_change_v2_get_encryption_key_size(packet);
        ESP_LOGI(TAG, "Encryption change v2 status=0x%02X enabled=%u key_size=%u",
                 status, enabled, key_size);
        if ((status != ERROR_CODE_SUCCESS || !enabled) && !s_abort_link) {
            recover_security_failure(
                hci_event_encryption_change_v2_get_connection_handle(packet),
                status,
                "encryption-v2");
        }
        break;
    }

    case GAP_EVENT_SECURITY_LEVEL: {
        const hci_con_handle_t handle =
            gap_event_security_level_get_handle(packet);
        const gap_security_level_t level =
            (gap_security_level_t)
                gap_event_security_level_get_security_level(packet);
        const uint8_t status = gap_event_security_level_get_status(packet);
        ESP_LOGI(TAG, "Security level status=0x%02X level=%u handle=0x%04X",
                 status, (unsigned)level, handle);
        if (handle != s_acl_handle) {
            ESP_LOGW(TAG, "Ignore security event for stale handle=0x%04X", handle);
            break;
        }
        if (status != ERROR_CODE_SUCCESS || level < LEVEL_2) {
            if (!s_abort_link) {
                recover_security_failure(handle, status, "security-level");
            }
        } else if (s_new_pair) {
            open_hid_channels("security-level");
        }
        break;
    }

    case HCI_EVENT_CONNECTION_REQUEST: {
        bd_addr_t addr;
        bool accept_incoming;
        hci_event_connection_request_get_bd_addr(packet, addr);
        const uint32_t cod =
            hci_event_connection_request_get_class_of_device(packet);
        ESP_LOGI(TAG, "Incoming ACL from %s cod=0x%06X",
                 bd_addr_to_str(addr), (unsigned)cod);
        accept_incoming = (cod & 0x000F00) == 0x000500 &&
                          s_reconnect_allowed && !s_abort_link &&
                          !s_forget_pending;
        if (accept_incoming && s_acl_pending) {
            if (s_new_pair && !s_acl_outgoing_pending) {
                cancel_scheduled_acl_create();
                s_acl_pending = false;
                s_new_pair = false;
            } else {
                accept_incoming = false;
            }
        }
        if (accept_incoming) {
            bd_addr_copy(s_current_addr, addr);
            s_device_found = false;
            s_discovery_enabled = false;
            s_new_pair = false;
            s_acl_pending = true;
            s_acl_outgoing_pending = false;
            s_acl_accept_requested = true;
            s_acl_accept_pending = false;
            s_connect_from_saved =
                s_have_saved && bd_addr_cmp(addr, s_saved_addr) == 0;
            request_inquiry_stop("incoming-acl");
            STATE_SET(DS5_DUAL_BT_STATE_CONNECTING |
                      (s_connect_from_saved ? DS5_DUAL_BT_STATE_FROM_SAVED : 0));
            try_send_acl_accept();
        } else {
            ESP_LOGI(TAG, "Reject incoming ACL from %s by connection policy",
                     bd_addr_to_str(addr));
            hci_send_cmd(&hci_reject_connection_request, addr,
                         ERROR_CODE_CONNECTION_REJECTED_DUE_TO_UNACCEPTABLE_BD_ADDR);
        }
        break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        const uint8_t status =
            hci_event_disconnection_complete_get_status(packet);
        const uint8_t reason =
            hci_event_disconnection_complete_get_reason(packet);
        if (status != ERROR_CODE_SUCCESS) {
            ESP_LOGW(TAG,
                     "Disconnection complete failed status=0x%02X reason=0x%02X",
                     status, reason);
            s_hci_disconnect_pending = false;
            state_publish_locked_fields(0, 0, -(int32_t)status);
            break;
        }
        handle_disconnected(reason);
        break;
    }

    case GAP_EVENT_RSSI_MEASUREMENT: {
        const hci_con_handle_t handle =
            gap_event_rssi_measurement_get_con_handle(packet);
        if (handle == s_acl_handle) {
            portENTER_CRITICAL(&s_state_mux);
            s_state.rssi =
                (int8_t)gap_event_rssi_measurement_get_rssi(packet);
            portEXIT_CRITICAL(&s_state_mux);
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------ L2CAP handler ---- */

static void l2cap_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size)
{
    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel == s_interrupt_cid) {
            if (size > 15 && packet[0] == DS5_HIDP_DATA_INPUT &&
                packet[1] == 0x31) {
                portENTER_CRITICAL(&s_state_mux);
                bool first = !(s_state.flags & DS5_DUAL_BT_STATE_FULL_REPORT);
                portEXIT_CRITICAL(&s_state_mux);
                if (first) {
                    STATE_SET(DS5_DUAL_BT_STATE_FULL_REPORT);
                }
            }
            if (s_rx_cb != NULL) {
                s_rx_cb(packet, size, esp_timer_get_time(), s_rx_cb_ctx);
            }
        } else if (channel == s_control_cid) {
            if (s_rx_cb != NULL) {
                s_rx_cb(packet, size, esp_timer_get_time(), s_rx_cb_ctx);
            }
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case L2CAP_EVENT_CHANNEL_OPENED: {
        const uint8_t status = l2cap_event_channel_opened_get_status(packet);
        const uint16_t local_cid =
            l2cap_event_channel_opened_get_local_cid(packet);
        const uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
        if (status != ERROR_CODE_SUCCESS) {
            ESP_LOGW(TAG, "L2CAP open failed psm=0x%04X status=0x%02X",
                     psm, status);
            s_control_cid = 0;
            s_interrupt_cid = 0;
            s_device_found = false;
            s_abort_link = true;
            s_forget_pending = false;
            /* Match upstream ownership after a failed HID open: do not keep
             * paging a bonded controller. Leave page scan enabled so the
             * controller can reconnect and initiate its HID channels. */
            set_reconnect_policy(true, false);
            state_publish_locked_fields(0, DS5_DUAL_BT_STATE_CONNECTING,
                                        -status);
            if (status != L2CAP_CONNECTION_BASEBAND_DISCONNECT) {
                request_acl_disconnect(
                    ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
            }
            break;
        }
        if (psm == PSM_HID_CONTROL) {
            ESP_LOGI(TAG, "HID Control opened cid=0x%04X", local_cid);
            s_control_cid = local_cid;
            STATE_SET(DS5_DUAL_BT_STATE_CONTROL_OPEN);
            /* our outgoing pairing path opens interrupt after control */
            if (s_new_pair && s_interrupt_cid == 0) {
                l2cap_create_channel(l2cap_packet_handler, s_current_addr,
                                     PSM_HID_INTERRUPT, MTU_INTERRUPT,
                                     &s_interrupt_cid);
            }
        } else if (psm == PSM_HID_INTERRUPT) {
            ESP_LOGI(TAG, "HID Interrupt opened cid=0x%04X", local_cid);
            s_interrupt_cid = local_cid;
            if (!s_abort_link && !s_forget_pending && s_reconnect_allowed) {
                saved_addr_store(s_current_addr);
            } else {
                ESP_LOGI(TAG, "Skip address save while link shutdown is pending");
                request_acl_disconnect(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
                break;
            }
            s_discovery_enabled = false;
            led_status_set(DS5_LED_STATE_BT_CONNECTED);
            gap_connectable_control(0);
            gap_discoverable_control(0);
            state_publish_locked_fields(
                DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                    (s_connect_from_saved ? DS5_DUAL_BT_STATE_FROM_SAVED : 0),
                DS5_DUAL_BT_STATE_CONNECTING, 0);
            init_feature_prefetch();
            send_connect_led_state();
        }
        break;
    }

    case L2CAP_EVENT_INCOMING_CONNECTION: {
        const uint16_t local_cid =
            l2cap_event_incoming_connection_get_local_cid(packet);
        const uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
        ESP_LOGI(TAG, "L2CAP incoming psm=0x%04X cid=0x%04X", psm, local_cid);
        if (s_abort_link || s_forget_pending || !s_reconnect_allowed) {
            ESP_LOGI(TAG, "Decline L2CAP incoming by connection policy");
            l2cap_decline_connection(local_cid);
        } else {
            l2cap_accept_connection(local_cid);
        }
        break;
    }

    case L2CAP_EVENT_CHANNEL_CLOSED: {
        const uint16_t local_cid =
            l2cap_event_channel_closed_get_local_cid(packet);
        if (local_cid == s_control_cid) {
            s_control_cid = 0;
            STATE_CLEAR(DS5_DUAL_BT_STATE_CONTROL_OPEN);
        } else if (local_cid == s_interrupt_cid) {
            s_interrupt_cid = 0;
            send_fifo_clear();
            STATE_CLEAR(DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                        DS5_DUAL_BT_STATE_FULL_REPORT);
        }
        if (s_control_cid == 0 && s_interrupt_cid == 0 &&
            s_acl_handle != HCI_CON_HANDLE_INVALID) {
            s_abort_link = true;
            set_reconnect_policy(true, false);
            request_acl_disconnect(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        }
        break;
    }

    case L2CAP_EVENT_CAN_SEND_NOW: {
        send_element_t element;
        if (send_fifo_pop(&element)) {
            uint8_t status =
                l2cap_send(s_interrupt_cid, element.data, element.len);
            bt_dualsense_raw_hidp_note_tx(element.data, element.len,
                                          status == ERROR_CODE_SUCCESS
                                              ? 0 : -EIO);
            if (status != ERROR_CODE_SUCCESS) {
                ESP_LOGW(TAG, "l2cap_send failed status=0x%02X", status);
            }
        }
        if (s_send_count > 0 && s_interrupt_cid != 0) {
            l2cap_request_can_send_now_event(s_interrupt_cid);
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------- command queue (cross-task) -- */

static void cmd_execute(const bt_cmd_t *cmd)
{
    switch (cmd->kind) {
    case BT_CMD_CONNECT:
        if (s_acl_handle != HCI_CON_HANDLE_INVALID || s_acl_pending ||
            s_abort_link || s_hci_disconnect_pending || s_forget_pending) {
            state_publish_locked_fields(0, 0, -EBUSY);
            break;
        }
        if (cmd->has_bda) {
            create_connection_to(cmd->bda, false);
        } else if (cmd->mode == DS5_DUAL_BT_CONNECT_SAVED_ONLY) {
            wait_for_controller_page("saved-only");
        } else if (cmd->mode == DS5_DUAL_BT_CONNECT_SCAN_ONLY) {
            start_inquiry();
        } else { /* AUTO */
            start_auto_connect("auto");
        }
        break;

    case BT_CMD_DISCONNECT:
        s_abort_link = true;
        s_forget_pending = false;
        set_reconnect_policy(cmd->flag, false);
        stop_inquiry_for_policy();
        cancel_pending_outgoing_acl();
        cancel_unsent_incoming_acl();
        state_publish_locked_fields(0,
                                    DS5_DUAL_BT_STATE_CONTROL_OPEN |
                                    DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                                    DS5_DUAL_BT_STATE_FULL_REPORT |
                                    DS5_DUAL_BT_STATE_CONNECTING |
                                    DS5_DUAL_BT_STATE_TARGET_FOUND |
                                    DS5_DUAL_BT_STATE_FROM_SAVED,
                                    0);
        request_acl_disconnect(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        if (s_acl_handle == HCI_CON_HANDLE_INVALID && !s_acl_pending) {
            handle_disconnected(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        }
        break;

    case BT_CMD_FORGET:
        s_abort_link = true;
        s_forget_pending = true;
        set_reconnect_policy(false, false);
        stop_inquiry_for_policy();
        cancel_pending_outgoing_acl();
        cancel_unsent_incoming_acl();
        if (cmd->mode & DS5_DUAL_FORGET_BONDS) {
            gap_delete_all_link_keys();
            ESP_LOGI(TAG, "All link keys deleted");
        }
        if (cmd->mode & DS5_DUAL_FORGET_SAVED_ADDR) {
            saved_addr_erase();
        }
        state_publish_locked_fields(0,
                                    DS5_DUAL_BT_STATE_CONTROL_OPEN |
                                    DS5_DUAL_BT_STATE_INTERRUPT_OPEN |
                                    DS5_DUAL_BT_STATE_FULL_REPORT |
                                    DS5_DUAL_BT_STATE_CONNECTING |
                                    DS5_DUAL_BT_STATE_TARGET_FOUND |
                                    DS5_DUAL_BT_STATE_FROM_SAVED,
                                    0);
        request_acl_disconnect(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        if (s_acl_handle == HCI_CON_HANDLE_INVALID && !s_acl_pending) {
            handle_disconnected(ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION);
        }
        break;

    case BT_CMD_SEND_REPORT: {
        int err = bt_write_report(cmd->data, cmd->len);
        if (err != 0) {
            state_publish_locked_fields(0, 0, err);
        }
        break;
    }

    case BT_CMD_FEATURE_GET:
        (void)bt_feature_get(cmd->report_id);
        break;

    case BT_CMD_FEATURE_SET:
        (void)bt_feature_set(cmd->report_id, cmd->data, cmd->len);
        break;

    default:
        break;
    }
}

static void cmd_drain(void *context)
{
    (void)context;
    s_cmd_drain_pending = false;
    bt_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdPASS) {
        cmd_execute(&cmd);
    }
}

static int cmd_submit(const bt_cmd_t *cmd)
{
    if (s_cmd_queue == NULL || !s_stack_ready) {
        return -EAGAIN;
    }
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdPASS) {
        return -ENOBUFS;
    }
    if (!s_cmd_drain_pending) {
        s_cmd_drain_pending = true;
        btstack_run_loop_execute_on_main_thread(&s_cmd_drain_reg);
    }
    return 0;
}

/* ------------------------------------------------------------ public API -- */

/* All BTstack setup MUST run on the same task that later executes the run
 * loop: btstack_run_loop_freertos_init() captures the current task handle
 * for its wake-up notifications, so initializing from app_main and running
 * the loop from another task leaves the run loop deaf to VHCI events (it
 * would only wake on timer expiry — seconds of HCI latency, dead inquiry,
 * timed-out L2CAP setup). This was the primary "connects but never opens
 * HID channels" failure. */
static void btstack_task(void *arg)
{
    (void)arg;

    if (btstack_init() != ERROR_CODE_SUCCESS) {
        ESP_LOGE(TAG, "btstack_init failed");
        vTaskDelete(NULL);
        return;
    }

    /* L2CAP + SDP: registering the HID services is what makes controller
     * initiated reconnects work (reference: bt_l2cap_init). */
    s_l2cap_cb_reg.callback = l2cap_packet_handler;
    l2cap_add_event_handler(&s_l2cap_cb_reg);
    sdp_init();
    l2cap_register_service(l2cap_packet_handler, PSM_HID_CONTROL,
                           MTU_CONTROL, LEVEL_2);
    l2cap_register_service(l2cap_packet_handler, PSM_HID_INTERRUPT,
                           MTU_INTERRUPT, LEVEL_2);
    l2cap_init();

    gap_ssp_set_enable(1);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_page_scan_activity(0x0012, 0x0012); /* 11.25 ms interlaced */
    gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
    gap_set_local_name("DS5Dongle");
    gap_connectable_control(1);
    gap_discoverable_control(1);

    s_hci_cb_reg.callback = hci_packet_handler;
    hci_add_event_handler(&s_hci_cb_reg);

    hci_power_control(HCI_POWER_ON);

    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "BTstack DualSense host started (saved=%d)", s_have_saved);

    btstack_run_loop_execute(); /* never returns */
}

esp_err_t bt_dualsense_raw_hidp_start(void)
{
    crc32_table_init();
    saved_addr_load();
    state_publish_locked_fields(0, 0, 0);

    s_cmd_queue = xQueueCreate(DS5_CMD_QUEUE_DEPTH, sizeof(bt_cmd_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_cmd_drain_reg.callback = cmd_drain;

    if (xTaskCreatePinnedToCore(btstack_task, "btstack", 6144, NULL,
                                configMAX_PRIORITIES - 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool bt_dualsense_raw_hidp_ready(void)
{
    portENTER_CRITICAL(&s_state_mux);
    bool ready = (s_state.flags & DS5_DUAL_BT_STATE_INTERRUPT_OPEN) != 0;
    portEXIT_CRITICAL(&s_state_mux);
    return ready;
}

void bt_dualsense_raw_hidp_get_state(bt_dualsense_raw_hidp_state_t *state)
{
    if (state == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_state_mux);
    *state = s_state;
    portEXIT_CRITICAL(&s_state_mux);
}

int bt_dualsense_raw_hidp_connect(const uint8_t *bda, size_t len, uint8_t mode)
{
    bt_cmd_t cmd = { .kind = BT_CMD_CONNECT, .mode = mode };
    if (bda != NULL && len >= 6) {
        cmd.has_bda = true;
        memcpy(cmd.bda, bda, 6);
    }
    return cmd_submit(&cmd);
}

int bt_dualsense_raw_hidp_disconnect(bool allow_reconnect)
{
    bt_cmd_t cmd = { .kind = BT_CMD_DISCONNECT, .flag = allow_reconnect };
    return cmd_submit(&cmd);
}

int bt_dualsense_raw_hidp_forget(uint8_t flags)
{
    bt_cmd_t cmd = { .kind = BT_CMD_FORGET, .mode = flags };
    return cmd_submit(&cmd);
}

int bt_dualsense_raw_hidp_send_report(const uint8_t *report,
                                      size_t report_len,
                                      bool realtime)
{
    (void)realtime;
    if (report == NULL || report_len == 0 ||
        report_len > sizeof(((bt_cmd_t *)0)->data)) {
        return -EINVAL;
    }
    if (!bt_dualsense_raw_hidp_ready()) {
        return -ENOTCONN;
    }
    bt_cmd_t cmd = { .kind = BT_CMD_SEND_REPORT, .len = (uint16_t)report_len };
    memcpy(cmd.data, report, report_len);
    return cmd_submit(&cmd);
}

int bt_dualsense_raw_hidp_get_feature(uint8_t report_id)
{
    bt_cmd_t cmd = { .kind = BT_CMD_FEATURE_GET, .report_id = report_id };
    return cmd_submit(&cmd);
}

int bt_dualsense_raw_hidp_set_feature(uint8_t report_id,
                                      const uint8_t *data,
                                      size_t len)
{
    if (data == NULL || len == 0 || len > sizeof(((bt_cmd_t *)0)->data)) {
        return -EINVAL;
    }
    bt_cmd_t cmd = { .kind = BT_CMD_FEATURE_SET,
                     .report_id = report_id,
                     .len = (uint16_t)len };
    memcpy(cmd.data, data, len);
    return cmd_submit(&cmd);
}

void bt_dualsense_raw_hidp_set_rx_callback(bt_dualsense_raw_hidp_rx_cb_t cb,
                                           void *ctx)
{
    s_rx_cb = cb;
    s_rx_cb_ctx = ctx;
}

void bt_dualsense_raw_hidp_set_state_callback(
    bt_dualsense_raw_hidp_state_cb_t cb, void *ctx)
{
    bt_dualsense_raw_hidp_state_t snapshot;

    portENTER_CRITICAL(&s_state_mux);
    s_state_cb = cb;
    s_state_cb_ctx = ctx;
    snapshot = s_state;
    portEXIT_CRITICAL(&s_state_mux);

    if (cb != NULL) {
        cb(&snapshot, esp_timer_get_time(), ctx);
    }
}
