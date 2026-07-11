#include "m61_usb_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m61_ds5_bridge_config.h"
#include "m61_ds5_dse.h"
#include "m61_esp32_transport.h"
#include "m61_ps_shortcut.h"
#include "dual_chip_scheduler_types.h"
#include "bflb_clock.h"
#include "bflb_core.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "FreeRTOS.h"
#include "mm.h"
#include "opus/opus.h"
#include "task.h"
#include "usbd_audio.h"
#include "usbd_core.h"
#include "usbd_hid.h"

#if defined(BL616)
#include "bl616_glb.h"
#endif

#ifndef CONFIG_M61_USB_GAMEPAD_ENABLE
#define CONFIG_M61_USB_GAMEPAD_ENABLE 1
#endif

#define USBD_VID 0x054C
#define USBD_PID_DS5 0x0CE6
#define USBD_PID_DSE 0x0DF2
#define USBD_MAX_POWER 500
#define ITF_NUM_AUDIO_CONTROL 0
#define ITF_NUM_AUDIO_STREAMING_OUT 1
#define ITF_NUM_AUDIO_STREAMING_IN 2
#define ITF_NUM_HID 3
#define ITF_NUM_HID_KBD 4
#define ITF_NUM_BASE 4
#define ITF_NUM_TOTAL 5
#define AUDIO_OUT_EP 0x01
#define AUDIO_IN_EP 0x82
#define AUDIO_OUT_PACKET_SIZE 392
#define AUDIO_IN_PACKET_SIZE 196
#define AUDIO_IN_STREAM_PACKET_SIZE 192
#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_SPEAKER_FU_ID 0x02
#define AUDIO_MIC_FU_ID 0x05
#define HID_IN_EP 0x84
#define HID_OUT_EP 0x03
#define HID_INT_EP_SIZE 64
#define HID_INT_EP_INTERVAL 1
#define HID_KBD_IN_EP 0x87
#define HID_KBD_EP_SIZE 8
#define HID_KBD_EP_INTERVAL 10
#define HID_KBD_REPORT_DESC_SIZE 45
#define USB_DUALSENSE_CONFIG_DESC_BASE_SIZE 0x00E3
#define USB_DUALSENSE_CONFIG_DESC_KBD_SIZE 25
#define USB_DUALSENSE_CONFIG_DESC_SIZE \
    (USB_DUALSENSE_CONFIG_DESC_BASE_SIZE + USB_DUALSENSE_CONFIG_DESC_KBD_SIZE)
#define HID_DS5_REPORT_DESC_SIZE 321
#define HID_DSE_REPORT_DESC_SIZE 437
#define FEATURE_CACHE_SLOTS 32
#define AUDIO_INPUT_CHANNELS 4
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES (AUDIO_INPUT_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_SPEAKER_CHANNELS 2
#define AUDIO_SPEAKER_INPUT_FRAME_SAMPLES 512
#define AUDIO_SPEAKER_FRAME_SAMPLES 480
#define AUDIO_SPEAKER_OPUS_CHANNELS 1
#define AUDIO_SPEAKER_OPUS_BANDWIDTH OPUS_BANDWIDTH_MEDIUMBAND
#define AUDIO_SPEAKER_OPUS_BITRATE 160000
#define AUDIO_MIC_CHANNELS 1
#define AUDIO_MIC_FRAME_SAMPLES 480
#define AUDIO_MIC_OPUS_QUEUE_DEPTH DS5_SCHED_M61_MIC_OPUS_CAPACITY
#define AUDIO_MIC_PCM_RING_SIZE 4096
#define AUDIO_MIC_PAUSE_AFTER_SPEAKER_MS 250
#define AUDIO_CODEC_TASK_STACK_WORDS 8192
#define AUDIO_CODEC_TASK_PRIORITY (configMAX_PRIORITIES - 6)
#define AUDIO_OPUS_ENCODER_STATE_MAX 49152U
#define AUDIO_OPUS_DECODER_STATE_MAX 24576U
#define AUDIO_CODEC_DIAG_PERIOD_MS 1000U
#define USB_RECONNECT_DELAY_MS 150U
#define USB_RECONNECT_TASK_STACK_WORDS 1024
#define USB_RECONNECT_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define USB_INPUT_PUMP_TASK_STACK_WORDS 512
#define USB_INPUT_PUMP_TASK_PRIORITY (configMAX_PRIORITIES - 5)
#define USB_AUDIO_INGRESS_TASK_STACK_WORDS 1024
#define USB_AUDIO_INGRESS_TASK_PRIORITY (configMAX_PRIORITIES - 5)
#define USB_CONTROL_INGRESS_DEPTH 4
#define USB_AUDIO_INGRESS_DEPTH 4
#define USB_KEYBOARD_TASK_STACK_WORDS 1024
#define USB_KEYBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define USB_KEYBOARD_TASK_PERIOD_MS 5U
#define USB_SHORTCUT_KEY_HOLD_MS 30U
#define USB_WAKE_SETTLE_MS 150U
#define USB_WAKE_KEY_HOLD_MS 80U
#define USB_WAKE_KEY_UP_SETTLE_MS 200U
#define USB_WAKE_REQUEST_TIMEOUT_MS 5000U
#define USB_WAKE_KEY_ATTEMPTS 2U
#define HID_KEY_G 0x0AU
#define HID_KEY_TAB 0x2BU
#define HID_KEY_F15 0x68U
#define HID_MODIFIER_LEFT_GUI 0x08U
#define MS_OS_20_VENDOR_CODE 0x01U
#define MS_OS_20_DESC_LEN 88U
#define BOS_DESC_LEN 33U
#define AUDIO_CODEC_ERR_STATE_TOO_SMALL -1001
#define HAPTICS_DOWNSAMPLE_FACTOR 16
#define HAPTICS_RESAMPLE_MODE_WDL_EQUIV 2
#ifndef CONFIG_M61_DS5_BRIDGE_VERSION_STRING
#define CONFIG_M61_DS5_BRIDGE_VERSION_STRING "m61-ds5-bridge"
#endif
#define DS5_USB_SET_STATE_LEN 47
#define DS5_USB_OUTPUT_REPORT_ID 0x02
#define DS5_USB_STATE_FLAGS0 0
#define DS5_USB_STATE_FLAGS1 1
#define DS5_USB_STATE_RUMBLE_RIGHT 2
#define DS5_USB_STATE_RUMBLE_LEFT 3
#define DS5_USB_STATE_AUDIO_CONTROL 7
#define DS5_USB_STATE_MUTE_LIGHT 8
#define DS5_USB_STATE_AUDIO_MUTE 9
#define DS5_USB_STATE_FLAGS2 38
#define DS5_USB_STATE_LIGHT_FADE 41
#define DS5_USB_STATE_LIGHT_BRIGHTNESS 42
#define DS5_USB_STATE_PLAYER_LIGHTS 43
#define DS5_USB_STATE_LED_RED 44

#if defined(BL616) || defined(BL616CL)
#define M61_USB_BASE ((uint32_t)0x20072000)
#elif defined(BL618DG)
#define M61_USB_BASE ((uint32_t)0x20087000)
#endif

#define M61_USB_OTG_CSR_OFFSET     0x080
#define M61_USB_GLB_ISR_OFFSET     0x0C0
#define M61_USB_DEV_CTL_OFFSET     0x100
#define M61_USB_DEV_ADR_OFFSET     0x104
#define M61_USB_PHY_TST_OFFSET     0x114
#define M61_USB_DEV_IGR_OFFSET     0x140
#define M61_USB_DEV_ISG0_OFFSET    0x144
#define M61_USB_DEV_ISG2_OFFSET    0x14C
#define M61_USB_VDMA_CXFPS1_OFFSET 0x300
#define M61_USB_DEV_ISG3_OFFSET    0x328
#define M61_USB_VDMA_CTRL_OFFSET   0x330

typedef struct {
    bool valid;
    uint8_t report_id;
    uint32_t len;
    uint8_t data[M61_DS5_USB_FEATURE_MAX_LEN];
} feature_cache_entry_t;

typedef enum {
    USB_CONTROL_CONFIG_SET = 1,
    USB_CONTROL_CONFIG_SAVE,
    USB_CONTROL_RECONNECT,
} usb_control_op_t;

typedef struct {
    uint32_t generation;
    uint8_t op;
    uint8_t len;
    uint8_t data[M61_DS5_USB_FEATURE_MAX_LEN];
} usb_control_ingress_t;

typedef struct {
    uint64_t captured_us;
    uint32_t generation;
    uint16_t len;
    uint8_t data[AUDIO_OUT_PACKET_SIZE];
} usb_audio_ingress_t;

#if CONFIG_M61_USB_GAMEPAD_ENABLE
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID_DS5, 0x0100, 0x01)
};
static uint8_t device_descriptor_runtime[sizeof(device_descriptor)];

static const uint8_t dualsense_ds5_report_desc[HID_DS5_REPORT_DESC_SIZE] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x23, 0x95, 0x2F, 0x91, 0x02,
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02,
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02,
    0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x0B, 0x09, 0x41, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x0C, 0x09, 0x42, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02,
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x82, 0x09, 0x2A, 0x95, 0x09, 0xB1, 0x02,
    0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x85, 0x09, 0x2D, 0x95, 0x02, 0xB1, 0x02,
    0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF0, 0x09, 0x30, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF2, 0x09, 0x32, 0x95, 0x0F, 0xB1, 0x02,
    0x85, 0xF4, 0x09, 0x35, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    0x85, 0xF6, 0x09, 0x37, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF7, 0x09, 0x38, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF8, 0x09, 0x39, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF9, 0x09, 0x3A, 0x95, 0x3F, 0xB1, 0x02,
    0xC0,
};

static const uint8_t dualsense_dse_report_desc[HID_DSE_REPORT_DESC_SIZE] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x23, 0x95, 0x3F, 0x91, 0x02,
    0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02,
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02,
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02,
    0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x0B, 0x09, 0x41, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x0C, 0x09, 0x42, 0x95, 0x29, 0xB1, 0x02,
    0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02,
    0x85, 0x22, 0x09, 0x40, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x82, 0x09, 0x2A, 0x95, 0x09, 0xB1, 0x02,
    0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x85, 0x09, 0x2D, 0x95, 0x02, 0xB1, 0x02,
    0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF0, 0x09, 0x30, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF2, 0x09, 0x32, 0x95, 0x34, 0xB1, 0x02,
    0x85, 0xF4, 0x09, 0x35, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    0x85, 0x60, 0x09, 0x41, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x61, 0x09, 0x42, 0xB1, 0x02, 0x85, 0x62,
    0x09, 0x43, 0xB1, 0x02, 0x85, 0x63, 0x09, 0x44,
    0xB1, 0x02, 0x85, 0x64, 0x09, 0x45, 0xB1, 0x02,
    0x85, 0x65, 0x09, 0x46, 0xB1, 0x02, 0x85, 0x68,
    0x09, 0x47, 0xB1, 0x02, 0x85, 0x70, 0x09, 0x48,
    0xB1, 0x02, 0x85, 0x71, 0x09, 0x49, 0xB1, 0x02,
    0x85, 0x72, 0x09, 0x4A, 0xB1, 0x02, 0x85, 0x73,
    0x09, 0x4B, 0xB1, 0x02, 0x85, 0x74, 0x09, 0x4C,
    0xB1, 0x02, 0x85, 0x75, 0x09, 0x4D, 0xB1, 0x02,
    0x85, 0x76, 0x09, 0x4E, 0xB1, 0x02, 0x85, 0x77,
    0x09, 0x4F, 0xB1, 0x02, 0x85, 0x78, 0x09, 0x50,
    0xB1, 0x02, 0x85, 0x79, 0x09, 0x51, 0xB1, 0x02,
    0x85, 0x7A, 0x09, 0x52, 0xB1, 0x02, 0x85, 0x7B,
    0x09, 0x53, 0xB1, 0x02, 0x85, 0xF6, 0x09, 0x37,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF7, 0x09, 0x38,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF8, 0x09, 0x39,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF9, 0x09, 0x3A,
    0x95, 0x3F, 0xB1, 0x02, 0xC0,
};

static const uint8_t keyboard_report_desc[HID_KBD_REPORT_DESC_SIZE] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_DUALSENSE_CONFIG_DESC_SIZE,
                               ITF_NUM_TOTAL,
                               0x01,
                               USB_CONFIG_SELF_POWERED,
                               USBD_MAX_POWER),

    /* Interface 0: Audio Control. */
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_AUDIO_CONTROL, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00,

    0x0A, 0x24, 0x01,
    0x00, 0x01,
    0x49, 0x00,
    0x02,
    ITF_NUM_AUDIO_STREAMING_OUT,
    ITF_NUM_AUDIO_STREAMING_IN,

    0x0C, 0x24, 0x02,
    0x01,
    0x01, 0x01,
    0x06,
    0x04,
    0x33, 0x00,
    0x00,
    0x00,

    0x0C, 0x24, 0x06,
    AUDIO_SPEAKER_FU_ID,
    0x01,
    0x01,
    0x03,
    0x00, 0x00, 0x00, 0x00, 0x00,

    0x09, 0x24, 0x03,
    0x03,
    0x01, 0x03,
    0x04,
    AUDIO_SPEAKER_FU_ID,
    0x00,

    0x0C, 0x24, 0x02,
    0x04,
    0x02, 0x04,
    0x03,
    0x02,
    0x03, 0x00,
    0x00,
    0x00,

    0x09, 0x24, 0x06,
    AUDIO_MIC_FU_ID,
    0x04,
    0x01,
    0x03,
    0x00,
    0x00,

    0x09, 0x24, 0x03,
    0x06,
    0x01, 0x01,
    0x01,
    AUDIO_MIC_FU_ID,
    0x00,

    /* Interface 1: Audio Streaming OUT, alternate 0/1. */
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_OUT, 0x00, 0x00,
    0x01, 0x02, 0x00, 0x00,

    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_OUT, 0x01, 0x01,
    0x01, 0x02, 0x00, 0x00,

    0x07, 0x24, 0x01,
    0x01,
    0x01,
    0x01, 0x00,

    0x0B, 0x24, 0x02,
    0x01,
    0x04,
    0x02,
    0x10,
    0x01,
    0x80, 0xBB, 0x00,

    0x09, USB_DESCRIPTOR_TYPE_ENDPOINT,
    AUDIO_OUT_EP,
    0x09,
    AUDIO_OUT_PACKET_SIZE & 0xFF,
    (AUDIO_OUT_PACKET_SIZE >> 8) & 0xFF,
    0x01,
    0x00,
    0x00,

    0x07, 0x25, 0x01,
    0x00,
    0x00,
    0x00, 0x00,

    /* Interface 2: Audio Streaming IN, alternate 0/1. */
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_IN, 0x00, 0x00,
    0x01, 0x02, 0x00, 0x00,

    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_IN, 0x01, 0x01,
    0x01, 0x02, 0x00, 0x00,

    0x07, 0x24, 0x01,
    0x06,
    0x01,
    0x01, 0x00,

    0x0B, 0x24, 0x02,
    0x01,
    0x02,
    0x02,
    0x10,
    0x01,
    0x80, 0xBB, 0x00,

    0x09, USB_DESCRIPTOR_TYPE_ENDPOINT,
    AUDIO_IN_EP,
    0x05,
    AUDIO_IN_PACKET_SIZE & 0xFF,
    (AUDIO_IN_PACKET_SIZE >> 8) & 0xFF,
    0x01,
    0x00,
    0x00,

    0x07, 0x25, 0x01,
    0x00,
    0x00,
    0x00, 0x00,

    /* Interface 3: DualSense HID. */
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_HID, 0x00, 0x02,
    0x03, 0x00, 0x00, 0x00,

    0x09, HID_DESCRIPTOR_TYPE_HID,
    0x11, 0x01,
    0x00,
    0x01,
    HID_DESCRIPTOR_TYPE_HID_REPORT,
    HID_DS5_REPORT_DESC_SIZE & 0xFF,
    (HID_DS5_REPORT_DESC_SIZE >> 8) & 0xFF,

    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT,
    HID_IN_EP,
    USB_ENDPOINT_TYPE_INTERRUPT,
    HID_INT_EP_SIZE, 0x00,
    HID_INT_EP_INTERVAL,

    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT,
    HID_OUT_EP,
    USB_ENDPOINT_TYPE_INTERRUPT,
    HID_INT_EP_SIZE, 0x00,
    HID_INT_EP_INTERVAL,

    /* Interface 4: optional HID boot keyboard for wake and PS shortcuts. */
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE,
    ITF_NUM_HID_KBD, 0x00, 0x01,
    0x03, 0x01, 0x01, 0x00,

    0x09, HID_DESCRIPTOR_TYPE_HID,
    0x11, 0x01,
    0x00,
    0x01,
    HID_DESCRIPTOR_TYPE_HID_REPORT,
    HID_KBD_REPORT_DESC_SIZE, 0x00,

    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT,
    HID_KBD_IN_EP,
    USB_ENDPOINT_TYPE_INTERRUPT,
    HID_KBD_EP_SIZE, 0x00,
    HID_KBD_EP_INTERVAL,
};
static uint8_t config_descriptor_runtime[sizeof(config_descriptor)];

static const uint8_t wake_bos_descriptor[] = {
    USB_BOS_HEADER_DESCRIPTOR_INIT(BOS_DESC_LEN, 1),
    USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_INIT(MS_OS_20_VENDOR_CODE,
                                                MS_OS_20_DESC_LEN),
};

static const uint8_t wake_ms_os_20_descriptor[] = {
    USB_MSOSV2_COMP_ID_SET_HEADER_DESCRIPTOR_INIT(MS_OS_20_DESC_LEN),

    WBVAL(0x0008),
    WBVAL(WINUSB_SUBSET_HEADER_CONFIGURATION_TYPE),
    0x00,
    0x00,
    WBVAL(MS_OS_20_DESC_LEN - 0x0A),

    WBVAL(0x0008),
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE),
    ITF_NUM_AUDIO_CONTROL,
    0x00,
    WBVAL(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    WBVAL(0x003E),
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),
    WBVAL(0x0004),
    WBVAL(48),
    'S', 0, 'e', 0, 'l', 0, 'e', 0, 'c', 0, 't', 0, 'i', 0, 'v', 0,
    'e', 0, 'S', 0, 'u', 0, 's', 0, 'p', 0, 'e', 0, 'n', 0, 'd', 0,
    'E', 0, 'n', 0, 'a', 0, 'b', 0, 'l', 0, 'e', 0, 'd', 0, 0, 0,
    WBVAL(0x0004),
    DBVAL(0x00000001),
};

static const struct usb_bos_descriptor wake_bos = {
    .string = wake_bos_descriptor,
    .string_len = sizeof(wake_bos_descriptor),
};

static const struct usb_msosv2_descriptor wake_ms_os_20 = {
    .compat_id = wake_ms_os_20_descriptor,
    .compat_id_len = sizeof(wake_ms_os_20_descriptor),
    .vendor_code = MS_OS_20_VENDOR_CODE,
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
    "Sony Interactive Entertainment",
    "DualSense Wireless Controller",
    "M61DS5COMPOSITE1",
};
static const char dualsense_edge_product_string[] =
    "DualSense Edge Wireless Controller";

static const uint8_t ds5_idle_payload[M61_DS5_USB_INPUT_PAYLOAD_LEN] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7,
    0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
    0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03,
    0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
    0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b
};

static volatile bool usb_ready;
static volatile bool usb_configured;
static volatile bool usb_busy;
static volatile bool keyboard_busy;
static volatile bool usb_suspended;
static volatile bool usb_out_armed;
static volatile bool audio_out_open;
static volatile bool audio_in_open;
static volatile bool audio_in_busy;
static volatile bool audio_codec_task_started;
static volatile bool pending_feature_request_valid;
static volatile bool pending_output_report_valid;
static volatile bool usb_initialized;
static volatile bool usb_input_pending;
static volatile uint32_t usb_sent_count;
static volatile uint32_t usb_drop_count;
static volatile uint32_t usb_generation = 1;
static volatile uint32_t usb_input_pending_generation;
static volatile int usb_init_result = -1;
static volatile uint8_t usb_last_event;
static volatile uint32_t usb_event_counts[16];
static volatile m61_usb_gamepad_diag_t usb_diag;
static volatile uint8_t pending_feature_report_id;
static volatile uint32_t pending_feature_report_len;
static volatile uint32_t audio_out_sample_rate = AUDIO_SAMPLE_RATE;
static volatile uint32_t audio_in_sample_rate = AUDIO_SAMPLE_RATE;
static volatile bool audio_speaker_mute;
static volatile bool audio_mic_mute;
static volatile int audio_speaker_volume_db;
static volatile int audio_mic_volume_db;
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_in_buffer[HID_INT_EP_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_out_buffer[HID_INT_EP_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t keyboard_in_buffer[HID_KBD_EP_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_control_buffer[M61_DS5_USB_FEATURE_MAX_LEN];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_out_buffer[AUDIO_OUT_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_in_buffer[AUDIO_IN_PACKET_SIZE];
static uint8_t last_input_payload[M61_DS5_USB_INPUT_PAYLOAD_LEN];
static uint8_t usb_input_pending_payload[M61_DS5_USB_INPUT_PAYLOAD_LEN];
typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[M61_DS5_MIC_OPUS_LEN];
} m61_mic_opus_slot_t;

typedef struct {
    m61_mic_opus_slot_t slots[AUDIO_MIC_OPUS_QUEUE_DEPTH];
    uint32_t sequence;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t reserved;
} m61_mic_opus_storage_t;

m61_mic_opus_storage_t g_m61_mic_opus_storage;

_Static_assert(sizeof(m61_mic_opus_slot_t) == 104U,
               "M61 microphone Opus slot layout drift");
_Static_assert(sizeof(g_m61_mic_opus_storage.slots) == 416U,
               "M61 microphone Opus typed ring budget drift");

typedef struct {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    m61_usb_gamepad_host_report_t report;
} m61_usb_control_slot_t;

typedef struct {
    m61_usb_control_slot_t slots[DS5_SCHED_M61_USB_CONTROL_CAPACITY];
    uint32_t sequence;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t reserved;
} m61_usb_control_storage_t;

m61_usb_control_storage_t g_m61_usb_control_storage;

_Static_assert(sizeof(m61_usb_control_slot_t) == 104U,
               "M61 USB control slot layout drift");
_Static_assert(sizeof(g_m61_usb_control_storage.slots) == 832U,
               "M61 USB reliable control ring budget drift");
static uint8_t mic_pcm_ring[AUDIO_MIC_PCM_RING_SIZE];
static volatile uint16_t mic_pcm_ring_head;
static volatile uint16_t mic_pcm_ring_tail;
static volatile uint16_t mic_pcm_ring_count;
static StaticTask_t audio_codec_task_tcb;
static StackType_t audio_codec_task_stack[AUDIO_CODEC_TASK_STACK_WORDS] __attribute__((aligned(16)));
static StaticTask_t usb_reconnect_task_tcb;
static StackType_t usb_reconnect_task_stack[USB_RECONNECT_TASK_STACK_WORDS] __attribute__((aligned(16)));
static StaticTask_t usb_input_pump_task_tcb;
static StackType_t usb_input_pump_task_stack[USB_INPUT_PUMP_TASK_STACK_WORDS] __attribute__((aligned(16)));
static StaticTask_t usb_audio_ingress_task_tcb;
static StackType_t usb_audio_ingress_task_stack[USB_AUDIO_INGRESS_TASK_STACK_WORDS] __attribute__((aligned(16)));
static StaticTask_t usb_keyboard_task_tcb;
static StackType_t usb_keyboard_task_stack[USB_KEYBOARD_TASK_STACK_WORDS] __attribute__((aligned(16)));
static uint32_t audio_opus_encoder_state[(AUDIO_OPUS_ENCODER_STATE_MAX + sizeof(uint32_t) - 1U) / sizeof(uint32_t)] __attribute__((aligned(16)));
static uint32_t audio_opus_decoder_state[(AUDIO_OPUS_DECODER_STATE_MAX + sizeof(uint32_t) - 1U) / sizeof(uint32_t)] __attribute__((aligned(16)));
static TaskHandle_t audio_codec_task_handle;
static TaskHandle_t usb_reconnect_task_handle;
static TaskHandle_t usb_input_pump_task_handle;
static TaskHandle_t usb_audio_ingress_task_handle;
static TaskHandle_t usb_keyboard_task_handle;
static m61_usb_gamepad_host_report_t pending_output_report;
static m61_ps_shortcut_t ps_shortcut;
static bool descriptor_wake_enabled;
static bool descriptor_keyboard_enabled;
static bool descriptor_usb_serial_enabled;
static bool descriptor_edge_enabled;
static uint8_t descriptor_polling_interval = HID_INT_EP_INTERVAL;
static bool shortcut_key_release_pending;
static uint32_t shortcut_key_release_time_ms;
static uint8_t keyboard_protocol = 1U;
static uint8_t keyboard_idle_rate;

typedef enum {
    USB_WAKE_IDLE = 0,
    USB_WAKE_REQUESTED,
    USB_WAKE_KEY_DOWN,
    USB_WAKE_KEY_UP_SENT,
    USB_WAKE_DONE,
} usb_wake_state_t;

static volatile usb_wake_state_t usb_wake_state;
static volatile uint32_t usb_wake_state_entered_ms;
static volatile bool usb_wake_resume_seen;
static volatile uint8_t usb_wake_key_attempts;

static void queue_host_report(uint8_t report_id, uint8_t report_type,
                              const uint8_t *data, uint32_t len);
static void queue_host_report_generation(uint8_t report_id,
                                         uint8_t report_type,
                                         const uint8_t *data,
                                         uint32_t len,
                                         uint32_t generation);
static void arm_audio_in(uint8_t busid);
static feature_cache_entry_t feature_cache[FEATURE_CACHE_SLOTS];
static uint8_t feature_cache_replace_index;
static usb_control_ingress_t usb_control_ingress[USB_CONTROL_INGRESS_DEPTH];
static volatile uint8_t usb_control_ingress_head;
static volatile uint8_t usb_control_ingress_tail;
static volatile uint8_t usb_control_ingress_count;
static usb_audio_ingress_t usb_audio_ingress[USB_AUDIO_INGRESS_DEPTH];
static volatile uint8_t usb_audio_ingress_head;
static volatile uint8_t usb_audio_ingress_tail;
static volatile uint8_t usb_audio_ingress_count;

typedef char ds5_report_desc_size_check[(sizeof(dualsense_ds5_report_desc) == HID_DS5_REPORT_DESC_SIZE) ? 1 : -1];
typedef char dse_report_desc_size_check[(sizeof(dualsense_dse_report_desc) == HID_DSE_REPORT_DESC_SIZE) ? 1 : -1];
typedef char ds5_config_desc_size_check[(sizeof(config_descriptor) == USB_DUALSENSE_CONFIG_DESC_SIZE) ? 1 : -1];
typedef char keyboard_report_desc_size_check[(sizeof(keyboard_report_desc) == HID_KBD_REPORT_DESC_SIZE) ? 1 : -1];
typedef char wake_bos_desc_size_check[(sizeof(wake_bos_descriptor) == BOS_DESC_LEN) ? 1 : -1];
typedef char wake_ms_os_20_desc_size_check[(sizeof(wake_ms_os_20_descriptor) == MS_OS_20_DESC_LEN) ? 1 : -1];
typedef char usb_audio_ingress_budget_check[(sizeof(usb_audio_ingress_t) <= 408U) ? 1 : -1];

static uintptr_t usb_lock(void)
{
    return bflb_irq_save();
}

static void usb_unlock(uintptr_t flags)
{
    bflb_irq_restore(flags);
}

static void usb_notify_from_isr(TaskHandle_t task)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (task == NULL) {
        return;
    }
    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static uint32_t usb_now_ms(void)
{
    return (uint32_t)bflb_mtimer_get_time_ms();
}

static bool usb_time_due(uint32_t now_ms, uint32_t due_ms)
{
    return (int32_t)(now_ms - due_ms) >= 0;
}

static void reset_keyboard_runtime(bool release_key)
{
    uintptr_t flags = usb_lock();

    m61_ps_shortcut_reset(&ps_shortcut);
    shortcut_key_release_pending = release_key;
    shortcut_key_release_time_ms = usb_now_ms();
    usb_wake_state = USB_WAKE_IDLE;
    usb_wake_state_entered_ms = 0;
    usb_wake_resume_seen = false;
    usb_wake_key_attempts = 0;
    usb_unlock(flags);
}

static bool keyboard_send_report(uint8_t modifier, uint8_t keycode)
{
    int ret;

    if (!descriptor_keyboard_enabled || !usb_ready || !usb_configured ||
        usb_suspended || keyboard_busy || !usb_device_is_configured(0)) {
        return false;
    }

    memset(keyboard_in_buffer, 0, sizeof(keyboard_in_buffer));
    keyboard_in_buffer[0] = modifier;
    keyboard_in_buffer[2] = keycode;
    keyboard_busy = true;
    ret = usbd_ep_start_write(0, HID_KBD_IN_EP,
                              keyboard_in_buffer,
                              sizeof(keyboard_in_buffer));
    if (ret != 0) {
        keyboard_busy = false;
        return false;
    }
    return true;
}

static bool process_wake_keyboard(uint32_t now_ms)
{
    usb_wake_state_t state = usb_wake_state;

    switch (state) {
        case USB_WAKE_REQUESTED:
            if (usb_wake_resume_seen) {
                if ((uint32_t)(now_ms - usb_wake_state_entered_ms) <
                    USB_WAKE_SETTLE_MS) {
                    return true;
                }
                if (keyboard_send_report(0, HID_KEY_F15)) {
                    usb_wake_state = USB_WAKE_KEY_DOWN;
                    usb_wake_state_entered_ms = now_ms;
                }
            } else if ((uint32_t)(now_ms - usb_wake_state_entered_ms) >
                       USB_WAKE_REQUEST_TIMEOUT_MS) {
                usb_wake_state = USB_WAKE_DONE;
            }
            return true;

        case USB_WAKE_KEY_DOWN:
            if ((uint32_t)(now_ms - usb_wake_state_entered_ms) >=
                    USB_WAKE_KEY_HOLD_MS &&
                keyboard_send_report(0, 0)) {
                usb_wake_state = USB_WAKE_KEY_UP_SENT;
                usb_wake_state_entered_ms = now_ms;
            }
            return true;

        case USB_WAKE_KEY_UP_SENT:
            if ((uint32_t)(now_ms - usb_wake_state_entered_ms) <
                USB_WAKE_KEY_UP_SETTLE_MS) {
                return true;
            }
            usb_wake_key_attempts++;
            if (usb_wake_key_attempts < USB_WAKE_KEY_ATTEMPTS) {
                if (keyboard_send_report(0, HID_KEY_F15)) {
                    usb_wake_state = USB_WAKE_KEY_DOWN;
                    usb_wake_state_entered_ms = now_ms;
                } else {
                    usb_wake_key_attempts--;
                }
            } else {
                usb_wake_state = USB_WAKE_DONE;
            }
            return true;

        case USB_WAKE_IDLE:
        case USB_WAKE_DONE:
        default:
            return false;
    }
}

static bool usb_input_pump_once(void)
{
    uint32_t generation;
    uintptr_t flags;
    int ret;

    flags = usb_lock();
    generation = usb_generation;
    if (!usb_input_pending || usb_input_pending_generation != generation ||
        usb_busy || !usb_ready || !usb_configured || usb_suspended) {
        usb_unlock(flags);
        return false;
    }
    usb_in_buffer[0] = M61_DS5_USB_INPUT_REPORT_ID;
    memcpy(usb_in_buffer + 1, usb_input_pending_payload,
           M61_DS5_USB_INPUT_PAYLOAD_LEN);
    usb_input_pending = false;
    usb_busy = true;
    usb_unlock(flags);

    if (!usb_device_is_configured(0)) {
        ret = -1;
    } else {
        ret = usbd_ep_start_write(0, HID_IN_EP, usb_in_buffer,
                                  M61_DS5_USB_INPUT_PAYLOAD_LEN + 1U);
    }
    if (ret == 0) {
        usb_sent_count++;
        return true;
    }

    flags = usb_lock();
    usb_busy = false;
    usb_diag.usb_input_retries++;
    if (usb_generation == generation && !usb_input_pending) {
        memcpy(usb_input_pending_payload, usb_in_buffer + 1,
               M61_DS5_USB_INPUT_PAYLOAD_LEN);
        usb_input_pending_generation = generation;
        usb_input_pending = true;
    }
    usb_unlock(flags);
    return false;
}

static void usb_input_pump_task(void *arg)
{
    (void)arg;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (usb_input_pump_once()) {
        }
        if (usb_input_pending && !usb_busy && usb_ready && usb_configured &&
            !usb_suspended) {
            vTaskDelay(pdMS_TO_TICKS(1));
            xTaskNotifyGive(usb_input_pump_task_handle);
        }
    }
}

static void ensure_usb_input_pump_task(void)
{
    if (usb_input_pump_task_handle != NULL) {
        return;
    }
    usb_input_pump_task_handle = xTaskCreateStatic(
        usb_input_pump_task,
        "ds5_usb_in",
        USB_INPUT_PUMP_TASK_STACK_WORDS,
        NULL,
        USB_INPUT_PUMP_TASK_PRIORITY,
        usb_input_pump_task_stack,
        &usb_input_pump_task_tcb);
    if (usb_input_pump_task_handle == NULL) {
        printf("M61 USB input pump task create failed\r\n");
    }
}

static void usb_keyboard_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t now_ms = usb_now_ms();

        if (descriptor_keyboard_enabled && !keyboard_busy &&
            !process_wake_keyboard(now_ms)) {
            if (shortcut_key_release_pending) {
                if (usb_time_due(now_ms, shortcut_key_release_time_ms) &&
                    keyboard_send_report(0, 0)) {
                    shortcut_key_release_pending = false;
                }
            } else if (m61_ds5_bridge_config_ps_shortcut_enabled()) {
                m61_ps_shortcut_action_t action;
                uintptr_t flags = usb_lock();

                action = m61_ps_shortcut_take_action(&ps_shortcut);
                usb_unlock(flags);
                if (action != M61_PS_SHORTCUT_NONE) {
                    uint8_t keycode = action == M61_PS_SHORTCUT_WIN_TAB ?
                                      HID_KEY_TAB : HID_KEY_G;

                    if (keyboard_send_report(HID_MODIFIER_LEFT_GUI, keycode)) {
                        shortcut_key_release_pending = true;
                        shortcut_key_release_time_ms =
                            now_ms + USB_SHORTCUT_KEY_HOLD_MS;
                    } else {
                        flags = usb_lock();
                        ps_shortcut.pending_action = action;
                        usb_unlock(flags);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(USB_KEYBOARD_TASK_PERIOD_MS));
    }
}

static void ensure_usb_keyboard_task(void)
{
    if (usb_keyboard_task_handle != NULL) {
        return;
    }
    usb_keyboard_task_handle = xTaskCreateStatic(usb_keyboard_task,
                                                 "ds5_kbd",
                                                 USB_KEYBOARD_TASK_STACK_WORDS,
                                                 NULL,
                                                 USB_KEYBOARD_TASK_PRIORITY,
                                                 usb_keyboard_task_stack,
                                                 &usb_keyboard_task_tcb);
    if (usb_keyboard_task_handle == NULL) {
        printf("M61 USB keyboard task create failed\r\n");
    }
}

static bool bytes_have_nonzero(const uint8_t *data, size_t len)
{
    if (!data) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            return true;
        }
    }
    return false;
}

static void flush_mic_queues(void)
{
    uintptr_t flags = usb_lock();

    memset(&g_m61_mic_opus_storage, 0, sizeof(g_m61_mic_opus_storage));
    mic_pcm_ring_head = 0;
    mic_pcm_ring_tail = 0;
    mic_pcm_ring_count = 0;
    usb_unlock(flags);
}

static void record_speaker_encode_diag(uint32_t encode_us, int encoded_len)
{
    uintptr_t flags = usb_lock();

    usb_diag.audio_speaker_encode_us_last = encode_us;
    usb_diag.audio_speaker_encode_us_total += encode_us;
    if (encode_us > usb_diag.audio_speaker_encode_us_max) {
        usb_diag.audio_speaker_encode_us_max = encode_us;
    }
    usb_diag.audio_speaker_last_opus_len =
        (encoded_len > 0) ? (uint16_t)encoded_len : 0;
    usb_unlock(flags);
}

static void resample_speaker_512_to_480(int16_t *dst, const int16_t *src)
{
    if (!dst || !src) {
        return;
    }

    for (uint32_t out = 0; out < AUDIO_SPEAKER_FRAME_SAMPLES; out++) {
        uint32_t src_pos_num = out * AUDIO_SPEAKER_INPUT_FRAME_SAMPLES;
        uint32_t src_frame = src_pos_num / AUDIO_SPEAKER_FRAME_SAMPLES;
        uint32_t frac = src_pos_num % AUDIO_SPEAKER_FRAME_SAMPLES;

        uint32_t index = src_frame * AUDIO_SPEAKER_CHANNELS;
        int32_t a = ((int32_t)src[index] + src[index + 1U]) / 2;
        int32_t b = ((int32_t)src[index + AUDIO_SPEAKER_CHANNELS] +
                     src[index + AUDIO_SPEAKER_CHANNELS + 1U]) / 2;
        int32_t delta = (b - a) * (int32_t)frac;

        if (delta >= 0) {
            delta += AUDIO_SPEAKER_FRAME_SAMPLES / 2;
        } else {
            delta -= AUDIO_SPEAKER_FRAME_SAMPLES / 2;
        }
        dst[out] = (int16_t)(a + delta / AUDIO_SPEAKER_FRAME_SAMPLES);
    }
}

static bool take_mic_opus(uint8_t *data)
{
    bool valid = false;
    uintptr_t flags;

    if (!data) {
        return false;
    }

    flags = usb_lock();
    while (g_m61_mic_opus_storage.count > 0U) {
        m61_mic_opus_slot_t *slot =
            &g_m61_mic_opus_storage.slots[g_m61_mic_opus_storage.tail];

        g_m61_mic_opus_storage.tail = (uint8_t)(
            (g_m61_mic_opus_storage.tail + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
        g_m61_mic_opus_storage.count--;
        if (slot->control.state != DS5_SCHED_SLOT_READY ||
            slot->meta.generation != usb_generation ||
            slot->meta.length != M61_DS5_MIC_OPUS_LEN) {
            slot->control.state = DS5_SCHED_SLOT_FREE;
            usb_diag.audio_mic_opus_dropped++;
            continue;
        }
        memcpy(data, slot->payload, M61_DS5_MIC_OPUS_LEN);
        slot->control.state = DS5_SCHED_SLOT_FREE;
        valid = true;
        break;
    }
    usb_unlock(flags);
    return valid;
}

static void push_mic_pcm_stereo(const int16_t *mono, uint16_t samples)
{
    uintptr_t flags;

    if (!mono || samples == 0) {
        return;
    }

    flags = usb_lock();
    for (uint16_t i = 0; i < samples; i++) {
        uint8_t bytes[4];
        uint16_t sample = (uint16_t)mono[i];

        if (mono[i] != 0) {
            usb_diag.audio_mic_pcm_nonzero_samples++;
        }
        bytes[0] = (uint8_t)(sample & 0xFF);
        bytes[1] = (uint8_t)((sample >> 8) & 0xFF);
        bytes[2] = bytes[0];
        bytes[3] = bytes[1];

        for (uint8_t j = 0; j < sizeof(bytes); j++) {
            if (mic_pcm_ring_count >= AUDIO_MIC_PCM_RING_SIZE) {
                mic_pcm_ring_tail = (uint16_t)((mic_pcm_ring_tail + 1U) % AUDIO_MIC_PCM_RING_SIZE);
                mic_pcm_ring_count--;
            }
            mic_pcm_ring[mic_pcm_ring_head] = bytes[j];
            mic_pcm_ring_head = (uint16_t)((mic_pcm_ring_head + 1U) % AUDIO_MIC_PCM_RING_SIZE);
            mic_pcm_ring_count++;
            usb_diag.audio_mic_pcm_bytes++;
        }
    }
    usb_unlock(flags);
}

static void fill_audio_in_buffer(void)
{
    uintptr_t flags;
    uint16_t filled = 0;
    uint16_t nonzero = 0;

    memset(audio_in_buffer, 0, AUDIO_IN_STREAM_PACKET_SIZE);
    if (!m61_ds5_bridge_config_mic_enabled() || audio_mic_mute || !audio_in_open) {
        return;
    }

    flags = usb_lock();
    for (uint16_t i = 0; i < AUDIO_IN_STREAM_PACKET_SIZE && mic_pcm_ring_count > 0; i++) {
        audio_in_buffer[i] = mic_pcm_ring[mic_pcm_ring_tail];
        if (audio_in_buffer[i] != 0) {
            nonzero++;
        }
        mic_pcm_ring_tail = (uint16_t)((mic_pcm_ring_tail + 1U) % AUDIO_MIC_PCM_RING_SIZE);
        mic_pcm_ring_count--;
        filled++;
    }
    if (nonzero > 0) {
        usb_diag.audio_mic_usb_nonzero_packets++;
        usb_diag.audio_mic_usb_nonzero_bytes += nonzero;
    }
    if (filled < AUDIO_IN_STREAM_PACKET_SIZE) {
        usb_diag.audio_mic_underflow++;
    }
    usb_unlock(flags);
}

static bool take_audio_ingress(usb_audio_ingress_t *slot)
{
    uintptr_t flags;

    if (slot == NULL) {
        return false;
    }
    flags = usb_lock();
    if (usb_audio_ingress_count == 0U) {
        usb_unlock(flags);
        return false;
    }
    *slot = usb_audio_ingress[usb_audio_ingress_tail];
    usb_audio_ingress_tail =
        (uint8_t)((usb_audio_ingress_tail + 1U) % USB_AUDIO_INGRESS_DEPTH);
    usb_audio_ingress_count--;
    usb_unlock(flags);
    return true;
}

static void usb_audio_ingress_task(void *arg)
{
    usb_audio_ingress_t slot;

    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (take_audio_ingress(&slot)) {
            if (slot.generation != usb_generation) {
                usb_diag.usb_audio_ingress_stale++;
                continue;
            }
            m61_audio_epoch_ingest_usb(
                slot.data,
                slot.len,
                slot.generation,
                slot.captured_us,
                audio_out_open && !audio_speaker_mute &&
                    m61_ds5_bridge_config_speaker_enabled(),
                m61_ds5_bridge_config_haptics_gain_q8());
        }
        arm_audio_in(0);
    }
}

static void ensure_usb_audio_ingress_task(void)
{
    if (usb_audio_ingress_task_handle != NULL) {
        return;
    }
    usb_audio_ingress_task_handle = xTaskCreateStatic(
        usb_audio_ingress_task,
        "ds5_usb_audio",
        USB_AUDIO_INGRESS_TASK_STACK_WORDS,
        NULL,
        USB_AUDIO_INGRESS_TASK_PRIORITY,
        usb_audio_ingress_task_stack,
        &usb_audio_ingress_task_tcb);
    if (usb_audio_ingress_task_handle == NULL) {
        printf("M61 USB audio ingress task create failed\r\n");
    }
}

static void update_audio_codec_runtime_diag(void)
{
    uint32_t heap_free = (uint32_t)kfree_size(0);

    usb_diag.audio_codec_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    usb_diag.audio_codec_heap_free = heap_free;
    if (heap_free && (usb_diag.audio_codec_heap_min == 0 ||
                      heap_free < usb_diag.audio_codec_heap_min)) {
        usb_diag.audio_codec_heap_min = heap_free;
    }
}

static void audio_codec_task(void *pvParameters)
{
    OpusEncoder *encoder = NULL;
    OpusDecoder *decoder = NULL;
    static m61_audio_epoch_encode_job_t speaker_job;
    static int16_t speaker_frame[AUDIO_SPEAKER_FRAME_SAMPLES *
                                 AUDIO_SPEAKER_OPUS_CHANNELS];
    static uint8_t speaker_opus[M61_DS5_SPEAKER_OPUS_LEN];
    static uint8_t mic_opus[M61_DS5_MIC_OPUS_LEN];
    static int16_t mic_pcm[AUDIO_MIC_FRAME_SAMPLES * AUDIO_MIC_CHANNELS];
    TickType_t next_diag_tick;
    TickType_t pause_mic_until_tick = 0;
    uint8_t speaker_catchup_loops = 0;

    (void)pvParameters;

    usb_diag.audio_codec_stage = 1;
    update_audio_codec_runtime_diag();
    next_diag_tick = xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_CODEC_DIAG_PERIOD_MS);

    int encoder_size = opus_encoder_get_size(AUDIO_SPEAKER_OPUS_CHANNELS);
    usb_diag.audio_codec_encoder_size = (uint32_t)encoder_size;
    if (encoder_size <= 0 || (uint32_t)encoder_size > AUDIO_OPUS_ENCODER_STATE_MAX) {
        usb_diag.audio_codec_encoder_error = AUDIO_CODEC_ERR_STATE_TOO_SMALL;
        printf("M61 Opus encoder state too large: need=%d max=%u\r\n",
               encoder_size,
               (unsigned int)AUDIO_OPUS_ENCODER_STATE_MAX);
    } else {
        int opus_error;

        encoder = (OpusEncoder *)audio_opus_encoder_state;
        opus_error = opus_encoder_init(encoder,
                                       AUDIO_SAMPLE_RATE,
                                       AUDIO_SPEAKER_OPUS_CHANNELS,
                                       OPUS_APPLICATION_AUDIO);
        usb_diag.audio_codec_encoder_error = opus_error;
        if (opus_error != OPUS_OK) {
            printf("M61 Opus encoder init failed: %d\r\n", opus_error);
            encoder = NULL;
        } else {
            opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(AUDIO_SPEAKER_OPUS_BITRATE));
            opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
            opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1));
            opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(AUDIO_SPEAKER_OPUS_BANDWIDTH));
            usb_diag.audio_speaker_opus_force_mono = 1;
            usb_diag.audio_speaker_opus_bandwidth = AUDIO_SPEAKER_OPUS_BANDWIDTH;
            usb_diag.audio_speaker_opus_bitrate = AUDIO_SPEAKER_OPUS_BITRATE;
            usb_diag.audio_codec_encoder_ready = 1;
        }
    }

    usb_diag.audio_codec_stage = 2;
    int decoder_size = opus_decoder_get_size(AUDIO_MIC_CHANNELS);
    usb_diag.audio_codec_decoder_size = (uint32_t)decoder_size;
    if (decoder_size <= 0 || (uint32_t)decoder_size > AUDIO_OPUS_DECODER_STATE_MAX) {
        usb_diag.audio_codec_decoder_error = AUDIO_CODEC_ERR_STATE_TOO_SMALL;
        printf("M61 Opus decoder state too large: need=%d max=%u\r\n",
               decoder_size,
               (unsigned int)AUDIO_OPUS_DECODER_STATE_MAX);
    } else {
        int opus_error;

        decoder = (OpusDecoder *)audio_opus_decoder_state;
        opus_error = opus_decoder_init(decoder, AUDIO_SAMPLE_RATE, AUDIO_MIC_CHANNELS);
        usb_diag.audio_codec_decoder_error = opus_error;
        if (opus_error != OPUS_OK) {
            printf("M61 Opus decoder init failed: %d\r\n", opus_error);
            decoder = NULL;
        } else {
            usb_diag.audio_codec_decoder_ready = 1;
        }
    }

    while (1) {
        bool did_work = false;
        bool speaker_backlog;
        m61_audio_epoch_stats_t epoch_stats;
        uint8_t speaker_budget = audio_out_open ? 2U : 1U;
        TickType_t now;

        while (speaker_budget > 0 && encoder &&
               m61_audio_epoch_take_encode_job(&speaker_job)) {
            uint64_t encode_start_us;
            uint64_t encode_elapsed_us;
            usb_diag.audio_codec_stage = 3;
            resample_speaker_512_to_480(speaker_frame, speaker_job.speaker_pcm);
            encode_start_us = bflb_mtimer_get_time_us();
            int encoded = opus_encode(encoder,
                                      speaker_frame,
                                      AUDIO_SPEAKER_FRAME_SAMPLES,
                                      speaker_opus,
                                      sizeof(speaker_opus));
            if (encoded > 0 && encoded < M61_DS5_SPEAKER_OPUS_LEN) {
                int pad_error = opus_packet_pad(speaker_opus,
                                                encoded,
                                                M61_DS5_SPEAKER_OPUS_LEN);
                encoded = pad_error == OPUS_OK ? M61_DS5_SPEAKER_OPUS_LEN
                                               : pad_error;
            }
            encode_elapsed_us = bflb_mtimer_get_time_us() - encode_start_us;
            if (encode_elapsed_us > UINT32_MAX) {
                encode_elapsed_us = UINT32_MAX;
            }
            record_speaker_encode_diag((uint32_t)encode_elapsed_us, encoded);
            did_work = true;
            pause_mic_until_tick =
                xTaskGetTickCount() + pdMS_TO_TICKS(AUDIO_MIC_PAUSE_AFTER_SPEAKER_MS);
            if (encoded > 0) {
                if (m61_audio_epoch_complete_encode(speaker_job.generation,
                                                    speaker_job.epoch,
                                                    speaker_opus,
                                                    (size_t)encoded)) {
                    uintptr_t flags = usb_lock();
                    usb_diag.audio_speaker_encoded++;
                    usb_unlock(flags);
                }
            } else {
                (void)m61_audio_epoch_complete_encode(speaker_job.generation,
                                                      speaker_job.epoch,
                                                      NULL,
                                                      0);
                uintptr_t flags = usb_lock();
                usb_diag.audio_speaker_encode_errors++;
                usb_unlock(flags);
            }
            speaker_budget--;
        }

        now = xTaskGetTickCount();
        m61_audio_epoch_get_stats(&epoch_stats);
        bool speaker_recent =
            audio_out_open &&
            (epoch_stats.encode_ready_slots > 0 ||
             epoch_stats.encoding_slots > 0 ||
             epoch_stats.complete_slots > 0 ||
             (int32_t)(now - pause_mic_until_tick) < 0);
        if (decoder &&
            m61_ds5_bridge_config_mic_enabled() &&
            !speaker_recent &&
            take_mic_opus(mic_opus)) {
            usb_diag.audio_codec_stage = 4;
            int decoded = opus_decode(decoder,
                                      mic_opus,
                                      M61_DS5_MIC_OPUS_LEN,
                                      mic_pcm,
                                      AUDIO_MIC_FRAME_SAMPLES,
                                      0);
            did_work = true;
            if (decoded > 0) {
                uintptr_t flags = usb_lock();
                usb_diag.audio_mic_decoded++;
                usb_unlock(flags);
                push_mic_pcm_stereo(mic_pcm, (uint16_t)decoded);
            } else {
                uintptr_t flags = usb_lock();
                usb_diag.audio_mic_decode_errors++;
                usb_unlock(flags);
            }
        }

        usb_diag.audio_codec_stage = 5;
        now = xTaskGetTickCount();
        if ((int32_t)(now - next_diag_tick) >= 0) {
            update_audio_codec_runtime_diag();
            next_diag_tick = now + pdMS_TO_TICKS(AUDIO_CODEC_DIAG_PERIOD_MS);
        }

        m61_audio_epoch_get_stats(&epoch_stats);
        speaker_backlog = epoch_stats.encode_ready_slots > 0;
        if (!did_work || !speaker_backlog || speaker_catchup_loops >= 4U) {
            speaker_catchup_loops = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            speaker_catchup_loops++;
        }
    }
}

static void ensure_audio_codec_task(void)
{
    if (audio_codec_task_started) {
        return;
    }

    audio_codec_task_handle = xTaskCreateStatic(audio_codec_task,
                                                "ds5_audio",
                                                AUDIO_CODEC_TASK_STACK_WORDS,
                                                NULL,
                                                AUDIO_CODEC_TASK_PRIORITY,
                                                audio_codec_task_stack,
                                                &audio_codec_task_tcb);
    if (audio_codec_task_handle != NULL) {
        audio_codec_task_started = true;
        usb_diag.audio_codec_started = 1;
    } else {
        printf("M61 audio codec task create failed\r\n");
    }
}

static void m61_usb_clock_recover(void)
{
    PERIPHERAL_CLOCK_USB_ENABLE();
#if defined(BL616)
    GLB_Set_USB_CLK_From_WIFIPLL(1);
#endif
}

static bool configured_edge_identity(void)
{
    const m61_ds5_bridge_config_body_t *config =
        m61_ds5_bridge_config_get();

    if (config->controller_mode == M61_DS5_CONTROLLER_MODE_DSE) {
        return true;
    }
    if (config->controller_mode == M61_DS5_CONTROLLER_MODE_DS5) {
        return false;
    }
    return m61_ds5_dse_is_edge();
}

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    uint16_t bcd_usb = descriptor_wake_enabled ? USB_2_1 : USB_2_0;
    uint16_t product_id = descriptor_edge_enabled ?
                          USBD_PID_DSE : USBD_PID_DS5;

    usb_diag.device_desc++;
    usb_diag.last_speed = speed;
    memcpy(device_descriptor_runtime, device_descriptor,
           sizeof(device_descriptor_runtime));
    device_descriptor_runtime[2] = (uint8_t)(bcd_usb & 0xFFU);
    device_descriptor_runtime[3] = (uint8_t)(bcd_usb >> 8);
    device_descriptor_runtime[10] = (uint8_t)(product_id & 0xFFU);
    device_descriptor_runtime[11] = (uint8_t)(product_id >> 8);
    if (!descriptor_usb_serial_enabled) {
        device_descriptor_runtime[16] = 0;
    }
    return device_descriptor_runtime;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    uint16_t total_length = descriptor_keyboard_enabled ?
                            USB_DUALSENSE_CONFIG_DESC_SIZE :
                            USB_DUALSENSE_CONFIG_DESC_BASE_SIZE;
    uint16_t hid_report_length = descriptor_edge_enabled ?
                                 HID_DSE_REPORT_DESC_SIZE :
                                 HID_DS5_REPORT_DESC_SIZE;
    bool gamepad_hid_seen = false;

    usb_diag.config_desc++;
    usb_diag.last_speed = speed;
    memcpy(config_descriptor_runtime, config_descriptor,
           sizeof(config_descriptor_runtime));
    config_descriptor_runtime[2] = (uint8_t)(total_length & 0xFFU);
    config_descriptor_runtime[3] = (uint8_t)(total_length >> 8);
    config_descriptor_runtime[4] = descriptor_keyboard_enabled ?
                                   ITF_NUM_TOTAL : ITF_NUM_BASE;
    if (descriptor_wake_enabled) {
        config_descriptor_runtime[7] |= USB_CONFIG_REMOTE_WAKEUP;
    } else {
        config_descriptor_runtime[7] &= (uint8_t)~USB_CONFIG_REMOTE_WAKEUP;
    }

    for (size_t offset = 0; offset + 2U <= sizeof(config_descriptor_runtime);) {
        uint8_t desc_len = config_descriptor_runtime[offset];
        uint8_t desc_type = config_descriptor_runtime[offset + 1U];

        if (desc_len < 2U || offset + desc_len > sizeof(config_descriptor_runtime)) {
            break;
        }
        if (desc_type == USB_DESCRIPTOR_TYPE_ENDPOINT && desc_len >= 7U) {
            uint8_t address = config_descriptor_runtime[offset + 2U];

            if (address == HID_IN_EP || address == HID_OUT_EP) {
                config_descriptor_runtime[offset + 6U] =
                    descriptor_polling_interval;
            }
        } else if (desc_type == HID_DESCRIPTOR_TYPE_HID &&
                   desc_len >= 9U && !gamepad_hid_seen) {
            config_descriptor_runtime[offset + 7U] =
                (uint8_t)(hid_report_length & 0xFFU);
            config_descriptor_runtime[offset + 8U] =
                (uint8_t)(hid_report_length >> 8);
            gamepad_hid_seen = true;
        }
        offset += desc_len;
    }
    return config_descriptor_runtime;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    usb_diag.qualifier_desc++;
    usb_diag.last_speed = speed;
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    usb_diag.string_desc++;
    if (index == 0) {
        usb_diag.string0_desc++;
    }
    usb_diag.last_speed = speed;
    usb_diag.last_string_index = index;
    if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
        return NULL;
    }
    if (index == 2U && descriptor_edge_enabled) {
        return dualsense_edge_product_string;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor dualsense_descriptor_base = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static const struct usb_descriptor dualsense_descriptor_wake = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .msosv2_descriptor = &wake_ms_os_20,
    .bos_descriptor = &wake_bos,
};

static void queue_feature_request(uint8_t report_id, uint32_t requested_len)
{
    uintptr_t flags = usb_lock();

    pending_feature_report_id = report_id;
    pending_feature_report_len = requested_len;
    pending_feature_request_valid = true;
    usb_unlock(flags);
}

static bool is_bridge_config_report(uint8_t report_id)
{
    return report_id == 0xF6 || report_id == 0xF7 ||
           report_id == 0xF8 || report_id == 0xF9;
}

static bool take_usb_control_ingress(usb_control_ingress_t *item)
{
    uintptr_t flags;

    if (item == NULL) {
        return false;
    }
    flags = usb_lock();
    if (usb_control_ingress_count == 0U) {
        usb_unlock(flags);
        return false;
    }
    *item = usb_control_ingress[usb_control_ingress_tail];
    usb_control_ingress_tail =
        (uint8_t)((usb_control_ingress_tail + 1U) % USB_CONTROL_INGRESS_DEPTH);
    usb_control_ingress_count--;
    usb_unlock(flags);
    return true;
}

static void usb_control_worker_task(void *arg)
{
    usb_control_ingress_t item;

    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (take_usb_control_ingress(&item)) {
            if (item.generation != usb_generation) {
                continue;
            }
            switch ((usb_control_op_t)item.op) {
                case USB_CONTROL_CONFIG_SET: {
                    uint8_t state[DS5_USB_SET_STATE_LEN];

                    m61_ds5_bridge_config_set_raw(item.data, item.len);
                    m61_ds5_bridge_config_make_controller_state(state,
                                                                sizeof(state));
                    queue_host_report_generation(0x02, HID_REPORT_OUTPUT,
                                                 state, sizeof(state),
                                                 item.generation);
                    printf("[Config] bridge config updated from HID report\r\n");
                    break;
                }
                case USB_CONTROL_CONFIG_SAVE:
                    (void)m61_ds5_bridge_config_save();
                    break;
                case USB_CONTROL_RECONNECT:
                    vTaskDelay(pdMS_TO_TICKS(USB_RECONNECT_DELAY_MS));
                    if (item.generation == usb_generation) {
                        (void)m61_usb_gamepad_reinit();
                    }
                    break;
                default:
                    break;
            }
        }
    }
}

static void ensure_usb_control_worker_task(void)
{
    if (usb_reconnect_task_handle != NULL) {
        return;
    }
    usb_reconnect_task_handle = xTaskCreateStatic(
        usb_control_worker_task,
        "ds5_usb_ctl",
        USB_RECONNECT_TASK_STACK_WORDS,
        NULL,
        USB_RECONNECT_TASK_PRIORITY,
        usb_reconnect_task_stack,
        &usb_reconnect_task_tcb);
    if (usb_reconnect_task_handle == NULL) {
        printf("M61 USB control worker task create failed\r\n");
    }
}

static bool queue_usb_control_from_isr(usb_control_op_t op,
                                       const uint8_t *data,
                                       uint32_t len)
{
    usb_control_ingress_t *item;

    if (len > M61_DS5_USB_FEATURE_MAX_LEN) {
        len = M61_DS5_USB_FEATURE_MAX_LEN;
    }
    if (usb_control_ingress_count >= USB_CONTROL_INGRESS_DEPTH) {
        usb_diag.usb_control_ingress_dropped++;
        return false;
    }
    item = &usb_control_ingress[usb_control_ingress_head];
    item->generation = usb_generation;
    item->op = (uint8_t)op;
    item->len = (uint8_t)len;
    if (len > 0U && data != NULL) {
        memcpy(item->data, data, len);
    }
    usb_control_ingress_head =
        (uint8_t)((usb_control_ingress_head + 1U) % USB_CONTROL_INGRESS_DEPTH);
    usb_control_ingress_count++;
    usb_notify_from_isr(usb_reconnect_task_handle);
    return true;
}

static int8_t bridge_config_rssi(void)
{
#if CONFIG_M61_DS5_DUAL_CHIP_TRANSPORT
    m61_esp32_transport_stats_t stats;

    m61_esp32_transport_get_stats(&stats);
    if (stats.rx_bt_state > 0U) {
        return stats.peer_bt_rssi;
    }
#endif
    return 0;
}

static bool get_bridge_config_report(uint8_t report_id, uint32_t requested_len, uint32_t *out_len)
{
    uint32_t len = requested_len;

    if (out_len == NULL) {
        return false;
    }
    if (!is_bridge_config_report(report_id)) {
        return false;
    }
    if (len > sizeof(usb_control_buffer)) {
        len = sizeof(usb_control_buffer);
    }

    switch (report_id) {
        case 0xF7: {
            uint32_t copy_len = sizeof(m61_ds5_bridge_config_body_t);
            if (copy_len > len) {
                copy_len = len;
            }
            memcpy(usb_control_buffer, m61_ds5_bridge_config_get(), copy_len);
            *out_len = copy_len;
            return true;
        }
        case 0xF8: {
            const char *version = CONFIG_M61_DS5_BRIDGE_VERSION_STRING;
            uint32_t copy_len = (uint32_t)strlen(version);
            if (copy_len > len) {
                copy_len = len;
            }
            memcpy(usb_control_buffer, version, copy_len);
            *out_len = copy_len;
            return true;
        }
        case 0xF9:
            if (len == 0U) {
                *out_len = 0;
                return true;
            }
            usb_control_buffer[0] = (uint8_t)bridge_config_rssi();
            if (len >= 2U) {
                uint8_t flags = 0x80;
                if (audio_in_open && !audio_mic_mute &&
                    m61_ds5_bridge_config_mic_enabled()) {
                    flags |= 0x01;
                }
                if (audio_out_open && !audio_speaker_mute &&
                    m61_ds5_bridge_config_speaker_enabled()) {
                    flags |= 0x02;
                }
                usb_control_buffer[1] = flags;
                *out_len = 2;
            } else {
                *out_len = 1;
            }
            return true;
        default:
            *out_len = 0;
            return true;
    }
}

static bool set_bridge_config_report(uint8_t report_id, const uint8_t *report, uint32_t report_len)
{
    const uint8_t *payload = report;
    uint32_t payload_len = report_len;

    if (!report || report_len == 0U) {
        return false;
    }
    if (report_id == 0U) {
        report_id = report[0];
        payload = report + 1;
        payload_len = report_len - 1U;
    }
    if (!is_bridge_config_report(report_id)) {
        return false;
    }
    if (report_id != 0xF6 || payload_len == 0U) {
        return true;
    }

    switch (payload[0]) {
        case 0x01:
            (void)queue_usb_control_from_isr(USB_CONTROL_CONFIG_SET,
                                             payload + 1,
                                             payload_len - 1U);
            break;
        case 0x02:
            (void)queue_usb_control_from_isr(USB_CONTROL_CONFIG_SAVE,
                                             NULL, 0);
            break;
        case 0x03:
            (void)queue_usb_control_from_isr(USB_CONTROL_RECONNECT,
                                             NULL, 0);
            break;
        default:
            break;
    }
    return true;
}

static void update_last_out_state_diag(uint8_t report_id, const uint8_t *data, uint32_t len)
{
    const uint8_t *payload = data;
    uint32_t payload_len = len;
    uint8_t id = report_id;

    if (!data || len == 0) {
        return;
    }
    if (id == 0) {
        id = data[0];
        payload = data + 1;
        payload_len = len - 1U;
    }
    if (id != DS5_USB_OUTPUT_REPORT_ID || payload_len < DS5_USB_SET_STATE_LEN) {
        return;
    }

    usb_diag.last_out_flags0 = payload[DS5_USB_STATE_FLAGS0];
    usb_diag.last_out_flags1 = payload[DS5_USB_STATE_FLAGS1];
    usb_diag.last_out_flags2 = payload[DS5_USB_STATE_FLAGS2];
    usb_diag.last_out_rumble_right = payload[DS5_USB_STATE_RUMBLE_RIGHT];
    usb_diag.last_out_rumble_left = payload[DS5_USB_STATE_RUMBLE_LEFT];
    usb_diag.last_out_audio_control = payload[DS5_USB_STATE_AUDIO_CONTROL];
    usb_diag.last_out_mute_light = payload[DS5_USB_STATE_MUTE_LIGHT];
    usb_diag.last_out_audio_mute = payload[DS5_USB_STATE_AUDIO_MUTE];
    usb_diag.last_out_player_lights = payload[DS5_USB_STATE_PLAYER_LIGHTS];
    usb_diag.last_out_light_fade = payload[DS5_USB_STATE_LIGHT_FADE];
    usb_diag.last_out_light_brightness = payload[DS5_USB_STATE_LIGHT_BRIGHTNESS];
    usb_diag.last_out_led_red = payload[DS5_USB_STATE_LED_RED];
    usb_diag.last_out_led_green = payload[DS5_USB_STATE_LED_RED + 1U];
    usb_diag.last_out_led_blue = payload[DS5_USB_STATE_LED_RED + 2U];
}

static feature_cache_entry_t *find_feature_cache(uint8_t report_id)
{
    for (size_t i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].valid && feature_cache[i].report_id == report_id) {
            return &feature_cache[i];
        }
    }
    return NULL;
}

static void reset_host_report_queues_locked(void)
{
    pending_output_report_valid = false;
    memset(&g_m61_usb_control_storage,
           0,
           sizeof(g_m61_usb_control_storage));
}

static void queue_host_report(uint8_t report_id, uint8_t report_type, const uint8_t *data, uint32_t len)
{
    queue_host_report_generation(report_id, report_type, data, len,
                                 usb_generation);
}

static void queue_host_report_generation(uint8_t report_id,
                                         uint8_t report_type,
                                         const uint8_t *data,
                                         uint32_t len,
                                         uint32_t generation)
{
    uintptr_t flags;

    if (!data || len == 0) {
        return;
    }
    if (len > M61_DS5_USB_OUTPUT_MAX_LEN) {
        len = M61_DS5_USB_OUTPUT_MAX_LEN;
    }

    flags = usb_lock();
    if (generation != usb_generation) {
        usb_unlock(flags);
        return;
    }
    if (report_type == HID_REPORT_OUTPUT) {
        if (pending_output_report_valid) {
            usb_diag.host_report_dropped++;
        }
        pending_output_report.report_id = report_id;
        pending_output_report.report_type = report_type;
        pending_output_report.len = len;
        memcpy(pending_output_report.data, data, len);
        pending_output_report_valid = true;
        usb_diag.last_out_report_id = report_id ? report_id : data[0];
        usb_diag.last_out_report_len = len;
        update_last_out_state_diag(report_id, data, len);
    } else if (g_m61_usb_control_storage.count <
               DS5_SCHED_M61_USB_CONTROL_CAPACITY) {
        m61_usb_control_slot_t *slot =
            &g_m61_usb_control_storage.slots[g_m61_usb_control_storage.head];

        slot->control.version++;
        slot->control.state = DS5_SCHED_SLOT_WRITING;
        ds5_sched_meta_init(&slot->meta,
                            bflb_mtimer_get_time_us(),
                            generation,
                            g_m61_usb_control_storage.sequence++,
                            (uint16_t)len,
                            DS5_SCHED_RELIABLE_CONTROL,
                            0);
        slot->report.report_id = report_id;
        slot->report.report_type = report_type;
        slot->report.len = len;
        memcpy(slot->report.data, data, len);
        slot->control.state = DS5_SCHED_SLOT_READY;
        g_m61_usb_control_storage.head = (uint8_t)(
            (g_m61_usb_control_storage.head + 1U) %
            DS5_SCHED_M61_USB_CONTROL_CAPACITY);
        g_m61_usb_control_storage.count++;
    } else {
        usb_diag.host_report_dropped++;
    }
    usb_unlock(flags);
}

static void arm_hid_out(uint8_t busid)
{
    if (!usb_ready || !usb_configured || usb_out_armed) {
        return;
    }

    if (usbd_ep_start_read(busid, HID_OUT_EP, usb_out_buffer, sizeof(usb_out_buffer)) == 0) {
        usb_out_armed = true;
    }
}

static void arm_audio_out(uint8_t busid)
{
    if (!usb_ready || !usb_configured || !audio_out_open) {
        return;
    }

    usbd_ep_start_read(busid, AUDIO_OUT_EP, audio_out_buffer, sizeof(audio_out_buffer));
}

static void arm_audio_in(uint8_t busid)
{
    if (!usb_ready || !usb_configured || !audio_in_open || audio_in_busy) {
        return;
    }

    fill_audio_in_buffer();
    if (usbd_ep_start_write(busid, AUDIO_IN_EP, audio_in_buffer, AUDIO_IN_STREAM_PACKET_SIZE) == 0) {
        audio_in_busy = true;
    }
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    usb_last_event = event;
    if (event < (sizeof(usb_event_counts) / sizeof(usb_event_counts[0]))) {
        usb_event_counts[event]++;
    }

    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED: {
            bool preserve_wake_request =
                event == USBD_EVENT_RESET &&
                usb_wake_state == USB_WAKE_REQUESTED;

            usb_ready = false;
            usb_generation++;
            usb_configured = false;
            usb_busy = false;
            usb_input_pending = false;
            pending_feature_request_valid = false;
            reset_host_report_queues_locked();
            keyboard_busy = false;
            usb_suspended = false;
            usb_out_armed = false;
            audio_out_open = false;
            audio_in_open = false;
            audio_in_busy = false;
            usb_audio_ingress_head = 0;
            usb_audio_ingress_tail = 0;
            usb_audio_ingress_count = 0;
            usb_control_ingress_head = 0;
            usb_control_ingress_tail = 0;
            usb_control_ingress_count = 0;
            m61_audio_epoch_reset(usb_generation);
            flush_mic_queues();
            if (preserve_wake_request) {
                m61_ps_shortcut_reset(&ps_shortcut);
                shortcut_key_release_pending = false;
            } else {
                reset_keyboard_runtime(false);
            }
            break;
        }
        case USBD_EVENT_CONFIGURED:
            usb_ready = true;
            usb_configured = true;
            usb_busy = false;
            keyboard_busy = false;
            usb_suspended = false;
            if (usb_wake_state == USB_WAKE_REQUESTED) {
                usb_wake_resume_seen = true;
            }
            usb_out_armed = false;
            arm_hid_out(0);
            arm_audio_out(busid);
            usb_notify_from_isr(usb_audio_ingress_task_handle);
            usb_notify_from_isr(usb_input_pump_task_handle);
            break;
        case USBD_EVENT_RESUME:
            usb_suspended = false;
            if (usb_wake_state == USB_WAKE_REQUESTED) {
                usb_wake_resume_seen = true;
            }
            arm_hid_out(0);
            arm_audio_out(busid);
            usb_notify_from_isr(usb_audio_ingress_task_handle);
            usb_notify_from_isr(usb_input_pump_task_handle);
            break;
        case USBD_EVENT_SUSPEND:
            usb_suspended = true;
            usb_out_armed = false;
            audio_in_busy = false;
            if (usb_wake_state == USB_WAKE_KEY_DOWN) {
                shortcut_key_release_pending = true;
                shortcut_key_release_time_ms = usb_now_ms();
            }
            usb_wake_state = USB_WAKE_IDLE;
            usb_wake_resume_seen = false;
            usb_wake_key_attempts = 0;
            break;
        default:
            break;
    }
}

static void usbd_hid_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    usb_busy = false;
    usb_notify_from_isr(usb_input_pump_task_handle);
}

static void usbd_hid_keyboard_in_callback(uint8_t busid, uint8_t ep,
                                          uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    keyboard_busy = false;
}

static void usbd_hid_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    usb_out_armed = false;
    if (nbytes) {
        usb_diag.hid_out_report++;
        queue_host_report(0, HID_REPORT_OUTPUT, usb_out_buffer, nbytes);
    }
    arm_hid_out(busid);
}

static void usbd_audio_out_ep_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    usb_audio_ingress_t *slot;

    (void)ep;

    if (nbytes) {
        if (nbytes > AUDIO_OUT_PACKET_SIZE) {
            nbytes = AUDIO_OUT_PACKET_SIZE;
        }
        usb_diag.audio_out_packets++;
        usb_diag.audio_out_bytes += nbytes;
        if (usb_audio_ingress_count >= USB_AUDIO_INGRESS_DEPTH) {
            usb_diag.usb_audio_ingress_dropped++;
        } else {
            slot = &usb_audio_ingress[usb_audio_ingress_head];
            slot->generation = usb_generation;
            slot->captured_us = bflb_mtimer_get_time_us();
            slot->len = (uint16_t)nbytes;
            memcpy(slot->data, audio_out_buffer, nbytes);
            usb_audio_ingress_head =
                (uint8_t)((usb_audio_ingress_head + 1U) % USB_AUDIO_INGRESS_DEPTH);
            usb_audio_ingress_count++;
            usb_notify_from_isr(usb_audio_ingress_task_handle);
        }
    }
    arm_audio_out(busid);
}

static void usbd_audio_in_ep_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    audio_in_busy = false;
    usb_diag.audio_in_packets++;
    usb_diag.audio_in_bytes += nbytes;
    usb_notify_from_isr(usb_audio_ingress_task_handle);
}

static struct usbd_endpoint hid_in_ep = {
    .ep_cb = usbd_hid_in_callback,
    .ep_addr = HID_IN_EP,
};

static struct usbd_endpoint hid_out_ep = {
    .ep_cb = usbd_hid_out_callback,
    .ep_addr = HID_OUT_EP,
};

static struct usbd_endpoint hid_keyboard_in_ep = {
    .ep_cb = usbd_hid_keyboard_in_callback,
    .ep_addr = HID_KBD_IN_EP,
};

static struct usbd_endpoint audio_out_ep = {
    .ep_cb = usbd_audio_out_ep_callback,
    .ep_addr = AUDIO_OUT_EP,
};

static struct usbd_endpoint audio_in_ep = {
    .ep_cb = usbd_audio_in_ep_callback,
    .ep_addr = AUDIO_IN_EP,
};

static struct audio_entity_info audio_entity_table[] = {
    {
        .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
        .bEntityId = AUDIO_SPEAKER_FU_ID,
        .ep = AUDIO_OUT_EP,
    },
    {
        .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
        .bEntityId = AUDIO_MIC_FU_ID,
        .ep = AUDIO_IN_EP,
    },
};

static struct usbd_interface audio_control_intf;
static struct usbd_interface audio_stream_out_intf;
static struct usbd_interface audio_stream_in_intf;
static struct usbd_interface hid_intf;
static struct usbd_interface hid_keyboard_intf;

static void set_button_bit(uint8_t *value, uint8_t bit, uint32_t buttons, dualsense_button_mask_t mask)
{
    if (buttons & (uint32_t)mask) {
        *value |= bit;
    }
}

static void make_payload_from_state(const dualsense_state_t *state, uint8_t *payload)
{
    memcpy(payload, ds5_idle_payload, M61_DS5_USB_INPUT_PAYLOAD_LEN);
    payload[0] = state->left_x;
    payload[1] = state->left_y;
    payload[2] = state->right_x;
    payload[3] = state->right_y;
    payload[4] = state->l2;
    payload[5] = state->r2;
    payload[7] = (state->dpad <= 8U) ? (state->dpad & 0x0F) : 0x08;
    payload[8] = 0;
    payload[9] = 0;

    set_button_bit(&payload[7], 0x10, state->buttons, DS5_BUTTON_SQUARE);
    set_button_bit(&payload[7], 0x20, state->buttons, DS5_BUTTON_CROSS);
    set_button_bit(&payload[7], 0x40, state->buttons, DS5_BUTTON_CIRCLE);
    set_button_bit(&payload[7], 0x80, state->buttons, DS5_BUTTON_TRIANGLE);
    set_button_bit(&payload[8], 0x01, state->buttons, DS5_BUTTON_L1);
    set_button_bit(&payload[8], 0x02, state->buttons, DS5_BUTTON_R1);
    set_button_bit(&payload[8], 0x04, state->buttons, DS5_BUTTON_L2);
    set_button_bit(&payload[8], 0x08, state->buttons, DS5_BUTTON_R2);
    set_button_bit(&payload[8], 0x10, state->buttons, DS5_BUTTON_CREATE);
    set_button_bit(&payload[8], 0x20, state->buttons, DS5_BUTTON_OPTIONS);
    set_button_bit(&payload[8], 0x40, state->buttons, DS5_BUTTON_L3);
    set_button_bit(&payload[8], 0x80, state->buttons, DS5_BUTTON_R3);
    set_button_bit(&payload[9], 0x01, state->buttons, DS5_BUTTON_PS);
    set_button_bit(&payload[9], 0x02, state->buttons, DS5_BUTTON_TOUCHPAD);
    set_button_bit(&payload[9], 0x04, state->buttons, DS5_BUTTON_MUTE);
    set_button_bit(&payload[9], 0x10, state->buttons, DS5_BUTTON_EDGE_FN_L);
    set_button_bit(&payload[9], 0x20, state->buttons, DS5_BUTTON_EDGE_FN_R);
    set_button_bit(&payload[9], 0x40, state->buttons, DS5_BUTTON_EDGE_PADDLE_L);
    set_button_bit(&payload[9], 0x80, state->buttons, DS5_BUTTON_EDGE_PADDLE_R);
}

static void register_usb_dualsense_device(void)
{
    const uint8_t *gamepad_report_desc;
    size_t gamepad_report_desc_len;

    descriptor_wake_enabled = m61_ds5_bridge_config_wake_enabled();
    descriptor_keyboard_enabled = descriptor_wake_enabled ||
                                  m61_ds5_bridge_config_ps_shortcut_enabled();
    descriptor_usb_serial_enabled =
        m61_ds5_bridge_config_usb_serial_enabled();
    descriptor_edge_enabled = configured_edge_identity();
    descriptor_polling_interval =
        m61_ds5_bridge_config_polling_interval();
    gamepad_report_desc = descriptor_edge_enabled ?
                          dualsense_dse_report_desc :
                          dualsense_ds5_report_desc;
    gamepad_report_desc_len = descriptor_edge_enabled ?
                               sizeof(dualsense_dse_report_desc) :
                               sizeof(dualsense_ds5_report_desc);
    reset_keyboard_runtime(false);

    usbd_desc_register(0, descriptor_wake_enabled ?
                          &dualsense_descriptor_wake :
                          &dualsense_descriptor_base);

    usbd_add_interface(0, usbd_audio_init_intf(0,
                                               &audio_control_intf,
                                               0x0100,
                                               audio_entity_table,
                                               sizeof(audio_entity_table) / sizeof(audio_entity_table[0])));
    usbd_add_interface(0, usbd_audio_init_intf(0,
                                               &audio_stream_out_intf,
                                               0x0100,
                                               audio_entity_table,
                                               sizeof(audio_entity_table) / sizeof(audio_entity_table[0])));
    usbd_add_interface(0, usbd_audio_init_intf(0,
                                               &audio_stream_in_intf,
                                               0x0100,
                                               audio_entity_table,
                                               sizeof(audio_entity_table) / sizeof(audio_entity_table[0])));
    usbd_add_interface(0, usbd_hid_init_intf(0,
                                             &hid_intf,
                                             gamepad_report_desc,
                                             gamepad_report_desc_len));
    if (descriptor_keyboard_enabled) {
        usbd_add_interface(0, usbd_hid_init_intf(0,
                                                 &hid_keyboard_intf,
                                                 keyboard_report_desc,
                                                 sizeof(keyboard_report_desc)));
    }

    usbd_add_endpoint(0, &audio_out_ep);
    usbd_add_endpoint(0, &audio_in_ep);
    usbd_add_endpoint(0, &hid_in_ep);
    usbd_add_endpoint(0, &hid_out_ep);
    if (descriptor_keyboard_enabled) {
        usbd_add_endpoint(0, &hid_keyboard_in_ep);
    }
}

void m61_usb_gamepad_init(void)
{
    m61_usb_clock_recover();
    memcpy(last_input_payload, ds5_idle_payload, sizeof(last_input_payload));
    memset(audio_in_buffer, 0, sizeof(audio_in_buffer));
    m61_audio_epoch_init(usb_generation);
    flush_mic_queues();
    ensure_audio_codec_task();
    ensure_usb_audio_ingress_task();
    ensure_usb_input_pump_task();
    ensure_usb_control_worker_task();
    ensure_usb_keyboard_task();
    register_usb_dualsense_device();
    usb_init_result = usbd_initialize(0, 0, usbd_event_handler);
    usb_initialized = (usb_init_result == 0);
    printf("M61 USB %s composite init result=%d\r\n",
           descriptor_edge_enabled ? "DualSense Edge" : "DualSense",
           usb_init_result);
}

void m61_usb_gamepad_deinit(void)
{
    if (usb_initialized) {
        usb_ready = false;
        usb_configured = false;
        usb_busy = false;
        keyboard_busy = false;
        usb_suspended = false;
        usb_out_armed = false;
        usbd_deinitialize(0);
        usb_initialized = false;
    }
}

int m61_usb_gamepad_reinit(void)
{
    m61_usb_gamepad_deinit();
    m61_usb_clock_recover();
    ensure_usb_audio_ingress_task();
    ensure_usb_input_pump_task();
    ensure_usb_control_worker_task();
    ensure_usb_keyboard_task();
    register_usb_dualsense_device();
    usb_init_result = usbd_initialize(0, 0, usbd_event_handler);
    usb_initialized = (usb_init_result == 0);
    printf("M61 USB %s composite reinit result=%d\r\n",
           descriptor_edge_enabled ? "DualSense Edge" : "DualSense",
           usb_init_result);
    return usb_init_result;
}

void m61_usb_gamepad_send_report01(const uint8_t *payload, size_t len)
{
    size_t copy_len;
    uintptr_t flags;

    if (!payload || len == 0) {
        return;
    }

    copy_len = len;
    if (copy_len > M61_DS5_USB_INPUT_PAYLOAD_LEN) {
        copy_len = M61_DS5_USB_INPUT_PAYLOAD_LEN;
    }

    flags = usb_lock();
    memcpy(last_input_payload, payload, copy_len);
    if (copy_len < M61_DS5_USB_INPUT_PAYLOAD_LEN) {
        memset(last_input_payload + copy_len, 0, M61_DS5_USB_INPUT_PAYLOAD_LEN - copy_len);
    }
    if (usb_input_pending) {
        usb_diag.usb_input_replaced++;
        usb_drop_count++;
    }
    memcpy(usb_input_pending_payload, last_input_payload,
           M61_DS5_USB_INPUT_PAYLOAD_LEN);
    usb_input_pending_generation = usb_generation;
    usb_input_pending = true;
    usb_unlock(flags);
    if (usb_input_pump_task_handle != NULL) {
        xTaskNotifyGive(usb_input_pump_task_handle);
    }
}

void m61_usb_gamepad_send_state(const dualsense_state_t *state)
{
    uint8_t payload[M61_DS5_USB_INPUT_PAYLOAD_LEN];

    if (!state) {
        return;
    }

    make_payload_from_state(state, payload);
    m61_usb_gamepad_send_report01(payload, sizeof(payload));
}

void m61_usb_gamepad_note_controller_state(const dualsense_state_t *state)
{
    uintptr_t flags;

    if (state == NULL) {
        return;
    }
    flags = usb_lock();
    m61_ps_shortcut_note(&ps_shortcut,
                         (state->buttons & DS5_BUTTON_PS) != 0U,
                         usb_now_ms(),
                         descriptor_keyboard_enabled &&
                         m61_ds5_bridge_config_ps_shortcut_enabled());
    usb_unlock(flags);
}

void m61_usb_gamepad_reset_controller_state(void)
{
    bool release_key = shortcut_key_release_pending ||
                       usb_wake_state == USB_WAKE_KEY_DOWN;

    reset_keyboard_runtime(release_key);
}

void m61_usb_gamepad_store_feature_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    feature_cache_entry_t *entry;
    uintptr_t flags;

    if (!data || len == 0) {
        return;
    }
    if (len > M61_DS5_USB_FEATURE_MAX_LEN) {
        len = M61_DS5_USB_FEATURE_MAX_LEN;
    }

    flags = usb_lock();
    entry = find_feature_cache(report_id);
    if (!entry) {
        for (size_t i = 0; i < FEATURE_CACHE_SLOTS; i++) {
            if (!feature_cache[i].valid) {
                entry = &feature_cache[i];
                break;
            }
        }
    }
    if (!entry) {
        entry = &feature_cache[feature_cache_replace_index++ % FEATURE_CACHE_SLOTS];
    }

    entry->valid = true;
    entry->report_id = report_id;
    entry->len = (uint32_t)len;
    memcpy(entry->data, data, len);
    usb_diag.feature_cache_stores++;
    usb_unlock(flags);
}

void m61_usb_gamepad_reset_feature_cache(void)
{
    uintptr_t flags = usb_lock();

    memset(feature_cache, 0, sizeof(feature_cache));
    feature_cache_replace_index = 0;
    pending_feature_request_valid = false;
    pending_feature_report_id = 0;
    pending_feature_report_len = 0;
    usb_unlock(flags);
}

void m61_usb_gamepad_reset_transport_queues(void)
{
    uintptr_t flags = usb_lock();

    pending_feature_request_valid = false;
    reset_host_report_queues_locked();
    usb_generation++;
    usb_input_pending = false;
    usb_busy = false;
    usb_control_ingress_head = 0;
    usb_control_ingress_tail = 0;
    usb_control_ingress_count = 0;
    usb_audio_ingress_head = 0;
    usb_audio_ingress_tail = 0;
    usb_audio_ingress_count = 0;
    usb_unlock(flags);
    m61_audio_epoch_reset(usb_generation);
    flush_mic_queues();
}

bool m61_usb_gamepad_take_feature_request(uint8_t *report_id, uint32_t *requested_len)
{
    bool valid;
    uintptr_t flags;

    if (!report_id || !requested_len) {
        return false;
    }

    flags = usb_lock();
    valid = pending_feature_request_valid;
    if (valid) {
        *report_id = pending_feature_report_id;
        *requested_len = pending_feature_report_len;
        pending_feature_request_valid = false;
    }
    usb_unlock(flags);
    return valid;
}

bool m61_usb_gamepad_take_host_report(m61_usb_gamepad_host_report_t *report)
{
    bool valid = false;
    uintptr_t flags;

    if (!report) {
        return false;
    }

    flags = usb_lock();
    while (g_m61_usb_control_storage.count > 0U) {
        m61_usb_control_slot_t *slot =
            &g_m61_usb_control_storage.slots[g_m61_usb_control_storage.tail];

        g_m61_usb_control_storage.tail = (uint8_t)(
            (g_m61_usb_control_storage.tail + 1U) %
            DS5_SCHED_M61_USB_CONTROL_CAPACITY);
        g_m61_usb_control_storage.count--;
        if (slot->control.state != DS5_SCHED_SLOT_READY ||
            slot->meta.generation != usb_generation ||
            slot->meta.length != slot->report.len) {
            slot->control.state = DS5_SCHED_SLOT_FREE;
            usb_diag.host_report_dropped++;
            continue;
        }
        *report = slot->report;
        slot->control.state = DS5_SCHED_SLOT_FREE;
        valid = true;
        break;
    }
    if (!valid && pending_output_report_valid) {
        *report = pending_output_report;
        pending_output_report_valid = false;
        valid = true;
    }
    usb_unlock(flags);
    return valid;
}

bool m61_usb_gamepad_take_audio_epoch_pair(m61_audio_epoch_pair_t *pair)
{
    return m61_audio_epoch_take_adjacent_pair(pair);
}

void m61_usb_gamepad_submit_mic_opus(const uint8_t *data, size_t len)
{
    uintptr_t flags;
    bool nonzero;

    if (!m61_ds5_bridge_config_mic_enabled() ||
        !data || len != M61_DS5_MIC_OPUS_LEN || !audio_in_open || audio_mic_mute) {
        return;
    }

    nonzero = bytes_have_nonzero(data, M61_DS5_MIC_OPUS_LEN);
    flags = usb_lock();
    if (g_m61_mic_opus_storage.count >= AUDIO_MIC_OPUS_QUEUE_DEPTH) {
        m61_mic_opus_slot_t *dropped =
            &g_m61_mic_opus_storage.slots[g_m61_mic_opus_storage.tail];
        dropped->control.state = DS5_SCHED_SLOT_EVICTED;
        g_m61_mic_opus_storage.tail = (uint8_t)(
            (g_m61_mic_opus_storage.tail + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
        g_m61_mic_opus_storage.count--;
        usb_diag.audio_mic_opus_dropped++;
    }
    m61_mic_opus_slot_t *slot =
        &g_m61_mic_opus_storage.slots[g_m61_mic_opus_storage.head];
    slot->control.version++;
    slot->control.state = DS5_SCHED_SLOT_WRITING;
    ds5_sched_meta_init(&slot->meta,
                        bflb_mtimer_get_time_us(),
                        usb_generation,
                        g_m61_mic_opus_storage.sequence++,
                        M61_DS5_MIC_OPUS_LEN,
                        DS5_SCHED_MIC_STREAM,
                        0);
    memcpy(slot->payload, data, M61_DS5_MIC_OPUS_LEN);
    slot->control.state = DS5_SCHED_SLOT_READY;
    g_m61_mic_opus_storage.head = (uint8_t)(
        (g_m61_mic_opus_storage.head + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
    g_m61_mic_opus_storage.count++;
    usb_diag.audio_mic_opus_packets++;
    if (nonzero) {
        usb_diag.audio_mic_opus_nonzero++;
    }
    usb_unlock(flags);
}

bool m61_usb_gamepad_audio_mic_enabled(void)
{
    return m61_ds5_bridge_config_mic_enabled();
}

bool m61_usb_gamepad_audio_in_active(void)
{
    if (!m61_ds5_bridge_config_mic_enabled()) {
        return false;
    }
    return audio_in_open && !audio_mic_mute;
}

bool m61_usb_gamepad_audio_speaker_active(void)
{
    return audio_out_open && !audio_speaker_mute &&
           m61_ds5_bridge_config_speaker_enabled();
}

bool m61_usb_gamepad_remote_wakeup(void)
{
    int ret;

    if (!descriptor_wake_enabled || !usb_suspended) {
        return false;
    }
    usb_wake_state = USB_WAKE_REQUESTED;
    usb_wake_state_entered_ms = usb_now_ms();
    usb_wake_resume_seen = false;
    usb_wake_key_attempts = 0;
    ret = usbd_send_remote_wakeup(0);
    if (ret != 0) {
        usb_wake_state = USB_WAKE_IDLE;
        return false;
    }
    return true;
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

int m61_usb_gamepad_init_result(void)
{
    return usb_init_result;
}

uint8_t m61_usb_gamepad_last_event(void)
{
    return usb_last_event;
}

uint32_t m61_usb_gamepad_event_count(uint8_t event)
{
    if (event >= (sizeof(usb_event_counts) / sizeof(usb_event_counts[0]))) {
        return 0;
    }
    return usb_event_counts[event];
}

void m61_usb_gamepad_get_diag(m61_usb_gamepad_diag_t *diag)
{
    m61_audio_epoch_stats_t epoch_stats;

    if (!diag) {
        return;
    }
    m61_audio_epoch_get_stats(&epoch_stats);

    diag->device_desc = usb_diag.device_desc;
    diag->config_desc = usb_diag.config_desc;
    diag->qualifier_desc = usb_diag.qualifier_desc;
    diag->string_desc = usb_diag.string_desc;
    diag->string0_desc = usb_diag.string0_desc;
    diag->hid_get_report = usb_diag.hid_get_report;
    diag->hid_get_idle = usb_diag.hid_get_idle;
    diag->hid_get_protocol = usb_diag.hid_get_protocol;
    diag->hid_set_report = usb_diag.hid_set_report;
    diag->hid_set_idle = usb_diag.hid_set_idle;
    diag->hid_set_protocol = usb_diag.hid_set_protocol;
    diag->hid_out_report = usb_diag.hid_out_report;
    diag->feature_cache_hits = usb_diag.feature_cache_hits;
    diag->feature_cache_misses = usb_diag.feature_cache_misses;
    diag->feature_cache_stores = usb_diag.feature_cache_stores;
    diag->host_report_dropped = usb_diag.host_report_dropped;
    diag->usb_input_replaced = usb_diag.usb_input_replaced;
    diag->usb_input_retries = usb_diag.usb_input_retries;
    diag->usb_control_ingress_dropped = usb_diag.usb_control_ingress_dropped;
    diag->usb_audio_ingress_dropped = usb_diag.usb_audio_ingress_dropped;
    diag->usb_audio_ingress_stale = usb_diag.usb_audio_ingress_stale;
    diag->audio_open = usb_diag.audio_open;
    diag->audio_close = usb_diag.audio_close;
    diag->audio_out_packets = usb_diag.audio_out_packets;
    diag->audio_out_bytes = usb_diag.audio_out_bytes;
    diag->audio_in_packets = usb_diag.audio_in_packets;
    diag->audio_in_bytes = usb_diag.audio_in_bytes;
    diag->audio_haptic_blocks = epoch_stats.epochs_completed;
    diag->audio_haptic_sample_pairs = epoch_stats.haptics_sample_pairs;
    diag->audio_haptic_nonzero_blocks = epoch_stats.haptics_nonzero_epochs;
    diag->audio_haptic_queue_dropped = epoch_stats.epochs_dropped;
    diag->audio_speaker_frames = epoch_stats.encode_jobs;
    diag->audio_speaker_encoded = usb_diag.audio_speaker_encoded;
    diag->audio_speaker_encode_errors = usb_diag.audio_speaker_encode_errors;
    diag->audio_speaker_queue_dropped = epoch_stats.epochs_dropped;
    diag->audio_speaker_opus_dropped = epoch_stats.encode_failures;
    diag->audio_speaker_encode_us_total = usb_diag.audio_speaker_encode_us_total;
    diag->audio_speaker_encode_us_last = usb_diag.audio_speaker_encode_us_last;
    diag->audio_speaker_encode_us_max = usb_diag.audio_speaker_encode_us_max;
    diag->audio_speaker_last_opus_len = usb_diag.audio_speaker_last_opus_len;
    diag->audio_epoch_generation = epoch_stats.generation;
    diag->audio_epoch_next = epoch_stats.next_epoch;
    diag->audio_epoch_generation_resets = epoch_stats.generation_resets;
    diag->audio_epoch_started = epoch_stats.epochs_started;
    diag->audio_epoch_completed = epoch_stats.epochs_completed;
    diag->audio_epoch_dropped = epoch_stats.epochs_dropped;
    diag->audio_epoch_stale = epoch_stats.epochs_stale;
    diag->audio_epoch_adjacent_pairs = epoch_stats.adjacent_pairs;
    diag->audio_epoch_gaps = epoch_stats.epoch_gaps;
    diag->audio_epoch_discontinuities = epoch_stats.epoch_discontinuities;
    diag->audio_epoch_speaker_state_discontinuities =
        epoch_stats.speaker_state_discontinuities;
    diag->audio_mic_opus_packets = usb_diag.audio_mic_opus_packets;
    diag->audio_mic_opus_nonzero = usb_diag.audio_mic_opus_nonzero;
    diag->audio_mic_opus_dropped = usb_diag.audio_mic_opus_dropped;
    diag->audio_mic_decoded = usb_diag.audio_mic_decoded;
    diag->audio_mic_decode_errors = usb_diag.audio_mic_decode_errors;
    diag->audio_mic_pcm_bytes = usb_diag.audio_mic_pcm_bytes;
    diag->audio_mic_pcm_nonzero_samples = usb_diag.audio_mic_pcm_nonzero_samples;
    diag->audio_mic_usb_nonzero_packets = usb_diag.audio_mic_usb_nonzero_packets;
    diag->audio_mic_usb_nonzero_bytes = usb_diag.audio_mic_usb_nonzero_bytes;
    diag->audio_mic_underflow = usb_diag.audio_mic_underflow;
    diag->audio_codec_stack_hwm = usb_diag.audio_codec_stack_hwm;
    diag->audio_codec_heap_free = usb_diag.audio_codec_heap_free;
    diag->audio_codec_heap_min = usb_diag.audio_codec_heap_min;
    diag->audio_codec_encoder_size = usb_diag.audio_codec_encoder_size;
    diag->audio_codec_decoder_size = usb_diag.audio_codec_decoder_size;
    diag->audio_codec_encoder_error = usb_diag.audio_codec_encoder_error;
    diag->audio_codec_decoder_error = usb_diag.audio_codec_decoder_error;
    diag->audio_set_volume = usb_diag.audio_set_volume;
    diag->audio_set_mute = usb_diag.audio_set_mute;
    diag->audio_set_freq = usb_diag.audio_set_freq;
    diag->last_speed = usb_diag.last_speed;
    diag->last_string_index = usb_diag.last_string_index;
    diag->last_report_id = usb_diag.last_report_id;
    diag->last_report_type = usb_diag.last_report_type;
    diag->last_out_report_id = usb_diag.last_out_report_id;
    diag->last_out_flags0 = usb_diag.last_out_flags0;
    diag->last_out_flags1 = usb_diag.last_out_flags1;
    diag->last_out_flags2 = usb_diag.last_out_flags2;
    diag->last_out_rumble_right = usb_diag.last_out_rumble_right;
    diag->last_out_rumble_left = usb_diag.last_out_rumble_left;
    diag->last_out_audio_control = usb_diag.last_out_audio_control;
    diag->last_out_mute_light = usb_diag.last_out_mute_light;
    diag->last_out_audio_mute = usb_diag.last_out_audio_mute;
    diag->last_out_player_lights = usb_diag.last_out_player_lights;
    diag->last_out_light_fade = usb_diag.last_out_light_fade;
    diag->last_out_light_brightness = usb_diag.last_out_light_brightness;
    diag->last_out_led_red = usb_diag.last_out_led_red;
    diag->last_out_led_green = usb_diag.last_out_led_green;
    diag->last_out_led_blue = usb_diag.last_out_led_blue;
    diag->audio_last_open_intf = usb_diag.audio_last_open_intf;
    diag->audio_last_close_intf = usb_diag.audio_last_close_intf;
    diag->audio_out_open = audio_out_open ? 1 : 0;
    diag->audio_in_open = audio_in_open ? 1 : 0;
    diag->audio_haptic_queue_depth = epoch_stats.complete_slots;
    diag->audio_speaker_queue_depth = epoch_stats.encode_ready_slots;
    diag->audio_speaker_opus_queue_depth = epoch_stats.complete_slots;
    diag->audio_mic_opus_queue_depth = g_m61_mic_opus_storage.count;
    diag->audio_mic_ring_bytes = mic_pcm_ring_count;
    diag->audio_codec_started = usb_diag.audio_codec_started;
    diag->audio_codec_stage = usb_diag.audio_codec_stage;
    diag->audio_codec_encoder_ready = usb_diag.audio_codec_encoder_ready;
    diag->audio_codec_decoder_ready = usb_diag.audio_codec_decoder_ready;
    diag->audio_speaker_opus_force_mono = usb_diag.audio_speaker_opus_force_mono;
    diag->audio_speaker_opus_bandwidth = usb_diag.audio_speaker_opus_bandwidth;
    diag->audio_speaker_opus_bitrate = usb_diag.audio_speaker_opus_bitrate;
    diag->audio_haptic_last_peak = epoch_stats.haptics_last_peak;
    diag->audio_haptic_downsample = HAPTICS_DOWNSAMPLE_FACTOR;
    diag->audio_haptic_resample_mode = HAPTICS_RESAMPLE_MODE_WDL_EQUIV;
    diag->audio_haptic_gain_q8 = m61_ds5_bridge_config_haptics_gain_q8();
    diag->audio_speaker_mute = audio_speaker_mute ? 1 : 0;
    diag->audio_mic_mute = audio_mic_mute ? 1 : 0;
    diag->audio_speaker_volume_db = audio_speaker_volume_db;
    diag->audio_mic_volume_db = audio_mic_volume_db;
    diag->last_out_report_len = usb_diag.last_out_report_len;
#ifdef M61_USB_BASE
    diag->reg_dev_ctl = getreg32(M61_USB_BASE + M61_USB_DEV_CTL_OFFSET);
    diag->reg_dev_adr = getreg32(M61_USB_BASE + M61_USB_DEV_ADR_OFFSET);
    diag->reg_phy_tst = getreg32(M61_USB_BASE + M61_USB_PHY_TST_OFFSET);
    diag->reg_otg_csr = getreg32(M61_USB_BASE + M61_USB_OTG_CSR_OFFSET);
    diag->reg_glb_isr = getreg32(M61_USB_BASE + M61_USB_GLB_ISR_OFFSET);
    diag->reg_dev_igr = getreg32(M61_USB_BASE + M61_USB_DEV_IGR_OFFSET);
    diag->reg_isg0 = getreg32(M61_USB_BASE + M61_USB_DEV_ISG0_OFFSET);
    diag->reg_isg2 = getreg32(M61_USB_BASE + M61_USB_DEV_ISG2_OFFSET);
    diag->reg_isg3 = getreg32(M61_USB_BASE + M61_USB_DEV_ISG3_OFFSET);
    diag->reg_vdma_ctrl = getreg32(M61_USB_BASE + M61_USB_VDMA_CTRL_OFFSET);
    diag->reg_vdma_cxfps1 = getreg32(M61_USB_BASE + M61_USB_VDMA_CXFPS1_OFFSET);
#endif
}

void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t **data, uint32_t *len)
{
    feature_cache_entry_t *entry;
    uint32_t requested_len = (len && *len) ? *len : M61_DS5_USB_FEATURE_MAX_LEN;
    uintptr_t flags;

    (void)busid;

    usb_diag.hid_get_report++;
    usb_diag.last_report_id = report_id;
    usb_diag.last_report_type = report_type;
    *data = usb_control_buffer;
    *len = 0;

    if (intf == ITF_NUM_HID_KBD) {
        if (requested_len >= HID_KBD_EP_SIZE) {
            memset(usb_control_buffer, 0, HID_KBD_EP_SIZE);
            *len = HID_KBD_EP_SIZE;
        }
        return;
    }

    if (report_type == HID_REPORT_INPUT && report_id == M61_DS5_USB_INPUT_REPORT_ID) {
        memcpy(usb_control_buffer, last_input_payload, sizeof(last_input_payload));
        *len = sizeof(last_input_payload);
        return;
    }

    if (report_type != HID_REPORT_FEATURE) {
        return;
    }

    if (get_bridge_config_report(report_id, requested_len, len)) {
        usb_diag.feature_cache_hits++;
        return;
    }

    if (m61_ds5_dse_should_nak_profile(report_id)) {
        usb_diag.feature_cache_misses++;
        queue_feature_request(report_id, requested_len);
        return;
    }

    flags = usb_lock();
    entry = find_feature_cache(report_id);
    if (entry) {
        uint32_t out_len = entry->len;
        if (out_len > requested_len) {
            out_len = requested_len;
        }
        if (out_len > sizeof(usb_control_buffer)) {
            out_len = sizeof(usb_control_buffer);
        }
        memcpy(usb_control_buffer, entry->data, out_len);
        *len = out_len;
        usb_diag.feature_cache_hits++;
        usb_unlock(flags);
        return;
    }
    usb_diag.feature_cache_misses++;
    usb_unlock(flags);

    queue_feature_request(report_id, requested_len);
}

uint8_t usbd_hid_get_idle(uint8_t busid, uint8_t intf, uint8_t report_id)
{
    (void)busid;

    usb_diag.hid_get_idle++;
    usb_diag.last_report_id = report_id;
    if (intf == ITF_NUM_HID_KBD) {
        return keyboard_idle_rate;
    }
    return 0;
}

uint8_t usbd_hid_get_protocol(uint8_t busid, uint8_t intf)
{
    (void)busid;

    usb_diag.hid_get_protocol++;
    if (intf == ITF_NUM_HID_KBD) {
        return keyboard_protocol;
    }
    return 1;
}

void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid;

    usb_diag.hid_set_report++;
    usb_diag.last_report_id = report_id;
    usb_diag.last_report_type = report_type;
    if (intf == ITF_NUM_HID_KBD) {
        return;
    }
    if (report_type == HID_REPORT_FEATURE &&
        set_bridge_config_report(report_id, report, report_len)) {
        return;
    }
    queue_host_report(report_id, report_type, report, report_len);
}

void usbd_hid_set_idle(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t duration)
{
    (void)busid;

    usb_diag.hid_set_idle++;
    usb_diag.last_report_id = report_id;
    if (intf == ITF_NUM_HID_KBD) {
        keyboard_idle_rate = duration;
    }
}

void usbd_hid_set_protocol(uint8_t busid, uint8_t intf, uint8_t protocol)
{
    (void)busid;

    usb_diag.hid_set_protocol++;
    usb_diag.last_report_type = protocol;
    if (intf == ITF_NUM_HID_KBD) {
        keyboard_protocol = protocol;
    }
}

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    usb_diag.audio_open++;
    usb_diag.audio_last_open_intf = intf;

    if (intf == ITF_NUM_AUDIO_STREAMING_OUT) {
        audio_out_open = true;
        m61_audio_epoch_reset(usb_generation);
        arm_audio_out(busid);
    } else if (intf == ITF_NUM_AUDIO_STREAMING_IN) {
        audio_in_open = true;
        audio_in_busy = false;
        flush_mic_queues();
        usb_notify_from_isr(usb_audio_ingress_task_handle);
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;

    usb_diag.audio_close++;
    usb_diag.audio_last_close_intf = intf;

    if (intf == ITF_NUM_AUDIO_STREAMING_OUT) {
        audio_out_open = false;
        m61_audio_epoch_reset(usb_generation);
    } else if (intf == ITF_NUM_AUDIO_STREAMING_IN) {
        audio_in_open = false;
        audio_in_busy = false;
        flush_mic_queues();
    }
}

void usbd_audio_set_volume(uint8_t busid, uint8_t ep, uint8_t ch, int volume_db)
{
    (void)busid;
    (void)ch;

    usb_diag.audio_set_volume++;
    if (ep == AUDIO_OUT_EP) {
        audio_speaker_volume_db = volume_db;
    } else if (ep == AUDIO_IN_EP) {
        audio_mic_volume_db = volume_db;
    }
}

int usbd_audio_get_volume(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ch;

    if (ep == AUDIO_OUT_EP) {
        return audio_speaker_volume_db;
    }
    if (ep == AUDIO_IN_EP) {
        return audio_mic_volume_db;
    }
    return 0;
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute)
{
    (void)busid;
    (void)ch;

    usb_diag.audio_set_mute++;
    if (ep == AUDIO_OUT_EP) {
        audio_speaker_mute = mute;
        m61_audio_epoch_reset(usb_generation);
    } else if (ep == AUDIO_IN_EP) {
        audio_mic_mute = mute;
        if (mute) {
            flush_mic_queues();
        }
    }
}

bool usbd_audio_get_mute(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ch;

    if (ep == AUDIO_OUT_EP) {
        return audio_speaker_mute;
    }
    if (ep == AUDIO_IN_EP) {
        return audio_mic_mute;
    }
    return false;
}

void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t ep, uint32_t sampling_freq)
{
    (void)busid;

    usb_diag.audio_set_freq++;
    if (ep == AUDIO_OUT_EP) {
        audio_out_sample_rate = sampling_freq;
    } else if (ep == AUDIO_IN_EP) {
        audio_in_sample_rate = sampling_freq;
    }
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid;

    if (ep == AUDIO_OUT_EP) {
        return audio_out_sample_rate;
    }
    if (ep == AUDIO_IN_EP) {
        return audio_in_sample_rate;
    }
    return AUDIO_SAMPLE_RATE;
}

#else
void m61_usb_gamepad_init(void) {}
void m61_usb_gamepad_deinit(void) {}
int m61_usb_gamepad_reinit(void) { return -1; }
void m61_usb_gamepad_send_report01(const uint8_t *payload, size_t len) { (void)payload; (void)len; }
void m61_usb_gamepad_send_state(const dualsense_state_t *state) { (void)state; }
void m61_usb_gamepad_note_controller_state(const dualsense_state_t *state) { (void)state; }
void m61_usb_gamepad_reset_controller_state(void) {}
void m61_usb_gamepad_store_feature_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    (void)report_id;
    (void)data;
    (void)len;
}
void m61_usb_gamepad_reset_feature_cache(void) {}
void m61_usb_gamepad_reset_transport_queues(void) {}
bool m61_usb_gamepad_take_feature_request(uint8_t *report_id, uint32_t *requested_len)
{
    (void)report_id;
    (void)requested_len;
    return false;
}
bool m61_usb_gamepad_take_host_report(m61_usb_gamepad_host_report_t *report)
{
    (void)report;
    return false;
}
bool m61_usb_gamepad_take_audio_epoch_pair(m61_audio_epoch_pair_t *pair)
{
    (void)pair;
    return false;
}
void m61_usb_gamepad_submit_mic_opus(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}
bool m61_usb_gamepad_audio_mic_enabled(void) { return false; }
bool m61_usb_gamepad_audio_in_active(void) { return false; }
bool m61_usb_gamepad_audio_speaker_active(void) { return false; }
bool m61_usb_gamepad_remote_wakeup(void) { return false; }
bool m61_usb_gamepad_ready(void) { return false; }
bool m61_usb_gamepad_configured(void) { return false; }
bool m61_usb_gamepad_busy(void) { return false; }
uint32_t m61_usb_gamepad_sent_count(void) { return 0; }
uint32_t m61_usb_gamepad_drop_count(void) { return 0; }
int m61_usb_gamepad_init_result(void) { return -1; }
uint8_t m61_usb_gamepad_last_event(void) { return 0; }
uint32_t m61_usb_gamepad_event_count(uint8_t event) { (void)event; return 0; }
void m61_usb_gamepad_get_diag(m61_usb_gamepad_diag_t *diag)
{
    if (diag) {
        memset(diag, 0, sizeof(*diag));
    }
}
#endif
