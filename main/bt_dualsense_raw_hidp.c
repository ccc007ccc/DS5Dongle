#include "bt_dualsense_raw_hidp.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/unistd.h>

#include "dualsense_output.h"
#include "dualsense_parser.h"
#include "dual_chip_spi_proto.h"
#include "led_status.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_l2cap_bt_api.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#ifndef CONFIG_DS5_SCAN_SECONDS
#define CONFIG_DS5_SCAN_SECONDS 30
#endif

#ifdef CONFIG_DS5_ACCEPT_ANY_GAMEPAD
#define DS5_ACCEPT_ANY_GAMEPAD 1
#else
#define DS5_ACCEPT_ANY_GAMEPAD 0
#endif

#ifndef CONFIG_DS5_LOG_UNCHANGED_MS
#define CONFIG_DS5_LOG_UNCHANGED_MS 1000
#endif

#ifndef CONFIG_DS5_BRINGUP_RETRIES
#define CONFIG_DS5_BRINGUP_RETRIES 8
#endif

#ifndef CONFIG_DS5_BRINGUP_RETRY_MS
#define CONFIG_DS5_BRINGUP_RETRY_MS 500
#endif

#ifdef CONFIG_DS5_RAW_HIDP_AUTO_CONNECT
#define DS5_RAW_HIDP_AUTO_CONNECT 1
#else
#define DS5_RAW_HIDP_AUTO_CONNECT 0
#endif

#define DS5_RAW_LOCAL_NAME "DS5Bridge-ESP32-Raw"
#define DS5_NVS_NAMESPACE "ds5bt"
#define DS5_NVS_LAST_BDA_KEY "last_bda"
#define DS5_NVS_BLACKLIST_KEY "blacklist"
#define DS5_HIDP_PSM_CONTROL 0x0011
#define DS5_HIDP_PSM_INTERRUPT 0x0013
#define DS5_HIDP_SERVICE_UUID 0x1124
#define DS5_HIDP_SET_PROTOCOL_REPORT 0x71
#define DS5_HIDP_GET_REPORT 0x40
#define DS5_HIDP_SET_REPORT 0x50
#define DS5_HIDP_DATA_OUTPUT 0xA2
#define DS5_HIDP_REPORT_TYPE_OUTPUT 0x02
#define DS5_HIDP_REPORT_TYPE_FEATURE 0x03
#define DS5_RAW_RX_TASK_STACK 4096
#define DS5_RAW_RX_BUFFER_LEN 768
#define DS5_RAW_RECONNECT_DELAY_US (2 * 1000 * 1000)
#define DS5_RAW_MAX_SAVED_RECONNECT_FAILURES 3
#define DS5_BLACKLIST_MAX 8

typedef enum {
    RAW_CH_CONTROL = 0,
    RAW_CH_INTERRUPT = 1,
} raw_hidp_channel_id_t;

typedef struct {
    raw_hidp_channel_id_t id;
    const char *name;
    uint16_t psm;
    int fd;
    uint32_t handle;
    int32_t tx_mtu;
    bool connected;
    TaskHandle_t task;
} raw_hidp_channel_t;

typedef enum {
    RAW_CONNECT_IDLE = 0,
    RAW_CONNECT_CONTROL,
    RAW_CONNECT_INTERRUPT,
} raw_connect_step_t;

typedef enum {
    RAW_TARGET_NONE = 0,
    RAW_TARGET_SAVED_AUTO,
    RAW_TARGET_DISCOVERY,
    RAW_TARGET_MANUAL,
} raw_target_origin_t;

static const char *TAG = "ds5_raw_hidp";

static esp_bd_addr_t s_target_bda;
static esp_bd_addr_t s_saved_bda;
static bool s_have_saved_bda;
static bool s_target_found;
static bool s_l2cap_ready;
static bool s_sdp_ready;
static bool s_connecting;
static bool s_sdp_searching;
static bool s_current_target_from_saved;
static raw_target_origin_t s_target_origin;
static bool s_auto_reconnect_enabled = true;
static bool s_have_last_state;
static bool s_have_full_report;
static uint8_t s_bringup_attempts;
static uint8_t s_saved_reconnect_failures;
static uint16_t s_state_seq;
static int8_t s_last_rssi;
static int32_t s_last_error;
static int64_t s_last_state_log_us;
static dualsense_state_t s_last_state;
static dualsense_output_context_t s_output_ctx;
static raw_connect_step_t s_pending_step;
static esp_bd_addr_t s_blacklist[DS5_BLACKLIST_MAX];
static uint8_t s_blacklist_count;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_bringup_timer;
static bt_dualsense_raw_hidp_rx_cb_t s_rx_cb;
static void *s_rx_cb_ctx;
static bt_dualsense_raw_hidp_state_cb_t s_state_cb;
static void *s_state_cb_ctx;

static raw_hidp_channel_t s_control = {
    .id = RAW_CH_CONTROL,
    .name = "control",
    .psm = DS5_HIDP_PSM_CONTROL,
    .fd = -1,
};

static raw_hidp_channel_t s_interrupt = {
    .id = RAW_CH_INTERRUPT,
    .name = "interrupt",
    .psm = DS5_HIDP_PSM_INTERRUPT,
    .fd = -1,
};

static void start_connect_flow(void);
static void start_discovery(void);
static void connect_hidp_control(void);
static void connect_hidp_interrupt(void);
static void send_bringup(const char *reason);
static void notify_state(void);
static void stop_bringup_timer(void);

static const char *bda_to_str(const esp_bd_addr_t bda, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return out;
}

static bool bda_is_zero(const esp_bd_addr_t bda)
{
    static const esp_bd_addr_t zero = { 0 };

    return bda == NULL || memcmp(bda, zero, sizeof(zero)) == 0;
}

static bool bda_equal(const esp_bd_addr_t a, const esp_bd_addr_t b)
{
    return a != NULL && b != NULL && memcmp(a, b, sizeof(esp_bd_addr_t)) == 0;
}

static bool blacklist_contains(const esp_bd_addr_t bda)
{
    if (bda_is_zero(bda)) {
        return false;
    }

    for (uint8_t i = 0; i < s_blacklist_count; ++i) {
        if (bda_equal(s_blacklist[i], bda)) {
            return true;
        }
    }
    return false;
}

static bool blacklist_add_unique(const esp_bd_addr_t bda)
{
    if (bda_is_zero(bda) || blacklist_contains(bda)) {
        return false;
    }
    if (s_blacklist_count >= DS5_BLACKLIST_MAX) {
        char addr[18];
        ESP_LOGW(TAG, "Raw HIDP blacklist full; dropping %s",
                 bda_to_str(bda, addr, sizeof(addr)));
        return false;
    }

    memcpy(s_blacklist[s_blacklist_count], bda, sizeof(esp_bd_addr_t));
    s_blacklist_count++;
    return true;
}

static bool blacklist_remove(const esp_bd_addr_t bda)
{
    if (bda_is_zero(bda)) {
        return false;
    }

    for (uint8_t i = 0; i < s_blacklist_count; ++i) {
        if (bda_equal(s_blacklist[i], bda)) {
            for (uint8_t j = i; j + 1U < s_blacklist_count; ++j) {
                memcpy(s_blacklist[j], s_blacklist[j + 1U], sizeof(esp_bd_addr_t));
            }
            memset(s_blacklist[s_blacklist_count - 1U], 0, sizeof(esp_bd_addr_t));
            s_blacklist_count--;
            return true;
        }
    }
    return false;
}

static int persist_blacklist(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DS5_NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Opening NVS namespace for blacklist failed: %s",
                 esp_err_to_name(err));
        return -EIO;
    }

    if (s_blacklist_count == 0) {
        err = nvs_erase_key(nvs, DS5_NVS_BLACKLIST_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_blob(nvs,
                           DS5_NVS_BLACKLIST_KEY,
                           s_blacklist,
                           (size_t)s_blacklist_count * sizeof(esp_bd_addr_t));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persisting blacklist failed: %s", esp_err_to_name(err));
        return -EIO;
    }

    ESP_LOGI(TAG, "Raw HIDP persisted blacklist entries=%u", (unsigned)s_blacklist_count);
    return 0;
}

static void load_blacklist(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DS5_NVS_NAMESPACE, NVS_READONLY, &nvs);

    s_blacklist_count = 0;
    memset(s_blacklist, 0, sizeof(s_blacklist));

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Opening NVS namespace for blacklist load failed: %s",
                 esp_err_to_name(err));
        return;
    }

    size_t size = 0;
    err = nvs_get_blob(nvs, DS5_NVS_BLACKLIST_KEY, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return;
    }
    if (err != ESP_OK || size == 0 || (size % sizeof(esp_bd_addr_t)) != 0 ||
        size > sizeof(s_blacklist)) {
        ESP_LOGW(TAG, "Raw HIDP blacklist blob invalid size=%u err=%s",
                 (unsigned)size,
                 esp_err_to_name(err));
        nvs_close(nvs);
        return;
    }

    err = nvs_get_blob(nvs, DS5_NVS_BLACKLIST_KEY, s_blacklist, &size);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reading blacklist failed: %s", esp_err_to_name(err));
        memset(s_blacklist, 0, sizeof(s_blacklist));
        return;
    }

    s_blacklist_count = (uint8_t)(size / sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "Raw HIDP loaded blacklist entries=%u", (unsigned)s_blacklist_count);
    for (uint8_t i = 0; i < s_blacklist_count; ++i) {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP blacklist[%u]=%s",
                 (unsigned)i,
                 bda_to_str(s_blacklist[i], addr, sizeof(addr)));
    }
}

static bool is_expected_target_bda(const esp_bd_addr_t bda)
{
    return s_target_found && bda_equal(s_target_bda, bda);
}

static bool target_origin_allows_blacklist_bypass(void)
{
    return s_target_origin == RAW_TARGET_DISCOVERY ||
           s_target_origin == RAW_TARGET_MANUAL;
}

static bool should_block_blacklisted_peer(const esp_bd_addr_t bda)
{
    if (!blacklist_contains(bda)) {
        return false;
    }
    if (!is_expected_target_bda(bda)) {
        return true;
    }
    return !target_origin_allows_blacklist_bypass();
}

static void maybe_add_blacklist_candidate(const esp_bd_addr_t bda, const char *reason)
{
    if (blacklist_add_unique(bda)) {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP blacklisted %s (%s)",
                 bda_to_str(bda, addr, sizeof(addr)),
                 reason != NULL ? reason : "unspecified");
    }
}

void bt_dualsense_raw_hidp_get_state(bt_dualsense_raw_hidp_state_t *state)
{
    uint32_t flags = 0;

    if (state == NULL) {
        return;
    }

    if (bt_dualsense_raw_hidp_ready()) {
        flags |= DS5_DUAL_BT_STATE_READY;
    }
    if (s_l2cap_ready) {
        flags |= DS5_DUAL_BT_STATE_L2CAP_READY;
    }
    if (s_sdp_ready) {
        flags |= DS5_DUAL_BT_STATE_SDP_READY;
    }
    if (s_control.connected) {
        flags |= DS5_DUAL_BT_STATE_CONTROL_OPEN;
    }
    if (s_interrupt.connected) {
        flags |= DS5_DUAL_BT_STATE_INTERRUPT_OPEN;
    }
    if (s_have_full_report) {
        flags |= DS5_DUAL_BT_STATE_FULL_REPORT;
    }
    if (s_connecting) {
        flags |= DS5_DUAL_BT_STATE_CONNECTING;
    }
    if (s_sdp_searching) {
        flags |= DS5_DUAL_BT_STATE_SDP_SEARCHING;
    }
    if (s_target_found) {
        flags |= DS5_DUAL_BT_STATE_TARGET_FOUND;
    }
    if (s_have_saved_bda) {
        flags |= DS5_DUAL_BT_STATE_HAVE_SAVED;
    }
    if (s_current_target_from_saved) {
        flags |= DS5_DUAL_BT_STATE_FROM_SAVED;
    }

    memset(state, 0, sizeof(*state));
    state->flags = flags;
    state->last_error = s_last_error;
    state->rssi = s_last_rssi;
    state->bringup_attempts = s_bringup_attempts;
    state->reconnect_failures = s_saved_reconnect_failures;
    state->control_mtu = s_control.tx_mtu > 0 ? (uint16_t)s_control.tx_mtu : 0;
    state->interrupt_mtu = s_interrupt.tx_mtu > 0 ? (uint16_t)s_interrupt.tx_mtu : 0;
    if (s_target_found) {
        memcpy(state->bda, s_target_bda, sizeof(state->bda));
    } else if (s_have_saved_bda) {
        memcpy(state->bda, s_saved_bda, sizeof(state->bda));
    }
    state->state_seq = s_state_seq;
}

static void notify_state(void)
{
    bt_dualsense_raw_hidp_state_t state;

    s_state_seq++;
    if (s_state_cb == NULL) {
        return;
    }

    bt_dualsense_raw_hidp_get_state(&state);
    s_state_cb(&state, esp_timer_get_time(), s_state_cb_ctx);
}

static bool text_contains_case_insensitive(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    for (const char *p = text; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool copy_eir_name(uint8_t *eir, char *name, size_t name_len)
{
    if (eir == NULL || name == NULL || name_len == 0) {
        return false;
    }

    uint8_t len = 0;
    uint8_t *eir_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (eir_name == NULL) {
        eir_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    }
    if (eir_name == NULL || len == 0) {
        return false;
    }

    size_t copy_len = len < name_len - 1 ? len : name_len - 1;
    memcpy(name, eir_name, copy_len);
    name[copy_len] = '\0';
    return true;
}

static bool cod_is_gamepad(uint32_t cod)
{
    uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
    uint32_t minor = esp_bt_gap_get_cod_minor_dev(cod);

    return major == ESP_BT_COD_MAJOR_DEV_PERIPHERAL &&
           (minor == ESP_BT_COD_MINOR_PERIPHERAL_GAMEPAD ||
            minor == ESP_BT_COD_MINOR_PERIPHERAL_JOYSTICK);
}

static bool is_dualsense_candidate(const char *name, bool has_cod, uint32_t cod)
{
    if (text_contains_case_insensitive(name, "DualSense") ||
        text_contains_case_insensitive(name, "Wireless Controller")) {
        return true;
    }

#if DS5_ACCEPT_ANY_GAMEPAD
    return has_cod && cod_is_gamepad(cod);
#else
    (void)has_cod;
    (void)cod;
    return false;
#endif
}

static uint8_t inquiry_len_from_seconds(void)
{
    int seconds = CONFIG_DS5_SCAN_SECONDS;
    int units = (seconds * 100 + 127) / 128;
    if (units < ESP_BT_GAP_MIN_INQ_LEN) {
        units = ESP_BT_GAP_MIN_INQ_LEN;
    } else if (units > ESP_BT_GAP_MAX_INQ_LEN) {
        units = ESP_BT_GAP_MAX_INQ_LEN;
    }
    return (uint8_t)units;
}

static bool load_saved_bda(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DS5_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS namespace failed while loading saved controller: %s",
                 esp_err_to_name(err));
        return false;
    }

    size_t len = sizeof(s_saved_bda);
    err = nvs_get_blob(nvs, DS5_NVS_LAST_BDA_KEY, s_saved_bda, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != sizeof(s_saved_bda)) {
        return false;
    }

    s_have_saved_bda = true;
    char addr[18];
    ESP_LOGI(TAG, "Raw HIDP loaded saved DualSense address %s",
             bda_to_str(s_saved_bda, addr, sizeof(addr)));
    return true;
}

static void clear_saved_bda_state(void)
{
    memset(s_saved_bda, 0, sizeof(s_saved_bda));
    s_have_saved_bda = false;
    s_saved_reconnect_failures = 0;
    s_current_target_from_saved = false;
    if (s_target_origin == RAW_TARGET_SAVED_AUTO) {
        s_target_origin = RAW_TARGET_NONE;
    }
}

static int forget_saved_bda(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DS5_NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        clear_saved_bda_state();
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS namespace failed while forgetting controller: %s",
                 esp_err_to_name(err));
        return -EIO;
    }

    err = nvs_erase_key(nvs, DS5_NVS_LAST_BDA_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Clearing saved DualSense address failed: %s", esp_err_to_name(err));
        return -EIO;
    }

    clear_saved_bda_state();
    memset(s_target_bda, 0, sizeof(s_target_bda));
    s_target_found = false;
    ESP_LOGI(TAG, "Raw HIDP cleared saved DualSense address");
    return 0;
}

static bool saved_bda_has_bond(void)
{
    int device_count;
    esp_bd_addr_t *devices;
    bool found = false;
    esp_err_t err;

    if (!s_have_saved_bda) {
        return false;
    }

    device_count = esp_bt_gap_get_bond_device_num();
    if (device_count <= 0) {
        return false;
    }

    devices = calloc((size_t)device_count, sizeof(*devices));
    if (devices == NULL) {
        ESP_LOGW(TAG, "Allocating bonded device list for %d entries failed; keeping saved address",
                 device_count);
        return true;
    }

    err = esp_bt_gap_get_bond_device_list(&device_count, devices);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reading bonded device list failed: %s", esp_err_to_name(err));
        free(devices);
        return true;
    }

    for (int i = 0; i < device_count; i++) {
        if (memcmp(devices[i], s_saved_bda, sizeof(s_saved_bda)) == 0) {
            found = true;
            break;
        }
    }

    free(devices);
    return found;
}

static int blacklist_bonded_devices(void)
{
    int device_count;
    esp_bd_addr_t *devices;
    esp_err_t err;

    device_count = esp_bt_gap_get_bond_device_num();
    if (device_count < 0) {
        ESP_LOGW(TAG, "Querying bonded device count for blacklist failed");
        return -EIO;
    }
    if (device_count == 0) {
        return 0;
    }

    devices = calloc((size_t)device_count, sizeof(*devices));
    if (devices == NULL) {
        return -ENOMEM;
    }

    err = esp_bt_gap_get_bond_device_list(&device_count, devices);
    if (err != ESP_OK) {
        free(devices);
        ESP_LOGW(TAG, "Reading bonded device list for blacklist failed: %s",
                 esp_err_to_name(err));
        return -EIO;
    }

    for (int i = 0; i < device_count; i++) {
        maybe_add_blacklist_candidate(devices[i], "forget-bond");
    }

    free(devices);
    return 0;
}

static int clear_bonded_devices(void)
{
    int device_count;
    esp_bd_addr_t *devices;
    esp_err_t err;
    int first_error = 0;

    device_count = esp_bt_gap_get_bond_device_num();
    if (device_count < 0) {
        ESP_LOGW(TAG, "Querying bonded device count failed");
        return -EIO;
    }
    if (device_count == 0) {
        ESP_LOGI(TAG, "Raw HIDP bond database already empty");
        return 0;
    }

    devices = calloc((size_t)device_count, sizeof(*devices));
    if (devices == NULL) {
        return -ENOMEM;
    }

    err = esp_bt_gap_get_bond_device_list(&device_count, devices);
    if (err != ESP_OK) {
        free(devices);
        ESP_LOGW(TAG, "Reading bonded device list failed: %s", esp_err_to_name(err));
        return -EIO;
    }

    for (int i = 0; i < device_count; i++) {
        char addr[18];

        err = esp_bt_gap_remove_bond_device(devices[i]);
        if (err != ESP_OK && first_error == 0) {
            first_error = -EIO;
        }
        ESP_LOGI(TAG, "Raw HIDP remove bond %s result=%s",
                 bda_to_str(devices[i], addr, sizeof(addr)),
                 esp_err_to_name(err));
    }

    free(devices);
    return first_error;
}

static void save_bda_if_needed(const esp_bd_addr_t bda)
{
    if (s_have_saved_bda && memcmp(s_saved_bda, bda, sizeof(esp_bd_addr_t)) == 0) {
        return;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DS5_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS namespace failed while saving controller: %s",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, DS5_NVS_LAST_BDA_KEY, bda, sizeof(esp_bd_addr_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        memcpy(s_saved_bda, bda, sizeof(esp_bd_addr_t));
        s_have_saved_bda = true;
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP saved DualSense address %s",
                 bda_to_str(s_saved_bda, addr, sizeof(addr)));
    } else {
        ESP_LOGW(TAG, "Saving DualSense address failed: %s", esp_err_to_name(err));
    }
}

static void reset_hidp_channels(void)
{
    s_control.connected = false;
    s_control.fd = -1;
    s_control.handle = 0;
    s_control.tx_mtu = 0;
    s_control.task = NULL;

    s_interrupt.connected = false;
    s_interrupt.fd = -1;
    s_interrupt.handle = 0;
    s_interrupt.tx_mtu = 0;
    s_interrupt.task = NULL;

    s_connecting = false;
    s_pending_step = RAW_CONNECT_IDLE;
    s_target_origin = RAW_TARGET_NONE;
    s_have_last_state = false;
    s_have_full_report = false;
    s_bringup_attempts = 0;
    dualsense_output_init(&s_output_ctx);
    notify_state();
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL || !s_auto_reconnect_enabled) {
        if (!s_auto_reconnect_enabled) {
            ESP_LOGI(TAG, "Raw HIDP reconnect suppressed by control plane");
            notify_state();
        }
        return;
    }

    esp_timer_stop(s_reconnect_timer);
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "Raw HIDP scheduling reconnect in %u ms",
             (unsigned)(DS5_RAW_RECONNECT_DELAY_US / 1000));
    ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer, DS5_RAW_RECONNECT_DELAY_US));
}

static void note_connect_failure(void)
{
    if (!s_current_target_from_saved) {
        return;
    }

    if (s_saved_reconnect_failures < UINT8_MAX) {
        s_saved_reconnect_failures++;
    }
    ESP_LOGI(TAG, "Raw HIDP saved DualSense reconnect failure %u/%u",
             (unsigned)s_saved_reconnect_failures,
             (unsigned)DS5_RAW_MAX_SAVED_RECONNECT_FAILURES);
    notify_state();
}

static void maybe_start_flow(void)
{
    if (s_auto_reconnect_enabled && s_l2cap_ready && s_sdp_ready) {
        start_connect_flow();
    }
}

static void start_discovery(void)
{
    if (s_connecting || s_control.connected || s_interrupt.connected) {
        return;
    }

    s_target_found = false;
    s_current_target_from_saved = false;
    if (s_target_origin != RAW_TARGET_MANUAL) {
        s_target_origin = RAW_TARGET_NONE;
    }
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "Raw HIDP scanning for DualSense over Classic Bluetooth BR/EDR");
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               inquiry_len_from_seconds(), 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Raw HIDP discovery failed: %s", esp_err_to_name(err));
        s_last_error = err;
        notify_state();
        schedule_reconnect();
    } else {
        notify_state();
    }
}

static void start_sdp_or_connect(void)
{
    if (!s_target_found || s_connecting || s_sdp_searching) {
        return;
    }

    esp_bt_uuid_t uuid = ESP_SDP_BUILD_BT_UUID16(DS5_HIDP_SERVICE_UUID);
    esp_err_t err = esp_sdp_search_record(s_target_bda, uuid);
    if (err == ESP_OK) {
        s_sdp_searching = true;
        ESP_LOGI(TAG, "Raw HIDP SDP search for HID service 0x%04X started",
                 DS5_HIDP_SERVICE_UUID);
        notify_state();
        return;
    }

    ESP_LOGW(TAG, "Raw HIDP SDP search failed to start: %s; connecting known HID PSMs",
             esp_err_to_name(err));
    s_last_error = err;
    notify_state();
    connect_hidp_control();
}

static void start_connect_flow(void)
{
    if (s_connecting || s_control.connected || s_interrupt.connected) {
        return;
    }

    if (s_have_saved_bda &&
        s_saved_reconnect_failures < DS5_RAW_MAX_SAVED_RECONNECT_FAILURES) {
        if (blacklist_contains(s_saved_bda)) {
            char addr[18];
            ESP_LOGI(TAG, "Raw HIDP saved DualSense %s is blacklisted; scanning for explicit re-pair",
                     bda_to_str(s_saved_bda, addr, sizeof(addr)));
            start_discovery();
            return;
        }
        memcpy(s_target_bda, s_saved_bda, sizeof(esp_bd_addr_t));
        s_target_found = true;
        s_current_target_from_saved = true;
        s_target_origin = RAW_TARGET_SAVED_AUTO;
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP trying saved DualSense address %s",
                 bda_to_str(s_target_bda, addr, sizeof(addr)));
        start_sdp_or_connect();
        return;
    }

    if (s_have_saved_bda) {
        ESP_LOGI(TAG, "Raw HIDP saved reconnect deferred after %u failures; scanning instead",
                 (unsigned)s_saved_reconnect_failures);
    }
    start_discovery();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    reset_hidp_channels();
    if (s_auto_reconnect_enabled) {
        start_connect_flow();
    }
}

static void bringup_timer_cb(void *arg)
{
    (void)arg;
    send_bringup("retry");
}

static esp_err_t connect_psm(raw_connect_step_t step, uint16_t psm)
{
    s_pending_step = step;
    s_connecting = true;
    led_status_set(DS5_LED_STATE_BT_CONNECTING);

    char addr[18];
    ESP_LOGI(TAG, "Raw HIDP connecting PSM 0x%04X to %s",
             psm, bda_to_str(s_target_bda, addr, sizeof(addr)));
    esp_err_t err = esp_bt_l2cap_connect(ESP_BT_L2CAP_SEC_AUTHENTICATE,
                                         psm,
                                         s_target_bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Raw HIDP L2CAP connect PSM 0x%04X failed: %s",
                 psm, esp_err_to_name(err));
        s_pending_step = RAW_CONNECT_IDLE;
        s_connecting = false;
        s_last_error = err;
    }
    notify_state();
    return err;
}

static void connect_hidp_control(void)
{
    if (connect_psm(RAW_CONNECT_CONTROL, DS5_HIDP_PSM_CONTROL) != ESP_OK) {
        schedule_reconnect();
    }
}

static void connect_hidp_interrupt(void)
{
    if (connect_psm(RAW_CONNECT_INTERRUPT, DS5_HIDP_PSM_INTERRUPT) != ESP_OK) {
        schedule_reconnect();
    }
}

int bt_dualsense_raw_hidp_connect(const uint8_t *bda, size_t len)
{
    if (bda != NULL && len != sizeof(esp_bd_addr_t)) {
        return -EINVAL;
    }
    if (bt_dualsense_raw_hidp_ready() || s_connecting || s_sdp_searching) {
        return 0;
    }

    s_auto_reconnect_enabled = true;
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }

    if (bda != NULL) {
        memcpy(s_target_bda, bda, sizeof(esp_bd_addr_t));
        s_target_found = true;
        s_current_target_from_saved = false;
        s_target_origin = RAW_TARGET_MANUAL;
        s_saved_reconnect_failures = 0;
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP control connect requested for %s",
                 bda_to_str(s_target_bda, addr, sizeof(addr)));
    } else {
        s_target_origin = RAW_TARGET_NONE;
        ESP_LOGI(TAG, "Raw HIDP control connect requested using saved address or scan");
    }

    if (!s_l2cap_ready || !s_sdp_ready) {
        notify_state();
        return -EAGAIN;
    }

    if (bda != NULL) {
        start_sdp_or_connect();
    } else {
        start_connect_flow();
    }
    return 0;
}

static void close_channel(raw_hidp_channel_t *channel)
{
    if (channel == NULL || !channel->connected || channel->fd < 0) {
        return;
    }

    close(channel->fd);
    channel->fd = -1;
    channel->connected = false;
    channel->handle = 0;
    channel->tx_mtu = 0;
}

int bt_dualsense_raw_hidp_disconnect(bool allow_reconnect)
{
    s_auto_reconnect_enabled = allow_reconnect;
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    stop_bringup_timer();
    esp_bt_gap_cancel_discovery();

    close_channel(&s_interrupt);
    close_channel(&s_control);
    reset_hidp_channels();
    led_status_set(allow_reconnect ? DS5_LED_STATE_BT_CONNECTING : DS5_LED_STATE_BOOT_OK);
    if (allow_reconnect) {
        start_connect_flow();
    }
    notify_state();
    return 0;
}

static raw_hidp_channel_t *channel_for_handle(uint32_t handle)
{
    if (s_control.connected && s_control.handle == handle) {
        return &s_control;
    }
    if (s_interrupt.connected && s_interrupt.handle == handle) {
        return &s_interrupt;
    }
    return NULL;
}

static raw_hidp_channel_t *pending_channel(void)
{
    switch (s_pending_step) {
    case RAW_CONNECT_CONTROL:
        return &s_control;
    case RAW_CONNECT_INTERRUPT:
        return &s_interrupt;
    default:
        return NULL;
    }
}

static int raw_write(raw_hidp_channel_t *channel, const uint8_t *data, size_t len)
{
    if (channel == NULL || !channel->connected || channel->fd < 0) {
        return -ENOTCONN;
    }
    if (data == NULL || len == 0) {
        return -EINVAL;
    }

    int written = write(channel->fd, data, len);
    if (written != (int)len) {
        ESP_LOGW(TAG, "Raw HIDP %s write len=%u result=%d errno=%d",
                 channel->name, (unsigned)len, written, errno);
        return written < 0 ? -errno : -EIO;
    }

    ESP_LOGI(TAG, "Raw HIDP %s tx len=%u first=0x%02X",
             channel->name, (unsigned)len, data[0]);
    return 0;
}

static int send_control_byte(uint8_t byte)
{
    return raw_write(&s_control, &byte, 1);
}

static int send_get_feature(uint8_t report_id)
{
    uint8_t packet[] = {
        (uint8_t)(DS5_HIDP_GET_REPORT | DS5_HIDP_REPORT_TYPE_FEATURE),
        report_id,
    };
    return raw_write(&s_control, packet, sizeof(packet));
}

static int send_set_report_output(const uint8_t *report, size_t report_len)
{
    uint8_t packet[1 + DS5_OUTPUT_REPORT32_BT_LEN];
    if (report == NULL || report_len > DS5_OUTPUT_REPORT32_BT_LEN) {
        return -EINVAL;
    }

    packet[0] = (uint8_t)(DS5_HIDP_SET_REPORT | DS5_HIDP_REPORT_TYPE_OUTPUT);
    memcpy(packet + 1, report, report_len);
    return raw_write(&s_control, packet, report_len + 1);
}

static int send_set_report_feature(uint8_t report_id, const uint8_t *data, size_t len)
{
    uint8_t packet[2 + 64];

    if ((data == NULL && len != 0) || len > sizeof(packet) - 2U) {
        return -EINVAL;
    }

    packet[0] = (uint8_t)(DS5_HIDP_SET_REPORT | DS5_HIDP_REPORT_TYPE_FEATURE);
    packet[1] = report_id;
    if (len != 0) {
        memcpy(packet + 2, data, len);
    }
    return raw_write(&s_control, packet, len + 2U);
}

static int send_data_output(const uint8_t *report, size_t report_len)
{
    uint8_t packet[1 + DS5_OUTPUT_REPORT36_BT_LEN];
    if (report == NULL || report_len > DS5_OUTPUT_REPORT36_BT_LEN) {
        return -EINVAL;
    }

    packet[0] = DS5_HIDP_DATA_OUTPUT;
    memcpy(packet + 1, report, report_len);
    return raw_write(&s_interrupt, packet, report_len + 1);
}

bool bt_dualsense_raw_hidp_ready(void)
{
    return s_control.connected && s_interrupt.connected;
}

int bt_dualsense_raw_hidp_forget(uint8_t flags)
{
    int first_error = 0;
    int err;

    if ((flags & ~(DS5_DUAL_FORGET_SAVED_ADDR | DS5_DUAL_FORGET_BONDS)) != 0 ||
        flags == 0) {
        return -EINVAL;
    }

    s_auto_reconnect_enabled = false;
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    stop_bringup_timer();
    esp_bt_gap_cancel_discovery();

    close_channel(&s_interrupt);
    close_channel(&s_control);
    reset_hidp_channels();

    if ((flags & DS5_DUAL_FORGET_BONDS) != 0) {
        err = blacklist_bonded_devices();
        if (err != 0 && first_error == 0) {
            first_error = err;
        }
        maybe_add_blacklist_candidate(s_saved_bda, "forget-saved");
        maybe_add_blacklist_candidate(s_target_bda, "forget-target");
        err = persist_blacklist();
        if (err != 0 && first_error == 0) {
            first_error = err;
        }
        err = clear_bonded_devices();
        if (err != 0 && first_error == 0) {
            first_error = err;
        }
    }
    if ((flags & DS5_DUAL_FORGET_SAVED_ADDR) != 0) {
        err = forget_saved_bda();
        if (err != 0 && first_error == 0) {
            first_error = err;
        }
    }

    s_target_found = false;
    memset(s_target_bda, 0, sizeof(s_target_bda));
    s_last_error = first_error;
    led_status_set(DS5_LED_STATE_BOOT_OK);
    notify_state();

    ESP_LOGI(TAG, "Raw HIDP forget request flags=0x%02X status=%d",
             (unsigned)flags, first_error);
    return first_error;
}

int bt_dualsense_raw_hidp_send_report(const uint8_t *report,
                                      size_t report_len,
                                      bool realtime)
{
    (void)realtime;

    if (report == NULL || report_len == 0) {
        return -EINVAL;
    }
    if (!bt_dualsense_raw_hidp_ready()) {
        return -ENOTCONN;
    }
    return send_data_output(report, report_len);
}

int bt_dualsense_raw_hidp_get_feature(uint8_t report_id)
{
    return send_get_feature(report_id);
}

int bt_dualsense_raw_hidp_set_feature(uint8_t report_id,
                                      const uint8_t *data,
                                      size_t len)
{
    return send_set_report_feature(report_id, data, len);
}

void bt_dualsense_raw_hidp_set_rx_callback(bt_dualsense_raw_hidp_rx_cb_t cb,
                                           void *ctx)
{
    s_rx_cb = cb;
    s_rx_cb_ctx = ctx;
}

void bt_dualsense_raw_hidp_set_state_callback(bt_dualsense_raw_hidp_state_cb_t cb,
                                              void *ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = ctx;
    notify_state();
}

static void send_output_init_reports(void)
{
    uint8_t report31[DS5_OUTPUT_REPORT31_BT_LEN];
    uint8_t report32[DS5_OUTPUT_REPORT32_BT_LEN];

    if (dualsense_output_make_report31(&s_output_ctx, report31, sizeof(report31))) {
        ESP_LOGI(TAG, "Raw HIDP DS5Dongle report 0x31 DATA/SET_REPORT output attempt");
        send_data_output(report31, sizeof(report31));
        send_set_report_output(report31, sizeof(report31));
    } else {
        ESP_LOGE(TAG, "Raw HIDP report 0x31 build failed");
    }

    if (dualsense_output_make_report32(&s_output_ctx, report32, sizeof(report32))) {
        ESP_LOGI(TAG, "Raw HIDP DS5Dongle report 0x32 DATA/SET_REPORT output attempt");
        send_data_output(report32, sizeof(report32));
        send_set_report_output(report32, sizeof(report32));
    } else {
        ESP_LOGE(TAG, "Raw HIDP report 0x32 build failed");
    }
}

static void stop_bringup_timer(void)
{
    if (s_bringup_timer == NULL) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_bringup_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Raw HIDP stopping bring-up timer failed: %s", esp_err_to_name(err));
    }
}

static void schedule_bringup_retry(void)
{
    if (s_bringup_timer == NULL || s_have_full_report) {
        return;
    }
    if (s_bringup_attempts >= CONFIG_DS5_BRINGUP_RETRIES) {
        ESP_LOGW(TAG,
                 "Raw HIDP bring-up attempts exhausted; still waiting for full report=0x31");
        return;
    }

    stop_bringup_timer();
    ESP_ERROR_CHECK(esp_timer_start_once(
        s_bringup_timer,
        (uint64_t)CONFIG_DS5_BRINGUP_RETRY_MS * 1000ULL));
}

static void send_bringup(const char *reason)
{
    if (!s_control.connected || !s_interrupt.connected || s_have_full_report) {
        return;
    }
    if (s_bringup_attempts >= CONFIG_DS5_BRINGUP_RETRIES) {
        return;
    }

    s_bringup_attempts++;
    notify_state();
    ESP_LOGI(TAG, "Raw HIDP DualSense bring-up attempt %u/%u reason=%s",
             (unsigned)s_bringup_attempts,
             (unsigned)CONFIG_DS5_BRINGUP_RETRIES,
             reason != NULL ? reason : "event");

    send_control_byte(DS5_HIDP_SET_PROTOCOL_REPORT);
    static const uint8_t feature_ids[] = {0x09, 0x20, 0x22, 0x05, 0x70};
    for (size_t i = 0; i < sizeof(feature_ids); i++) {
        ESP_LOGI(TAG, "Raw HIDP requested feature report 0x%02X", feature_ids[i]);
        send_get_feature(feature_ids[i]);
    }

    send_output_init_reports();
    schedule_bringup_retry();
}

static void handle_input_report(const uint8_t *data, size_t len)
{
    dualsense_state_t state;
    dualsense_parse_result_t parse;

    if (s_rx_cb != NULL) {
        s_rx_cb(data, len, esp_timer_get_time(), s_rx_cb_ctx);
    }

    if (!dualsense_parse_report(data, len, &state, &parse)) {
        if (parse.kind == DS5_REPORT_MIC_AUDIO) {
            ESP_LOGD(TAG, "Raw HIDP microphone/audio payload len=%u skipped", (unsigned)len);
        } else {
            ESP_LOGW(TAG, "Raw HIDP unsupported interrupt report len=%u first=0x%02X",
                     (unsigned)len, len > 0 ? data[0] : 0);
        }
        return;
    }

    if (state.is_full_report && !s_have_full_report) {
        s_have_full_report = true;
        stop_bringup_timer();
        ESP_LOGI(TAG, "Raw HIDP full report=0x31 received; bring-up complete after %u attempts",
                 (unsigned)s_bringup_attempts);
        notify_state();
    }

    int64_t now = esp_timer_get_time();
    bool changed = !s_have_last_state || !dualsense_state_equal(&s_last_state, &state);
    bool heartbeat_due = !s_have_last_state ||
                         (now - s_last_state_log_us) >=
                         (int64_t)CONFIG_DS5_LOG_UNCHANGED_MS * 1000;

    if (changed || heartbeat_due) {
        char line[320];
        dualsense_format_state(&state, line, sizeof(line));
        ESP_LOGI(TAG, "%s", line);
        s_last_state_log_us = now;
    }

    s_last_state = state;
    s_have_last_state = true;
}

static void rx_task(void *arg)
{
    raw_hidp_channel_t *channel = (raw_hidp_channel_t *)arg;
    uint8_t data[DS5_RAW_RX_BUFFER_LEN];

    while (channel->fd >= 0) {
        int len = read(channel->fd, data, sizeof(data));
        if (len == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (len < 0) {
            ESP_LOGW(TAG, "Raw HIDP %s read ended len=%d errno=%d",
                     channel->name, len, errno);
            break;
        }

        ESP_LOGI(TAG, "Raw HIDP %s rx len=%d first=0x%02X",
                 channel->name, len, data[0]);
        if (channel->id == RAW_CH_INTERRUPT) {
            handle_input_report(data, (size_t)len);
        } else if (s_rx_cb != NULL && len >= 3 && data[0] == 0xA3) {
            s_rx_cb(data, (size_t)len, esp_timer_get_time(), s_rx_cb_ctx);
        }
    }

    channel->task = NULL;
    vTaskDelete(NULL);
}

static void start_rx_task(raw_hidp_channel_t *channel)
{
    if (channel->task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreate(rx_task,
                                channel->id == RAW_CH_CONTROL ? "hidp_ctl_rx" : "hidp_int_rx",
                                DS5_RAW_RX_TASK_STACK,
                                channel,
                                5,
                                &channel->task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Raw HIDP failed to start %s rx task", channel->name);
        channel->task = NULL;
    }
}

static void l2cap_callback(esp_bt_l2cap_cb_event_t event, esp_bt_l2cap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_L2CAP_INIT_EVT:
        ESP_LOGI(TAG, "Raw HIDP L2CAP init status=%d", param->init.status);
        if (param->init.status == ESP_BT_L2CAP_SUCCESS) {
            esp_err_t err = esp_bt_l2cap_vfs_register();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Raw HIDP L2CAP VFS register failed: %s", esp_err_to_name(err));
                s_last_error = err;
            } else {
                s_l2cap_ready = true;
                maybe_start_flow();
            }
        } else {
            s_last_error = param->init.status;
        }
        notify_state();
        break;

    case ESP_BT_L2CAP_CL_INIT_EVT:
        ESP_LOGI(TAG, "Raw HIDP L2CAP client init status=%d handle=0x%" PRIX32,
                 param->cl_init.status, param->cl_init.handle);
        if (param->cl_init.status != ESP_BT_L2CAP_SUCCESS) {
            s_last_error = param->cl_init.status;
            note_connect_failure();
            s_connecting = false;
            s_pending_step = RAW_CONNECT_IDLE;
            notify_state();
            schedule_reconnect();
        }
        break;

    case ESP_BT_L2CAP_OPEN_EVT: {
        raw_hidp_channel_t *channel = pending_channel();
        bool blocked_blacklisted = should_block_blacklisted_peer(param->open.rem_bda);
        ESP_LOGI(TAG,
                 "Raw HIDP L2CAP open status=%d handle=0x%" PRIX32 " fd=%d mtu=%" PRId32,
                 param->open.status,
                 param->open.handle,
                 param->open.fd,
                 param->open.tx_mtu);

        if (blocked_blacklisted) {
            char addr[18];
            if (param->open.fd >= 0) {
                close(param->open.fd);
            }
            s_last_error = -EPERM;
            ESP_LOGW(TAG, "Raw HIDP rejected blacklisted incoming L2CAP open from %s",
                     bda_to_str(param->open.rem_bda, addr, sizeof(addr)));
            notify_state();
            break;
        }

        if (param->open.status != ESP_BT_L2CAP_SUCCESS || channel == NULL) {
            s_last_error = param->open.status;
            note_connect_failure();
            s_connecting = false;
            s_pending_step = RAW_CONNECT_IDLE;
            notify_state();
            schedule_reconnect();
            break;
        }

        channel->fd = param->open.fd;
        channel->handle = param->open.handle;
        channel->tx_mtu = param->open.tx_mtu;
        channel->connected = true;
        save_bda_if_needed(param->open.rem_bda);
        start_rx_task(channel);
        ESP_LOGI(TAG, "Raw HIDP %s connected: fd=%d tx_mtu=%" PRId32,
                 channel->name, channel->fd, channel->tx_mtu);
        s_last_error = 0;

        s_pending_step = RAW_CONNECT_IDLE;
        s_connecting = false;
        if (channel->id == RAW_CH_CONTROL) {
            connect_hidp_interrupt();
        } else if (s_control.connected && s_interrupt.connected) {
            if (target_origin_allows_blacklist_bypass() &&
                blacklist_remove(param->open.rem_bda)) {
                char addr[18];
                ESP_LOGI(TAG, "Raw HIDP removed %s from blacklist after successful pair",
                         bda_to_str(param->open.rem_bda, addr, sizeof(addr)));
                if (persist_blacklist() != 0) {
                    s_last_error = -EIO;
                }
            }
            s_current_target_from_saved = false;
            s_saved_reconnect_failures = 0;
            led_status_set(DS5_LED_STATE_BT_CONNECTED);
            send_bringup("l2cap-open");
        }
        notify_state();
        break;
    }

    case ESP_BT_L2CAP_CLOSE_EVT: {
        raw_hidp_channel_t *channel = channel_for_handle(param->close.handle);
        bool failed_pending_connect = channel == NULL &&
                                      (s_connecting ||
                                       s_pending_step != RAW_CONNECT_IDLE);
        ESP_LOGI(TAG, "Raw HIDP L2CAP close status=%d handle=0x%" PRIX32 " async=%d channel=%s",
                 param->close.status,
                 param->close.handle,
                 param->close.async ? 1 : 0,
                 channel != NULL ? channel->name : "?");
        if (channel != NULL) {
            channel->connected = false;
            channel->fd = -1;
            channel->handle = 0;
        }
        s_last_error = param->close.status;
        if (failed_pending_connect) {
            note_connect_failure();
            s_connecting = false;
            s_pending_step = RAW_CONNECT_IDLE;
        }
        stop_bringup_timer();
        led_status_set(DS5_LED_STATE_BT_CONNECTING);
        notify_state();
        schedule_reconnect();
        break;
    }

    case ESP_BT_L2CAP_UNINIT_EVT:
        ESP_LOGI(TAG, "Raw HIDP L2CAP uninit status=%d", param->uninit.status);
        break;

    case ESP_BT_L2CAP_START_EVT:
    case ESP_BT_L2CAP_SRV_STOP_EVT:
        break;

    default:
        ESP_LOGD(TAG, "Raw HIDP unhandled L2CAP event %d", event);
        break;
    }
}

static void sdp_callback(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        ESP_LOGI(TAG, "Raw HIDP SDP init status=%d", param->init.status);
        if (param->init.status == ESP_SDP_SUCCESS) {
            s_sdp_ready = true;
            maybe_start_flow();
        } else {
            s_last_error = param->init.status;
        }
        notify_state();
        break;

    case ESP_SDP_SEARCH_COMP_EVT:
        s_sdp_searching = false;
        ESP_LOGI(TAG, "Raw HIDP SDP search status=%d records=%d",
                 param->search.status, param->search.record_count);
        if (param->search.status != ESP_SDP_SUCCESS) {
            s_last_error = param->search.status;
        }
        if (param->search.status == ESP_SDP_SUCCESS && param->search.records != NULL) {
            for (int i = 0; i < param->search.record_count; i++) {
                ESP_LOGI(TAG,
                         "Raw HIDP SDP record %d service=\"%s\" l2cap_psm=0x%04" PRIX32
                         " rfcomm=%" PRId32 " profile=0x%04" PRIX32,
                         i,
                         param->search.records[i].hdr.service_name != NULL ?
                         param->search.records[i].hdr.service_name : "?",
                         param->search.records[i].hdr.l2cap_psm,
                         param->search.records[i].hdr.rfcomm_channel_number,
                         param->search.records[i].hdr.profile_version);
            }
        }
        notify_state();
        connect_hidp_control();
        break;

    case ESP_SDP_DEINIT_EVT:
        ESP_LOGI(TAG, "Raw HIDP SDP deinit status=%d", param->deinit.status);
        break;

    case ESP_SDP_CREATE_RECORD_COMP_EVT:
    case ESP_SDP_REMOVE_RECORD_COMP_EVT:
        break;

    default:
        ESP_LOGD(TAG, "Raw HIDP unhandled SDP event %d", event);
        break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
        bool has_cod = false;
        uint32_t cod = 0;
        int8_t rssi = 0;

        for (int i = 0; i < param->disc_res.num_prop; ++i) {
            esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
            switch (prop->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME: {
                size_t copy_len = prop->len < ESP_BT_GAP_MAX_BDNAME_LEN ?
                                  (size_t)prop->len : ESP_BT_GAP_MAX_BDNAME_LEN;
                memcpy(name, prop->val, copy_len);
                name[copy_len] = '\0';
                break;
            }
            case ESP_BT_GAP_DEV_PROP_COD:
                has_cod = true;
                cod = *(uint32_t *)prop->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *)prop->val;
                s_last_rssi = rssi;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                if (name[0] == '\0') {
                    copy_eir_name((uint8_t *)prop->val, name, sizeof(name));
                }
                break;
            default:
                break;
            }
        }

        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP found %s name=\"%s\" cod=0x%06" PRIX32 " rssi=%d",
                 bda_to_str(param->disc_res.bda, addr, sizeof(addr)),
                 name[0] != '\0' ? name : "?",
                 cod,
                 rssi);

        if (!s_target_found && is_dualsense_candidate(name, has_cod, cod)) {
            memcpy(s_target_bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
            s_target_found = true;
            s_current_target_from_saved = false;
            s_target_origin = RAW_TARGET_DISCOVERY;
            ESP_LOGI(TAG, "Raw HIDP selected candidate %s; stopping inquiry", addr);
            notify_state();
            esp_bt_gap_cancel_discovery();
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Raw HIDP discovery stopped");
            if (s_target_found) {
                start_sdp_or_connect();
            } else {
                notify_state();
                schedule_reconnect();
            }
        } else {
            ESP_LOGI(TAG, "Raw HIDP discovery started");
            notify_state();
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        char addr[18];
        bool blocked_blacklisted = should_block_blacklisted_peer(param->auth_cmpl.bda);
        ESP_LOGI(TAG, "Raw HIDP auth %s for %s link_key_type=%d name=\"%s\"",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "failed",
                 bda_to_str(param->auth_cmpl.bda, addr, sizeof(addr)),
                 param->auth_cmpl.lk_type,
                 (char *)param->auth_cmpl.device_name);
        if (blocked_blacklisted && param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Raw HIDP blacklisted peer %s authenticated unexpectedly; dropping bond",
                     addr);
            (void)esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
        }
        if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
            s_last_error = param->auth_cmpl.stat;
        }
        notify_state();
        break;
    }

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code;
        char addr[18];

        if (should_block_blacklisted_peer(param->pin_req.bda)) {
            memset(pin_code, 0, sizeof(pin_code));
            esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin_code);
            ESP_LOGW(TAG, "Raw HIDP rejected PIN request from blacklisted %s",
                     bda_to_str(param->pin_req.bda, addr, sizeof(addr)));
            break;
        }

        memset(pin_code, 0, sizeof(pin_code));
        memcpy(pin_code, "0000", 4);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        ESP_LOGI(TAG, "Raw HIDP answered legacy PIN request with 0000");
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT: {
        char addr[18];

        if (should_block_blacklisted_peer(param->cfm_req.bda)) {
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
            ESP_LOGW(TAG, "Raw HIDP rejected SSP confirmation from blacklisted %s value=%" PRIu32,
                     bda_to_str(param->cfm_req.bda, addr, sizeof(addr)),
                     param->cfm_req.num_val);
            break;
        }

        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        ESP_LOGI(TAG, "Raw HIDP accepted SSP confirmation value=%" PRIu32,
                 param->cfm_req.num_val);
        break;
    }

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "Raw HIDP SSP passkey notification: %" PRIu32,
                 param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
        ESP_LOGW(TAG, "Raw HIDP rejected unexpected SSP passkey entry request");
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP ACL connect status=%d handle=0x%04X addr=%s",
                 param->acl_conn_cmpl_stat.stat,
                 param->acl_conn_cmpl_stat.handle,
                 bda_to_str(param->acl_conn_cmpl_stat.bda, addr, sizeof(addr)));
        if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS &&
            should_block_blacklisted_peer(param->acl_conn_cmpl_stat.bda)) {
            s_last_error = -EPERM;
            ESP_LOGW(TAG, "Raw HIDP blacklisted incoming ACL from %s will be denied",
                     addr);
            notify_state();
        }
        break;
    }

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP ACL disconnect reason=0x%02X handle=0x%04X addr=%s",
                 param->acl_disconn_cmpl_stat.reason,
                 param->acl_disconn_cmpl_stat.handle,
                 bda_to_str(param->acl_disconn_cmpl_stat.bda, addr, sizeof(addr)));
        break;
    }

    case ESP_BT_GAP_ENC_CHG_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP encryption change mode=%d addr=%s",
                 param->enc_chg.enc_mode,
                 bda_to_str(param->enc_chg.bda, addr, sizeof(addr)));
        break;
    }

    default:
        ESP_LOGD(TAG, "Raw HIDP unhandled GAP event %d", event);
        break;
    }
}

esp_err_t bt_dualsense_raw_hidp_start(void)
{
    reset_hidp_channels();

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "BLE memory release returned %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_bt_l2cap_register_callback(l2cap_callback));
    ESP_ERROR_CHECK(esp_sdp_register_callback(sdp_callback));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DS5_RAW_LOCAL_NAME));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                                  &iocap, sizeof(iocap)));

    esp_bt_pin_code_t pin_code;
    memset(pin_code, 0, sizeof(pin_code));
    memcpy(pin_code, "0000", 4);
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                             ESP_BT_NON_DISCOVERABLE));

    load_saved_bda();
    load_blacklist();
    if (s_have_saved_bda && !saved_bda_has_bond()) {
        ESP_LOGW(TAG, "Raw HIDP saved address has no matching bond; clearing stale entry");
        (void)forget_saved_bda();
    }

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_cb,
        .name = "raw_hidp_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

    const esp_timer_create_args_t bringup_timer_args = {
        .callback = bringup_timer_cb,
        .name = "raw_hidp_bringup",
    };
    ESP_ERROR_CHECK(esp_timer_create(&bringup_timer_args, &s_bringup_timer));

    err = esp_bt_l2cap_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_l2cap_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_sdp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sdp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t *local_bda = esp_bt_dev_get_address();
    if (local_bda != NULL) {
        char addr[18];
        ESP_LOGI(TAG, "Raw HIDP local BR/EDR address: %s",
                 bda_to_str(local_bda, addr, sizeof(addr)));
    }

    if (DS5_RAW_HIDP_AUTO_CONNECT) {
        ESP_LOGI(TAG, "Raw HIDP auto-connect enabled at startup");
        start_connect_flow();
    }

    return ESP_OK;
}
