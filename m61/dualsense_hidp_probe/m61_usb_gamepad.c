#include "m61_usb_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bflb_clock.h"
#include "bflb_core.h"
#include "bflb_irq.h"
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
#define USBD_PID 0x0CE6
#define USBD_MAX_POWER 500
#define ITF_NUM_AUDIO_CONTROL 0
#define ITF_NUM_AUDIO_STREAMING_OUT 1
#define ITF_NUM_AUDIO_STREAMING_IN 2
#define ITF_NUM_HID 3
#define ITF_NUM_TOTAL 4
#define AUDIO_OUT_EP 0x01
#define AUDIO_IN_EP 0x82
#define AUDIO_OUT_PACKET_SIZE 392
#define AUDIO_IN_PACKET_SIZE 196
#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_SPEAKER_FU_ID 0x02
#define AUDIO_MIC_FU_ID 0x05
#define HID_IN_EP 0x84
#define HID_OUT_EP 0x03
#define HID_INT_EP_SIZE 64
#define HID_INT_EP_INTERVAL 1
#define USB_DUALSENSE_CONFIG_DESC_SIZE 0x00E3
#define HID_DUALSENSE_REPORT_DESC_SIZE 321
#define FEATURE_CACHE_SLOTS 12
#define AUDIO_INPUT_CHANNELS 4
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES (AUDIO_INPUT_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define HAPTICS_DOWNSAMPLE_FACTOR 16
#define HAPTICS_RESAMPLE_MODE_WDL_EQUIV 2
#ifndef HAPTICS_GAIN_Q8
#define HAPTICS_GAIN_Q8 256
#endif
#define HAPTICS_QUEUE_DEPTH 8

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

#if CONFIG_M61_USB_GAMEPAD_ENABLE
static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t dualsense_report_desc[HID_DUALSENSE_REPORT_DESC_SIZE] = {
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
    HID_DUALSENSE_REPORT_DESC_SIZE & 0xFF,
    (HID_DUALSENSE_REPORT_DESC_SIZE >> 8) & 0xFF,

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
static volatile bool usb_suspended;
static volatile bool usb_out_armed;
static volatile bool audio_out_open;
static volatile bool audio_in_open;
static volatile bool audio_in_busy;
static volatile bool pending_feature_request_valid;
static volatile bool pending_host_report_valid;
static volatile bool usb_initialized;
static volatile uint32_t usb_sent_count;
static volatile uint32_t usb_drop_count;
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
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t usb_control_buffer[M61_DS5_USB_FEATURE_MAX_LEN];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_out_buffer[AUDIO_OUT_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_in_silence[AUDIO_IN_PACKET_SIZE];
static uint8_t last_input_payload[M61_DS5_USB_INPUT_PAYLOAD_LEN];
static uint8_t haptics_accum[M61_DS5_HAPTICS_BLOCK_LEN];
static uint8_t haptics_accum_pos;
static bool haptics_prev_valid;
static int16_t haptics_prev_left;
static int16_t haptics_prev_right;
static uint8_t haptics_phase;
static uint8_t haptics_accum_peak;
static bool haptics_accum_nonzero;
static uint8_t haptics_queue[HAPTICS_QUEUE_DEPTH][M61_DS5_HAPTICS_BLOCK_LEN];
static volatile uint8_t haptics_queue_head;
static volatile uint8_t haptics_queue_tail;
static volatile uint8_t haptics_queue_count;
static m61_usb_gamepad_host_report_t pending_host_report;
static feature_cache_entry_t feature_cache[FEATURE_CACHE_SLOTS];
static uint8_t feature_cache_replace_index;

typedef char ds5_report_desc_size_check[(sizeof(dualsense_report_desc) == HID_DUALSENSE_REPORT_DESC_SIZE) ? 1 : -1];
typedef char ds5_config_desc_size_check[(sizeof(config_descriptor) == USB_DUALSENSE_CONFIG_DESC_SIZE) ? 1 : -1];

static uintptr_t usb_lock(void)
{
    return bflb_irq_save();
}

static void usb_unlock(uintptr_t flags)
{
    bflb_irq_restore(flags);
}

static int16_t read_i16_le(const uint8_t *data)
{
    uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return (int16_t)value;
}

static uint8_t abs_i8(int8_t value)
{
    if (value == INT8_MIN) {
        return 128;
    }
    return (uint8_t)(value < 0 ? -value : value);
}

static int8_t haptic_pcm16_to_i8(int16_t sample)
{
    int32_t scaled = ((int32_t)sample * HAPTICS_GAIN_Q8) / 256;
    int32_t value;

    if (scaled > 32768) {
        scaled = 32768;
    } else if (scaled < -32768) {
        scaled = -32768;
    }

    value = (scaled * 127) / 32768;
    if (value > 127) {
        value = 127;
    } else if (value < -128) {
        value = -128;
    }

    return (int8_t)value;
}

static void reset_haptics_accumulator(void)
{
    haptics_accum_pos = 0;
    haptics_prev_valid = false;
    haptics_prev_left = 0;
    haptics_prev_right = 0;
    haptics_phase = 0;
    haptics_accum_peak = 0;
    haptics_accum_nonzero = false;
}

static void flush_haptics_queue(void)
{
    uintptr_t flags;

    flags = usb_lock();
    haptics_queue_head = 0;
    haptics_queue_tail = 0;
    haptics_queue_count = 0;
    reset_haptics_accumulator();
    usb_unlock(flags);
}

static void queue_haptics_block(const uint8_t *block, bool nonzero, uint8_t peak)
{
    uintptr_t flags;

    if (!block) {
        return;
    }

    flags = usb_lock();
    if (haptics_queue_count >= HAPTICS_QUEUE_DEPTH) {
        haptics_queue_tail = (uint8_t)((haptics_queue_tail + 1U) % HAPTICS_QUEUE_DEPTH);
        haptics_queue_count--;
        usb_diag.audio_haptic_queue_dropped++;
    }

    memcpy(haptics_queue[haptics_queue_head], block, M61_DS5_HAPTICS_BLOCK_LEN);
    haptics_queue_head = (uint8_t)((haptics_queue_head + 1U) % HAPTICS_QUEUE_DEPTH);
    haptics_queue_count++;
    usb_diag.audio_haptic_blocks++;
    if (nonzero) {
        usb_diag.audio_haptic_nonzero_blocks++;
    }
    usb_diag.audio_haptic_last_peak = peak;
    usb_unlock(flags);
}

static void process_audio_haptics(const uint8_t *data, uint32_t nbytes)
{
    uint32_t frames;

    if (!data || nbytes < AUDIO_FRAME_BYTES) {
        return;
    }

    frames = nbytes / AUDIO_FRAME_BYTES;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *frame = data + (i * AUDIO_FRAME_BYTES);
        int16_t current_left = read_i16_le(frame + 4);
        int16_t current_right = read_i16_le(frame + 6);

        if (!haptics_prev_valid) {
            haptics_prev_left = current_left;
            haptics_prev_right = current_right;
            haptics_prev_valid = true;
            continue;
        }

        if (haptics_phase == 0) {
            int8_t left = haptic_pcm16_to_i8(haptics_prev_left);
            int8_t right = haptic_pcm16_to_i8(haptics_prev_right);
            uint8_t peak_left = abs_i8(left);
            uint8_t peak_right = abs_i8(right);

            haptics_accum[haptics_accum_pos++] = (uint8_t)left;
            haptics_accum[haptics_accum_pos++] = (uint8_t)right;
            usb_diag.audio_haptic_sample_pairs++;

            if (left || right) {
                haptics_accum_nonzero = true;
            }
            if (peak_left > haptics_accum_peak) {
                haptics_accum_peak = peak_left;
            }
            if (peak_right > haptics_accum_peak) {
                haptics_accum_peak = peak_right;
            }

            if (haptics_accum_pos >= M61_DS5_HAPTICS_BLOCK_LEN) {
                queue_haptics_block(haptics_accum,
                                    haptics_accum_nonzero,
                                    haptics_accum_peak);
                haptics_accum_pos = 0;
                haptics_accum_peak = 0;
                haptics_accum_nonzero = false;
            }
        }

        haptics_prev_left = current_left;
        haptics_prev_right = current_right;
        haptics_phase++;
        if (haptics_phase >= HAPTICS_DOWNSAMPLE_FACTOR) {
            haptics_phase = 0;
        }
    }
}

static void m61_usb_clock_recover(void)
{
    PERIPHERAL_CLOCK_USB_ENABLE();
#if defined(BL616)
    GLB_Set_USB_CLK_From_WIFIPLL(1);
#endif
}

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    usb_diag.device_desc++;
    usb_diag.last_speed = speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    usb_diag.config_desc++;
    usb_diag.last_speed = speed;
    return config_descriptor;
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
    return string_descriptors[index];
}

static const struct usb_descriptor dualsense_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static void queue_feature_request(uint8_t report_id, uint32_t requested_len)
{
    uintptr_t flags = usb_lock();

    pending_feature_report_id = report_id;
    pending_feature_report_len = requested_len;
    pending_feature_request_valid = true;
    usb_unlock(flags);
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

static void queue_host_report(uint8_t report_id, uint8_t report_type, const uint8_t *data, uint32_t len)
{
    uintptr_t flags;

    if (!data || len == 0) {
        return;
    }
    if (len > M61_DS5_USB_OUTPUT_MAX_LEN) {
        len = M61_DS5_USB_OUTPUT_MAX_LEN;
    }

    flags = usb_lock();
    if (pending_host_report_valid) {
        usb_diag.host_report_dropped++;
    }
    pending_host_report.report_id = report_id;
    pending_host_report.report_type = report_type;
    pending_host_report.len = len;
    memcpy(pending_host_report.data, data, len);
    pending_host_report_valid = true;
    usb_diag.last_out_report_id = report_id ? report_id : data[0];
    usb_diag.last_out_report_len = len;
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

    if (usbd_ep_start_write(busid, AUDIO_IN_EP, audio_in_silence, sizeof(audio_in_silence)) == 0) {
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
        case USBD_EVENT_DISCONNECTED:
            usb_ready = false;
            usb_configured = false;
            usb_busy = false;
            usb_suspended = false;
            usb_out_armed = false;
            audio_out_open = false;
            audio_in_open = false;
            audio_in_busy = false;
            flush_haptics_queue();
            break;
        case USBD_EVENT_CONFIGURED:
            usb_ready = true;
            usb_configured = true;
            usb_busy = false;
            usb_suspended = false;
            usb_out_armed = false;
            arm_hid_out(0);
            arm_audio_out(busid);
            arm_audio_in(busid);
            break;
        case USBD_EVENT_RESUME:
            usb_suspended = false;
            arm_hid_out(0);
            arm_audio_out(busid);
            arm_audio_in(busid);
            break;
        case USBD_EVENT_SUSPEND:
            usb_suspended = true;
            usb_out_armed = false;
            audio_in_busy = false;
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
    (void)ep;

    if (nbytes) {
        usb_diag.audio_out_packets++;
        usb_diag.audio_out_bytes += nbytes;
        process_audio_haptics(audio_out_buffer, nbytes);
    }
    arm_audio_out(busid);
}

static void usbd_audio_in_ep_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    audio_in_busy = false;
    usb_diag.audio_in_packets++;
    usb_diag.audio_in_bytes += nbytes;
    arm_audio_in(busid);
}

static struct usbd_endpoint hid_in_ep = {
    .ep_cb = usbd_hid_in_callback,
    .ep_addr = HID_IN_EP,
};

static struct usbd_endpoint hid_out_ep = {
    .ep_cb = usbd_hid_out_callback,
    .ep_addr = HID_OUT_EP,
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
    usbd_desc_register(0, &dualsense_descriptor);

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
                                             dualsense_report_desc,
                                             sizeof(dualsense_report_desc)));

    usbd_add_endpoint(0, &audio_out_ep);
    usbd_add_endpoint(0, &audio_in_ep);
    usbd_add_endpoint(0, &hid_in_ep);
    usbd_add_endpoint(0, &hid_out_ep);
}

void m61_usb_gamepad_init(void)
{
    m61_usb_clock_recover();
    memcpy(last_input_payload, ds5_idle_payload, sizeof(last_input_payload));
    memset(audio_in_silence, 0, sizeof(audio_in_silence));
    flush_haptics_queue();
    register_usb_dualsense_device();
    usb_init_result = usbd_initialize(0, 0, usbd_event_handler);
    usb_initialized = (usb_init_result == 0);
    printf("M61 USB DualSense composite init result=%d\r\n", usb_init_result);
}

void m61_usb_gamepad_deinit(void)
{
    if (usb_initialized) {
        usb_ready = false;
        usb_configured = false;
        usb_busy = false;
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
    register_usb_dualsense_device();
    usb_init_result = usbd_initialize(0, 0, usbd_event_handler);
    usb_initialized = (usb_init_result == 0);
    printf("M61 USB DualSense composite reinit result=%d\r\n", usb_init_result);
    return usb_init_result;
}

void m61_usb_gamepad_send_report01(const uint8_t *payload, size_t len)
{
    size_t copy_len;
    int ret;

    if (!payload || len == 0) {
        return;
    }

    copy_len = len;
    if (copy_len > M61_DS5_USB_INPUT_PAYLOAD_LEN) {
        copy_len = M61_DS5_USB_INPUT_PAYLOAD_LEN;
    }

    memcpy(last_input_payload, payload, copy_len);
    if (copy_len < M61_DS5_USB_INPUT_PAYLOAD_LEN) {
        memset(last_input_payload + copy_len, 0, M61_DS5_USB_INPUT_PAYLOAD_LEN - copy_len);
    }

    if (!usb_ready || !usb_configured || usb_busy || usb_suspended || !usb_device_is_configured(0)) {
        usb_drop_count++;
        return;
    }

    usb_in_buffer[0] = M61_DS5_USB_INPUT_REPORT_ID;
    memcpy(usb_in_buffer + 1, last_input_payload, M61_DS5_USB_INPUT_PAYLOAD_LEN);
    usb_busy = true;
    ret = usbd_ep_start_write(0, HID_IN_EP, usb_in_buffer, M61_DS5_USB_INPUT_PAYLOAD_LEN + 1);
    if (ret == 0) {
        usb_sent_count++;
    } else {
        usb_busy = false;
        usb_drop_count++;
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
    bool valid;
    uintptr_t flags;

    if (!report) {
        return false;
    }

    flags = usb_lock();
    valid = pending_host_report_valid;
    if (valid) {
        *report = pending_host_report;
        pending_host_report_valid = false;
    }
    usb_unlock(flags);
    return valid;
}

bool m61_usb_gamepad_take_haptics_block(uint8_t *data, size_t len)
{
    bool valid;
    uintptr_t flags;

    if (!data || len < M61_DS5_HAPTICS_BLOCK_LEN) {
        return false;
    }

    flags = usb_lock();
    valid = haptics_queue_count > 0;
    if (valid) {
        memcpy(data, haptics_queue[haptics_queue_tail], M61_DS5_HAPTICS_BLOCK_LEN);
        haptics_queue_tail = (uint8_t)((haptics_queue_tail + 1U) % HAPTICS_QUEUE_DEPTH);
        haptics_queue_count--;
    }
    usb_unlock(flags);
    return valid;
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
    if (!diag) {
        return;
    }

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
    diag->audio_open = usb_diag.audio_open;
    diag->audio_close = usb_diag.audio_close;
    diag->audio_out_packets = usb_diag.audio_out_packets;
    diag->audio_out_bytes = usb_diag.audio_out_bytes;
    diag->audio_in_packets = usb_diag.audio_in_packets;
    diag->audio_in_bytes = usb_diag.audio_in_bytes;
    diag->audio_haptic_blocks = usb_diag.audio_haptic_blocks;
    diag->audio_haptic_sample_pairs = usb_diag.audio_haptic_sample_pairs;
    diag->audio_haptic_nonzero_blocks = usb_diag.audio_haptic_nonzero_blocks;
    diag->audio_haptic_queue_dropped = usb_diag.audio_haptic_queue_dropped;
    diag->audio_set_volume = usb_diag.audio_set_volume;
    diag->audio_set_mute = usb_diag.audio_set_mute;
    diag->audio_set_freq = usb_diag.audio_set_freq;
    diag->last_speed = usb_diag.last_speed;
    diag->last_string_index = usb_diag.last_string_index;
    diag->last_report_id = usb_diag.last_report_id;
    diag->last_report_type = usb_diag.last_report_type;
    diag->last_out_report_id = usb_diag.last_out_report_id;
    diag->audio_last_open_intf = usb_diag.audio_last_open_intf;
    diag->audio_last_close_intf = usb_diag.audio_last_close_intf;
    diag->audio_out_open = audio_out_open ? 1 : 0;
    diag->audio_in_open = audio_in_open ? 1 : 0;
    diag->audio_haptic_queue_depth = haptics_queue_count;
    diag->audio_haptic_last_peak = usb_diag.audio_haptic_last_peak;
    diag->audio_haptic_downsample = HAPTICS_DOWNSAMPLE_FACTOR;
    diag->audio_haptic_resample_mode = HAPTICS_RESAMPLE_MODE_WDL_EQUIV;
    diag->audio_haptic_gain_q8 = HAPTICS_GAIN_Q8;
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
    (void)intf;

    usb_diag.hid_get_report++;
    usb_diag.last_report_id = report_id;
    usb_diag.last_report_type = report_type;
    *data = usb_control_buffer;
    *len = 0;

    if (report_type == HID_REPORT_INPUT && report_id == M61_DS5_USB_INPUT_REPORT_ID) {
        memcpy(usb_control_buffer, last_input_payload, sizeof(last_input_payload));
        *len = sizeof(last_input_payload);
        return;
    }

    if (report_type != HID_REPORT_FEATURE) {
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
    (void)intf;

    usb_diag.hid_get_idle++;
    usb_diag.last_report_id = report_id;
    return 0;
}

uint8_t usbd_hid_get_protocol(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;

    usb_diag.hid_get_protocol++;
    return 1;
}

void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t report_type, uint8_t *report, uint32_t report_len)
{
    (void)busid;
    (void)intf;

    usb_diag.hid_set_report++;
    usb_diag.last_report_id = report_id;
    usb_diag.last_report_type = report_type;
    queue_host_report(report_id, report_type, report, report_len);
}

void usbd_hid_set_idle(uint8_t busid, uint8_t intf, uint8_t report_id, uint8_t duration)
{
    (void)busid;
    (void)intf;
    (void)duration;

    usb_diag.hid_set_idle++;
    usb_diag.last_report_id = report_id;
}

void usbd_hid_set_protocol(uint8_t busid, uint8_t intf, uint8_t protocol)
{
    (void)busid;
    (void)intf;

    usb_diag.hid_set_protocol++;
    usb_diag.last_report_type = protocol;
}

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    usb_diag.audio_open++;
    usb_diag.audio_last_open_intf = intf;

    if (intf == ITF_NUM_AUDIO_STREAMING_OUT) {
        audio_out_open = true;
        flush_haptics_queue();
        arm_audio_out(busid);
    } else if (intf == ITF_NUM_AUDIO_STREAMING_IN) {
        audio_in_open = true;
        audio_in_busy = false;
        arm_audio_in(busid);
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;

    usb_diag.audio_close++;
    usb_diag.audio_last_close_intf = intf;

    if (intf == ITF_NUM_AUDIO_STREAMING_OUT) {
        audio_out_open = false;
        flush_haptics_queue();
    } else if (intf == ITF_NUM_AUDIO_STREAMING_IN) {
        audio_in_open = false;
        audio_in_busy = false;
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
    } else if (ep == AUDIO_IN_EP) {
        audio_mic_mute = mute;
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
void m61_usb_gamepad_store_feature_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    (void)report_id;
    (void)data;
    (void)len;
}
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
bool m61_usb_gamepad_take_haptics_block(uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}
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
