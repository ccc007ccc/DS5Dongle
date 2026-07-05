#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_hid.h"

#define HID_INT_EP          0x81
#define HID_INT_EP_SIZE     16
#define HID_INT_EP_INTERVAL 10

#define USBD_VID       0x1209
#define USBD_PID       0x5D51
#define USBD_MAX_POWER 100

#define HID_CONFIG_SIZE 34
#define GAMEPAD_REPORT_SIZE 9

static const uint8_t gamepad_report_desc[] = {
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

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(HID_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),

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
    sizeof(gamepad_report_desc) & 0xff,
    (sizeof(gamepad_report_desc) >> 8) & 0xff,

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
    "M61 HID Gamepad Probe",
    "M61HIDPROBE",
};

static volatile bool hid_ready;
static volatile bool hid_busy;
static volatile bool hid_suspended;
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t report_buffer[GAMEPAD_REPORT_SIZE];
static struct usbd_interface intf0;

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

static const struct usb_descriptor hid_descriptor = {
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
            hid_ready = false;
            hid_busy = false;
            hid_suspended = false;
            break;
        case USBD_EVENT_CONFIGURED:
            hid_ready = true;
            hid_busy = false;
            hid_suspended = false;
            break;
        case USBD_EVENT_SUSPEND:
            hid_suspended = true;
            break;
        case USBD_EVENT_RESUME:
            hid_suspended = false;
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
    hid_busy = false;
}

static struct usbd_endpoint hid_in_ep = {
    .ep_cb = usbd_hid_int_callback,
    .ep_addr = HID_INT_EP,
};

void hid_gamepad_probe_init(uint8_t busid, uintptr_t reg_base)
{
    usbd_desc_register(busid, &hid_descriptor);
    usbd_add_interface(busid, usbd_hid_init_intf(busid, &intf0, gamepad_report_desc, sizeof(gamepad_report_desc)));
    usbd_add_endpoint(busid, &hid_in_ep);
    usbd_initialize(busid, reg_base, usbd_event_handler);
}

void hid_gamepad_probe_poll(uint8_t busid)
{
    if (!hid_ready || hid_busy || hid_suspended || !usb_device_is_configured(busid)) {
        return;
    }

    memset(report_buffer, 0, sizeof(report_buffer));
    report_buffer[2] = 8;     /* neutral hat */
    report_buffer[3] = 0x80;  /* X */
    report_buffer[4] = 0x80;  /* Y */
    report_buffer[5] = 0x80;  /* Rx */
    report_buffer[6] = 0x80;  /* Ry */

    hid_busy = true;
    usbd_ep_start_write(busid, HID_INT_EP, report_buffer, sizeof(report_buffer));
}

void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;

    memset(report_buffer, 0, sizeof(report_buffer));
    report_buffer[2] = 8;
    report_buffer[3] = 0x80;
    report_buffer[4] = 0x80;
    report_buffer[5] = 0x80;
    report_buffer[6] = 0x80;
    *data = report_buffer;
    *len = sizeof(report_buffer);
}

uint8_t usbd_hid_get_idle(uint8_t busid, uint8_t intf, uint8_t report_id)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    return 0;
}

uint8_t usbd_hid_get_protocol(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
    return 1;
}

void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)report_type;
    (void)report;
    (void)report_len;
}

void usbd_hid_set_idle(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t duration)
{
    (void)busid;
    (void)intf;
    (void)report_id;
    (void)duration;
}

void usbd_hid_set_protocol(uint8_t busid, uint8_t intf, uint8_t protocol)
{
    (void)busid;
    (void)intf;
    (void)protocol;
}
