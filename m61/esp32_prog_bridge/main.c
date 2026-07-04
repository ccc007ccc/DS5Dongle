#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "bflb_uart.h"
#include "board.h"
#include "ring_buffer.h"
#include "usbd_cdc_acm.h"
#include "usbd_core.h"

#include "FreeRTOS.h"
#include "portmacro.h"
#include "task.h"

#define DBG_TAG "M61_BRIDGE"
#include "log.h"

#if defined(BL616)
#include "bl616_hbn.h"
#include "bl616_sys.h"
#elif defined(BL618DG)
#include "bl618dg_hbn.h"
#include "bl618dg_sys.h"
#endif

#ifndef CONFIG_M61_TARGET_UART_ID
#define CONFIG_M61_TARGET_UART_ID 1
#endif

#ifndef CONFIG_M61_HOST_UART_ENABLE
#define CONFIG_M61_HOST_UART_ENABLE 1
#endif

#ifndef CONFIG_M61_HOST_UART_ID
#define CONFIG_M61_HOST_UART_ID 0
#endif

#ifndef CONFIG_M61_HOST_UART_BAUD
#define CONFIG_M61_HOST_UART_BAUD 115200
#endif

#ifndef CONFIG_M61_HOST_UART_TX_PIN
#define CONFIG_M61_HOST_UART_TX_PIN 21
#endif

#ifndef CONFIG_M61_HOST_UART_RX_PIN
#define CONFIG_M61_HOST_UART_RX_PIN 22
#endif

#ifndef CONFIG_M61_TARGET_UART_TX_PIN
#define CONFIG_M61_TARGET_UART_TX_PIN 23
#endif

#ifndef CONFIG_M61_TARGET_UART_RX_PIN
#define CONFIG_M61_TARGET_UART_RX_PIN 24
#endif

#ifndef CONFIG_M61_BOOT_PIN
#define CONFIG_M61_BOOT_PIN 27
#endif

#ifndef CONFIG_M61_EN_PIN
#define CONFIG_M61_EN_PIN 28
#endif

#ifndef CONFIG_M61_MAP_CDC_LINES_TO_BOOT_RESET
#define CONFIG_M61_MAP_CDC_LINES_TO_BOOT_RESET 0
#endif

#if CONFIG_M61_TARGET_UART_ID == 0
#define TARGET_UART_NAME "uart0"
#define TARGET_UART_TX_FUNC GPIO_UART_FUNC_UART0_TX
#define TARGET_UART_RX_FUNC GPIO_UART_FUNC_UART0_RX
#elif CONFIG_M61_TARGET_UART_ID == 1
#define TARGET_UART_NAME "uart1"
#define TARGET_UART_TX_FUNC GPIO_UART_FUNC_UART1_TX
#define TARGET_UART_RX_FUNC GPIO_UART_FUNC_UART1_RX
#else
#error "Only target UART id 0 or 1 is supported by this bridge skeleton"
#endif

#if CONFIG_M61_HOST_UART_ID == 0
#define HOST_UART_NAME "uart0"
#define HOST_UART_TX_FUNC GPIO_UART_FUNC_UART0_TX
#define HOST_UART_RX_FUNC GPIO_UART_FUNC_UART0_RX
#elif CONFIG_M61_HOST_UART_ID == 1
#define HOST_UART_NAME "uart1"
#define HOST_UART_TX_FUNC GPIO_UART_FUNC_UART1_TX
#define HOST_UART_RX_FUNC GPIO_UART_FUNC_UART1_RX
#else
#error "Only host UART id 0 or 1 is supported by this bridge skeleton"
#endif

#if CONFIG_M61_HOST_UART_ENABLE && (CONFIG_M61_HOST_UART_ID == CONFIG_M61_TARGET_UART_ID)
#error "Host UART and target UART must be different"
#endif

#define CDC_IN_EP 0x83
#define CDC_OUT_EP 0x04
#define CDC_INT_EP 0x85

#define USBD_VID 0xFFFF
#define USBD_PID 0x4001
#define USBD_MAX_POWER 100
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#define CDC_RX_BUFFER_SIZE 512
#define USB_TO_UART_RB_SIZE 8192
#define UART_TO_USB_RB_SIZE 8192
#define HOST_TX_RB_SIZE 8192
#define CMD_BUFFER_SIZE 64
#define DEFAULT_TARGET_BAUD 115200

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x00,
    0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    "DS5Dongle",
    "M61 ESP32 Programming Bridge",
    "M61ESP32BRIDGE",
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_rx_buffer[CDC_RX_BUFFER_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_tx_buffer[CDC_MAX_MPS];
static uint8_t usb_to_uart_storage[USB_TO_UART_RB_SIZE];
static uint8_t uart_to_usb_storage[UART_TO_USB_RB_SIZE];
static uint8_t host_tx_storage[HOST_TX_RB_SIZE];
static Ring_Buffer_Type usb_to_uart_rb;
static Ring_Buffer_Type uart_to_usb_rb;
static Ring_Buffer_Type host_tx_rb;

static struct bflb_device_s *gpio;
static struct bflb_device_s *host_uart;
static struct bflb_device_s *target_uart;
static TaskHandle_t bridge_task_handle;

static volatile bool usb_ready;
static volatile bool usb_tx_busy;
static volatile bool usb_out_armed;
static volatile bool uart_reconfig_pending;
static volatile bool usb_overflow;
static volatile bool host_uart_enabled;
static volatile bool dtr_state;
static volatile bool rts_state;
static volatile struct cdc_line_coding line_coding = {
    .dwDTERate = DEFAULT_TARGET_BAUD,
    .bCharFormat = 0,
    .bParityType = 0,
    .bDataBits = 8,
};

static volatile uintptr_t rb_irq_flags;

static void rb_lock(void)
{
    rb_irq_flags = bflb_irq_save();
}

static void rb_unlock(void)
{
    bflb_irq_restore(rb_irq_flags);
}

static void bridge_notify(void)
{
    if (!bridge_task_handle) {
        return;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(bridge_task_handle, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    } else {
        xTaskNotifyGive(bridge_task_handle);
    }
}

static void ring_buffers_init(void)
{
    Ring_Buffer_Init(&usb_to_uart_rb,
                     usb_to_uart_storage,
                     sizeof(usb_to_uart_storage),
                     rb_lock,
                     rb_unlock);
    Ring_Buffer_Init(&uart_to_usb_rb,
                     uart_to_usb_storage,
                     sizeof(uart_to_usb_storage),
                     rb_lock,
                     rb_unlock);
    Ring_Buffer_Init(&host_tx_rb,
                     host_tx_storage,
                     sizeof(host_tx_storage),
                     rb_lock,
                     rb_unlock);
}

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
        return NULL;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static void target_pin_release(uint8_t pin)
{
    bflb_gpio_init(gpio, pin, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
}

static void target_pin_assert_low(uint8_t pin)
{
    bflb_gpio_init(gpio, pin, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_reset(gpio, pin);
}

static void target_boot_set(bool asserted)
{
    if (asserted) {
        target_pin_assert_low(CONFIG_M61_BOOT_PIN);
    } else {
        target_pin_release(CONFIG_M61_BOOT_PIN);
    }
}

static void target_en_reset_set(bool asserted)
{
    if (asserted) {
        target_pin_assert_low(CONFIG_M61_EN_PIN);
    } else {
        target_pin_release(CONFIG_M61_EN_PIN);
    }
}

static void target_hard_reset(void)
{
    target_boot_set(false);
    target_en_reset_set(true);
    bflb_mtimer_delay_ms(100);
    target_en_reset_set(false);
    bflb_mtimer_delay_ms(200);
}

static void target_enter_bootloader(bool hold_boot)
{
    target_boot_set(true);
    bflb_mtimer_delay_ms(50);
    target_en_reset_set(true);
    bflb_mtimer_delay_ms(100);
    target_en_reset_set(false);
    bflb_mtimer_delay_ms(150);
    if (!hold_boot) {
        target_boot_set(false);
    }
}

static void target_run_app(void)
{
    target_boot_set(false);
    target_hard_reset();
}

static void target_uart_gpio_init(void)
{
    bflb_gpio_uart_init(gpio, CONFIG_M61_TARGET_UART_TX_PIN, TARGET_UART_TX_FUNC);
    bflb_gpio_uart_init(gpio, CONFIG_M61_TARGET_UART_RX_PIN, TARGET_UART_RX_FUNC);
}

static void target_uart_configure(uint32_t baudrate)
{
    struct bflb_uart_config_s cfg = {
        .baudrate = baudrate ? baudrate : DEFAULT_TARGET_BAUD,
        .direction = UART_DIRECTION_TXRX,
        .data_bits = UART_DATA_BITS_8,
        .stop_bits = UART_STOP_BITS_1,
        .parity = UART_PARITY_NONE,
        .bit_order = UART_LSB_FIRST,
        .flow_ctrl = UART_FLOWCTRL_NONE,
        .tx_fifo_threshold = 7,
        .rx_fifo_threshold = 0,
    };

    bflb_uart_deinit(target_uart);
    bflb_uart_init(target_uart, &cfg);
    LOG_I("target %s baud=%lu tx_gpio=%d rx_gpio=%d\r\n",
          TARGET_UART_NAME,
          (unsigned long)cfg.baudrate,
          CONFIG_M61_TARGET_UART_TX_PIN,
          CONFIG_M61_TARGET_UART_RX_PIN);
}

static void host_uart_gpio_init(void)
{
#if CONFIG_M61_HOST_UART_ENABLE
    bflb_gpio_uart_init(gpio, CONFIG_M61_HOST_UART_TX_PIN, HOST_UART_TX_FUNC);
    bflb_gpio_uart_init(gpio, CONFIG_M61_HOST_UART_RX_PIN, HOST_UART_RX_FUNC);
#endif
}

static void host_uart_configure(uint32_t baudrate)
{
#if CONFIG_M61_HOST_UART_ENABLE
    struct bflb_uart_config_s cfg = {
        .baudrate = baudrate,
        .direction = UART_DIRECTION_TXRX,
        .data_bits = UART_DATA_BITS_8,
        .stop_bits = UART_STOP_BITS_1,
        .parity = UART_PARITY_NONE,
        .bit_order = UART_LSB_FIRST,
        .flow_ctrl = UART_FLOWCTRL_NONE,
        .tx_fifo_threshold = 7,
        .rx_fifo_threshold = 0,
    };

    bflb_uart_deinit(host_uart);
    bflb_uart_init(host_uart, &cfg);
    host_uart_enabled = true;
    LOG_I("host %s baud=%lu tx_gpio=%d rx_gpio=%d\r\n",
          HOST_UART_NAME,
          (unsigned long)cfg.baudrate,
          CONFIG_M61_HOST_UART_TX_PIN,
          CONFIG_M61_HOST_UART_RX_PIN);
#else
    (void)baudrate;
#endif
}

static void queue_host_bytes(const uint8_t *data, uint32_t len)
{
#if CONFIG_M61_HOST_UART_ENABLE
    if (!host_uart_enabled) {
        return;
    }

    uint32_t empty = Ring_Buffer_Get_Empty_Length(&host_tx_rb);
    if (len > empty) {
        len = empty;
        usb_overflow = true;
    }
    if (len) {
        Ring_Buffer_Write(&host_tx_rb, (uint8_t *)data, len);
        bridge_notify();
    }
#else
    (void)data;
    (void)len;
#endif
}

static void queue_usb_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t host_len = len;
    uint32_t empty = Ring_Buffer_Get_Empty_Length(&uart_to_usb_rb);
    if (len > empty) {
        len = empty;
        usb_overflow = true;
    }
    if (len) {
        Ring_Buffer_Write(&uart_to_usb_rb, (uint8_t *)data, len);
        bridge_notify();
    }
    queue_host_bytes(data, host_len);
}

static void queue_usb_text(const char *text)
{
    queue_usb_bytes((const uint8_t *)text, (uint32_t)strlen(text));
}

static void queue_usb_printf(const char *fmt, ...)
{
    char buffer[160];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }
    if ((size_t)len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    queue_usb_bytes((const uint8_t *)buffer, (uint32_t)len);
}

static void m61_reboot_to_uart_download(void)
{
#if defined(BL616) || defined(BL618DG)
    queue_usb_text("\r\nOK reboot-isp; M61 entering UART download mode\r\n");
    bflb_mtimer_delay_ms(50);
    HBN_Set_User_Boot_Config(1);
    bl_sys_reset_por();
    while (1) {
    }
#else
    queue_usb_text("\r\nERR reboot-isp unsupported on this chip\r\n");
#endif
}

static void target_uart_write_byte(uint8_t ch)
{
    bflb_uart_putchar(target_uart, ch);
}

static void target_uart_write_bytes(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        target_uart_write_byte(data[i]);
    }
}

static void command_help(void)
{
    queue_usb_text(
        "\r\n"
        "M61 ESP32 bridge commands:\r\n"
        "  ~m61 boot       latch ESP32 ROM download mode\r\n"
        "  ~m61 boot-hold  enter ROM download mode and keep BOOT low\r\n"
        "  ~m61 reset      pulse EN/RST without BOOT\r\n"
        "  ~m61 run        release BOOT and reset into app\r\n"
        "  ~m61 reboot-isp reboot M61 itself into UART download mode\r\n"
        "  ~m61 status     print bridge state\r\n"
        "  ~m61 help       print this help\r\n"
        "\r\n");
}

static void command_status(void)
{
    queue_usb_printf(
        "\r\nM61 bridge status:\r\n"
        "  usb_ready=%d host_uart=%d dtr=%d rts=%d overflow=%d\r\n"
        "  host_uart=%s baud=%d tx_gpio=%d rx_gpio=%d\r\n"
        "  target_uart=%s baud=%lu tx_gpio=%d rx_gpio=%d\r\n"
        "  boot_gpio=%d level=%d en_gpio=%d level=%d\r\n\r\n",
        usb_ready ? 1 : 0,
        host_uart_enabled ? 1 : 0,
        dtr_state ? 1 : 0,
        rts_state ? 1 : 0,
        usb_overflow ? 1 : 0,
        HOST_UART_NAME,
        CONFIG_M61_HOST_UART_BAUD,
        CONFIG_M61_HOST_UART_TX_PIN,
        CONFIG_M61_HOST_UART_RX_PIN,
        TARGET_UART_NAME,
        (unsigned long)line_coding.dwDTERate,
        CONFIG_M61_TARGET_UART_TX_PIN,
        CONFIG_M61_TARGET_UART_RX_PIN,
        CONFIG_M61_BOOT_PIN,
        bflb_gpio_read(gpio, CONFIG_M61_BOOT_PIN) ? 1 : 0,
        CONFIG_M61_EN_PIN,
        bflb_gpio_read(gpio, CONFIG_M61_EN_PIN) ? 1 : 0);
}

static void handle_command(char *line)
{
    char *cmd = line;

    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }

    if (strcmp(cmd, "boot") == 0) {
        target_enter_bootloader(false);
        queue_usb_text("\r\nOK boot\r\n");
    } else if (strcmp(cmd, "boot-hold") == 0) {
        target_enter_bootloader(true);
        queue_usb_text("\r\nOK boot-hold\r\n");
    } else if (strcmp(cmd, "reset") == 0) {
        target_hard_reset();
        queue_usb_text("\r\nOK reset\r\n");
    } else if (strcmp(cmd, "run") == 0) {
        target_run_app();
        queue_usb_text("\r\nOK run\r\n");
    } else if (strcmp(cmd, "reboot-isp") == 0 || strcmp(cmd, "isp") == 0) {
        m61_reboot_to_uart_download();
    } else if (strcmp(cmd, "status") == 0) {
        command_status();
    } else if (strcmp(cmd, "help") == 0 || *cmd == '\0') {
        command_help();
    } else {
        queue_usb_text("\r\nERR unknown command; send '~m61 help'\r\n");
    }
}

static void command_parser_push(uint8_t ch)
{
    static bool line_start = true;
    static bool in_command = false;
    static char cmd_buffer[CMD_BUFFER_SIZE];
    static uint32_t cmd_len;
    static const char prefix[] = "~m61 ";

    if (!in_command) {
        if (line_start && ch == '~') {
            in_command = true;
            cmd_len = 0;
            cmd_buffer[cmd_len++] = (char)ch;
            return;
        }

        target_uart_write_byte(ch);
        line_start = (ch == '\r' || ch == '\n');
        return;
    }

    if (cmd_len < sizeof(cmd_buffer) - 1) {
        cmd_buffer[cmd_len++] = (char)ch;
    } else {
        target_uart_write_bytes((const uint8_t *)cmd_buffer, cmd_len);
        target_uart_write_byte(ch);
        in_command = false;
        line_start = false;
        return;
    }

    uint32_t prefix_check_len = cmd_len;
    if (prefix_check_len > sizeof(prefix) - 1) {
        prefix_check_len = sizeof(prefix) - 1;
    }
    if (memcmp(cmd_buffer, prefix, prefix_check_len) != 0) {
        target_uart_write_bytes((const uint8_t *)cmd_buffer, cmd_len);
        in_command = false;
        line_start = false;
        return;
    }

    if (ch == '\r' || ch == '\n') {
        cmd_buffer[cmd_len - 1] = '\0';
        if (cmd_len >= sizeof(prefix)) {
            handle_command(&cmd_buffer[sizeof(prefix) - 1]);
        } else {
            command_help();
        }
        in_command = false;
        line_start = true;
    }
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_SUSPEND:
            usb_ready = false;
            usb_tx_busy = false;
            usb_out_armed = false;
            bridge_notify();
            break;
        case USBD_EVENT_CONFIGURED:
        case USBD_EVENT_RESUME:
            usb_ready = true;
            usb_tx_busy = false;
            usb_out_armed = false;
            bridge_notify();
            break;
        default:
            break;
    }
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    if (!usb_ready) {
        return;
    }

    uint32_t empty = Ring_Buffer_Get_Empty_Length(&usb_to_uart_rb);
    if (nbytes > empty) {
        nbytes = empty;
        usb_overflow = true;
    }
    if (nbytes) {
        Ring_Buffer_Write(&usb_to_uart_rb, cdc_rx_buffer, nbytes);
    }

    usbd_ep_start_read(busid, CDC_OUT_EP, cdc_rx_buffer, sizeof(cdc_rx_buffer));
    usb_out_armed = true;
    bridge_notify();
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        usbd_ep_start_write(0, CDC_IN_EP, NULL, 0);
        return;
    }

    usb_tx_busy = false;
    bridge_notify();
}

static struct usbd_endpoint cdc_acm_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out,
};

static struct usbd_endpoint cdc_acm_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in,
};

static struct usbd_interface intf0;
static struct usbd_interface intf1;

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    dtr_state = dtr;
#if CONFIG_M61_MAP_CDC_LINES_TO_BOOT_RESET
    target_boot_set(dtr);
#endif
    bridge_notify();
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    (void)busid;
    (void)intf;
    rts_state = rts;
#if CONFIG_M61_MAP_CDC_LINES_TO_BOOT_RESET
    target_en_reset_set(rts);
#endif
    bridge_notify();
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    line_coding = *coding;
    uart_reconfig_pending = true;
    bridge_notify();
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *coding)
{
    (void)busid;
    (void)intf;
    *coding = line_coding;
}

static void usb_cdc_init(void)
{
    usbd_desc_register(0, &cdc_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf0));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf1));
    usbd_add_endpoint(0, &cdc_acm_out_ep);
    usbd_add_endpoint(0, &cdc_acm_in_ep);
    usbd_initialize(0, 0, usbd_event_handler);
}

static void service_usb_out_arm(void)
{
    if (usb_ready && !usb_out_armed) {
        usbd_ep_start_read(0, CDC_OUT_EP, cdc_rx_buffer, sizeof(cdc_rx_buffer));
        usb_out_armed = true;
    }
}

static void service_usb_to_uart(void)
{
    uint8_t ch;

    while (Ring_Buffer_Get_Length(&usb_to_uart_rb) > 0) {
        Ring_Buffer_Read(&usb_to_uart_rb, &ch, 1);
        command_parser_push(ch);
    }
}

static void service_host_to_uart(void)
{
#if CONFIG_M61_HOST_UART_ENABLE
    if (!host_uart_enabled) {
        return;
    }

    while (bflb_uart_rxavailable(host_uart)) {
        int ch = bflb_uart_getchar(host_uart);
        if (ch < 0) {
            break;
        }
        command_parser_push((uint8_t)ch);
    }
#endif
}

static void service_uart_to_usb_input(void)
{
    bool wrote = false;

    while (bflb_uart_rxavailable(target_uart)) {
        bool can_usb = usb_ready && (Ring_Buffer_Get_Empty_Length(&uart_to_usb_rb) > 0);
        bool can_host = false;

#if CONFIG_M61_HOST_UART_ENABLE
        can_host = host_uart_enabled && (Ring_Buffer_Get_Empty_Length(&host_tx_rb) > 0);
#endif

        if (!can_usb && !can_host) {
            break;
        }

        int ch = bflb_uart_getchar(target_uart);
        if (ch < 0) {
            break;
        }
        uint8_t byte = (uint8_t)ch;

        if (can_usb) {
            Ring_Buffer_Write(&uart_to_usb_rb, &byte, 1);
            wrote = true;
        }
        if (can_host) {
            Ring_Buffer_Write(&host_tx_rb, &byte, 1);
            wrote = true;
        }
    }

    if (wrote) {
        bridge_notify();
    }
}

static void service_usb_tx(void)
{
    if (!usb_ready || usb_tx_busy) {
        return;
    }

    uint32_t len = Ring_Buffer_Get_Length(&uart_to_usb_rb);
    if (!len) {
        return;
    }
    if (len > sizeof(cdc_tx_buffer)) {
        len = sizeof(cdc_tx_buffer);
    }

    Ring_Buffer_Read(&uart_to_usb_rb, cdc_tx_buffer, len);
    usb_tx_busy = true;
    usbd_ep_start_write(0, CDC_IN_EP, cdc_tx_buffer, len);
}

static void service_host_tx(void)
{
#if CONFIG_M61_HOST_UART_ENABLE
    uint8_t ch;

    if (!host_uart_enabled) {
        return;
    }

    while (Ring_Buffer_Get_Length(&host_tx_rb) > 0 && bflb_uart_txready(host_uart)) {
        Ring_Buffer_Read(&host_tx_rb, &ch, 1);
        bflb_uart_putchar(host_uart, ch);
    }
#endif
}

static void bridge_task(void *param)
{
    (void)param;

    command_help();

    while (1) {
        if (uart_reconfig_pending) {
            uart_reconfig_pending = false;
            target_uart_configure(line_coding.dwDTERate);
        }

        service_usb_out_arm();
        service_usb_to_uart();
        service_host_to_uart();
        service_uart_to_usb_input();
        service_usb_tx();
        service_host_tx();

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    board_init();

#if defined(BOARD_USB_VIA_GPIO)
    board_usb_gpio_init();
#endif

    gpio = bflb_device_get_by_name("gpio");
#if CONFIG_M61_HOST_UART_ENABLE
    host_uart = bflb_device_get_by_name(HOST_UART_NAME);
#endif
    target_uart = bflb_device_get_by_name(TARGET_UART_NAME);
    if (!gpio || !target_uart || (CONFIG_M61_HOST_UART_ENABLE && !host_uart)) {
        printf("M61 bridge init failed: gpio=%p host=%p target=%p\r\n", gpio, host_uart, target_uart);
        while (1) {
            bflb_mtimer_delay_ms(1000);
        }
    }

    target_boot_set(false);
    target_en_reset_set(false);
    ring_buffers_init();
    host_uart_gpio_init();
    host_uart_configure(CONFIG_M61_HOST_UART_BAUD);
    target_uart_gpio_init();
    target_uart_configure(DEFAULT_TARGET_BAUD);
    usb_cdc_init();

    xTaskCreate(bridge_task, "bridge", 2048, NULL, 15, &bridge_task_handle);
    vTaskStartScheduler();

    while (1) {
    }
}
