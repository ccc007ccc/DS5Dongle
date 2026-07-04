#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dualsense_output.h"
#include "dualsense_parser.h"
#include "m61_usb_gamepad.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "shell.h"

#include "bflb_gpio.h"
#include "bflb_mtd.h"
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
#define HIDP_TX_MAX_LEN 192
#define DS5_LAST_ADDR_KEY "ds5_last_bda"

#ifndef CONFIG_M61_DS5_AUTO_START
#define CONFIG_M61_DS5_AUTO_START 1
#endif

#ifndef CONFIG_M61_DS5_AUTO_START_DELAY_MS
#define CONFIG_M61_DS5_AUTO_START_DELAY_MS 1500
#endif

#ifndef CONFIG_M61_DS5_AUTO_RETRY_MS
#define CONFIG_M61_DS5_AUTO_RETRY_MS 5000
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

#define DS5_AUTO_TASK_PERIOD_MS 250

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

static struct bt_br_discovery_result discovery_results[HIDP_DISCOVERY_SLOTS];
static struct bt_conn *default_conn;
static struct bt_conn *pending_conn;
static bt_addr_t last_dualsense_addr;
static bool have_last_dualsense_addr;
static bool bt_ready;
static bool storage_ready;
static bool auto_connect_after_scan;
static bool auto_sequence_started;
static bool auto_security_requested;
static bool auto_sdp_requested;
static bool auto_hidp_requested;
static bool br_security_ready;
static bool full_report_seen;
static bool hidp_report_log_enabled = true;
static bool hidp_full_report_banner_printed;
static uint32_t hidp_parsed_reports;
static uint32_t hidp_full_reports;
static uint32_t hidp_mic_audio_reports;
static uint8_t auto_bringup_attempts;
static TickType_t br_connected_tick;
static TickType_t auto_next_action_tick;
static TickType_t auto_next_hidp_tick;
static TickType_t auto_next_bringup_tick;
static dualsense_output_context_t output_ctx;
static struct bflb_device_s *status_led_gpio;
static bool status_led_override;
static enum status_led_mode status_led_override_mode;

#if CONFIG_M61_DS5_AUTO_START
static bool auto_start_enabled = true;
#else
static bool auto_start_enabled;
#endif

static struct bt_uuid_16 hid_service_uuid = BT_UUID_INIT_16(BT_SDP_HID_SVCLASS);
extern struct net_buf_pool sdp_pool;
static struct bt_sdp_discover_params sdp_params;

static struct hidp_channel hid_control = {
    .name = "control",
    .psm = HIDP_PSM_CONTROL,
};

static struct hidp_channel hid_interrupt = {
    .name = "interrupt",
    .psm = HIDP_PSM_INTERRUPT,
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
    auto_bringup_attempts = 0;
    auto_next_hidp_tick = 0;
    auto_next_bringup_tick = 0;
    br_connected_tick = xTaskGetTickCount();
}

static bool request_is_pending_or_done(int err)
{
    return err == 0 || err == -EALREADY;
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
    }
#endif
}

static void remember_dualsense_addr(const bt_addr_t *addr, bool persist)
{
    last_dualsense_addr = *addr;
    have_last_dualsense_addr = true;
    print_addr("stored DualSense addr ", &last_dualsense_addr);
    if (persist) {
        persist_last_dualsense_addr();
    }
}

static void forget_dualsense_addr(void)
{
    have_last_dualsense_addr = false;
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

#if CONFIG_M61_STATUS_LED_ENABLE
static void status_led_write_pin(uint32_t pin, bool on)
{
    if (!status_led_gpio) {
        return;
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
    status_led_write_pin(CONFIG_M61_STATUS_LED_RED_PIN, red);
    status_led_write_pin(CONFIG_M61_STATUS_LED_GREEN_PIN, green);
    status_led_write_pin(CONFIG_M61_STATUS_LED_BLUE_PIN, blue);
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
    if (default_conn && hid_interrupt.connected) {
        return STATUS_LED_CONNECTED;
    }

    if (bt_ready && auto_start_enabled) {
        return STATUS_LED_CONNECTING;
    }

    return STATUS_LED_BOOT;
}

static void status_led_init(void)
{
    status_led_gpio = bflb_device_get_by_name("gpio");
    if (!status_led_gpio) {
        printf("status LED gpio device not found\r\n");
        return;
    }

    bflb_gpio_init(status_led_gpio,
                   CONFIG_M61_STATUS_LED_RED_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(status_led_gpio,
                   CONFIG_M61_STATUS_LED_GREEN_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_init(status_led_gpio,
                   CONFIG_M61_STATUS_LED_BLUE_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
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

    printf("led enabled=1 active_high=%d red=%u green=%u blue=%u override=%d mode=%s\r\n",
           CONFIG_M61_STATUS_LED_ACTIVE_HIGH ? 1 : 0,
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
    } else if (strcmp(name, "boot") == 0 || strcmp(name, "green") == 0) {
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

    if (hid_control.connected && hid_interrupt.connected) {
        auto_next_bringup_tick = xTaskGetTickCount();
    }
}

static void hidp_l2cap_disconnected(struct bt_l2cap_chan *chan)
{
    struct hidp_channel *channel = CONTAINER_OF(chan, struct hidp_channel, br.chan);

    printf("HIDP %s disconnected\r\n", channel->name);
    channel->connected = false;
    hidp_channel_prepare(channel);
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
        m61_usb_gamepad_send_state(&state);
        if (state.is_full_report) {
            full_report_seen = true;
            if (first_full_report || !hidp_full_report_banner_printed) {
                printf("M61 HIDP full report path is alive\r\n");
                hidp_full_report_banner_printed = true;
            }
        }
    } else if (channel == &hid_interrupt && parse.kind == DS5_REPORT_MIC_AUDIO) {
        hidp_mic_audio_reports++;
        if (hidp_report_log_enabled) {
            printf("parsed microphone/audio payload skipped len=%u\r\n", buf->len);
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
#if defined(CONFIG_BT_L2CAP_DYNAMIC_CHANNEL)
    channel->br.chan.required_sec_level = BT_SECURITY_L2;
#endif
}

static int hidp_channel_connect(struct hidp_channel *channel)
{
    int err;

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

static int hidp_send_raw(struct hidp_channel *channel, const uint8_t *data, size_t len)
{
    struct net_buf *buf;
    int ret;

    if (!channel->connected) {
        return -ENOTCONN;
    }
    if (!len || len > channel->br.tx.mtu) {
        return -EMSGSIZE;
    }

    buf = bt_l2cap_create_pdu(NULL, 0);
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

static int hidp_send_data_output(const uint8_t *report, size_t report_len)
{
    uint8_t packet[HIDP_TX_MAX_LEN];

    if (!report || report_len + 1 > sizeof(packet)) {
        return -EMSGSIZE;
    }

    packet[0] = HIDP_TRANSACTION_DATA_OUTPUT;
    memcpy(packet + 1, report, report_len);
    return hidp_send_raw(&hid_interrupt, packet, report_len + 1);
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

    memset(discovery_results, 0, sizeof(discovery_results));
    err = bt_br_discovery_start(&param,
                                discovery_results,
                                HIDP_DISCOVERY_SLOTS,
                                bt_br_discv_cb);
    if (err) {
        printf("BR/EDR discovery failed: %d\r\n", err);
    } else {
        printf("BR/EDR discovery started\r\n");
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

    if (conn->type != BT_CONN_TYPE_BR) {
        return;
    }

    bt_conn_get_info(conn, &info);
    bt_addr_to_str(info.br.dst, addr_str, sizeof(addr_str));

    if (err) {
        printf("BR/EDR connect failed: %s err=%u\r\n", addr_str, err);
        if (pending_conn == conn) {
            bt_conn_unref(pending_conn);
            pending_conn = NULL;
        }
        if (auto_start_enabled) {
            auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
        }
        return;
    }

    printf("BR/EDR connected: %s\r\n", addr_str);
    if (pending_conn == conn) {
        default_conn = pending_conn;
        pending_conn = NULL;
    } else {
        if (default_conn && default_conn != conn) {
            bt_conn_unref(default_conn);
        }
        default_conn = bt_conn_ref(conn);
    }
    auto_sequence_started = true;
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
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }
    if (pending_conn == conn) {
        bt_conn_unref(pending_conn);
        pending_conn = NULL;
    }
    auto_connect_after_scan = false;
    auto_reset_link_state();
    if (auto_start_enabled) {
        auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
    }
    hidp_channel_prepare(&hid_control);
    hidp_channel_prepare(&hid_interrupt);
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
            remember_dualsense_addr(&results[i].addr, true);
        }
    }

    printf("BR/EDR discovery complete, count=%u\r\n", (unsigned int)count);
    if (auto_connect_after_scan) {
        auto_connect_after_scan = false;
        if (have_last_dualsense_addr && !default_conn) {
            int err = connect_addr(&last_dualsense_addr);
            if (err && auto_start_enabled) {
                auto_schedule_retry(CONFIG_M61_DS5_AUTO_RETRY_MS);
            }
        } else if (!have_last_dualsense_addr) {
            printf("autoconnect scan did not find DualSense\r\n");
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

    printf("M61 DualSense HIDP probe ready. Use 'ds5 scan'.\r\n");
}

static void app_start_task(void *pvParameters)
{
    (void)pvParameters;

    btble_controller_init(configMAX_PRIORITIES - 1);
    hci_driver_init();
    bt_enable(bt_enable_cb);

    vTaskDelete(NULL);
}

static void auto_connect_task(void *pvParameters)
{
    (void)pvParameters;

    auto_next_action_tick = xTaskGetTickCount() +
                            pdMS_TO_TICKS(CONFIG_M61_DS5_AUTO_START_DELAY_MS);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (!auto_start_enabled || !bt_ready) {
            vTaskDelay(pdMS_TO_TICKS(DS5_AUTO_TASK_PERIOD_MS));
            continue;
        }

        if (!default_conn) {
            if (!auto_sequence_started && tick_due(now, auto_next_action_tick)) {
                int err;

                if (have_last_dualsense_addr) {
                    print_addr("auto: connecting saved DualSense ", &last_dualsense_addr);
                    err = connect_addr(&last_dualsense_addr);
                } else {
                    printf("auto: no saved DualSense address; scanning\r\n");
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

            if (!auto_hidp_requested && tick_due(now, auto_next_hidp_tick)) {
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
    printf("  ds5 scan\r\n");
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
        printf("bt_ready=%d pending=%d connected=%d hid_control=%d hid_interrupt=%d have_last=%d\r\n",
               bt_ready ? 1 : 0,
               pending_conn ? 1 : 0,
               default_conn ? 1 : 0,
               hid_control.connected ? 1 : 0,
               hid_interrupt.connected ? 1 : 0,
               have_last_dualsense_addr ? 1 : 0);
        printf("auto=%d sequence=%d security=%d sdp=%d hidp=%d full_report=%d bringup=%u/%u\r\n",
               auto_start_enabled ? 1 : 0,
               auto_sequence_started ? 1 : 0,
               br_security_ready ? 1 : 0,
               auto_sdp_requested ? 1 : 0,
               auto_hidp_requested ? 1 : 0,
               full_report_seen ? 1 : 0,
               (unsigned int)auto_bringup_attempts,
               (unsigned int)CONFIG_M61_DS5_AUTO_BRINGUP_RETRIES);
        printf("usb_gamepad ready=%d configured=%d busy=%d sent=%lu dropped=%lu\r\n",
               m61_usb_gamepad_ready() ? 1 : 0,
               m61_usb_gamepad_configured() ? 1 : 0,
               m61_usb_gamepad_busy() ? 1 : 0,
               (unsigned long)m61_usb_gamepad_sent_count(),
               (unsigned long)m61_usb_gamepad_drop_count());
        printf("hidp_reports parsed=%lu full=%lu mic_audio=%lu log=%s\r\n",
               (unsigned long)hidp_parsed_reports,
               (unsigned long)hidp_full_reports,
               (unsigned long)hidp_mic_audio_reports,
               hidp_report_log_enabled ? "normal" : "quiet");
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

    if (strcmp(argv[1], "auto") == 0) {
        if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            auto_start_enabled = false;
            auto_connect_after_scan = false;
            auto_sequence_started = false;
            printf("auto-start disabled\r\n");
            return 0;
        }

        if (argc >= 3 && strcmp(argv[2], "on") == 0) {
            auto_start_enabled = true;
            auto_schedule_retry(0);
            printf("auto-start enabled\r\n");
            return 0;
        }

        if (argc < 3 || strcmp(argv[2], "now") == 0) {
            auto_start_enabled = true;
            auto_connect_after_scan = false;
            auto_schedule_retry(0);
            printf("auto-start requested now\r\n");
            return 0;
        }

        printf("usage: ds5 auto [on|off|now]\r\n");
        return -EINVAL;
    }

    if (strcmp(argv[1], "scan") == 0) {
        start_inquiry();
        return 0;
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

        connect_addr(&addr);
        return 0;
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

    uart0 = bflb_device_get_by_name("uart0");
    shell_init_with_task(uart0);
    status_led_init();
    m61_usb_gamepad_init();

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
                configMAX_PRIORITIES - 3,
                &auto_task_handle);
    xTaskCreate(status_led_task,
                "m61_led",
                512,
                NULL,
                configMAX_PRIORITIES - 4,
                &led_task_handle);

    vTaskStartScheduler();

    while (1) {
    }
}
