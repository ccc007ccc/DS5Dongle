#include "bt_dualsense_host.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dualsense_output.h"
#include "dualsense_parser.h"
#include "led_status.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#ifndef CONFIG_DS5_SCAN_SECONDS
#define CONFIG_DS5_SCAN_SECONDS 30
#endif

#ifdef CONFIG_DS5_ACCEPT_ANY_GAMEPAD
#define DS5_ACCEPT_ANY_GAMEPAD 1
#else
#define DS5_ACCEPT_ANY_GAMEPAD 0
#endif

#ifndef CONFIG_DS5_PRINT_RAW_REPORTS
#define CONFIG_DS5_PRINT_RAW_REPORTS 0
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

#define DS5_LOCAL_NAME "DS5Bridge-ESP32-S1"
#define DS5_NVS_NAMESPACE "ds5bt"
#define DS5_NVS_LAST_BDA_KEY "last_bda"
#define DS5_SAVED_RECONNECT_MIN_INTERVAL_US (5 * 1000 * 1000)
#define DS5_CONNECT_RETRY_DELAY_US (2 * 1000 * 1000)
#define DS5_MAX_SAVED_RECONNECT_FAILURES 3

static const char *TAG = "ds5_bt_host";

static esp_bd_addr_t s_target_bda;
static esp_bd_addr_t s_connected_bda;
static esp_bd_addr_t s_saved_bda;
static bool s_target_found;
static bool s_have_saved_bda;
static bool s_connecting;
static bool s_connected;
static bool s_current_connect_was_saved;
static dualsense_state_t s_last_state;
static bool s_have_last_state;
static int64_t s_last_state_log_us;
static int64_t s_last_unsupported_log_us;
static int64_t s_last_saved_connect_us;
static uint8_t s_saved_reconnect_failures;
static uint32_t s_unsupported_report_count;
static dualsense_output_context_t s_output_ctx;
static bool s_feature_requests_sent;
static bool s_have_full_report;
static uint8_t s_bringup_attempts;
static esp_timer_handle_t s_connect_retry_timer;
static esp_timer_handle_t s_bringup_retry_timer;

static void connect_target(void);
static void start_discovery(void);
static void start_connect_flow(void);
static void schedule_connect_retry(void);
static void send_dualsense_bringup_output(const char *reason);

static const char *bda_to_str(const esp_bd_addr_t bda, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return out;
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
               tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i])) {
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

    size_t copy_len = len < (name_len - 1) ? len : (name_len - 1);
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
    (void) has_cod;
    (void) cod;
    return false;
#endif
}

static uint8_t inquiry_len_from_seconds(void)
{
    int seconds = CONFIG_DS5_SCAN_SECONDS;
    int units = (seconds * 100 + 127) / 128; /* GAP units are 1.28 seconds. */
    if (units < ESP_BT_GAP_MIN_INQ_LEN) {
        units = ESP_BT_GAP_MIN_INQ_LEN;
    } else if (units > ESP_BT_GAP_MAX_INQ_LEN) {
        units = ESP_BT_GAP_MAX_INQ_LEN;
    }
    return (uint8_t) units;
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

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK || len != sizeof(s_saved_bda)) {
        ESP_LOGW(TAG, "Saved controller address unavailable: %s len=%u",
                 esp_err_to_name(err), (unsigned) len);
        return false;
    }

    char addr[18];
    s_have_saved_bda = true;
    ESP_LOGI(TAG, "Loaded saved DualSense address %s",
             bda_to_str(s_saved_bda, addr, sizeof(addr)));
    return true;
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

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving DualSense address failed: %s", esp_err_to_name(err));
        return;
    }

    memcpy(s_saved_bda, bda, sizeof(esp_bd_addr_t));
    s_have_saved_bda = true;

    char addr[18];
    ESP_LOGI(TAG, "Saved DualSense address %s for automatic reconnect",
             bda_to_str(s_saved_bda, addr, sizeof(addr)));
}

static bool try_saved_reconnect(bool force)
{
    if (!s_have_saved_bda || s_connected || s_connecting) {
        return false;
    }
    if (s_saved_reconnect_failures >= DS5_MAX_SAVED_RECONNECT_FAILURES) {
        ESP_LOGI(TAG, "Saved DualSense reconnect deferred after %u failures; scanning instead",
                 (unsigned) s_saved_reconnect_failures);
        return false;
    }

    int64_t now = esp_timer_get_time();
    if (!force && s_last_saved_connect_us != 0 &&
        now - s_last_saved_connect_us < DS5_SAVED_RECONNECT_MIN_INTERVAL_US) {
        return false;
    }

    memcpy(s_target_bda, s_saved_bda, sizeof(esp_bd_addr_t));
    s_target_found = true;
    s_last_saved_connect_us = now;

    char addr[18];
    ESP_LOGI(TAG, "Trying saved DualSense address %s before discovery",
             bda_to_str(s_target_bda, addr, sizeof(addr)));
    s_current_connect_was_saved = true;
    connect_target();
    return true;
}

static void start_discovery(void)
{
    if (s_connected || s_connecting) {
        return;
    }

    s_target_found = false;
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "Scanning for DualSense over Classic Bluetooth BR/EDR");
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               inquiry_len_from_seconds(), 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_gap_start_discovery failed: %s", esp_err_to_name(err));
    }
}

static void start_connect_flow(void)
{
    if (try_saved_reconnect(false)) {
        return;
    }
    start_discovery();
}

static void connect_retry_timer_cb(void *arg)
{
    (void) arg;
    start_connect_flow();
}

static void schedule_connect_retry(void)
{
    if (s_connect_retry_timer == NULL || s_connected || s_connecting) {
        return;
    }

    esp_timer_stop(s_connect_retry_timer);
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    ESP_LOGI(TAG, "Scheduling Bluetooth reconnect in %u ms",
             (unsigned) (DS5_CONNECT_RETRY_DELAY_US / 1000));
    ESP_ERROR_CHECK(esp_timer_start_once(s_connect_retry_timer,
                                         DS5_CONNECT_RETRY_DELAY_US));
}

static void connect_target(void)
{
    if (s_connected || s_connecting || !s_target_found) {
        return;
    }

    char addr[18];
    ESP_LOGI(TAG, "Connecting HID host to %s", bda_to_str(s_target_bda, addr, sizeof(addr)));
    s_connecting = true;
    led_status_set(DS5_LED_STATE_BT_CONNECTING);
    esp_err_t err = esp_bt_hid_host_connect(s_target_bda);
    if (err != ESP_OK) {
        s_connecting = false;
        s_current_connect_was_saved = false;
        ESP_LOGE(TAG, "esp_bt_hid_host_connect failed: %s", esp_err_to_name(err));
        schedule_connect_retry();
    }
}

static void note_connect_failure(void)
{
    if (!s_current_connect_was_saved) {
        return;
    }

    if (s_saved_reconnect_failures < UINT8_MAX) {
        s_saved_reconnect_failures++;
    }
    ESP_LOGI(TAG, "Saved DualSense reconnect failure %u/%u",
             (unsigned) s_saved_reconnect_failures,
             (unsigned) DS5_MAX_SAVED_RECONNECT_FAILURES);
}

static void stop_bringup_retry(void)
{
    if (s_bringup_retry_timer == NULL) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_bringup_retry_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Stopping DualSense bring-up retry timer failed: %s",
                 esp_err_to_name(err));
    }
}

static void schedule_bringup_retry(void)
{
    if (s_bringup_retry_timer == NULL || !s_connected || s_have_full_report) {
        return;
    }
    if (s_bringup_attempts >= CONFIG_DS5_BRINGUP_RETRIES) {
        ESP_LOGW(TAG,
                 "DualSense bring-up attempts exhausted; still waiting for full report=0x31");
        return;
    }

    stop_bringup_retry();
    ESP_ERROR_CHECK(esp_timer_start_once(
        s_bringup_retry_timer,
        (uint64_t) CONFIG_DS5_BRINGUP_RETRY_MS * 1000ULL));
}

static void bringup_retry_timer_cb(void *arg)
{
    (void) arg;
    send_dualsense_bringup_output("retry");
}

static void request_dualsense_feature_reports(bool force)
{
    if (!s_connected || (!force && s_feature_requests_sent)) {
        return;
    }

    static const struct {
        uint8_t report_id;
        int size;
    } feature_reports[] = {
        {0x09, 20},
        {0x20, 64},
        {0x22, 64},
        {0x05, 41},
        {0x70, 64},
    };

    bool requested_any = false;
    for (size_t i = 0; i < sizeof(feature_reports) / sizeof(feature_reports[0]); ++i) {
        esp_err_t err = esp_bt_hid_host_get_report(s_connected_bda,
                                                   ESP_HIDH_REPORT_TYPE_FEATURE,
                                                   feature_reports[i].report_id,
                                                   feature_reports[i].size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Feature report 0x%02X request failed: %s",
                     feature_reports[i].report_id, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Requested feature report 0x%02X len=%d",
                     feature_reports[i].report_id, feature_reports[i].size);
            requested_any = true;
        }
    }

    if (requested_any) {
        s_feature_requests_sent = true;
    }
}

static void send_ds5dongle_report31_output(void)
{
    uint8_t report[DS5_OUTPUT_REPORT31_BT_LEN];
    if (!dualsense_output_make_report31(&s_output_ctx, report, sizeof(report))) {
        ESP_LOGE(TAG, "DS5Dongle report 0x31 build failed");
        return;
    }

    esp_err_t err = esp_bt_hid_host_send_data(s_connected_bda, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS5Dongle report 0x31 DATA output send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS5Dongle report 0x31 DATA output sent seq=%u",
                 (unsigned) ((report[1] >> 4) & 0x0F));
    }

    err = esp_bt_hid_host_set_report(s_connected_bda,
                                     ESP_HIDH_REPORT_TYPE_OUTPUT,
                                     report,
                                     sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS5Dongle report 0x31 SET_REPORT output failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS5Dongle report 0x31 SET_REPORT output queued");
    }
}

static void send_ds5dongle_report32_output(void)
{
    uint8_t report[DS5_OUTPUT_REPORT32_BT_LEN];
    if (!dualsense_output_make_report32(&s_output_ctx, report, sizeof(report))) {
        ESP_LOGE(TAG, "DS5Dongle report 0x32 build failed");
        return;
    }

    esp_err_t err = esp_bt_hid_host_send_data(s_connected_bda, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS5Dongle report 0x32 DATA output send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS5Dongle report 0x32 DATA output sent seq=%u",
                 (unsigned) ((report[1] >> 4) & 0x0F));
    }

    err = esp_bt_hid_host_set_report(s_connected_bda,
                                     ESP_HIDH_REPORT_TYPE_OUTPUT,
                                     report,
                                     sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS5Dongle report 0x32 SET_REPORT output failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS5Dongle report 0x32 SET_REPORT output queued");
    }
}

static void send_dualsense_bringup_output(const char *reason)
{
    if (!s_connected || s_have_full_report) {
        return;
    }
    if (s_bringup_attempts >= CONFIG_DS5_BRINGUP_RETRIES) {
        return;
    }

    s_bringup_attempts++;
    ESP_LOGI(TAG, "DualSense bring-up attempt %u/%u reason=%s",
             (unsigned) s_bringup_attempts,
             (unsigned) CONFIG_DS5_BRINGUP_RETRIES,
             reason != NULL ? reason : "event");

    request_dualsense_feature_reports(s_bringup_attempts > 1);
    send_ds5dongle_report31_output();
    send_ds5dongle_report32_output();
    schedule_bringup_retry();
}

static void log_raw_report(const uint8_t *data, uint16_t len)
{
#if CONFIG_DS5_PRINT_RAW_REPORTS
    char line[160];
    size_t used = 0;
    uint16_t dump_len = len < 48 ? len : 48;

    for (uint16_t i = 0; i < dump_len && used < sizeof(line); ++i) {
        int written = snprintf(line + used, sizeof(line) - used, "%02X%s",
                               data[i], i + 1 == dump_len ? "" : " ");
        if (written < 0 || (size_t) written >= sizeof(line) - used) {
            break;
        }
        used += (size_t) written;
    }
    ESP_LOGD(TAG, "raw hid len=%u %s%s", len, line, len > dump_len ? " ..." : "");
#else
    (void) data;
    (void) len;
#endif
}

static void log_unsupported_report(const uint8_t *data, uint16_t len)
{
    int64_t now = esp_timer_get_time();
    bool log_now = s_unsupported_report_count < 5 ||
                   (now - s_last_unsupported_log_us) >= 1000000;
    s_unsupported_report_count++;

    if (!log_now) {
        return;
    }

    char bytes[64];
    size_t used = 0;
    uint16_t dump_len = len < 16 ? len : 16;
    for (uint16_t i = 0; i < dump_len && used < sizeof(bytes); ++i) {
        int written = snprintf(bytes + used, sizeof(bytes) - used, "%02X%s",
                               data[i], i + 1 == dump_len ? "" : " ");
        if (written < 0 || (size_t) written >= sizeof(bytes) - used) {
            break;
        }
        used += (size_t) written;
    }
    s_last_unsupported_log_us = now;
    ESP_LOGW(TAG, "Unsupported HID interrupt report #%lu len=%u first=0x%02X data=%s%s",
             (unsigned long) s_unsupported_report_count,
             len,
             len > 0 ? data[0] : 0,
             bytes,
             len > dump_len ? " ..." : "");
}

static void handle_input_report(const uint8_t *data, uint16_t len)
{
    dualsense_state_t state;
    dualsense_parse_result_t parse;

    log_raw_report(data, len);

    if (!dualsense_parse_report(data, len, &state, &parse)) {
        if (parse.kind == DS5_REPORT_MIC_AUDIO) {
            ESP_LOGD(TAG, "DualSense microphone/audio payload len=%u skipped in stage 1", len);
        } else {
            log_unsupported_report(data, len);
        }
        return;
    }

    if (state.is_full_report && !s_have_full_report) {
        s_have_full_report = true;
        stop_bringup_retry();
        ESP_LOGI(TAG, "DualSense full report=0x31 received; bring-up complete after %u attempts",
                 (unsigned) s_bringup_attempts);
    }

    int64_t now = esp_timer_get_time();
    bool changed = !s_have_last_state || !dualsense_state_equal(&s_last_state, &state);
    bool heartbeat_due = !s_have_last_state ||
                         (now - s_last_state_log_us) >= (int64_t) CONFIG_DS5_LOG_UNCHANGED_MS * 1000;

    if (changed || heartbeat_due) {
        char line[320];
        dualsense_format_state(&state, line, sizeof(line));
        ESP_LOGI(TAG, "%s", line);
        s_last_state_log_us = now;
    }

    s_last_state = state;
    s_have_last_state = true;
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
                                  (size_t) prop->len : ESP_BT_GAP_MAX_BDNAME_LEN;
                memcpy(name, prop->val, copy_len);
                name[copy_len] = '\0';
                break;
            }
            case ESP_BT_GAP_DEV_PROP_COD:
                has_cod = true;
                cod = *(uint32_t *) prop->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *) prop->val;
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                if (name[0] == '\0') {
                    copy_eir_name((uint8_t *) prop->val, name, sizeof(name));
                }
                break;
            default:
                break;
            }
        }

        char addr[18];
        ESP_LOGI(TAG, "Found %s name=\"%s\" cod=0x%06" PRIX32 " rssi=%d",
                 bda_to_str(param->disc_res.bda, addr, sizeof(addr)),
                 name[0] != '\0' ? name : "?",
                 cod,
                 rssi);

        if (!s_target_found && is_dualsense_candidate(name, has_cod, cod)) {
            memcpy(s_target_bda, param->disc_res.bda, sizeof(esp_bd_addr_t));
            s_target_found = true;
            ESP_LOGI(TAG, "Selected candidate %s; stopping inquiry", addr);
            esp_bt_gap_cancel_discovery();
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped");
            if (s_connected || s_connecting) {
                break;
            } else if (s_target_found) {
                s_current_connect_was_saved = false;
                connect_target();
            } else if (try_saved_reconnect(false)) {
                break;
            } else {
                if (s_have_saved_bda &&
                    s_saved_reconnect_failures >= DS5_MAX_SAVED_RECONNECT_FAILURES) {
                    ESP_LOGI(TAG, "Discovery fallback complete; allowing saved reconnect again");
                    s_saved_reconnect_failures = 0;
                }
                start_connect_flow();
            }
        } else {
            ESP_LOGI(TAG, "Discovery started");
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "Auth %s for %s link_key_type=%d name=\"%s\"",
                 param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "ok" : "failed",
                 bda_to_str(param->auth_cmpl.bda, addr, sizeof(addr)),
                 param->auth_cmpl.lk_type,
                 (char *) param->auth_cmpl.device_name);
        break;
    }

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code;
        memset(pin_code, 0, sizeof(pin_code));
        memcpy(pin_code, "0000", 4);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        ESP_LOGI(TAG, "Answered legacy PIN request with 0000");
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        ESP_LOGI(TAG, "Accepted SSP confirmation value=%" PRIu32, param->cfm_req.num_val);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey notification: %" PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
        ESP_LOGW(TAG, "Rejected unexpected SSP passkey entry request");
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "ACL connect status=%d handle=0x%04X addr=%s",
                 param->acl_conn_cmpl_stat.stat,
                 param->acl_conn_cmpl_stat.handle,
                 bda_to_str(param->acl_conn_cmpl_stat.bda, addr, sizeof(addr)));
        break;
    }

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "ACL disconnect reason=0x%02X handle=0x%04X addr=%s",
                 param->acl_disconn_cmpl_stat.reason,
                 param->acl_disconn_cmpl_stat.handle,
                 bda_to_str(param->acl_disconn_cmpl_stat.bda, addr, sizeof(addr)));
        break;
    }

    default:
        ESP_LOGD(TAG, "Unhandled GAP event %d", event);
        break;
    }
}

static void hidh_callback(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDH_INIT_EVT:
        ESP_LOGI(TAG, "HID host initialized status=%d", param->init.status);
        start_connect_flow();
        break;

    case ESP_HIDH_OPEN_EVT: {
        char addr[18];
        ESP_LOGI(TAG, "HID open status=%d conn=%d handle=%u addr=%s",
                 param->open.status,
                 param->open.conn_status,
                 param->open.handle,
                 bda_to_str(param->open.bd_addr, addr, sizeof(addr)));

        if (param->open.status == ESP_HIDH_OK &&
            param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING) {
            s_connecting = true;
            break;
        }

        if (param->open.status == ESP_HIDH_OK &&
            param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
            s_connecting = false;
            s_connected = true;
            s_current_connect_was_saved = false;
            s_saved_reconnect_failures = 0;
            memcpy(s_connected_bda, param->open.bd_addr, sizeof(esp_bd_addr_t));
            save_bda_if_needed(param->open.bd_addr);
            s_have_last_state = false;
            s_unsupported_report_count = 0;
            s_feature_requests_sent = false;
            s_have_full_report = false;
            s_bringup_attempts = 0;
            dualsense_output_init(&s_output_ctx);
            led_status_set(DS5_LED_STATE_BT_CONNECTED);
            ESP_LOGI(TAG, "DualSense HID connected; requesting report protocol");
            esp_bt_hid_host_set_protocol(s_connected_bda, ESP_HIDH_REPORT_MODE);
            send_dualsense_bringup_output("hid-open");
        } else {
            note_connect_failure();
            s_connecting = false;
            s_connected = false;
            s_current_connect_was_saved = false;
            schedule_connect_retry();
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "HID close status=%d reason=0x%02X conn=%d handle=%u",
                 param->close.status,
                 param->close.reason,
                 param->close.conn_status,
                 param->close.handle);
        s_connecting = false;
        s_connected = false;
        s_current_connect_was_saved = false;
        s_have_last_state = false;
        s_feature_requests_sent = false;
        s_have_full_report = false;
        s_bringup_attempts = 0;
        stop_bringup_retry();
        led_status_set(DS5_LED_STATE_BT_CONNECTING);
        schedule_connect_retry();
        break;

    case ESP_HIDH_GET_DSCP_EVT:
        ESP_LOGI(TAG,
                 "Descriptor status=%d vid=0x%04X pid=0x%04X ver=0x%04X desc_len=%u added=%u",
                 param->dscp.status,
                 param->dscp.vendor_id,
                 param->dscp.product_id,
                 param->dscp.version,
                 param->dscp.dl_len,
                 param->dscp.added ? 1 : 0);
        send_dualsense_bringup_output("descriptor");
        break;

    case ESP_HIDH_SET_PROTO_EVT:
        ESP_LOGI(TAG, "Set protocol status=%d handle=%u", param->set_proto.status,
                 param->set_proto.handle);
        send_dualsense_bringup_output("set-protocol");
        break;

    case ESP_HIDH_DATA_IND_EVT:
        if (param->data_ind.status == ESP_HIDH_OK && param->data_ind.data != NULL) {
            handle_input_report(param->data_ind.data, param->data_ind.len);
        } else {
            ESP_LOGW(TAG, "Data indication status=%d len=%u",
                     param->data_ind.status, param->data_ind.len);
        }
        break;

    case ESP_HIDH_GET_RPT_EVT:
        ESP_LOGI(TAG, "Get report status=%d handle=%u len=%u first=0x%02X",
                 param->get_rpt.status,
                 param->get_rpt.handle,
                 param->get_rpt.len,
                 (param->get_rpt.data != NULL && param->get_rpt.len > 0) ?
                 param->get_rpt.data[0] : 0);
        break;

    case ESP_HIDH_SET_RPT_EVT:
        ESP_LOGI(TAG, "Set report status=%d handle=%u",
                 param->set_rpt.status,
                 param->set_rpt.handle);
        break;

    case ESP_HIDH_DATA_EVT:
        ESP_LOGI(TAG, "Send data status=%d handle=%u reason=0x%02X",
                 param->send_data.status,
                 param->send_data.handle,
                 param->send_data.reason);
        break;

    case ESP_HIDH_ADD_DEV_EVT:
        ESP_LOGI(TAG, "HID device added status=%d handle=%u",
                 param->add_dev.status, param->add_dev.handle);
        send_dualsense_bringup_output("add-dev");
        break;

    case ESP_HIDH_RMV_DEV_EVT:
        ESP_LOGI(TAG, "HID device removed status=%d handle=%u",
                 param->rmv_dev.status, param->rmv_dev.handle);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled HIDH event %d", event);
        break;
    }
}

esp_err_t bt_dualsense_host_start(void)
{
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
    ESP_ERROR_CHECK(esp_bt_hid_host_register_callback(hidh_callback));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DS5_LOCAL_NAME));

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                                  &iocap, sizeof(iocap)));

    esp_bt_pin_code_t pin_code;
    memset(pin_code, 0, sizeof(pin_code));
    memcpy(pin_code, "0000", 4);
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code));
    /* HID controllers may page the host when the user presses PS after pairing. */
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                             ESP_BT_NON_DISCOVERABLE));

    load_saved_bda();

    const esp_timer_create_args_t retry_timer_args = {
        .callback = connect_retry_timer_cb,
        .name = "ds5_bt_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_connect_retry_timer));

    const esp_timer_create_args_t bringup_timer_args = {
        .callback = bringup_retry_timer_cb,
        .name = "ds5_bringup",
    };
    ESP_ERROR_CHECK(esp_timer_create(&bringup_timer_args, &s_bringup_retry_timer));

    const uint8_t *local_bda = esp_bt_dev_get_address();
    if (local_bda != NULL) {
        char addr[18];
        ESP_LOGI(TAG, "Local BR/EDR address: %s",
                 bda_to_str(local_bda, addr, sizeof(addr)));
    } else {
        ESP_LOGW(TAG, "Local BR/EDR address unavailable");
    }
    return esp_bt_hid_host_init();
}
