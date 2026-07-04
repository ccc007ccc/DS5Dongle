#include "m61_usb_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_hid.h"

#ifndef CONFIG_M61_USB_GAMEPAD_ENABLE
#define CONFIG_M61_USB_GAMEPAD_ENABLE 1
#endif

#define USBD_VID 0x1209
#define USBD_PID 0x5D51
#define USBD_MAX_POWER 100
#define HID_INT_EP 0x81
#define HID_INT_EP_SIZE 16
#define HID_INT_EP_INTERVAL 1
#define USB_HID_CONFIG_DESC_SIZE 34
#define HID_GAMEPAD_REPORT_DESC_SIZE 75
#define HID_GAMEPAD_REPORT_SIZE 9

typedef struct __attribute__((packed)) {
    uint16_t buttons;
    uint8_t hat_and_pad;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t l2;
    uint8_t r2;
} usb_gamepad_report_t;

#if CONFIG_M61_USB_GAMEPAD_ENABLE
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_HID_CONFIG_DESC_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),

    0x09,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    0x00,
    0x00,
    0x01,
    0x03,
    0x00,
    0x00,
    0x00,

    0x09,
    HID_DESCRIPTOR_TYPE_HID,
    0x11,
    0x01,
    0x00,
    0x01,
    0x22,
    HID_GAMEPAD_REPORT_DESC_SIZE,
    0x00,

    0x07,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    HID_INT_EP,
    0x03,
    HID_INT_EP_SIZE,
    0x00,
    HID_INT_EP_INTERVAL,
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
    "M61 DualSense Gamepad",
    "M61DS5GAMEPAD",
};

static const uint8_t gamepad_report_desc[HID_GAMEPAD_REPORT_DESC_SIZE] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x09,       /* Usage Page (Button) */
    0x19, 0x01,       /* Usage Minimum (Button 1) */
    0x29, 0x10,       /* Usage Maximum (Button 16) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x10,       /* Report Count (16) */
    0x81, 0x02,       /* Input (Data,Var,Abs) */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x39,       /* Usage (Hat switch) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x08,       /* Logical Maximum (8, idle) */
    0x35, 0x00,       /* Physical Minimum (0) */
    0x46, 0x3B, 0x01, /* Physical Maximum (315) */
    0x65, 0x14,       /* Unit (English Rotation, degrees) */
    0x75, 0x04,       /* Report Size (4) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x42,       /* Input (Data,Var,Abs,Null) */
    0x65, 0x00,       /* Unit (None) */
    0x75, 0x04,       /* Report Size (4) */
    0x95, 0x01,       /* Report Count (1) */
    0x81, 0x03,       /* Input (Const,Var,Abs) */
    0x09, 0x30,       /* Usage (X) */
    0x09, 0x31,       /* Usage (Y) */
    0x09, 0x33,       /* Usage (Rx) */
    0x09, 0x34,       /* Usage (Ry) */
    0x09, 0x32,       /* Usage (Z) */
    0x09, 0x35,       /* Usage (Rz) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xFF, 0x00, /* Logical Maximum (255) */
    0x75, 0x08,       /* Report Size (8) */
    0x95, 0x06,       /* Report Count (6) */
    0x81, 0x02,       /* Input (Data,Var,Abs) */
    0xC0,             /* End Collection */
};

static volatile bool usb_ready;
static volatile bool usb_configured;
static volatile bool usb_busy;
static volatile bool usb_suspended;
static volatile uint32_t usb_sent_count;
static volatile uint32_t usb_drop_count;
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_report_buffer[HID_GAMEPAD_REPORT_SIZE];
static usb_gamepad_report_t last_report;

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
    if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
        return NULL;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor gamepad_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
            usb_ready = false;
            usb_configured = false;
            usb_busy = false;
            usb_suspended = false;
            break;
        case USBD_EVENT_CONFIGURED:
            usb_ready = true;
            usb_configured = true;
            usb_busy = false;
            usb_suspended = false;
            break;
        case USBD_EVENT_RESUME:
            usb_suspended = false;
            break;
        case USBD_EVENT_SUSPEND:
            usb_suspended = true;
            break;
        default:
            break;
    }
}

static void usbd_hid_int_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    usb_busy = false;
}

static struct usbd_endpoint hid_in_ep = {
    .ep_cb = usbd_hid_int_callback,
    .ep_addr = HID_INT_EP,
};

static struct usbd_interface intf0;

static uint16_t map_buttons(uint32_t buttons)
{
    uint16_t out = 0;

    if (buttons & DS5_BUTTON_CROSS) {
        out |= 1u << 0;
    }
    if (buttons & DS5_BUTTON_CIRCLE) {
        out |= 1u << 1;
    }
    if (buttons & DS5_BUTTON_SQUARE) {
        out |= 1u << 2;
    }
    if (buttons & DS5_BUTTON_TRIANGLE) {
        out |= 1u << 3;
    }
    if (buttons & DS5_BUTTON_L1) {
        out |= 1u << 4;
    }
    if (buttons & DS5_BUTTON_R1) {
        out |= 1u << 5;
    }
    if (buttons & DS5_BUTTON_L2) {
        out |= 1u << 6;
    }
    if (buttons & DS5_BUTTON_R2) {
        out |= 1u << 7;
    }
    if (buttons & DS5_BUTTON_CREATE) {
        out |= 1u << 8;
    }
    if (buttons & DS5_BUTTON_OPTIONS) {
        out |= 1u << 9;
    }
    if (buttons & DS5_BUTTON_L3) {
        out |= 1u << 10;
    }
    if (buttons & DS5_BUTTON_R3) {
        out |= 1u << 11;
    }
    if (buttons & DS5_BUTTON_PS) {
        out |= 1u << 12;
    }
    if (buttons & DS5_BUTTON_TOUCHPAD) {
        out |= 1u << 13;
    }
    if (buttons & DS5_BUTTON_MUTE) {
        out |= 1u << 14;
    }
    if (buttons & (DS5_BUTTON_EDGE_FN_L | DS5_BUTTON_EDGE_FN_R |
                   DS5_BUTTON_EDGE_PADDLE_L | DS5_BUTTON_EDGE_PADDLE_R)) {
        out |= 1u << 15;
    }

    return out;
}

static usb_gamepad_report_t make_report(const dualsense_state_t *state)
{
    usb_gamepad_report_t report = {
        .buttons = map_buttons(state->buttons),
        .hat_and_pad = (state->dpad <= 8U) ? state->dpad : 8U,
        .lx = state->left_x,
        .ly = state->left_y,
        .rx = state->right_x,
        .ry = state->right_y,
        .l2 = state->l2,
        .r2 = state->r2,
    };

    return report;
}

void m61_usb_gamepad_init(void)
{
    usbd_desc_register(0, &gamepad_descriptor);
    usbd_add_interface(0, usbd_hid_init_intf(0, &intf0, gamepad_report_desc, sizeof(gamepad_report_desc)));
    usbd_add_endpoint(0, &hid_in_ep);
    usbd_initialize(0, 0, usbd_event_handler);
}

void m61_usb_gamepad_send_state(const dualsense_state_t *state)
{
    if (!state) {
        return;
    }

    usb_gamepad_report_t report = make_report(state);
    last_report = report;

    if (!usb_ready || !usb_configured || usb_busy || usb_suspended || !usb_device_is_configured(0)) {
        usb_drop_count++;
        return;
    }

    memcpy(usb_report_buffer, &report, sizeof(report));
    usb_busy = true;
    usbd_ep_start_write(0, HID_INT_EP, usb_report_buffer, sizeof(report));
    usb_sent_count++;
}

bool m61_usb_gamepad_ready(void)
{
    return usb_ready;
}

bool m61_usb_gamepad_configured(void)
{
    return usb_configured;
}

bool m61_usb_gamepad_busy(void)
{
    return usb_busy;
}

uint32_t m61_usb_gamepad_sent_count(void)
{
    return usb_sent_count;
}

uint32_t m61_usb_gamepad_drop_count(void)
{
    return usb_drop_count;
}

void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;

    memcpy(usb_report_buffer, &last_report, sizeof(last_report));
    *data = usb_report_buffer;
    *len = sizeof(last_report);
}

#else
void m61_usb_gamepad_init(void) {}
void m61_usb_gamepad_send_state(const dualsense_state_t *state) { (void)state; }
bool m61_usb_gamepad_ready(void) { return false; }
bool m61_usb_gamepad_configured(void) { return false; }
bool m61_usb_gamepad_busy(void) { return false; }
uint32_t m61_usb_gamepad_sent_count(void) { return 0; }
uint32_t m61_usb_gamepad_drop_count(void) { return 0; }
#endif
