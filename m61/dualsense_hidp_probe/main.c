#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dualsense_output.h"
#include "dualsense_parser.h"
#include "m61_audio_epoch.h"
#include "m61_bt_tx_scheduler.h"
#include "m61_usb_gamepad.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "shell.h"

#include "bflb_gpio.h"
#include "bflb_clock.h"
#include "bflb_mtimer.h"
#include "bflb_mtd.h"
#include "bflb_pwm_v2.h"
#include "easyflash.h"
#include "rfparam_adapter.h"

#include "bluetooth.h"
#include "bt_uuid.h"
#include "buf.h"
#include "conn.h"
#include "conn_internal.h"
#include "hci_core.h"
#include "hci_driver.h"
#include "hci_host.h"
#include "l2cap.h"
#include "l2cap_internal.h"
#include "sdp.h"

#include <net/buf.h>

#if defined(BL616)
#include "btble_lib_api.h"
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bl616_sys.h"
#elif defined(BL618DG)
#include "btble_lib_api.h"
#include "bl618dg_glb.h"
#include "bl618dg_hbn.h"
#include "bl618dg_sys.h"
#endif

#define HIDP_PSM_CONTROL 0x0011
#define HIDP_PSM_INTERRUPT 0x0013
#define HIDP_TRANSACTION_SET_REPORT 0x50
#define HIDP_TRANSACTION_SET_PROTOCOL_REPORT 0x71
#define HIDP_TRANSACTION_DATA_OUTPUT 0xA2
#define HIDP_REPORT_TYPE_INPUT 0x01
#define HIDP_REPORT_TYPE_OUTPUT 0x02
#define HIDP_REPORT_TYPE_FEATURE 0x03
#define HIDP_DISCOVERY_SLOTS 10
#define HIDP_EIR_MAX_LEN 240
#define HIDP_TX_MAX_LEN (1U + DS5_OUTPUT_BT_MAX_LEN)
_Static_assert(CONFIG_BT_L2CAP_TX_MTU >= HIDP_TX_MAX_LEN,
               "Bluetooth L2CAP TX pool is too small for DualSense audio");
#define DS5_LAST_ADDR_KEY "ds5_last_bda"
#define DS5_AUDIO_BUFFER_LENGTH_DEFAULT 48
#define DS5_STATE_FLAGS0 0
#define DS5_STATE_FLAGS1 1
#define DS5_STATE_RUMBLE_RIGHT 2
#define DS5_STATE_RUMBLE_LEFT 3
#define DS5_STATE_AUDIO_CONTROL 7
#define DS5_STATE_MUTE_LIGHT 8
#define DS5_STATE_AUDIO_MUTE 9
#define DS5_STATE_FLAGS2 38
#define DS5_STATE_LIGHT_FADE 41
#define DS5_STATE_LIGHT_BRIGHTNESS 42
#define DS5_STATE_PLAYER_LIGHTS 43
#define DS5_STATE_LED_RED 44

#ifndef CONFIG_M61_DS5_AUTO_START
#define CONFIG_M61_DS5_AUTO_START 0
#endif

#ifndef CONFIG_M61_DS5_AUTO_START_DELAY_MS
#define CONFIG_M61_DS5_AUTO_START_DELAY_MS 1500
#endif

#ifndef CONFIG_M61_DS5_AUTO_RETRY_MS
#define CONFIG_M61_DS5_AUTO_RETRY_MS 5000
#endif

#ifndef CONFIG_M61_DS5_AUTO_SAVED_ADDR_ATTEMPTS
#define CONFIG_M61_DS5_AUTO_SAVED_ADDR_ATTEMPTS 3
#endif

#ifndef CONFIG_M61_DS5_SECURITY_TIMEOUT_MS
#define CONFIG_M61_DS5_SECURITY_TIMEOUT_MS 2500
#endif

#ifndef CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES
#define CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES 8
#endif

#ifndef CONFIG_M61_DS5_AUTO_BRINGUP_RETRY_MS
#define CONFIG_M61_DS5_AUTO_BRINGUP_RETRY_MS 750
#endif

#ifndef CONFIG_M61_DS5_USB_OUTPUT_MIN_INTERVAL_MS
#define CONFIG_M61_DS5_USB_OUTPUT_MIN_INTERVAL_MS 20
#endif

#ifndef CONFIG_M61_DS5_MIC_STATUS_REFRESH_MS
#define CONFIG_M61_DS5_MIC_STATUS_REFRESH_MS 500
#endif

#ifndef CONFIG_M61_DS5_AUDIO_REPORT_INTERVAL_MS
#define CONFIG_M61_DS5_AUDIO_REPORT_INTERVAL_MS 10
#endif

#ifndef CONFIG_M61_DS5_USB_BRIDGE_TASK_DELAY_MS
#define CONFIG_M61_DS5_USB_BRIDGE_TASK_DELAY_MS 1
#endif

/*
 * SDK defaults: HCI TX=max-3, BT/controller RX=max-4.  Codec/BT RX use max-4;
 * the bridge uses max-5 and is guaranteed a bounded window because codec
 * blocks for one tick after every encoded frame instead of catch-up spinning.
 */
#define M61_BT_BRIDGE_TASK_PRIORITY (configMAX_PRIORITIES - 5)
#define M61_AUTO_TASK_PRIORITY      (configMAX_PRIORITIES - 8)
#define M61_LED_TASK_PRIORITY       (configMAX_PRIORITIES - 9)

_Static_assert(CONFIG_BT_HCI_TX_PRIO > M61_BT_BRIDGE_TASK_PRIORITY,
               "HCI TX must outrank the M61 bridge");
_Static_assert(CONFIG_BT_RX_PRIO > M61_BT_BRIDGE_TASK_PRIORITY,
               "BT RX must outrank the M61 bridge");
_Static_assert(CONFIG_BT_CTLR_RX_PRIO > M61_BT_BRIDGE_TASK_PRIORITY,
               "controller RX must outrank the M61 bridge");

#ifndef CONFIG_M61_DS5_BT_AUDIO_ALLOC_TIMEOUT_MS
#define CONFIG_M61_DS5_BT_AUDIO_ALLOC_TIMEOUT_MS 0
#endif

#define HIDP_BT_AUDIO_ALLOC_TIMEOUT K_MSEC(CONFIG_M61_DS5_BT_AUDIO_ALLOC_TIMEOUT_MS)

#ifndef CONFIG_M61_DS5_FEATURE_REPORTS_PER_TICK
#define CONFIG_M61_DS5_FEATURE_REPORTS_PER_TICK 2
#endif

#ifndef CONFIG_M61_DS5_HOST_REPORTS_PER_TICK
#define CONFIG_M61_DS5_HOST_REPORTS_PER_TICK 4
#endif

#ifndef CONFIG_M61_USB_START_AFTER_BT_TIMEOUT_MS
#define CONFIG_M61_USB_START_AFTER_BT_TIMEOUT_MS 3000
#endif

#ifndef CONFIG_M61_USB_START_DELAY_AFTER_BT_MS
#define CONFIG_M61_USB_START_DELAY_AFTER_BT_MS 200
#endif

#ifndef CONFIG_M61_STATUS_LED_ENABLE
#define CONFIG_M61_STATUS_LED_ENABLE 1
#endif

#ifndef CONFIG_M61_STATUS_LED_ACTIVE_HIGH
#define CONFIG_M61_STATUS_LED_ACTIVE_HIGH 1
#endif

#ifndef CONFIG_M61_STATUS_LED_RED_PIN
#define CONFIG_M61_STATUS_LED_RED_PIN GPIO_PIN_12
#endif

#ifndef CONFIG_M61_STATUS_LED_GREEN_PIN
#define CONFIG_M61_STATUS_LED_GREEN_PIN GPIO_PIN_14
#endif

#ifndef CONFIG_M61_STATUS_LED_BLUE_PIN
#define CONFIG_M61_STATUS_LED_BLUE_PIN GPIO_PIN_15
#endif

#ifndef CONFIG_M61_STATUS_LED_BLINK_MS
#define CONFIG_M61_STATUS_LED_BLINK_MS 250
#endif

#ifndef CONFIG_M61_STATUS_LED_PWM_PERIOD
#define CONFIG_M61_STATUS_LED_PWM_PERIOD 1000U
#endif

#ifndef CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE
#define CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE 120U
#endif

_Static_assert(CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE <= 1000U,
               "status LED brightness must be 0..1000 permille");

#ifndef CONFIG_M61_PAIR_BUTTON_ENABLE
#define CONFIG_M61_PAIR_BUTTON_ENABLE 1
#endif

/* Ai-M61-32S-Kit BOOT is BL616 GPIO2, active high.  Override this with
 * GPIO_PIN_0 when using an external button on boards where BOOT is not readable
 * after startup. */
#ifndef CONFIG_M61_PAIR_BUTTON_PIN
#define CONFIG_M61_PAIR_BUTTON_PIN GPIO_PIN_2
#endif

#ifndef CONFIG_M61_PAIR_BUTTON_ACTIVE_HIGH
#define CONFIG_M61_PAIR_BUTTON_ACTIVE_HIGH 1
#endif

#ifndef CONFIG_M61_PAIR_BUTTON_HOLD_MS
#define CONFIG_M61_PAIR_BUTTON_HOLD_MS 1500U
#endif

#define DS5_AUTO_TASK_PERIOD_MS 250
#define DS5_DISCONNECT_CLEANUP_DELAY_MS 750

struct hidp_channel {
    struct bt_l2cap_br_chan br;
    const char *name;
    uint16_t psm;
    bool connected;
};

enum status_led_mode {
    STATUS_LED_OFF,
    STATUS_LED_BOOT,
    STATUS_LED_CONNECTING,
    STATUS_LED_CONNECTED,
    STATUS_LED_RED,
    STATUS_LED_GREEN,
    STATUS_LED_BLUE,
};

static struct bflb_device_s *uart0;
static TaskHandle_t app_start_handle;
static TaskHandle_t auto_task_handle;
static TaskHandle_t led_task_handle;
static TaskHandle_t bridge_task_handle;

static struct bt_br_discovery_result discovery_results[HIDP_DISCOVERY_SLOTS];
static struct bt_conn *default_conn;
static struct bt_conn *pending_conn;
static bt_addr_t last_dualsense_addr;
static bool have_last_dualsense_addr;
static bool bt_ready;
static bool storage_ready;
static bool br_discovery_active;
static bool br_discovery_found_dualsense;
static bool pairing_mode_active;
static bool pairing_mode_after_disconnect;
static bool auto_connect_after_scan;
static bool auto_sequence_started;
static bool auto_security_requested;
static bool auto_sdp_requested;
static bool auto_hidp_requested;
static bool br_security_ready;
static bool hidp_active_open_allowed;
static bool br_connectable_enabled;
static bool br_discoverable_enabled;
static bool hidp_l2cap_servers_registered;
static bool full_report_seen;
static bool hidp_report_log_enabled;
static bool hidp_full_report_banner_printed;
static uint32_t hidp_parsed_reports;
static uint32_t hidp_full_reports;
static uint32_t hidp_mic_audio_reports;
static uint32_t hidp_haptics_reports_sent;
static uint32_t hidp_haptics_send_errors;
static uint32_t hidp_haptics_not_connected;
static int hidp_haptics_last_error;
static uint32_t hidp_audio_reports_sent;
static uint32_t hidp_audio_status_reports_sent;
static uint32_t hidp_audio_send_errors;
static uint32_t hidp_audio_not_connected;
static int hidp_audio_last_error;
static uint32_t hidp_usb_output_reports_forwarded;
static uint32_t hidp_usb_output_reports_coalesced;
static uint32_t hidp_usb_output_reports_failed;
static int hidp_usb_output_last_error;
static TickType_t hidp_usb_output_next_tick;
static bool hidp_mic_status_known;
static bool hidp_last_mic_active;
static TickType_t hidp_next_mic_status_tick;
static TickType_t hidp_next_audio_report_tick;
static bool usb_start_after_dualsense_done;
static bool dualsense_headphones_connected;
static uint8_t auto_bringup_attempts;
static uint8_t auto_saved_addr_attempts;
static TickType_t br_connected_tick;
static TickType_t auto_next_action_tick;
static TickType_t auto_next_hidp_tick;
static TickType_t auto_next_bringup_tick;
static dualsense_output_context_t output_ctx;
static struct bflb_device_s *status_led_gpio;
static struct bflb_device_s *status_led_pwm;
static bool status_led_pwm_ready;
static bool status_led_red_on;
static bool status_led_green_on;
static bool status_led_blue_on;
static bool status_led_booting = true;
static struct bflb_device_s *pair_button_gpio;
static uint16_t pair_button_hold_ms;
static bool pair_button_hold_fired;
static bool status_led_override;
static enum status_led_mode status_led_override_mode;
static bool hid_control_prepare_pending;
static bool hid_interrupt_prepare_pending;
static bool disconnect_cleanup_scheduled;
static TickType_t disconnect_cleanup_tick;
static m61_bt_tx_scheduler_t hidp_tx_scheduler;

#if CONFIG_M61_DS5_AUTO_START
static bool auto_start_enabled = true;
#else
static bool auto_start_enabled;
#endif

static struct bt_uuid_16 hid_service_uuid = BT_UUID_INIT_16(BT_SDP_HID_SVCLASS);
extern struct net_buf_pool sdp_pool;
static struct bt_sdp_discover_params sdp_params;

static int hidp_control_l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan);
static int hidp_interrupt_l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan);
static int request_pairing_mode(void);

static struct hidp_channel hid_control = {
    .name = "control",
    .psm = HIDP_PSM_CONTROL,
};

static struct hidp_channel hid_interrupt = {
    .name = "interrupt",
    .psm = HIDP_PSM_INTERRUPT,
};

static struct bt_l2cap_server hid_control_server = {
    .psm = HIDP_PSM_CONTROL,
    .sec_level = BT_SECURITY_L2,
    .accept = hidp_control_l2cap_accept,
};

static struct bt_l2cap_server hid_interrupt_server = {
    .psm = HIDP_PSM_INTERRUPT,
    .sec_level = BT_SECURITY_L2,
    .accept = hidp_interrupt_l2cap_accept,
};

extern void shell_init_with_task(struct bflb_device_s *shell);

static void print_addr(const char *prefix, const bt_addr_t *addr)
{
    char addr_str[BT_ADDR_STR_LEN];

    bt_addr_to_str(addr, addr_str, sizeof(addr_str));
    printf("%s%s\r\n", prefix, addr_str);
}

static bool tick_due(TickType_t now, TickType_t due)
{
    return (int32_t)(now - due) >= 0;
}

static uint32_t hidp_tx_realtime_stale_count(void)
{
    uintptr_t flags = bflb_irq_save();
    uint32_t stale =
        hidp_tx_scheduler.metrics.classes[M61_BT_TX_CLASS_REALTIME].stale;

    bflb_irq_restore(flags);
    return stale;
}

static void auto_schedule_retry(uint32_t delay_ms)
{
    auto_sequence_started = false;
    auto_next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
}

static void auto_reset_link_state(void)
{
    auto_security_requested = false;
    auto_sdp_requested = false;
    auto_hidp_requested = false;
    br_security_ready = false;
    full_report_seen = false;
    hidp_full_report_banner_printed = false;
    hidp_parsed_reports = 0;
    hidp_full_reports = 0;
    hidp_mic_audio_reports = 0;
    hidp_audio_reports_sent = 0;
    hidp_audio_status_reports_sent = 0;
    hidp_audio_send_errors = 0;
    hidp_audio_not_connected = 0;
    hidp_audio_last_error = 0;
    hidp_usb_output_reports_forwarded = 0;
    hidp_usb_output_reports_coalesced = 0;
    hidp_usb_output_reports_failed = 0;
    hidp_usb_output_last_error = 0;
    hidp_usb_output_next_tick = 0;
    hidp_mic_status_known = false;
    hidp_last_mic_active = false;
    hidp_next_mic_status_tick = 0;
    hidp_next_audio_report_tick = 0;
    dualsense_headphones_connected = false;
    auto_bringup_attempts = 0;
    auto_next_hidp_tick = 0;
    auto_next_bringup_tick = 0;
    br_connected_tick = xTaskGetTickCount();
}

static void auto_reset_saved_addr_attempts(void)
{
    auto_saved_addr_attempts = 0;
}

static void schedule_deferred_disconnect_cleanup(uint32_t delay_ms)
{
    disconnect_cleanup_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    disconnect_cleanup_scheduled = true;
}

static bool deferred_conn_unref_pending(void)
{
    return false;
}

static bool deferred_disconnect_cleanup_active(void)
{
    return disconnect_cleanup_scheduled ||
           hid_control_prepare_pending ||
           hid_interrupt_prepare_pending ||
           deferred_conn_unref_pending();
}

static bool request_is_pending_or_done(int err)
{
    return err == 0 || err == -EALREADY;
}

static int br_scan_state_result(const char *action, int err, bool *state, bool target)
{
    if (err == 0 || err == -EALREADY) {
        *state = target;
        return 0;
    }

    printf("BR/EDR %s failed: %d\r\n", action, err);
    return err;
}

static void br_set_scan_mode(bool connectable, bool discoverable,
                             const char *reason)
{
    int err;

    if (!bt_ready) {
        return;
    }

    if (connectable) {
        err = bt_br_set_connectable(true);
        br_scan_state_result("set connectable", err, &br_connectable_enabled, true);
    } else {
        err = bt_br_set_connectable(false);
        br_scan_state_result("clear connectable", err, &br_connectable_enabled, false);
    }
    if (discoverable) {
        err = bt_br_set_discoverable(true);
        br_scan_state_result("set discoverable", err, &br_discoverable_enabled, true);
    } else {
        err = bt_br_set_discoverable(false);
        br_scan_state_result("clear discoverable", err, &br_discoverable_enabled, false);
    }

    printf("BR/EDR scan mode reason=%s connectable=%d discoverable=%d\r\n",
           reason ? reason : "manual",
           br_connectable_enabled ? 1 : 0,
           br_discoverable_enabled ? 1 : 0);
}

static void persist_last_dualsense_addr(void)
{
#if defined(CONFIG_BT_SETTINGS)
    EfErrCode err;

    if (!storage_ready || !have_last_dualsense_addr) {
        return;
    }

    err = ef_set_env_blob(DS5_LAST_ADDR_KEY,
                          last_dualsense_addr.val,
                          sizeof(last_dualsense_addr.val));
    if (err == EF_NO_ERR) {
        err = ef_save_env();
    }
    printf("persist last DualSense addr result=%d\r\n", err);
#endif
}

static void load_last_dualsense_addr(void)
{
#if defined(CONFIG_BT_SETTINGS)
    uint8_t raw[sizeof(last_dualsense_addr.val)];
    size_t saved_len = 0;
    size_t read_len;

    if (!storage_ready) {
        return;
    }

    read_len = ef_get_env_blob(DS5_LAST_ADDR_KEY, raw, sizeof(raw), &saved_len);
    if (read_len == sizeof(raw) && saved_len == sizeof(raw)) {
        memcpy(last_dualsense_addr.val, raw, sizeof(raw));
        have_last_dualsense_addr = true;
        print_addr("loaded saved DualSense addr ", &last_dualsense_addr);
    } else {
        printf("no saved DualSense addr loaded\r\n");
    }
#endif
}

static void remember_dualsense_addr(const bt_addr_t *addr, bool persist)
{
    last_dualsense_addr = *addr;
    have_last_dualsense_addr = true;
    auto_reset_saved_addr_attempts();
    print_addr("stored DualSense addr ", &last_dualsense_addr);
    if (persist) {
        persist_last_dualsense_addr();
    }
}

static void forget_dualsense_addr(void)
{
    have_last_dualsense_addr = false;
    auto_reset_saved_addr_attempts();
    memset(&last_dualsense_addr, 0, sizeof(last_dualsense_addr));
#if defined(CONFIG_BT_SETTINGS)
    if (storage_ready) {
        EfErrCode err = ef_del_and_save_env(DS5_LAST_ADDR_KEY);
        printf("forget saved DualSense addr result=%d\r\n", err);
        return;
    }
#endif
    printf("forgot RAM DualSense addr\r\n");
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int parse_addr(const char *text, bt_addr_t *addr)
{
    uint8_t msb_first[6];
    size_t count = 0;

    while (*text) {
        int hi;
        int lo;

        while (*text == ':' || *text == '-' || *text == ' ') {
            text++;
        }
        if (!*text) {
            break;
        }
        if (count >= sizeof(msb_first)) {
            return -EINVAL;
        }

        hi = hex_nibble(*text++);
        if (hi < 0 || !*text) {
            return -EINVAL;
        }
        lo = hex_nibble(*text++);
        if (lo < 0) {
            return -EINVAL;
        }

        msb_first[count++] = (uint8_t)((hi << 4) | lo);
    }

    if (count != sizeof(msb_first)) {
        return -EINVAL;
    }

    for (size_t i = 0; i < sizeof(msb_first); i++) {
        addr->val[i] = msb_first[sizeof(msb_first) - 1 - i];
    }

    return 0;
}

static int parse_hex_bytes(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t count = 0;

    while (*text) {
        int hi;
        int lo;

        while (*text == ':' || *text == '-' || *text == ' ' || *text == ',') {
            text++;
        }
        if (!*text) {
            break;
        }
        if (count >= out_size) {
            return -ENOSPC;
        }

        hi = hex_nibble(*text++);
        if (hi < 0 || !*text) {
            return -EINVAL;
        }
        lo = hex_nibble(*text++);
        if (lo < 0) {
            return -EINVAL;
        }

        out[count++] = (uint8_t)((hi << 4) | lo);
    }

    *out_len = count;
    return count ? 0 : -EINVAL;
}

static int parse_hex_args(int argc, char **argv, uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t total = 0;

    for (int i = 0; i < argc; i++) {
        size_t chunk_len = 0;
        int err = parse_hex_bytes(argv[i], out + total, out_size - total, &chunk_len);
        if (err) {
            return err;
        }
        total += chunk_len;
    }

    *out_len = total;
    return total ? 0 : -EINVAL;
}

static void print_hex_line(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
        if (i + 1 < len) {
            printf(" ");
        }
    }
    printf("\r\n");
}

static int parse_u8_hex(const char *text, uint8_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (!text || !*text || !out) {
        return -EINVAL;
    }

    value = strtoul(text, &end, 16);
    if (*end != '\0' || value > 0xFF) {
        return -EINVAL;
    }

    *out = (uint8_t)value;
    return 0;
}

static void m61_reboot_to_uart_download(void)
{
#if defined(BL616) || defined(BL618DG)
    printf("rebooting M61 to UART download mode\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    HBN_Set_User_Boot_Config(1);
    bl_sys_reset_por();
    while (1) {
    }
#else
    printf("M61 UART download reboot unsupported on this chip\r\n");
#endif
}

static void m61_native_usb_gpio_init(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (!gpio) {
        printf("USB gpio device not found; native USB pins not configured\r\n");
        return;
    }

#if defined(BL616)
    bflb_gpio_init(gpio, GPIO_PIN_32, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(gpio, GPIO_PIN_33, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    printf("M61 native USB pins configured: BL616 GPIO32=USB_DP GPIO33=USB_DM\r\n");
#elif defined(BL618DG)
    bflb_gpio_init(gpio, GPIO_PIN_40, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(gpio, GPIO_PIN_41, GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    printf("M61 native USB pins configured: BL618DG GPIO40=USB_DP GPIO41=USB_DM\r\n");
#else
    printf("M61 native USB pin override unsupported on this chip\r\n");
#endif
}

static int m61_usb_pins_command(int argc, char **argv)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (!gpio) {
        printf("USB gpio device not found\r\n");
        return -ENODEV;
    }

#if defined(BL616)
    const uint32_t dp_pin = GPIO_PIN_32;
    const uint32_t dm_pin = GPIO_PIN_33;
    const char *target = "BL616 GPIO32=USB_DP GPIO33=USB_DM";
#elif defined(BL618DG)
    const uint32_t dp_pin = GPIO_PIN_40;
    const uint32_t dm_pin = GPIO_PIN_41;
    const char *target = "BL618DG GPIO40=USB_DP GPIO41=USB_DM";
#else
    printf("USB pin test unsupported on this chip\r\n");
    return -ENOTSUP;
#endif

    if (argc < 3 || strcmp(argv[2], "help") == 0 || strcmp(argv[2], "status") == 0) {
        printf("USB pin test target: %s\r\n", target);
        printf("Disconnect native USB from PC before driving pins. Keep CH340 power/serial connected.\r\n");
        printf("Usage:\r\n");
        printf("  ds5 usb-pins dp-high\r\n");
        printf("  ds5 usb-pins dm-high\r\n");
        printf("  ds5 usb-pins both-low\r\n");
        printf("  ds5 usb-pins both-high\r\n");
        printf("  ds5 usb-pins restore\r\n");
        return 0;
    }

    if (strcmp(argv[2], "restore") == 0) {
        m61_native_usb_gpio_init();
        printf("USB pins restored to native USB mode; reset or replug native USB for a clean enum retry\r\n");
        return 0;
    }

    bflb_gpio_init(gpio, dp_pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio, dm_pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);

    if (strcmp(argv[2], "dp-high") == 0) {
        bflb_gpio_set(gpio, dp_pin);
        bflb_gpio_reset(gpio, dm_pin);
        printf("USB pin test: DP high, DM low on %s\r\n", target);
        return 0;
    }

    if (strcmp(argv[2], "dm-high") == 0) {
        bflb_gpio_reset(gpio, dp_pin);
        bflb_gpio_set(gpio, dm_pin);
        printf("USB pin test: DP low, DM high on %s\r\n", target);
        return 0;
    }

    if (strcmp(argv[2], "both-low") == 0) {
        bflb_gpio_reset(gpio, dp_pin);
        bflb_gpio_reset(gpio, dm_pin);
        printf("USB pin test: DP low, DM low on %s\r\n", target);
        return 0;
    }

    if (strcmp(argv[2], "both-high") == 0) {
        bflb_gpio_set(gpio, dp_pin);
        bflb_gpio_set(gpio, dm_pin);
        printf("USB pin test: DP high, DM high on %s\r\n", target);
        return 0;
    }

    printf("usage: ds5 usb-pins [dp-high|dm-high|both-low|both-high|restore]\r\n");
    return -EINVAL;
}

static void m61_usb_bus_detach_pulse(uint32_t delay_ms)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (!gpio) {
        printf("USB gpio device not found; skipping detach pulse\r\n");
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        return;
    }

#if defined(BL616)
    const uint32_t dp_pin = GPIO_PIN_32;
    const uint32_t dm_pin = GPIO_PIN_33;
#elif defined(BL618DG)
    const uint32_t dp_pin = GPIO_PIN_40;
    const uint32_t dm_pin = GPIO_PIN_41;
#else
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    return;
#endif

    bflb_gpio_init(gpio, dp_pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(gpio, dm_pin, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_reset(gpio, dp_pin);
    bflb_gpio_reset(gpio, dm_pin);
    printf("USB bus detach pulse: DP/DM low for %lu ms\r\n", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    m61_native_usb_gpio_init();
}

static int m61_usb_cycle_command(void)
{
    m61_usb_gamepad_deinit();
    m61_usb_bus_detach_pulse(350);
    return m61_usb_gamepad_reinit();
}

static void pair_button_init(void)
{
#if CONFIG_M61_PAIR_BUTTON_ENABLE
    pair_button_gpio = bflb_device_get_by_name("gpio");
    if (!pair_button_gpio) {
        printf("pair button gpio device not found\r\n");
        return;
    }

    bflb_gpio_init(pair_button_gpio,
                   CONFIG_M61_PAIR_BUTTON_PIN,
                   GPIO_INPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_0);
    printf("pair button pin=%u active_high=%u hold_ms=%u\r\n",
           (unsigned int)CONFIG_M61_PAIR_BUTTON_PIN,
           (unsigned int)CONFIG_M61_PAIR_BUTTON_ACTIVE_HIGH,
           (unsigned int)CONFIG_M61_PAIR_BUTTON_HOLD_MS);
#endif
}

static void pair_button_poll(void)
{
#if CONFIG_M61_PAIR_BUTTON_ENABLE
    bool level;
    bool pressed;

    if (!pair_button_gpio) {
        return;
    }
    level = bflb_gpio_read(pair_button_gpio, CONFIG_M61_PAIR_BUTTON_PIN);
    pressed = CONFIG_M61_PAIR_BUTTON_ACTIVE_HIGH ? level : !level;
    if (!pressed) {
        pair_button_hold_ms = 0U;
        pair_button_hold_fired = false;
        return;
    }
    if (pair_button_hold_fired) {
        return;
    }
    if (pair_button_hold_ms < CONFIG_M61_PAIR_BUTTON_HOLD_MS) {
        pair_button_hold_ms = (uint16_t)(pair_button_hold_ms +
                                         DS5_AUTO_TASK_PERIOD_MS);
    }
    if (pair_button_hold_ms >= CONFIG_M61_PAIR_BUTTON_HOLD_MS) {
        pair_button_hold_fired = true;
        printf("pair button long press: entering pairing mode\r\n");
        (void)request_pairing_mode();
    }
#endif
}

#if CONFIG_M61_STATUS_LED_ENABLE
static void status_led_write_pin(uint32_t pin, bool on)
{
    if (!status_led_gpio) {
        return;
    }

    if (status_led_pwm_ready) {
        if (pin == CONFIG_M61_STATUS_LED_RED_PIN) {
            if (on) {
                bflb_pwm_v2_channel_positive_start(status_led_pwm, PWM_CH2);
            } else {
                bflb_pwm_v2_channel_positive_stop(status_led_pwm, PWM_CH2);
            }
            return;
        }
        if (pin == CONFIG_M61_STATUS_LED_GREEN_PIN) {
            if (on) {
                bflb_pwm_v2_channel_positive_start(status_led_pwm, PWM_CH3);
            } else {
                bflb_pwm_v2_channel_positive_stop(status_led_pwm, PWM_CH3);
            }
            return;
        }
        if (pin == CONFIG_M61_STATUS_LED_BLUE_PIN) {
            if (on) {
                bflb_pwm_v2_channel_negative_start(status_led_pwm, PWM_CH3);
            } else {
                bflb_pwm_v2_channel_negative_stop(status_led_pwm, PWM_CH3);
            }
            return;
        }
    }

#if CONFIG_M61_STATUS_LED_ACTIVE_HIGH
    if (on) {
        bflb_gpio_set(status_led_gpio, pin);
    } else {
        bflb_gpio_reset(status_led_gpio, pin);
    }
#else
    if (on) {
        bflb_gpio_reset(status_led_gpio, pin);
    } else {
        bflb_gpio_set(status_led_gpio, pin);
    }
#endif
}

static void status_led_set_rgb(bool red, bool green, bool blue)
{
    if (red == status_led_red_on && green == status_led_green_on &&
        blue == status_led_blue_on) {
        return;
    }

    status_led_write_pin(CONFIG_M61_STATUS_LED_RED_PIN, red);
    status_led_write_pin(CONFIG_M61_STATUS_LED_GREEN_PIN, green);
    status_led_write_pin(CONFIG_M61_STATUS_LED_BLUE_PIN, blue);
    status_led_red_on = red;
    status_led_green_on = green;
    status_led_blue_on = blue;
}

static const char *status_led_mode_name(enum status_led_mode mode)
{
    switch (mode) {
        case STATUS_LED_OFF:
            return "off";
        case STATUS_LED_BOOT:
            return "boot";
        case STATUS_LED_CONNECTING:
            return "connecting";
        case STATUS_LED_CONNECTED:
            return "connected";
        case STATUS_LED_RED:
            return "red";
        case STATUS_LED_GREEN:
            return "green";
        case STATUS_LED_BLUE:
            return "blue";
        default:
            return "unknown";
    }
}

static void status_led_apply_mode(enum status_led_mode mode, bool blink_on)
{
    switch (mode) {
        case STATUS_LED_BOOT:
            status_led_set_rgb(true, false, false);
            break;
        case STATUS_LED_GREEN:
            status_led_set_rgb(false, true, false);
            break;
        case STATUS_LED_CONNECTING:
            status_led_set_rgb(false, false, blink_on);
            break;
        case STATUS_LED_CONNECTED:
        case STATUS_LED_BLUE:
            status_led_set_rgb(false, false, true);
            break;
        case STATUS_LED_RED:
            status_led_set_rgb(true, false, false);
            break;
        case STATUS_LED_OFF:
        default:
            status_led_set_rgb(false, false, false);
            break;
    }
}

static enum status_led_mode status_led_auto_mode(void)
{
    if (status_led_booting) {
        return STATUS_LED_BOOT;
    }

    if (default_conn && hid_control.connected && hid_interrupt.connected) {
        return STATUS_LED_CONNECTED;
    }

    if (pairing_mode_active) {
        return STATUS_LED_CONNECTING;
    }

    return STATUS_LED_OFF;
}

static void status_led_init(void)
{
    const struct bflb_pwm_v2_config_s pwm_config = {
        .clk_source = BFLB_SYSTEM_PBCLK,
        .clk_div = 80,
        .period = CONFIG_M61_STATUS_LED_PWM_PERIOD,
    };
    struct bflb_pwm_v2_channel_config_s channel_config = {
        .positive_polarity = PWM_POLARITY_ACTIVE_HIGH,
        .negative_polarity = PWM_POLARITY_ACTIVE_HIGH,
        .positive_stop_state = PWM_STATE_INACTIVE,
        .negative_stop_state = PWM_STATE_INACTIVE,
        .positive_brake_state = PWM_STATE_INACTIVE,
        .negative_brake_state = PWM_STATE_INACTIVE,
        .dead_time = 0,
    };
    uint16_t high_threshold =
        (uint16_t)(((uint32_t)CONFIG_M61_STATUS_LED_PWM_PERIOD *
                    CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE) /
                   1000U);

    status_led_gpio = bflb_device_get_by_name("gpio");
    if (!status_led_gpio) {
        printf("status LED gpio device not found\r\n");
        return;
    }

    status_led_pwm = bflb_device_get_by_name("pwm_v2_0");
    if (status_led_pwm && CONFIG_M61_STATUS_LED_ACTIVE_HIGH &&
        CONFIG_M61_STATUS_LED_RED_PIN == GPIO_PIN_12 &&
        CONFIG_M61_STATUS_LED_GREEN_PIN == GPIO_PIN_14 &&
        CONFIG_M61_STATUS_LED_BLUE_PIN == GPIO_PIN_15) {
        /* BL616 PWM0 pin group repeats every eight GPIOs: GPIO12 is CH2P,
         * GPIO14 is CH3P and GPIO15 is CH3N.  Hardware PWM keeps LED dimming
         * entirely outside the realtime audio/Bluetooth task path. */
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_RED_PIN,
                       GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLDOWN |
                           GPIO_SMT_EN | GPIO_DRV_1);
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_GREEN_PIN,
                       GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLDOWN |
                           GPIO_SMT_EN | GPIO_DRV_1);
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_BLUE_PIN,
                       GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLDOWN |
                           GPIO_SMT_EN | GPIO_DRV_1);
        /* GPIO15 carries CH3N.  BL616 defaults PWM0 to single-ended routing,
         * which leaves negative endpoints disconnected even if NEN is set. */
        bflb_pwm_v2_feature_control(status_led_pwm,
                                    PWM_CMD_IO_SEL,
                                    PWM_IO_SEL_DIFF_END);
        bflb_pwm_v2_init(status_led_pwm, &pwm_config);
        bflb_pwm_v2_channel_init(status_led_pwm, PWM_CH2, &channel_config);
        bflb_pwm_v2_channel_init(status_led_pwm, PWM_CH3, &channel_config);
        bflb_pwm_v2_channel_set_threshold(status_led_pwm,
                                          PWM_CH2,
                                          0,
                                          high_threshold);
        bflb_pwm_v2_channel_set_threshold(status_led_pwm,
                                          PWM_CH3,
                                          0,
                                          high_threshold);
        bflb_pwm_v2_start(status_led_pwm);
        status_led_pwm_ready = true;
        printf("status LED hardware PWM brightness=%u/1000\r\n",
               (unsigned int)CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE);
    } else {
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_RED_PIN,
                       GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_GREEN_PIN,
                       GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        bflb_gpio_init(status_led_gpio,
                       CONFIG_M61_STATUS_LED_BLUE_PIN,
                       GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        printf("status LED PWM unavailable; using digital GPIO fallback\r\n");
    }

    status_led_red_on = false;
    status_led_green_on = false;
    status_led_blue_on = false;
    status_led_apply_mode(STATUS_LED_BOOT, true);
}

static void status_led_task(void *pvParameters)
{
    bool blink_on = false;

    (void)pvParameters;

    while (1) {
        enum status_led_mode mode =
            status_led_override ? status_led_override_mode : status_led_auto_mode();

        blink_on = !blink_on;
        status_led_apply_mode(mode, blink_on);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_M61_STATUS_LED_BLINK_MS));
    }
}

static void status_led_print(void)
{
    enum status_led_mode mode =
        status_led_override ? status_led_override_mode : status_led_auto_mode();

    printf("led enabled=1 active_high=%d pwm=%d brightness=%u/1000 red=%u green=%u blue=%u override=%d mode=%s\r\n",
           CONFIG_M61_STATUS_LED_ACTIVE_HIGH ? 1 : 0,
           status_led_pwm_ready ? 1 : 0,
           (unsigned int)CONFIG_M61_STATUS_LED_BRIGHTNESS_PERMILLE,
           (unsigned int)CONFIG_M61_STATUS_LED_RED_PIN,
           (unsigned int)CONFIG_M61_STATUS_LED_GREEN_PIN,
           (unsigned int)CONFIG_M61_STATUS_LED_BLUE_PIN,
           status_led_override ? 1 : 0,
           status_led_mode_name(mode));
}

static int status_led_set_override_from_name(const char *name)
{
    if (strcmp(name, "auto") == 0) {
        status_led_override = false;
        printf("led auto mode\r\n");
        return 0;
    }

    if (strcmp(name, "off") == 0) {
        status_led_override_mode = STATUS_LED_OFF;
    } else if (strcmp(name, "boot") == 0) {
        status_led_override_mode = STATUS_LED_BOOT;
    } else if (strcmp(name, "green") == 0) {
        status_led_override_mode = STATUS_LED_GREEN;
    } else if (strcmp(name, "connecting") == 0) {
        status_led_override_mode = STATUS_LED_CONNECTING;
    } else if (strcmp(name, "connected") == 0 || strcmp(name, "blue") == 0) {
        status_led_override_mode = STATUS_LED_BLUE;
    } else if (strcmp(name, "red") == 0) {
        status_led_override_mode = STATUS_LED_RED;
    } else {
        return -EINVAL;
    }

    status_led_override = true;
    status_led_apply_mode(status_led_override_mode, true);
    printf("led override mode=%s\r\n", status_led_mode_name(status_led_override_mode));
    return 0;
}

static void status_led_self_test(void)
{
    status_led_override = true;

    status_led_override_mode = STATUS_LED_RED;
    status_led_apply_mode(status_led_override_mode, true);
    vTaskDelay(pdMS_TO_TICKS(350));

    status_led_override_mode = STATUS_LED_GREEN;
    status_led_apply_mode(status_led_override_mode, true);
    vTaskDelay(pdMS_TO_TICKS(350));

    status_led_override_mode = STATUS_LED_BLUE;
    status_led_apply_mode(status_led_override_mode, true);
    vTaskDelay(pdMS_TO_TICKS(350));

    status_led_override = false;
    status_led_apply_mode(status_led_auto_mode(), true);
    printf("led self-test complete; returned to auto mode\r\n");
}

static void status_led_finish_boot(void)
{
    status_led_booting = false;
    status_led_apply_mode(status_led_auto_mode(), false);
}
#else
static void status_led_init(void)
{
}

static void status_led_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelete(NULL);
}

static void status_led_print(void)
{
    printf("led enabled=0\r\n");
}

static int status_led_set_override_from_name(const char *name)
{
    (void)name;
    printf("led disabled\r\n");
    return -ENOTSUP;
}

static void status_led_self_test(void)
{
    printf("led disabled\r\n");
}


static void status_led_finish_boot(void)
{
}
#endif

static bool eir_name_matches(const uint8_t *eir, size_t eir_len, const char *needle)
{
    size_t pos = 0;
    size_t needle_len = strlen(needle);

    while (pos < eir_len) {
        uint8_t field_len = eir[pos];

        if (field_len == 0) {
            break;
        }
        if (pos + 1U + field_len > eir_len || field_len < 1U) {
            break;
        }

        uint8_t field_type = eir[pos + 1U];
        const uint8_t *field_data = &eir[pos + 2U];
        size_t field_data_len = field_len - 1U;

        if (field_type == 0x08 || field_type == 0x09) {
            printf("name: %.*s\r\n", (int)field_data_len, field_data);
            if (field_data_len >= needle_len) {
                for (size_t i = 0; i <= field_data_len - needle_len; i++) {
                    if (memcmp(&field_data[i], needle, needle_len) == 0) {
                        return true;
                    }
                }
            }
        }

        pos += 1U + field_len;
    }

    return false;
}

static void hidp_channel_prepare(struct hidp_channel *channel);
static void schedule_hidp_channel_prepare(struct hidp_channel *channel, uint32_t delay_ms);
static void process_deferred_disconnect_cleanup(TickType_t now);
static void bt_br_discv_cb(struct bt_br_discovery_result *results, size_t count);

static void hidp_l2cap_connected(struct bt_l2cap_chan *chan)
{
    struct hidp_channel *channel = CONTAINER_OF(chan, struct hidp_channel, br.chan);

    channel->connected = true;
    printf("HIDP %s connected: rx_cid=0x%04x tx_cid=0x%04x rx_mtu=%u tx_mtu=%u\r\n",
           channel->name,
           channel->br.rx.cid,
           channel->br.tx.cid,
           channel->br.rx.mtu,
           channel->br.tx.mtu);

    if (channel->br.tx.mtu < HIDP_TX_MAX_LEN) {
        printf("HIDP %s unusable MTU: need >=%u, negotiated %u\r\n",
               channel->name,
               (unsigned int)HIDP_TX_MAX_LEN,
               channel->br.tx.mtu);
    }

    if (hid_control.connected && hid_interrupt.connected) {
        pairing_mode_active = false;
        br_set_scan_mode(false, false, "hidp-ready");
        auto_next_bringup_tick = xTaskGetTickCount();
    }
}

static void hidp_l2cap_disconnected(struct bt_l2cap_chan *chan)
{
    struct hidp_channel *channel = CONTAINER_OF(chan, struct hidp_channel, br.chan);

    printf("HIDP %s disconnected\r\n", channel->name);
    channel->connected = false;
    hidp_mic_status_known = false;
    hidp_last_mic_active = false;
    hidp_next_mic_status_tick = 0;
    hidp_next_audio_report_tick = 0;
    auto_hidp_requested = false;
    auto_next_hidp_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(DS5_DISCONNECT_CLEANUP_DELAY_MS);
    schedule_hidp_channel_prepare(channel, DS5_DISCONNECT_CLEANUP_DELAY_MS);
}

static int hidp_l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    struct hidp_channel *channel = CONTAINER_OF(chan, struct hidp_channel, br.chan);
    uint8_t hdr = buf->len ? buf->data[0] : 0;
    uint8_t report_id = buf->len > 1 ? buf->data[1] : 0;
    dualsense_state_t state = { 0 };
    dualsense_parse_result_t parse = { 0 };

    if (hidp_report_log_enabled) {
        printf("HIDP %s rx len=%u hdr=0x%02x report=0x%02x data=",
               channel->name,
               buf->len,
               hdr,
               report_id);
        print_hex_line(buf->data, buf->len > 32 ? 32 : buf->len);
    }

    if (channel == &hid_control && buf->len >= 3 && buf->data[0] == 0xA3) {
        m61_usb_gamepad_store_feature_report(buf->data[1], buf->data + 2, buf->len - 2);
    }

    if (channel == &hid_interrupt &&
        dualsense_parse_report(buf->data, buf->len, &state, &parse)) {
        bool first_full_report = state.is_full_report && !full_report_seen;

        hidp_parsed_reports++;
        if (state.is_full_report) {
            hidp_full_reports++;
        }
        if (hidp_report_log_enabled) {
            char line[320];

            dualsense_format_state(&state, line, sizeof(line));
            printf("parsed %s\r\n", line);
        }
        if (state.is_full_report) {
            dualsense_headphones_connected = state.headphones;
        }
        if (state.is_full_report && parse.payload_len >= M61_DS5_USB_INPUT_PAYLOAD_LEN) {
            m61_usb_gamepad_send_report01(buf->data + parse.payload_offset,
                                          M61_DS5_USB_INPUT_PAYLOAD_LEN);
        } else {
            m61_usb_gamepad_send_state(&state);
        }
        if (state.is_full_report) {
            full_report_seen = true;
            if (first_full_report || !hidp_full_report_banner_printed) {
                printf("M61 HIDP full report path is alive\r\n");
                hidp_full_report_banner_printed = true;
            }
        }
    } else if (channel == &hid_interrupt && parse.kind == DS5_REPORT_MIC_AUDIO) {
        hidp_mic_audio_reports++;
        if (parse.payload_len > 1U) {
            m61_usb_gamepad_submit_mic_opus(buf->data + parse.payload_offset + 1U,
                                            parse.payload_len - 1U);
        }
        if (hidp_report_log_enabled) {
            printf("parsed microphone/audio payload len=%u\r\n", buf->len);
        }
    }
    return 0;
}

static void hidp_l2cap_encrypt_change(struct bt_l2cap_chan *chan, uint8_t hci_status)
{
    struct hidp_channel *channel = CONTAINER_OF(chan, struct hidp_channel, br.chan);

    printf("HIDP %s encrypt_change status=%u\r\n", channel->name, hci_status);
}

static const struct bt_l2cap_chan_ops hidp_l2cap_ops = {
    .connected = hidp_l2cap_connected,
    .disconnected = hidp_l2cap_disconnected,
    .encrypt_change = hidp_l2cap_encrypt_change,
    .recv = hidp_l2cap_recv,
};

static void hidp_channel_prepare(struct hidp_channel *channel)
{
    memset(&channel->br, 0, sizeof(channel->br));
    channel->br.chan.ops = &hidp_l2cap_ops;
    /* The BR/EDR stack defaults an application-created outgoing channel to
     * the Bluetooth minimum MTU (48).  Explicitly advertise the same 672-byte
     * receive capacity used by the registered incoming HIDP servers so the
     * DualSense may negotiate full 0x31/0x32 reports in either direction. */
    channel->br.rx.mtu = CONFIG_BT_L2CAP_TX_MTU;
#if defined(CONFIG_BT_L2CAP_DYNAMIC_CHANNEL)
    channel->br.chan.required_sec_level = BT_SECURITY_L2;
#endif
}

static int hidp_l2cap_accept_channel(struct hidp_channel *channel,
                                     struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    struct bt_conn_info info;
    char addr_str[BT_ADDR_STR_LEN] = "<unknown>";

    if (!channel || !conn || !chan || conn->type != BT_CONN_TYPE_BR) {
        return -EACCES;
    }

    process_deferred_disconnect_cleanup(xTaskGetTickCount());
    if (channel->connected || channel->br.chan.conn || deferred_disconnect_cleanup_active()) {
        printf("HIDP %s incoming busy connected=%d chan_conn=%d cleanup=%d\r\n",
               channel->name,
               channel->connected ? 1 : 0,
               channel->br.chan.conn ? 1 : 0,
               deferred_disconnect_cleanup_active() ? 1 : 0);
        return -ENOMEM;
    }

    if (bt_conn_get_info(conn, &info) == 0) {
        bt_addr_to_str(info.br.dst, addr_str, sizeof(addr_str));
    }

    hidp_channel_prepare(channel);
    *chan = &channel->br.chan;
    auto_hidp_requested = true;
    printf("HIDP %s incoming accepted from %s PSM 0x%04x\r\n",
           channel->name,
           addr_str,
           channel->psm);

    return 0;
}

static int hidp_control_l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
    return hidp_l2cap_accept_channel(&hid_control, conn, chan);
}

static int hidp_interrupt_l2cap_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
    return hidp_l2cap_accept_channel(&hid_interrupt, conn, chan);
}

static int hidp_l2cap_servers_register(void)
{
    int ctrl_err;
    int intr_err;

    if (hidp_l2cap_servers_registered) {
        return 0;
    }

    ctrl_err = bt_l2cap_br_server_register(&hid_control_server);
    intr_err = bt_l2cap_br_server_register(&hid_interrupt_server);
    if (ctrl_err && ctrl_err != -EADDRINUSE) {
        printf("HIDP control server register failed: %d\r\n", ctrl_err);
    }
    if (intr_err && intr_err != -EADDRINUSE) {
        printf("HIDP interrupt server register failed: %d\r\n", intr_err);
    }

    if ((ctrl_err == 0 || ctrl_err == -EADDRINUSE) &&
        (intr_err == 0 || intr_err == -EADDRINUSE)) {
        hidp_l2cap_servers_registered = true;
        printf("HIDP BR/EDR servers ready control=0x%04x interrupt=0x%04x\r\n",
               HIDP_PSM_CONTROL,
               HIDP_PSM_INTERRUPT);
        return 0;
    }

    return ctrl_err ? ctrl_err : intr_err;
}

static void schedule_hidp_channel_prepare(struct hidp_channel *channel, uint32_t delay_ms)
{
    if (channel == &hid_control) {
        hid_control_prepare_pending = true;
    } else if (channel == &hid_interrupt) {
        hid_interrupt_prepare_pending = true;
    }
    schedule_deferred_disconnect_cleanup(delay_ms);
}

static void process_deferred_disconnect_cleanup(TickType_t now)
{
    if (!deferred_disconnect_cleanup_active()) {
        return;
    }
    if (disconnect_cleanup_scheduled && !tick_due(now, disconnect_cleanup_tick)) {
        return;
    }

    if (hid_control_prepare_pending) {
        if (!hid_control.connected) {
            hidp_channel_prepare(&hid_control);
            printf("HIDP control cleanup prepared\r\n");
        }
        hid_control_prepare_pending = false;
    }
    if (hid_interrupt_prepare_pending) {
        if (!hid_interrupt.connected) {
            hidp_channel_prepare(&hid_interrupt);
            printf("HIDP interrupt cleanup prepared\r\n");
        }
        hid_interrupt_prepare_pending = false;
    }

    disconnect_cleanup_scheduled = false;
}

static int hidp_channel_connect(struct hidp_channel *channel)
{
    int err;

    process_deferred_disconnect_cleanup(xTaskGetTickCount());
    if (deferred_disconnect_cleanup_active()) {
        return -EAGAIN;
    }
    if (!default_conn) {
        return -ENOTCONN;
    }
    if (channel->connected || channel->br.chan.conn) {
        return -EALREADY;
    }

    hidp_channel_prepare(channel);
    err = bt_l2cap_chan_connect(default_conn, &channel->br.chan, channel->psm);
    if (err) {
        printf("HIDP %s connect PSM 0x%04x failed: %d\r\n", channel->name, channel->psm, err);
    } else {
        printf("HIDP %s connect PSM 0x%04x pending\r\n", channel->name, channel->psm);
    }
    return err;
}

static int hidp_send_raw_timeout(struct hidp_channel *channel,
                                 const uint8_t *data,
                                 size_t len,
                                 s32_t alloc_timeout)
{
    struct net_buf *buf;
    int ret;

    if (!channel->connected) {
        return -ENOTCONN;
    }
    if (!len || len > channel->br.tx.mtu) {
        return -EMSGSIZE;
    }

    buf = bt_l2cap_create_pdu_timeout(NULL, 0, alloc_timeout);
    if (!buf) {
        return -ENOMEM;
    }

    net_buf_add_mem(buf, data, len);
    ret = bt_l2cap_chan_send(&channel->br.chan, buf);
    if (ret < 0) {
        net_buf_unref(buf);
        return ret;
    }

    return 0;
}

static int hidp_send_raw(struct hidp_channel *channel, const uint8_t *data, size_t len)
{
    return hidp_send_raw_timeout(channel, data, len, K_FOREVER);
}

static int hidp_set_protocol_report(void)
{
    uint8_t packet[] = { HIDP_TRANSACTION_SET_PROTOCOL_REPORT };
    return hidp_send_raw(&hid_control, packet, sizeof(packet));
}

static int hidp_get_report(uint8_t report_type, uint8_t report_id)
{
    uint8_t packet[] = { (uint8_t)(0x40 | (report_type & 0x03)), report_id };
    return hidp_send_raw(&hid_control, packet, sizeof(packet));
}

static int hidp_set_report(uint8_t report_type, const uint8_t *report, size_t report_len)
{
    uint8_t packet[HIDP_TX_MAX_LEN];

    if (!report || report_len + 1 > sizeof(packet)) {
        return -EMSGSIZE;
    }

    packet[0] = (uint8_t)(HIDP_TRANSACTION_SET_REPORT | (report_type & 0x03));
    memcpy(packet + 1, report, report_len);
    return hidp_send_raw(&hid_control, packet, report_len + 1);
}

static int hidp_send_data_output_timeout(const uint8_t *report,
                                         size_t report_len,
                                         s32_t alloc_timeout)
{
    uint8_t packet[HIDP_TX_MAX_LEN];

    if (!report || report_len + 1 > sizeof(packet)) {
        return -EMSGSIZE;
    }

    packet[0] = HIDP_TRANSACTION_DATA_OUTPUT;
    memcpy(packet + 1, report, report_len);
    return hidp_send_raw_timeout(&hid_interrupt, packet, report_len + 1, alloc_timeout);
}

static int hidp_send_data_output(const uint8_t *report, size_t report_len)
{
    return hidp_send_data_output_timeout(report, report_len, K_FOREVER);
}

static int hidp_alloc_interrupt_report(size_t report_len,
                                       s32_t alloc_timeout,
                                       struct net_buf **buf_out,
                                       uint8_t **report_out)
{
    struct net_buf *buf;
    uint8_t *packet;

    if (!buf_out || !report_out || report_len + 1U > HIDP_TX_MAX_LEN) {
        return -EINVAL;
    }
    if (!hid_interrupt.connected) {
        return -ENOTCONN;
    }
    if (report_len + 1U > hid_interrupt.br.tx.mtu) {
        return -EMSGSIZE;
    }
    buf = bt_l2cap_create_pdu_timeout(NULL, 0, alloc_timeout);
    if (!buf) {
        return -ENOMEM;
    }
    if (net_buf_tailroom(buf) < report_len + 1U) {
        net_buf_unref(buf);
        return -EMSGSIZE;
    }
    packet = net_buf_add(buf, report_len + 1U);
    packet[0] = HIDP_TRANSACTION_DATA_OUTPUT;
    *buf_out = buf;
    *report_out = packet + 1U;
    return 0;
}

static int hidp_submit_interrupt_report(struct net_buf *buf)
{
    int err = bt_l2cap_chan_send(&hid_interrupt.br.chan, buf);

    if (err < 0) {
        net_buf_unref(buf);
        return err;
    }

    /* The Bouffalo BR/EDR L2CAP API returns bytes sent on success. */
    return 0;
}

static int hidp_send_ds5_output_init(void)
{
    uint8_t report31[DS5_OUTPUT_REPORT31_BT_LEN];
    uint8_t report32[DS5_OUTPUT_REPORT32_BT_LEN];
    int first_err = 0;
    int err;

    if (!dualsense_output_make_report31(&output_ctx, report31, sizeof(report31))) {
        printf("build output 0x31 failed\r\n");
        return -EINVAL;
    }
    err = hidp_send_data_output(report31, sizeof(report31));
    printf("output 0x31 over interrupt result=%d\r\n", err);
    if (err && !first_err) {
        first_err = err;
    }

    err = hidp_set_report(HIDP_REPORT_TYPE_OUTPUT, report31, sizeof(report31));
    printf("set-report 0x31 over control result=%d\r\n", err);
    if (err && !first_err) {
        first_err = err;
    }

    if (!dualsense_output_make_report32(&output_ctx, report32, sizeof(report32))) {
        printf("build output 0x32 failed\r\n");
        return -EINVAL;
    }
    err = hidp_send_data_output(report32, sizeof(report32));
    printf("output 0x32 over interrupt result=%d\r\n", err);
    if (err && !first_err) {
        first_err = err;
    }

    err = hidp_set_report(HIDP_REPORT_TYPE_OUTPUT, report32, sizeof(report32));
    printf("set-report 0x32 over control result=%d\r\n", err);
    if (err && !first_err) {
        first_err = err;
    }

    return first_err;
}

static int hidp_dualsense_bringup(void)
{
    static const uint8_t feature_ids[] = { 0x09, 0x20, 0x22, 0x05, 0x70 };
    int first_err = 0;
    int err;

    err = hidp_set_protocol_report();
    printf("set protocol report result=%d\r\n", err);
    if (err && !first_err) {
        first_err = err;
    }

    for (size_t i = 0; i < sizeof(feature_ids); i++) {
        err = hidp_get_report(HIDP_REPORT_TYPE_FEATURE, feature_ids[i]);
        printf("get feature 0x%02x result=%d\r\n", feature_ids[i], err);
        if (err && !first_err) {
            first_err = err;
        }
    }

    err = hidp_send_ds5_output_init();
    if (err && !first_err) {
        first_err = err;
    }

    return first_err;
}

static int hidp_set_feature_from_usb(uint8_t report_id, const uint8_t *data, size_t len)
{
    uint8_t packet[HIDP_TX_MAX_LEN];

    if (!data || len + 2 > sizeof(packet)) {
        return -EMSGSIZE;
    }

    packet[0] = (uint8_t)(HIDP_TRANSACTION_SET_REPORT | HIDP_REPORT_TYPE_FEATURE);
    packet[1] = report_id;
    memcpy(packet + 2, data, len);
    dualsense_feature_fill_crc(packet + 1, len + 1);
    return hidp_send_raw_timeout(&hid_control,
                                 packet,
                                 len + 2,
                                 K_NO_WAIT);
}

static int hidp_send_usb_output_report(const uint8_t *data, size_t len, bool includes_report_id)
{
    struct net_buf *buf;
    uint8_t *report31;
    const uint8_t *payload = data;
    size_t payload_len = len;
    int err;

    if (!data || len == 0) {
        return -EINVAL;
    }

    if (includes_report_id) {
        if (data[0] != 0x02 || len < 2) {
            return -EINVAL;
        }
        payload = data + 1;
        payload_len = len - 1;
    }

    err = hidp_alloc_interrupt_report(DS5_OUTPUT_REPORT31_BT_LEN,
                                      K_NO_WAIT,
                                      &buf,
                                      &report31);
    if (err) {
        return err;
    }
    if (!dualsense_output_make_report31_from_usb(&output_ctx,
                                                 payload,
                                                 payload_len,
                                                 report31,
                                                 DS5_OUTPUT_REPORT31_BT_LEN)) {
        net_buf_unref(buf);
        return -EINVAL;
    }
    return hidp_submit_interrupt_report(buf);
}

static int hidp_send_audio_status_report(bool mic_active)
{
    struct net_buf *buf;
    uint8_t *report32;
    int err;

    if (!hid_interrupt.connected) {
        hidp_audio_not_connected++;
        hidp_audio_last_error = -ENOTCONN;
        return -ENOTCONN;
    }

    err = hidp_alloc_interrupt_report(DS5_OUTPUT_REPORT32_BT_LEN,
                                      HIDP_BT_AUDIO_ALLOC_TIMEOUT,
                                      &buf,
                                      &report32);
    if (err) {
        hidp_audio_send_errors++;
        hidp_audio_last_error = err;
        return err;
    }
    if (!dualsense_output_make_report32_audio_status(&output_ctx,
                                                     mic_active,
                                                     DS5_AUDIO_BUFFER_LENGTH_DEFAULT,
                                                     report32,
                                                     DS5_OUTPUT_REPORT32_BT_LEN)) {
        net_buf_unref(buf);
        hidp_audio_send_errors++;
        hidp_audio_last_error = -EINVAL;
        return -EINVAL;
    }

    err = hidp_submit_interrupt_report(buf);
    if (err) {
        hidp_audio_send_errors++;
        hidp_audio_last_error = err;
    } else {
        hidp_audio_status_reports_sent++;
        hidp_audio_last_error = 0;
    }
    return err;
}

static int __attribute__((unused)) hidp_send_audio_report(const uint8_t *haptics,
                                  const uint8_t *speaker_opus,
                                  bool headset,
                                  bool mic_active)
{
    static const uint8_t zero_haptics[M61_DS5_HAPTICS_BLOCK_LEN];
    uint8_t report36[DS5_OUTPUT_REPORT36_BT_LEN];
    const uint8_t *haptics_payload = haptics;
    uint8_t speaker_block = headset ? DS5_OUTPUT_HEADSET_BLOCK_ID : DS5_OUTPUT_SPEAKER_BLOCK_ID;
    bool has_haptics = haptics != NULL;
    bool has_speaker = speaker_opus != NULL;
    int err;

    if (!has_haptics && !has_speaker) {
        return -EINVAL;
    }
    if (!hid_interrupt.connected) {
        if (has_haptics) {
            hidp_haptics_not_connected++;
            hidp_haptics_last_error = -ENOTCONN;
        }
        if (has_speaker) {
            hidp_audio_not_connected++;
            hidp_audio_last_error = -ENOTCONN;
        }
        return -ENOTCONN;
    }
    if (!haptics_payload && has_speaker) {
        haptics_payload = zero_haptics;
    }

    if (!dualsense_output_make_report36_audio(&output_ctx,
                                              haptics_payload,
                                              haptics_payload ? M61_DS5_HAPTICS_BLOCK_LEN : 0,
                                              speaker_opus,
                                              has_speaker ? M61_DS5_SPEAKER_OPUS_LEN : 0,
                                              speaker_block,
                                              mic_active,
                                              DS5_AUDIO_BUFFER_LENGTH_DEFAULT,
                                              report36,
                                              sizeof(report36))) {
        if (has_haptics) {
            hidp_haptics_send_errors++;
            hidp_haptics_last_error = -EINVAL;
        }
        if (has_speaker) {
            hidp_audio_send_errors++;
            hidp_audio_last_error = -EINVAL;
        }
        return -EINVAL;
    }

    err = hidp_send_data_output_timeout(report36,
                                        sizeof(report36),
                                        HIDP_BT_AUDIO_ALLOC_TIMEOUT);
    if (err) {
        if (has_haptics) {
            hidp_haptics_send_errors++;
            hidp_haptics_last_error = err;
        }
        if (has_speaker) {
            hidp_audio_send_errors++;
            hidp_audio_last_error = err;
        }
    } else {
        if (has_haptics) {
            hidp_haptics_reports_sent++;
            hidp_haptics_last_error = 0;
        }
        if (has_speaker) {
            hidp_audio_reports_sent++;
            hidp_audio_last_error = 0;
        }
    }
    return err;
}

static int hidp_send_audio_pair(const m61_audio_epoch_pair_t *pair,
                                bool headset,
                                bool mic_active)
{
    struct net_buf *buf;
    uint8_t *packet;
    int err;

    if (!pair) {
        return -EINVAL;
    }
    if (!hid_interrupt.connected) {
        hidp_audio_not_connected++;
        hidp_audio_last_error = -ENOTCONN;
        return -ENOTCONN;
    }
    if (1U + DS5_OUTPUT_AUDIO_RT_BT_LEN > hid_interrupt.br.tx.mtu) {
        hidp_audio_send_errors++;
        hidp_audio_last_error = -EMSGSIZE;
        return -EMSGSIZE;
    }

    buf = bt_l2cap_create_pdu_timeout(NULL, 0, HIDP_BT_AUDIO_ALLOC_TIMEOUT);
    if (!buf) {
        hidp_audio_send_errors++;
        hidp_audio_last_error = -ENOMEM;
        return -ENOMEM;
    }
    if (net_buf_tailroom(buf) < 1U + DS5_OUTPUT_AUDIO_RT_BT_LEN) {
        net_buf_unref(buf);
        hidp_audio_send_errors++;
        hidp_audio_last_error = -EMSGSIZE;
        return -EMSGSIZE;
    }
    packet = net_buf_add(buf, 1U + DS5_OUTPUT_AUDIO_RT_BT_LEN);
    packet[0] = HIDP_TRANSACTION_DATA_OUTPUT;
    if (!dualsense_output_make_audio_rt(
            &output_ctx,
            pair->haptics[0],
            pair->haptics[1],
            pair->speaker_enabled ? pair->speaker_opus[0] : NULL,
            pair->speaker_enabled ? pair->speaker_opus[1] : NULL,
            M61_DS5_SPEAKER_OPUS_LEN,
            headset ? DS5_OUTPUT_HEADSET_BLOCK_ID
                    : DS5_OUTPUT_SPEAKER_BLOCK_ID,
            mic_active,
            DS5_AUDIO_BUFFER_LENGTH_DEFAULT,
            packet + 1U,
            DS5_OUTPUT_AUDIO_RT_BT_LEN)) {
        net_buf_unref(buf);
        hidp_audio_send_errors++;
        hidp_audio_last_error = -EINVAL;
        return -EINVAL;
    }

    err = bt_l2cap_chan_send(&hid_interrupt.br.chan, buf);
    if (err < 0) {
        net_buf_unref(buf);
        hidp_audio_send_errors++;
        hidp_audio_last_error = err;
        return err;
    }
    hidp_audio_reports_sent++;
    hidp_haptics_reports_sent++;
    hidp_audio_last_error = 0;
    hidp_haptics_last_error = 0;
    return 0;
}

static void __attribute__((unused)) handle_usb_host_report(
    const m61_usb_gamepad_host_report_t *report)
{
    uint8_t report_id;
    const uint8_t *payload;
    size_t payload_len;
    int err = 0;

    if (!report || report->len == 0) {
        return;
    }

    report_id = report->report_id;
    payload = report->data;
    payload_len = report->len;

    if (report->report_type == HIDP_REPORT_TYPE_OUTPUT) {
        bool includes_report_id = (report_id == 0);

        err = hidp_send_usb_output_report(payload, payload_len, includes_report_id);
        if (err) {
            hidp_usb_output_reports_failed++;
            hidp_usb_output_last_error = err;
            printf("usb output report forward failed id=0x%02x len=%u err=%d\r\n",
                   includes_report_id ? payload[0] : report_id,
                   (unsigned int)payload_len,
                   err);
        } else {
            hidp_usb_output_reports_forwarded++;
            hidp_usb_output_last_error = 0;
        }
        return;
    }

    if (report->report_type == HIDP_REPORT_TYPE_FEATURE) {
        if (report_id == 0) {
            report_id = payload[0];
            payload++;
            payload_len--;
        }
        err = hidp_set_feature_from_usb(report_id, payload, payload_len);
        if (err) {
            printf("usb feature set forward failed id=0x%02x len=%u err=%d\r\n",
                   report_id,
                   (unsigned int)payload_len,
                   err);
        }
    }
}

static void usb_hid_bridge_task(void *pvParameters)
{
    bool feature_pending = false;
    bool feature_set_pending = false;
    bool scheduler_link_ready = false;
    uint8_t pending_feature_report_id = 0;
    uint32_t pending_feature_requested_len = 0;
    m61_usb_gamepad_host_report_t pending_feature_set;

    (void)pvParameters;

    while (1) {
        TickType_t now = xTaskGetTickCount();
        uint64_t now_us = bflb_mtimer_get_time_us();
        uint32_t generation = m61_usb_gamepad_audio_generation();
        m61_usb_gamepad_host_report_t host_report;
        m61_bt_tx_selection_t selection;
        uint8_t feature_reports_this_tick = 0;
        uint8_t host_reports_this_tick = 0;
        bool host_mic_active = m61_usb_gamepad_audio_in_active();
        bool bt_mic_active = host_mic_active;
        bool link_ready = hid_interrupt.connected;
        uint32_t eligible_classes = 0;

        m61_usb_gamepad_realtime_task();

        if (hidp_tx_scheduler.generation != generation) {
            m61_bt_tx_scheduler_reset_generation(&hidp_tx_scheduler,
                                                  generation);
            hidp_mic_status_known = false;
        }
        if (!link_ready && scheduler_link_ready) {
            m61_bt_tx_scheduler_reset_generation(&hidp_tx_scheduler,
                                                  generation);
        }
        scheduler_link_ready = link_ready;

        while (m61_bt_tx_scheduler_ingest_epoch_pair(&hidp_tx_scheduler)) {
        }

        if (link_ready &&
            (!hidp_mic_status_known ||
             hidp_last_mic_active != bt_mic_active ||
             (bt_mic_active && tick_due(now, hidp_next_mic_status_tick)))) {
            if (!hidp_tx_scheduler.state32.pending ||
                hidp_tx_scheduler.state32.payload.mic_active != bt_mic_active) {
                m61_bt_tx_scheduler_publish_state32(&hidp_tx_scheduler,
                                                     bt_mic_active,
                                                     generation,
                                                     now_us);
            }
        }

        while (host_reports_this_tick < CONFIG_M61_DS5_HOST_REPORTS_PER_TICK &&
               m61_usb_gamepad_take_host_report(&host_report)) {
            if (host_report.report_type == HIDP_REPORT_TYPE_OUTPUT) {
                if (hidp_tx_scheduler.state31.pending) {
                    hidp_usb_output_reports_coalesced++;
                }
                m61_bt_tx_scheduler_publish_state31(
                    &hidp_tx_scheduler,
                    host_report.data,
                    host_report.len,
                    host_report.report_id == 0,
                    generation,
                    now_us);
            } else {
                pending_feature_set = host_report;
                feature_set_pending = true;
                host_reports_this_tick++;
                break;
            }
            host_reports_this_tick++;
        }

        if (link_ready) {
            eligible_classes = M61_BT_TX_CLASS_MASK(
                                   M61_BT_TX_CLASS_REALTIME) |
                               M61_BT_TX_CLASS_MASK(
                                   M61_BT_TX_CLASS_STATE32);
            if (tick_due(now, hidp_usb_output_next_tick)) {
                eligible_classes |= M61_BT_TX_CLASS_MASK(
                    M61_BT_TX_CLASS_STATE31);
            }
        }

        if (m61_bt_tx_scheduler_select(&hidp_tx_scheduler,
                                       now_us,
                                       eligible_classes,
                                       &selection)) {
            int err = -ESTALE;

            if (selection.generation !=
                m61_usb_gamepad_audio_generation()) {
                m61_bt_tx_scheduler_reset_generation(
                    &hidp_tx_scheduler,
                    m61_usb_gamepad_audio_generation());
            } else if (m61_bt_tx_scheduler_selection_is_current(
                           &hidp_tx_scheduler, &selection)) {
                switch (selection.tx_class) {
                case M61_BT_TX_CLASS_REALTIME:
                    err = hidp_send_audio_pair(
                        selection.payload.realtime,
                        dualsense_headphones_connected,
                        bt_mic_active);
                    break;
                case M61_BT_TX_CLASS_STATE31:
                    err = hidp_send_usb_output_report(
                        selection.payload.state31->report,
                        selection.payload.state31->len,
                        selection.payload.state31->includes_id);
                    break;
                case M61_BT_TX_CLASS_STATE32:
                    err = hidp_send_audio_status_report(
                        selection.payload.state32->mic_active);
                    break;
                default:
                    err = -EINVAL;
                    break;
                }

                if (err == -ENOMEM || err == -EAGAIN) {
                    m61_bt_tx_scheduler_finish(
                        &hidp_tx_scheduler,
                        &selection,
                        M61_BT_TX_FINISH_RETRY);
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                m61_bt_tx_scheduler_finish(
                    &hidp_tx_scheduler,
                    &selection,
                    err == 0 ? M61_BT_TX_FINISH_SUCCESS
                             : M61_BT_TX_FINISH_DROP);
                if (selection.tx_class == M61_BT_TX_CLASS_STATE31) {
                    if (err == 0) {
                        hidp_usb_output_reports_forwarded++;
                        hidp_usb_output_last_error = 0;
                        hidp_usb_output_next_tick =
                            now + pdMS_TO_TICKS(
                                      CONFIG_M61_DS5_USB_OUTPUT_MIN_INTERVAL_MS);
                    } else {
                        hidp_usb_output_reports_failed++;
                        hidp_usb_output_last_error = err;
                    }
                } else if (selection.tx_class == M61_BT_TX_CLASS_STATE32 &&
                           err == 0) {
                    hidp_mic_status_known = true;
                    hidp_last_mic_active =
                        selection.payload.state32->mic_active;
                    hidp_next_mic_status_tick =
                        now + pdMS_TO_TICKS(
                                  CONFIG_M61_DS5_MIC_STATUS_REFRESH_MS);
                }
            }
        }

        while (feature_reports_this_tick < CONFIG_M61_DS5_FEATURE_REPORTS_PER_TICK) {
            int err;

            if (!feature_pending &&
                !m61_usb_gamepad_take_feature_request(
                    &pending_feature_report_id,
                    &pending_feature_requested_len)) {
                break;
            }
            feature_pending = true;
            err = hidp_send_raw_timeout(
                &hid_control,
                (uint8_t[]){
                    (uint8_t)(0x40 | HIDP_REPORT_TYPE_FEATURE),
                    pending_feature_report_id,
                },
                2,
                K_NO_WAIT);
            if (err && err != -ENOTCONN) {
                printf("usb feature get forward failed id=0x%02x requested=%lu err=%d\r\n",
                       pending_feature_report_id,
                       (unsigned long)pending_feature_requested_len,
                       err);
            }
            if (err != 0) {
                break;
            }
            feature_pending = false;
            feature_reports_this_tick++;
        }

        if (feature_set_pending) {
            uint8_t report_id = pending_feature_set.report_id;
            const uint8_t *payload = pending_feature_set.data;
            size_t payload_len = pending_feature_set.len;
            int err;

            if (report_id == 0 && payload_len > 0) {
                report_id = payload[0];
                payload++;
                payload_len--;
            }
            err = hidp_set_feature_from_usb(report_id, payload, payload_len);
            if (err == 0) {
                feature_set_pending = false;
            } else if (err != -ENOMEM && err != -EAGAIN) {
                printf("usb feature set forward failed id=0x%02x len=%u err=%d\r\n",
                       report_id,
                       (unsigned int)payload_len,
                       err);
                feature_set_pending = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_M61_DS5_USB_BRIDGE_TASK_DELAY_MS));
    }
}

static uint8_t hid_sdp_discover_cb(struct bt_conn *conn, struct bt_sdp_client_result *result)
{
    (void)conn;

    if (!result || !result->resp_buf) {
        printf("SDP HID service not found\r\n");
        return BT_SDP_DISCOVER_UUID_STOP;
    }

    printf("SDP HID result len=%u next=%d uuid_type=%u\r\n",
           result->resp_buf->len,
           result->next_record_hint ? 1 : 0,
           result->uuid ? result->uuid->type : 0xff);
    print_hex_line(result->resp_buf->data, result->resp_buf->len > 96 ? 96 : result->resp_buf->len);

    return result->next_record_hint ? BT_SDP_DISCOVER_UUID_CONTINUE : BT_SDP_DISCOVER_UUID_STOP;
}

static int hid_sdp_discover(void)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    memset(&sdp_params, 0, sizeof(sdp_params));
    sdp_params.uuid = &hid_service_uuid.uuid;
    sdp_params.func = hid_sdp_discover_cb;
    sdp_params.pool = &sdp_pool;

    int err = bt_sdp_discover(default_conn, &sdp_params);
    if (err) {
        printf("SDP HID discover failed: %d\r\n", err);
    } else {
        printf("SDP HID discover pending\r\n");
    }
    return err;
}

static int start_inquiry(void)
{
    struct bt_br_discovery_param param = {
        .length = 0x08,
        .limited = 0,
    };
    int err;

    if (!bt_ready) {
        printf("Bluetooth not ready\r\n");
        return -EAGAIN;
    }
    if (br_discovery_active) {
        printf("BR/EDR discovery already active\r\n");
        return -EALREADY;
    }

    memset(discovery_results, 0, sizeof(discovery_results));
    br_discovery_found_dualsense = false;
    err = bt_br_discovery_start(&param,
                                discovery_results,
                                HIDP_DISCOVERY_SLOTS,
                                bt_br_discv_cb);
    if (err) {
        printf("BR/EDR discovery failed: %d\r\n", err);
    } else {
        br_discovery_active = true;
        printf("BR/EDR discovery started\r\n");
    }
    return err;
}

static int request_pairing_mode(void)
{
    int err;

    if (!bt_ready) {
        return -EAGAIN;
    }
    pairing_mode_active = true;
    auto_connect_after_scan = true;

    if (default_conn) {
        pairing_mode_after_disconnect = true;
        err = bt_conn_disconnect(default_conn,
                                 BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (err) {
            pairing_mode_after_disconnect = false;
            pairing_mode_active = false;
        }
        return err;
    }
    if (pending_conn) {
        printf("pairing mode deferred: outgoing connection pending\r\n");
        pairing_mode_active = false;
        auto_connect_after_scan = false;
        return -EBUSY;
    }
    if (br_discovery_active) {
        return 0;
    }

    br_set_scan_mode(true, false, "pairing-inquiry");
    err = start_inquiry();
    if (err && err != -EALREADY) {
        pairing_mode_active = false;
        auto_connect_after_scan = false;
    }
    return err;
}

static int connect_addr(const bt_addr_t *addr)
{
    struct bt_br_conn_param param = {
        .allow_role_switch = true,
    };
    struct bt_conn *conn;

    if (!bt_ready) {
        return -EAGAIN;
    }
    process_deferred_disconnect_cleanup(xTaskGetTickCount());
    if (deferred_disconnect_cleanup_active()) {
        printf("BR/EDR cleanup pending; retry later\r\n");
        return -EAGAIN;
    }
    if (br_discovery_active) {
        printf("BR/EDR discovery active; retry connect later\r\n");
        return -EAGAIN;
    }
    if (default_conn || pending_conn) {
        return -EALREADY;
    }

    print_addr("Connecting ", addr);
    conn = bt_conn_create_br(addr, &param);
    if (!conn) {
        printf("BR/EDR ACL create failed\r\n");
        return -EIO;
    }

    pending_conn = conn;
    printf("BR/EDR ACL create pending\r\n");
    return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info;
    char addr_str[BT_ADDR_STR_LEN];
    bool outgoing_conn;

    if (conn->type != BT_CONN_TYPE_BR) {
        return;
    }

    bt_conn_get_info(conn, &info);
    bt_addr_to_str(info.br.dst, addr_str, sizeof(addr_str));

    if (err) {
        printf("BR/EDR connect failed: %s err=%u\r\n", addr_str, err);
        if (pending_conn == conn) {
            pending_conn = NULL;
        }
        if (auto_start_enabled) {
            auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
        }
        pairing_mode_active = false;
        pairing_mode_after_disconnect = false;
        br_set_scan_mode(true, false, "connect-failed");
        return;
    }

    outgoing_conn = (pending_conn == conn);
    printf("BR/EDR connected: %s role=%s\r\n",
           addr_str,
           outgoing_conn ? "outgoing" : "incoming");
    auto_reset_saved_addr_attempts();
    pairing_mode_after_disconnect = false;
    auto_connect_after_scan = false;
    if (outgoing_conn) {
        default_conn = pending_conn;
        pending_conn = NULL;
    } else {
        if (default_conn && default_conn != conn) {
            printf("BR/EDR replacing stale default_conn\r\n");
        }
        default_conn = conn;
    }
    auto_sequence_started = true;
    hidp_active_open_allowed = outgoing_conn;
    auto_reset_link_state();
    remember_dualsense_addr(info.br.dst, true);
    hidp_channel_prepare(&hid_control);
    hidp_channel_prepare(&hid_interrupt);

    err = bt_conn_set_security(conn, BT_SECURITY_L2);
    auto_security_requested = true;
    printf("BR/EDR security request L2: %d\r\n", err);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct bt_conn_info info;
    char addr_str[BT_ADDR_STR_LEN];

    if (conn->type != BT_CONN_TYPE_BR) {
        return;
    }

    bt_conn_get_info(conn, &info);
    bt_addr_to_str(info.br.dst, addr_str, sizeof(addr_str));
    printf("BR/EDR disconnected: %s reason=%u\r\n", addr_str, reason);

    if (default_conn == conn) {
        default_conn = NULL;
    }
    if (pending_conn == conn) {
        pending_conn = NULL;
    }
    hidp_active_open_allowed = false;
    auto_connect_after_scan = false;
    auto_reset_link_state();
    auto_reset_saved_addr_attempts();
    schedule_hidp_channel_prepare(&hid_control, DS5_DISCONNECT_CLEANUP_DELAY_MS);
    schedule_hidp_channel_prepare(&hid_interrupt, DS5_DISCONNECT_CLEANUP_DELAY_MS);
    br_set_scan_mode(true, false, "disconnected");
    auto_sequence_started = true;
    printf("auto: idle after disconnect; waiting for incoming reconnect or 'ds5 auto now'\r\n");
    if (pairing_mode_after_disconnect) {
        pairing_mode_after_disconnect = false;
        (void)request_pairing_mode();
    } else {
        pairing_mode_active = false;
    }
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (conn->type != BT_CONN_TYPE_BR) {
        return;
    }

    printf("BR/EDR security changed: level=%u err=%u\r\n", level, err);
    br_security_ready = (err == BT_SECURITY_ERR_SUCCESS && level >= BT_SECURITY_L2);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    (void)conn;
    printf("auth passkey display: %06u\r\n", passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    printf("auth passkey confirm: %06u\r\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_pairing_confirm(struct bt_conn *conn)
{
    printf("auth pairing confirm\r\n");
    bt_conn_auth_pairing_confirm(conn);
}

static void auth_pincode_entry(struct bt_conn *conn, bool highsec)
{
    const char *pin = highsec ? "0000000000000000" : "0000";

    printf("auth pincode entry: using %s\r\n", highsec ? "16-digit zero PIN" : "0000");
    bt_conn_auth_pincode_entry(conn, pin);
}

static void auth_cancel(struct bt_conn *conn)
{
    (void)conn;
    printf("auth canceled\r\n");
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    (void)conn;
    printf("auth pairing complete bonded=%d\r\n", bonded ? 1 : 0);
}

static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    (void)conn;
    printf("auth pairing failed reason=%u\r\n", reason);
}

static const struct bt_conn_auth_cb auth_callbacks = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .pairing_confirm = auth_pairing_confirm,
    .pincode_entry = auth_pincode_entry,
    .cancel = auth_cancel,
    .pairing_complete = auth_pairing_complete,
    .pairing_failed = auth_pairing_failed,
};

static void bt_br_discv_cb(struct bt_br_discovery_result *results, size_t count)
{
    br_discovery_active = false;

    for (size_t i = 0; results && i < count; i++) {
        char addr_str[BT_ADDR_STR_LEN];
        uint32_t dev_class;
        bool is_dualsense;

        bt_addr_to_str(&results[i].addr, addr_str, sizeof(addr_str));
        dev_class = results[i].cod[0] |
                    ((uint32_t)results[i].cod[1] << 8) |
                    ((uint32_t)results[i].cod[2] << 16);

        printf("found addr=%s class=0x%06lx rssi=%d\r\n",
               addr_str,
               (unsigned long)dev_class,
               results[i].rssi);

        is_dualsense = eir_name_matches((const uint8_t *)results[i].eir,
                                        HIDP_EIR_MAX_LEN,
                                        "DualSense") ||
                       eir_name_matches((const uint8_t *)results[i].eir,
                                        HIDP_EIR_MAX_LEN,
                                        "Wireless Controller");
        if (is_dualsense) {
            br_discovery_found_dualsense = true;
            remember_dualsense_addr(&results[i].addr, true);
        }
    }

    printf("BR/EDR discovery complete, count=%u\r\n", (unsigned int)count);
    if (auto_connect_after_scan) {
        auto_connect_after_scan = false;
        if (br_discovery_found_dualsense && have_last_dualsense_addr &&
            !default_conn) {
            int err = connect_addr(&last_dualsense_addr);
            if (err && auto_start_enabled) {
                auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
            }
            if (err) {
                pairing_mode_active = false;
            }
        } else {
            printf("autoconnect scan did not find DualSense\r\n");
            pairing_mode_active = false;
            if (auto_start_enabled) {
                auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
            }
        }
    }
}

static void bt_enable_cb(int err)
{
    if (err) {
        printf("Bluetooth enable failed: %d\r\n", err);
        return;
    }

    bt_addr_le_t bt_addr;
    bt_get_local_public_address(&bt_addr);
    printf("Local public addr: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
           bt_addr.a.val[5],
           bt_addr.a.val[4],
           bt_addr.a.val[3],
           bt_addr.a.val[2],
           bt_addr.a.val[1],
           bt_addr.a.val[0]);

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&auth_callbacks);
    bt_set_bondable(true);
    bt_ready = true;
    hidp_l2cap_servers_register();
    br_set_scan_mode(true, false, "bt-ready-passive");

    printf("M61 DualSense HIDP probe ready. Use 'ds5 scan'.\r\n");
    status_led_finish_boot();
}

static void app_start_task(void *pvParameters)
{
    (void)pvParameters;

    btble_controller_init(configMAX_PRIORITIES - 1);
    hci_driver_init();
    bt_enable(bt_enable_cb);

    TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_M61_USB_START_AFTER_BT_TIMEOUT_MS);
    while (!bt_ready && !tick_due(xTaskGetTickCount(), deadline)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!bt_ready) {
        printf("Bluetooth ready timeout; USB will wait for DualSense full report\r\n");
    }

    printf("USB DualSense registration waits for controller full report\r\n");
    while (!full_report_seen) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    printf("DualSense full report seen; starting USB composite device\r\n");
    vTaskDelay(pdMS_TO_TICKS(CONFIG_M61_USB_START_DELAY_AFTER_BT_MS));
    m61_usb_bus_detach_pulse(350);
    m61_usb_gamepad_init();
    usb_start_after_dualsense_done = true;

    vTaskDelete(NULL);
}

static void auto_connect_task(void *pvParameters)
{
    (void)pvParameters;

    auto_next_action_tick = xTaskGetTickCount() +
                            pdMS_TO_TICKS(CONFIG_M61_DS5_AUTO_START_DELAY_MS);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        pair_button_poll();
        process_deferred_disconnect_cleanup(now);

        if (!bt_ready) {
            vTaskDelay(pdMS_TO_TICKS(DS5_AUTO_TASK_PERIOD_MS));
            continue;
        }

        if (!default_conn) {
            if (auto_start_enabled && !pairing_mode_active &&
                !auto_sequence_started && tick_due(now, auto_next_action_tick)) {
                int err;

                if (have_last_dualsense_addr &&
                    auto_saved_addr_attempts < CONFIG_M61_DS5_AUTO_SAVED_ADDR_ATTEMPTS) {
                    auto_saved_addr_attempts++;
                    printf("auto: saved-address connect attempt %u/%u\r\n",
                           (unsigned int)auto_saved_addr_attempts,
                           (unsigned int)CONFIG_M61_DS5_AUTO_SAVED_ADDR_ATTEMPTS);
                    print_addr("auto: connecting saved DualSense ", &last_dualsense_addr);
                    err = connect_addr(&last_dualsense_addr);
                } else {
                    if (have_last_dualsense_addr) {
                        printf("auto: saved-address attempts exhausted; scanning for DualSense\r\n");
                    } else {
                        printf("auto: no saved DualSense address; scanning\r\n");
                    }
                    auto_connect_after_scan = true;
                    err = start_inquiry();
                }

                if (err) {
                    printf("auto: start failed err=%d; retry later\r\n", err);
                    auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
                } else {
                    auto_sequence_started = true;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(DS5_AUTO_TASK_PERIOD_MS));
            continue;
        }

        if (!auto_security_requested) {
            int err = bt_conn_set_security(default_conn, BT_SECURITY_L2);
            auto_security_requested = true;
            printf("auto: security request result=%d\r\n", err);
        }

        bool security_timeout =
            auto_security_requested &&
            tick_due(now, br_connected_tick + pdMS_TO_TICKS(CONFIG_M61_DS5_SECURITY_TIMEOUT_MS));
        bool can_open_hidp = br_security_ready || security_timeout;

        if (can_open_hidp) {
            if (!auto_sdp_requested) {
                int err = hid_sdp_discover();
                auto_sdp_requested = true;
                printf("auto: SDP HID discover result=%d\r\n", err);
            }

            if (!hidp_active_open_allowed) {
                if (!auto_hidp_requested) {
                    auto_hidp_requested = true;
                    printf("auto: waiting for incoming HIDP channels\r\n");
                }
            } else if (!auto_hidp_requested && tick_due(now, auto_next_hidp_tick)) {
                int ctrl_err = hidp_channel_connect(&hid_control);
                int intr_err = hidp_channel_connect(&hid_interrupt);
                auto_hidp_requested =
                    request_is_pending_or_done(ctrl_err) &&
                    request_is_pending_or_done(intr_err);
                printf("auto: HIDP connect control=%d interrupt=%d\r\n", ctrl_err, intr_err);
                if (!auto_hidp_requested) {
                    auto_next_hidp_tick =
                        now + pdMS_TO_TICKS(CONFIG_M61_DS5_AUTO_RETRY_MS);
                }
            }
        }

        if (hid_control.connected &&
            hid_interrupt.connected &&
            !full_report_seen &&
            auto_bringup_attempts < CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES &&
            tick_due(now, auto_next_bringup_tick)) {
            int err;

            auto_bringup_attempts++;
            printf("auto: DualSense bring-up attempt %u/%u\r\n",
                   (unsigned int)auto_bringup_attempts,
                   (unsigned int)CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES);
            err = hidp_dualsense_bringup();
            printf("auto: DualSense bring-up result=%d\r\n", err);
            auto_next_bringup_tick =
                now + pdMS_TO_TICKS(CONFIG_M61_DS5_AUTO_BRINGUP_RETRY_MS);

            if (auto_bringup_attempts >= CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES &&
                !full_report_seen) {
                printf("auto: bring-up attempts exhausted; still waiting for report=0x31\r\n");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DS5_AUTO_TASK_PERIOD_MS));
    }
}

static void print_help(void)
{
    printf("Usage:\r\n");
    printf("  ds5 status\r\n");
    printf("  ds5 auto [on|off|now]\r\n");
    printf("  ds5 pair|scan\r\n");
    printf("  ds5 autoconnect\r\n");
    printf("  ds5 connect <aa:bb:cc:dd:ee:ff|last>\r\n");
    printf("  ds5 security\r\n");
    printf("  ds5 sdp\r\n");
    printf("  ds5 hidp\r\n");
    printf("  ds5 set-protocol\r\n");
    printf("  ds5 get-feature <id_hex>\r\n");
    printf("  ds5 output-init\r\n");
    printf("  ds5 bringup\r\n");
    printf("  ds5 log [normal|quiet]\r\n");
    printf("  ds5 send-ctrl <hex bytes>\r\n");
    printf("  ds5 send-intr <hex bytes>\r\n");
    printf("  ds5 forget\r\n");
    printf("  ds5 disconnect\r\n");
    printf("  ds5 usb-reinit\r\n");
    printf("  ds5 usb-cycle\r\n");
    printf("  ds5 decoder-bench [on|off]\r\n");
    printf("  ds5 usb-pins [status|dp-high|dm-high|both-low|both-high|restore]\r\n");
    printf("  ds5 reboot-isp\r\n");
}

int cmd_ds5(int argc, char **argv)
{
    uint8_t raw[HIDP_TX_MAX_LEN];
    size_t raw_len;
    int err;

    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_help();
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        m61_usb_gamepad_diag_t usb_diag;
        m61_usb_gamepad_get_diag(&usb_diag);

        printf("bt_ready=%d discovery=%d pairing=%d pending=%d connected=%d hid_control=%d hid_interrupt=%d have_last=%d\r\n",
               bt_ready ? 1 : 0,
               br_discovery_active ? 1 : 0,
               pairing_mode_active ? 1 : 0,
               pending_conn ? 1 : 0,
               default_conn ? 1 : 0,
               hid_control.connected ? 1 : 0,
               hid_interrupt.connected ? 1 : 0,
               have_last_dualsense_addr ? 1 : 0);
        printf("auto=%d sequence=%d security=%d sdp=%d hidp=%d full_report=%d usb_after_ds=%d bringup=%u/%u\r\n",
               auto_start_enabled ? 1 : 0,
               auto_sequence_started ? 1 : 0,
               br_security_ready ? 1 : 0,
               auto_sdp_requested ? 1 : 0,
               auto_hidp_requested ? 1 : 0,
               full_report_seen ? 1 : 0,
               usb_start_after_dualsense_done ? 1 : 0,
               (unsigned int)auto_bringup_attempts,
               (unsigned int)CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES);
        printf("bredr servers=%d connectable=%d discoverable=%d hidp_active_open=%d\r\n",
               hidp_l2cap_servers_registered ? 1 : 0,
               br_connectable_enabled ? 1 : 0,
               br_discoverable_enabled ? 1 : 0,
               hidp_active_open_allowed ? 1 : 0);
        printf("auto_saved attempts=%u/%u scan_after=%d\r\n",
               (unsigned int)auto_saved_addr_attempts,
               (unsigned int)CONFIG_M61_DS5_AUTO_SAVED_ADDR_ATTEMPTS,
               auto_connect_after_scan ? 1 : 0);
        printf("cleanup scheduled=%d control_prepare=%d interrupt_prepare=%d conn_unref=%d\r\n",
               disconnect_cleanup_scheduled ? 1 : 0,
               hid_control_prepare_pending ? 1 : 0,
               hid_interrupt_prepare_pending ? 1 : 0,
               deferred_conn_unref_pending() ? 1 : 0);
        printf("usb_gamepad ready=%d configured=%d busy=%d sent=%lu dropped=%lu\r\n",
               m61_usb_gamepad_ready() ? 1 : 0,
               m61_usb_gamepad_configured() ? 1 : 0,
               m61_usb_gamepad_busy() ? 1 : 0,
               (unsigned long)m61_usb_gamepad_sent_count(),
               (unsigned long)m61_usb_gamepad_drop_count());
        printf("usb_events init_result=%d last=%u error=%lu reset=%lu connected=%lu disconnected=%lu suspend=%lu resume=%lu configured=%lu init=%lu\r\n",
               m61_usb_gamepad_init_result(),
               (unsigned int)m61_usb_gamepad_last_event(),
               (unsigned long)m61_usb_gamepad_event_count(0),
               (unsigned long)m61_usb_gamepad_event_count(1),
               (unsigned long)m61_usb_gamepad_event_count(3),
               (unsigned long)m61_usb_gamepad_event_count(4),
               (unsigned long)m61_usb_gamepad_event_count(5),
               (unsigned long)m61_usb_gamepad_event_count(6),
               (unsigned long)m61_usb_gamepad_event_count(7),
               (unsigned long)m61_usb_gamepad_event_count(11));
        printf("usb_desc dev=%lu cfg=%lu qual=%lu str=%lu str0=%lu last_speed=%u last_str=%u\r\n",
               (unsigned long)usb_diag.device_desc,
               (unsigned long)usb_diag.config_desc,
               (unsigned long)usb_diag.qualifier_desc,
               (unsigned long)usb_diag.string_desc,
               (unsigned long)usb_diag.string0_desc,
               (unsigned int)usb_diag.last_speed,
               (unsigned int)usb_diag.last_string_index);
        printf("usb_hid get_report=%lu get_idle=%lu get_protocol=%lu set_report=%lu set_idle=%lu set_protocol=%lu last_report=0x%02x/%u\r\n",
               (unsigned long)usb_diag.hid_get_report,
               (unsigned long)usb_diag.hid_get_idle,
               (unsigned long)usb_diag.hid_get_protocol,
               (unsigned long)usb_diag.hid_set_report,
               (unsigned long)usb_diag.hid_set_idle,
               (unsigned long)usb_diag.hid_set_protocol,
               (unsigned int)usb_diag.last_report_id,
               (unsigned int)usb_diag.last_report_type);
        printf("usb_ds5 out=%lu last_out=0x%02x/%lu feature hit=%lu miss=%lu store=%lu feature_set_q=%u/%u host_drop=%lu\r\n",
               (unsigned long)usb_diag.hid_out_report,
               (unsigned int)usb_diag.last_out_report_id,
               (unsigned long)usb_diag.last_out_report_len,
               (unsigned long)usb_diag.feature_cache_hits,
               (unsigned long)usb_diag.feature_cache_misses,
               (unsigned long)usb_diag.feature_cache_stores,
               (unsigned int)usb_diag.feature_set_queue_depth,
               (unsigned int)usb_diag.feature_set_queue_high_water,
               (unsigned long)usb_diag.host_report_dropped);
        printf("usb_ds5_last flags=%02x/%02x/%02x rumble=%u/%u audio=0x%02x mute=%02x/%02x player=0x%02x light=%u/%u led=%02x%02x%02x\r\n",
               (unsigned int)usb_diag.last_out_flags0,
               (unsigned int)usb_diag.last_out_flags1,
               (unsigned int)usb_diag.last_out_flags2,
               (unsigned int)usb_diag.last_out_rumble_right,
               (unsigned int)usb_diag.last_out_rumble_left,
               (unsigned int)usb_diag.last_out_audio_control,
               (unsigned int)usb_diag.last_out_mute_light,
               (unsigned int)usb_diag.last_out_audio_mute,
               (unsigned int)usb_diag.last_out_player_lights,
               (unsigned int)usb_diag.last_out_light_fade,
               (unsigned int)usb_diag.last_out_light_brightness,
               (unsigned int)usb_diag.last_out_led_red,
               (unsigned int)usb_diag.last_out_led_green,
               (unsigned int)usb_diag.last_out_led_blue);
        printf("bt_state flags=%02x/%02x/%02x rumble=%u/%u audio=0x%02x mute=%02x/%02x player=0x%02x light=%u/%u led=%02x%02x%02x\r\n",
               (unsigned int)output_ctx.set_state[DS5_STATE_FLAGS0],
               (unsigned int)output_ctx.set_state[DS5_STATE_FLAGS1],
               (unsigned int)output_ctx.set_state[DS5_STATE_FLAGS2],
               (unsigned int)output_ctx.set_state[DS5_STATE_RUMBLE_RIGHT],
               (unsigned int)output_ctx.set_state[DS5_STATE_RUMBLE_LEFT],
               (unsigned int)output_ctx.set_state[DS5_STATE_AUDIO_CONTROL],
               (unsigned int)output_ctx.set_state[DS5_STATE_MUTE_LIGHT],
               (unsigned int)output_ctx.set_state[DS5_STATE_AUDIO_MUTE],
               (unsigned int)output_ctx.set_state[DS5_STATE_PLAYER_LIGHTS],
               (unsigned int)output_ctx.set_state[DS5_STATE_LIGHT_FADE],
               (unsigned int)output_ctx.set_state[DS5_STATE_LIGHT_BRIGHTNESS],
               (unsigned int)output_ctx.set_state[DS5_STATE_LED_RED],
               (unsigned int)output_ctx.set_state[DS5_STATE_LED_RED + 1],
               (unsigned int)output_ctx.set_state[DS5_STATE_LED_RED + 2]);
        printf("usb_audio open=%lu close=%lu out_open=%u in_open=%u last_open=%u last_close=%u out_pkts=%lu out_bytes=%lu ingress=%u/%u ingress_drop=%lu ingress_gap=%lu in_pkts=%lu in_bytes=%lu\r\n",
               (unsigned long)usb_diag.audio_open,
               (unsigned long)usb_diag.audio_close,
               (unsigned int)usb_diag.audio_out_open,
               (unsigned int)usb_diag.audio_in_open,
               (unsigned int)usb_diag.audio_last_open_intf,
               (unsigned int)usb_diag.audio_last_close_intf,
               (unsigned long)usb_diag.audio_out_packets,
               (unsigned long)usb_diag.audio_out_bytes,
               (unsigned int)usb_diag.audio_ingress_depth,
               (unsigned int)usb_diag.audio_ingress_high_water,
               (unsigned long)usb_diag.audio_ingress_dropped,
               (unsigned long)usb_diag.audio_ingress_gaps,
               (unsigned long)usb_diag.audio_in_packets,
               (unsigned long)usb_diag.audio_in_bytes);
        printf("usb_haptics queued=%lu sample_pairs=%lu nonzero=%lu qdrop=%lu deadline=%lu qdepth=%u peak=%u ds=%u mode=%u gain_q8=%u\r\n",
               (unsigned long)usb_diag.audio_haptic_blocks,
               (unsigned long)usb_diag.audio_haptic_sample_pairs,
               (unsigned long)usb_diag.audio_haptic_nonzero_blocks,
               (unsigned long)usb_diag.audio_haptic_queue_dropped,
               (unsigned long)usb_diag.audio_haptic_deadline_pairs,
               (unsigned int)usb_diag.audio_haptic_queue_depth,
               (unsigned int)usb_diag.audio_haptic_last_peak,
               (unsigned int)usb_diag.audio_haptic_downsample,
               (unsigned int)usb_diag.audio_haptic_resample_mode,
               (unsigned int)usb_diag.audio_haptic_gain_q8);
        uint32_t speaker_encode_attempts =
            usb_diag.audio_speaker_encoded + usb_diag.audio_speaker_encode_errors;
        uint32_t speaker_encode_avg_us =
            speaker_encode_attempts
                ? (usb_diag.audio_speaker_encode_us_total / speaker_encode_attempts)
                : 0;
        printf("usb_speaker frames=%lu encoded=%lu enc_err=%lu qdrop=%lu odrop=%lu cancel=%lu qdepth=%u oqdepth=%u enc_us=%lu/%lu/%lu opus_len=%u mono=%u bw=%u bitrate=%lu\r\n",
               (unsigned long)usb_diag.audio_speaker_frames,
               (unsigned long)usb_diag.audio_speaker_encoded,
               (unsigned long)usb_diag.audio_speaker_encode_errors,
               (unsigned long)usb_diag.audio_speaker_queue_dropped,
               (unsigned long)usb_diag.audio_speaker_opus_dropped,
               (unsigned long)usb_diag.audio_speaker_encode_cancelled,
               (unsigned int)usb_diag.audio_speaker_queue_depth,
               (unsigned int)usb_diag.audio_speaker_opus_queue_depth,
               (unsigned long)usb_diag.audio_speaker_encode_us_last,
               (unsigned long)speaker_encode_avg_us,
               (unsigned long)usb_diag.audio_speaker_encode_us_max,
               (unsigned int)usb_diag.audio_speaker_last_opus_len,
               (unsigned int)usb_diag.audio_speaker_opus_force_mono,
               (unsigned int)usb_diag.audio_speaker_opus_bandwidth,
               (unsigned long)usb_diag.audio_speaker_opus_bitrate);
        printf("usb_perf enabled=%u samples=%lu enc_p50/p95/p99=%lu/%lu/%lu cycles=%lu/%lu/%lu instret_avg=%lu\r\n",
               (unsigned int)usb_diag.perf_profile_enabled,
               (unsigned long)usb_diag.perf_encode_samples,
               (unsigned long)usb_diag.perf_encode_us_p50,
               (unsigned long)usb_diag.perf_encode_us_p95,
               (unsigned long)usb_diag.perf_encode_us_p99,
               (unsigned long)usb_diag.perf_encode_cycles_last,
               (unsigned long)usb_diag.perf_encode_cycles_average,
               (unsigned long)usb_diag.perf_encode_cycles_max,
               (unsigned long)usb_diag.perf_encode_instret_average);
        printf("usb_cache ic_access/miss/ppm=%lu/%lu/%lu dc_read/miss/ppm=%lu/%lu/%lu\r\n",
               (unsigned long)usb_diag.perf_icache_access_average,
               (unsigned long)usb_diag.perf_icache_miss_average,
               (unsigned long)usb_diag.perf_icache_miss_ppm,
               (unsigned long)usb_diag.perf_dcache_read_average,
               (unsigned long)usb_diag.perf_dcache_read_miss_average,
               (unsigned long)usb_diag.perf_dcache_read_miss_ppm);
        printf("usb_decode_perf enabled=%u samples=%lu dec_us=%lu/%lu/%lu dec_p50/p95/p99=%lu/%lu/%lu cycles=%lu/%lu/%lu instret_avg=%lu bench_frames=%lu bench_errors=%lu\r\n",
               (unsigned int)usb_diag.audio_decoder_benchmark_enabled,
               (unsigned long)usb_diag.perf_decode_samples,
               (unsigned long)usb_diag.perf_decode_us_last,
               (unsigned long)usb_diag.perf_decode_us_average,
               (unsigned long)usb_diag.perf_decode_us_max,
               (unsigned long)usb_diag.perf_decode_us_p50,
               (unsigned long)usb_diag.perf_decode_us_p95,
               (unsigned long)usb_diag.perf_decode_us_p99,
               (unsigned long)usb_diag.perf_decode_cycles_last,
               (unsigned long)usb_diag.perf_decode_cycles_average,
               (unsigned long)usb_diag.perf_decode_cycles_max,
               (unsigned long)usb_diag.perf_decode_instret_average,
               (unsigned long)usb_diag.audio_decoder_benchmark_frames,
               (unsigned long)usb_diag.audio_decoder_benchmark_errors);
        printf("usb_decode_cache ic_access/miss/ppm=%lu/%lu/%lu dc_read/miss/ppm=%lu/%lu/%lu\r\n",
               (unsigned long)usb_diag.perf_decode_icache_access_average,
               (unsigned long)usb_diag.perf_decode_icache_miss_average,
               (unsigned long)usb_diag.perf_decode_icache_miss_ppm,
               (unsigned long)usb_diag.perf_decode_dcache_read_average,
               (unsigned long)usb_diag.perf_decode_dcache_read_miss_average,
               (unsigned long)usb_diag.perf_decode_dcache_read_miss_ppm);
        printf("usb_latency ingress=%lu age_us=%lu/%lu/%lu/%lu irq_mask_cycles_max=%lu\r\n",
               (unsigned long)usb_diag.perf_ingress_samples,
               (unsigned long)usb_diag.perf_ingress_age_us_last,
               (unsigned long)usb_diag.perf_ingress_age_us_p95,
               (unsigned long)usb_diag.perf_ingress_age_us_p99,
               (unsigned long)usb_diag.perf_ingress_age_us_max,
               (unsigned long)usb_diag.perf_irq_mask_cycles_max);
        printf("usb_mic opus=%lu opus_nz=%lu odrop=%lu decoded=%lu dec_err=%lu pcm_bytes=%lu pcm_nz=%lu usb_nz_pkts=%lu usb_nz_bytes=%lu underflow=%lu oqdepth=%u ring=%u\r\n",
               (unsigned long)usb_diag.audio_mic_opus_packets,
               (unsigned long)usb_diag.audio_mic_opus_nonzero,
               (unsigned long)usb_diag.audio_mic_opus_dropped,
               (unsigned long)usb_diag.audio_mic_decoded,
               (unsigned long)usb_diag.audio_mic_decode_errors,
               (unsigned long)usb_diag.audio_mic_pcm_bytes,
               (unsigned long)usb_diag.audio_mic_pcm_nonzero_samples,
               (unsigned long)usb_diag.audio_mic_usb_nonzero_packets,
               (unsigned long)usb_diag.audio_mic_usb_nonzero_bytes,
               (unsigned long)usb_diag.audio_mic_underflow,
               (unsigned int)usb_diag.audio_mic_opus_queue_depth,
               (unsigned int)usb_diag.audio_mic_ring_bytes);
        printf("usb_codec started=%u stage=%u enc_ready=%u dec_ready=%u enc_size=%lu dec_size=%lu enc_err=%d dec_err=%d stack_hwm=%lu heap=%lu heap_min=%lu\r\n",
               (unsigned int)usb_diag.audio_codec_started,
               (unsigned int)usb_diag.audio_codec_stage,
               (unsigned int)usb_diag.audio_codec_encoder_ready,
               (unsigned int)usb_diag.audio_codec_decoder_ready,
               (unsigned long)usb_diag.audio_codec_encoder_size,
               (unsigned long)usb_diag.audio_codec_decoder_size,
               usb_diag.audio_codec_encoder_error,
               usb_diag.audio_codec_decoder_error,
               (unsigned long)usb_diag.audio_codec_stack_hwm,
               (unsigned long)usb_diag.audio_codec_heap_free,
               (unsigned long)usb_diag.audio_codec_heap_min);
        printf("usb_audio_ctl volume=%lu mute=%lu freq=%lu spk_db=%d mic_db=%d spk_mute=%u mic_mute=%u\r\n",
               (unsigned long)usb_diag.audio_set_volume,
               (unsigned long)usb_diag.audio_set_mute,
               (unsigned long)usb_diag.audio_set_freq,
               usb_diag.audio_speaker_volume_db,
               usb_diag.audio_mic_volume_db,
               (unsigned int)usb_diag.audio_speaker_mute,
               (unsigned int)usb_diag.audio_mic_mute);
        printf("usb_regs dev_ctl=%08lx dev_adr=%08lx phy_tst=%08lx otg=%08lx glb_isr=%08lx dev_igr=%08lx\r\n",
               (unsigned long)usb_diag.reg_dev_ctl,
               (unsigned long)usb_diag.reg_dev_adr,
               (unsigned long)usb_diag.reg_phy_tst,
               (unsigned long)usb_diag.reg_otg_csr,
               (unsigned long)usb_diag.reg_glb_isr,
               (unsigned long)usb_diag.reg_dev_igr);
        printf("usb_irq isg0=%08lx isg2=%08lx isg3=%08lx vdma=%08lx cxfps1=%08lx\r\n",
               (unsigned long)usb_diag.reg_isg0,
               (unsigned long)usb_diag.reg_isg2,
               (unsigned long)usb_diag.reg_isg3,
               (unsigned long)usb_diag.reg_vdma_ctrl,
               (unsigned long)usb_diag.reg_vdma_cxfps1);
        printf("hidp_reports parsed=%lu full=%lu mic_audio=%lu log=%s\r\n",
               (unsigned long)hidp_parsed_reports,
               (unsigned long)hidp_full_reports,
               (unsigned long)hidp_mic_audio_reports,
               hidp_report_log_enabled ? "normal" : "quiet");
        printf("hidp_haptics sent=%lu errors=%lu noconn=%lu last_err=%d\r\n",
               (unsigned long)hidp_haptics_reports_sent,
               (unsigned long)hidp_haptics_send_errors,
               (unsigned long)hidp_haptics_not_connected,
               hidp_haptics_last_error);
        printf("hidp_audio sent=%lu status=%lu errors=%lu stale=%lu noconn=%lu last_err=%d mic_enabled=%u mic_active=%u bt_mic=%u headset=%u buf_len=%u mic_refresh_ms=%u report_ms=%u bridge_delay_ms=%u bt_alloc_ms=%u\r\n",
               (unsigned long)hidp_audio_reports_sent,
               (unsigned long)hidp_audio_status_reports_sent,
               (unsigned long)hidp_audio_send_errors,
               (unsigned long)hidp_tx_realtime_stale_count(),
               (unsigned long)hidp_audio_not_connected,
               hidp_audio_last_error,
               m61_usb_gamepad_audio_mic_enabled() ? 1U : 0U,
               m61_usb_gamepad_audio_in_active() ? 1U : 0U,
               hidp_last_mic_active ? 1U : 0U,
               dualsense_headphones_connected ? 1U : 0U,
               (unsigned int)DS5_AUDIO_BUFFER_LENGTH_DEFAULT,
               (unsigned int)CONFIG_M61_DS5_MIC_STATUS_REFRESH_MS,
               (unsigned int)CONFIG_M61_DS5_AUDIO_REPORT_INTERVAL_MS,
               (unsigned int)CONFIG_M61_DS5_USB_BRIDGE_TASK_DELAY_MS,
               (unsigned int)CONFIG_M61_DS5_BT_AUDIO_ALLOC_TIMEOUT_MS);
        {
            m61_bt_tx_metrics_t tx_metrics;
            uintptr_t flags = bflb_irq_save();

            m61_bt_tx_scheduler_get_metrics(&hidp_tx_scheduler, &tx_metrics);
            bflb_irq_restore(flags);
            printf("hidp_usb_output forwarded=%lu coalesced=%lu failed=%lu last_err=%d pending=%lu min_interval_ms=%u\r\n",
                   (unsigned long)hidp_usb_output_reports_forwarded,
                   (unsigned long)hidp_usb_output_reports_coalesced,
                   (unsigned long)hidp_usb_output_reports_failed,
                   hidp_usb_output_last_error,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].pending,
                   (unsigned int)CONFIG_M61_DS5_USB_OUTPUT_MIN_INTERVAL_MS);
            printf("hidp_tx rt=%lu/%lu pending=%lu replaced=%lu stale=%lu retry=%lu drop=%lu reject=%lu state31=%lu/%lu pending=%lu replaced=%lu retry=%lu drop=%lu reject=%lu state32=%lu/%lu pending=%lu replaced=%lu retry=%lu drop=%lu reject=%lu\r\n",
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].transmitted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].accepted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].pending,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].replaced,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].stale,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].retried,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].dropped,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_REALTIME].rejected,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].transmitted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].accepted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].pending,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].replaced,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].retried,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].dropped,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE31].rejected,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].transmitted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].accepted,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].pending,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].replaced,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].retried,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].dropped,
                   (unsigned long)tx_metrics.classes[M61_BT_TX_CLASS_STATE32].rejected);
            printf("hidp_tx scheduler_bytes=%u selection_bytes=%u bridge_stack_hwm=%lu\r\n",
                   (unsigned int)sizeof(hidp_tx_scheduler),
                   (unsigned int)sizeof(m61_bt_tx_selection_t),
                   bridge_task_handle
                       ? (unsigned long)uxTaskGetStackHighWaterMark(
                             bridge_task_handle)
                       : 0UL);
        }
        if (have_last_dualsense_addr) {
            print_addr("last DualSense ", &last_dualsense_addr);
        }
        status_led_print();
        return 0;
    }

    if (strcmp(argv[1], "reboot-isp") == 0 || strcmp(argv[1], "isp") == 0) {
        m61_reboot_to_uart_download();
        return 0;
    }

    if (strcmp(argv[1], "usb-pins") == 0) {
        return m61_usb_pins_command(argc, argv);
    }

    if (strcmp(argv[1], "usb-reinit") == 0) {
        return m61_usb_gamepad_reinit();
    }

    if (strcmp(argv[1], "usb-cycle") == 0) {
        return m61_usb_cycle_command();
    }

    if (strcmp(argv[1], "decoder-bench") == 0) {
        if (argc < 3) {
            printf("decoder benchmark=%s\r\n",
                   m61_usb_gamepad_decoder_benchmark_enabled()
                       ? "on"
                       : "off");
            return 0;
        }
        if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
            bool enabled = strcmp(argv[2], "on") == 0;
            int result = m61_usb_gamepad_set_decoder_benchmark(enabled);

            if (result != 0) {
                printf("decoder benchmark unavailable: profile build required\r\n");
                return result;
            }
            printf("decoder benchmark requested=%s\r\n",
                   enabled ? "on" : "off");
            return 0;
        }
        printf("Usage: ds5 decoder-bench [on|off]\r\n");
        return -1;
    }

    if (strcmp(argv[1], "auto") == 0) {
        if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            auto_start_enabled = false;
            auto_connect_after_scan = false;
            auto_sequence_started = false;
            auto_reset_saved_addr_attempts();
            printf("auto-start disabled\r\n");
            return 0;
        }

        if (argc >= 3 && strcmp(argv[2], "on") == 0) {
            auto_start_enabled = true;
            auto_reset_saved_addr_attempts();
            auto_schedule_retry(0);
            printf("auto-start enabled\r\n");
            return 0;
        }

        if (argc < 3 || strcmp(argv[2], "now") == 0) {
            auto_start_enabled = true;
            auto_connect_after_scan = false;
            auto_reset_saved_addr_attempts();
            auto_schedule_retry(0);
            printf("auto-start requested now\r\n");
            return 0;
        }

        printf("usage: ds5 auto [on|off|now]\r\n");
        return -EINVAL;
    }

    if (strcmp(argv[1], "scan") == 0 || strcmp(argv[1], "pair") == 0) {
        return request_pairing_mode();
    }

    if (strcmp(argv[1], "autoconnect") == 0) {
        if (default_conn) {
            printf("already connected\r\n");
            return 0;
        }
        if (have_last_dualsense_addr) {
            return connect_addr(&last_dualsense_addr);
        }
        auto_connect_after_scan = true;
        return start_inquiry();
    }

    if (strcmp(argv[1], "connect") == 0) {
        bt_addr_t addr;

        if (argc < 3) {
            printf("missing address or 'last'\r\n");
            return -EINVAL;
        }

        if (strcmp(argv[2], "last") == 0) {
            if (!have_last_dualsense_addr) {
                printf("no stored DualSense address; run ds5 scan first\r\n");
                return -ENOENT;
            }
            addr = last_dualsense_addr;
        } else {
            err = parse_addr(argv[2], &addr);
            if (err) {
                printf("invalid address\r\n");
                return err;
            }
        }

        err = connect_addr(&addr);
        printf("connect request result=%d\r\n", err);
        return err;
    }

    if (strcmp(argv[1], "security") == 0) {
        if (!default_conn) {
            printf("not connected\r\n");
            return -ENOTCONN;
        }
        err = bt_conn_set_security(default_conn, BT_SECURITY_L2);
        printf("security request result=%d\r\n", err);
        return err;
    }

    if (strcmp(argv[1], "sdp") == 0) {
        return hid_sdp_discover();
    }

    if (strcmp(argv[1], "hidp") == 0) {
        hidp_channel_connect(&hid_control);
        hidp_channel_connect(&hid_interrupt);
        return 0;
    }

    if (strcmp(argv[1], "set-protocol") == 0) {
        err = hidp_set_protocol_report();
        printf("set protocol report result=%d\r\n", err);
        return err;
    }

    if (strcmp(argv[1], "get-feature") == 0) {
        uint8_t report_id;

        if (argc < 3) {
            printf("missing feature report id\r\n");
            return -EINVAL;
        }
        err = parse_u8_hex(argv[2], &report_id);
        if (err) {
            printf("invalid feature report id\r\n");
            return err;
        }
        err = hidp_get_report(HIDP_REPORT_TYPE_FEATURE, report_id);
        printf("get feature 0x%02x result=%d\r\n", report_id, err);
        return err;
    }

    if (strcmp(argv[1], "output-init") == 0) {
        return hidp_send_ds5_output_init();
    }

    if (strcmp(argv[1], "bringup") == 0) {
        return hidp_dualsense_bringup();
    }

    if (strcmp(argv[1], "log") == 0) {
        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            printf("hidp report log=%s parsed=%lu full=%lu mic_audio=%lu\r\n",
                   hidp_report_log_enabled ? "normal" : "quiet",
                   (unsigned long)hidp_parsed_reports,
                   (unsigned long)hidp_full_reports,
                   (unsigned long)hidp_mic_audio_reports);
            return 0;
        }
        if (strcmp(argv[2], "normal") == 0 || strcmp(argv[2], "on") == 0) {
            hidp_report_log_enabled = true;
            printf("hidp report log=normal\r\n");
            return 0;
        }
        if (strcmp(argv[2], "quiet") == 0 || strcmp(argv[2], "off") == 0) {
            hidp_report_log_enabled = false;
            printf("hidp report log=quiet\r\n");
            return 0;
        }

        printf("usage: ds5 log [normal|quiet]\r\n");
        return -EINVAL;
    }

    if (strcmp(argv[1], "send-ctrl") == 0 || strcmp(argv[1], "send-intr") == 0) {
        struct hidp_channel *channel =
            strcmp(argv[1], "send-ctrl") == 0 ? &hid_control : &hid_interrupt;

        if (argc < 3) {
            printf("missing hex bytes\r\n");
            return -EINVAL;
        }
        err = parse_hex_args(argc - 2, &argv[2], raw, sizeof(raw), &raw_len);
        if (err) {
            printf("invalid hex bytes: %d\r\n", err);
            return err;
        }
        err = hidp_send_raw(channel, raw, raw_len);
        printf("send %s len=%u result=%d\r\n", channel->name, (unsigned int)raw_len, err);
        return err;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        if (!default_conn) {
            printf("not connected\r\n");
            return -ENOTCONN;
        }
        err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        printf("disconnect result=%d\r\n", err);
        return err;
    }

    if (strcmp(argv[1], "forget") == 0) {
        forget_dualsense_addr();
        return 0;
    }

    print_help();
    return -EINVAL;
}

SHELL_CMD_EXPORT_ALIAS(cmd_ds5, ds5, DualSense HIDP probe);

static void print_m61_help(void)
{
    printf("Usage:\r\n");
    printf("  m61 reboot-isp\r\n");
    printf("  m61 led [status|test|auto|off|red|green|blue|connecting|connected]\r\n");
}

int cmd_m61(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "reboot-isp") == 0 || strcmp(argv[1], "isp") == 0)) {
        m61_reboot_to_uart_download();
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "led") == 0) {
        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            status_led_print();
            return 0;
        }

        if (strcmp(argv[2], "test") == 0) {
            status_led_self_test();
            return 0;
        }

        int err = status_led_set_override_from_name(argv[2]);
        if (err) {
            printf("usage: m61 led [status|test|auto|off|red|green|blue|connecting|connected]\r\n");
        }
        return err;
    }

    print_m61_help();
    return -EINVAL;
}

SHELL_CMD_EXPORT_ALIAS(cmd_m61, m61, M61 board commands);

int main(void)
{
    board_init();

#if defined(BOARD_USB_VIA_GPIO)
    board_usb_gpio_init();
#endif
    m61_native_usb_gpio_init();

    uart0 = bflb_device_get_by_name("uart0");
    shell_init_with_task(uart0);
    status_led_init();
    pair_button_init();

#if defined(CONFIG_BT_SETTINGS)
    bflb_mtd_init();
    if (easyflash_init() == EF_NO_ERR) {
        storage_ready = true;
        load_last_dualsense_addr();
    } else {
        printf("easyflash init failed; saved DualSense address disabled\r\n");
    }
#endif

    if (rfparam_init(0, NULL, 0) != 0) {
        printf("PHY RF init failed\r\n");
        return 0;
    }
    printf("PHY RF init success\r\n");

    dualsense_output_init(&output_ctx);
    m61_bt_tx_scheduler_init(&hidp_tx_scheduler,
                             m61_usb_gamepad_audio_generation());
    hidp_channel_prepare(&hid_control);
    hidp_channel_prepare(&hid_interrupt);

    xTaskCreate(app_start_task,
                "bt_start",
                2048,
                NULL,
                configMAX_PRIORITIES - 2,
                &app_start_handle);
    xTaskCreate(auto_connect_task,
                "ds5_auto",
                2048,
                NULL,
                M61_AUTO_TASK_PRIORITY,
                &auto_task_handle);
    xTaskCreate(usb_hid_bridge_task,
                "usb_hid_bridge",
                1536,
                NULL,
                M61_BT_BRIDGE_TASK_PRIORITY,
                &bridge_task_handle);
    xTaskCreate(status_led_task,
                "m61_led",
                512,
                NULL,
                M61_LED_TASK_PRIORITY,
                &led_task_handle);

    vTaskStartScheduler();

    while (1) {
    }
}
