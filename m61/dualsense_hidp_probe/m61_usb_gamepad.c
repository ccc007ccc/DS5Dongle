#include "m61_usb_gamepad.h"
#include "m61_audio_epoch.h"
#include "m61_perf_profile.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define AUDIO_OUT_SLOT_STRIDE ((AUDIO_OUT_PACKET_SIZE + 31U) & ~31U)
#define AUDIO_INGRESS_DEPTH 8
#define AUDIO_INGRESS_INVALID_SLOT UINT8_MAX
#define AUDIO_IN_PACKET_SIZE 196
#define AUDIO_IN_STREAM_PACKET_SIZE 192
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
#define FEATURE_SET_QUEUE_DEPTH 8
#define AUDIO_INPUT_CHANNELS 4
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES (AUDIO_INPUT_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_SPEAKER_CHANNELS 2
#define AUDIO_SPEAKER_FRAME_SAMPLES 480
#define AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM 480
#define AUDIO_SPEAKER_OPUS_CHANNELS 1
#define AUDIO_SPEAKER_FRAME_BYTES (AUDIO_SPEAKER_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_SPEAKER_QUEUE_DEPTH 2
#define AUDIO_SPEAKER_OPUS_QUEUE_DEPTH 2
#define AUDIO_SPEAKER_OPUS_FORCE_CHANNELS 1
#define AUDIO_SPEAKER_OPUS_BANDWIDTH OPUS_BANDWIDTH_MEDIUMBAND
#ifndef CONFIG_M61_DS5_SPEAKER_OPUS_BITRATE
#define CONFIG_M61_DS5_SPEAKER_OPUS_BITRATE 160000
#endif
#define AUDIO_SPEAKER_OPUS_BITRATE CONFIG_M61_DS5_SPEAKER_OPUS_BITRATE
#define AUDIO_MIC_CHANNELS 1
#define AUDIO_MIC_FRAME_SAMPLES 480
#define AUDIO_MIC_OPUS_QUEUE_DEPTH 2
#define AUDIO_MIC_PACKET_DEPTH 32
#define AUDIO_MIC_PACKET_INVALID_SLOT UINT8_MAX
#ifndef CONFIG_M61_DS5_MIC_DEFAULT_ENABLED
#define CONFIG_M61_DS5_MIC_DEFAULT_ENABLED 0
#endif
#define AUDIO_CODEC_TASK_STACK_WORDS 8192
#define AUDIO_CODEC_TASK_PRIORITY (configMAX_PRIORITIES - 4)
#define AUDIO_INGRESS_TASK_STACK_WORDS 1024
#define AUDIO_INGRESS_TASK_PRIORITY (configMAX_PRIORITIES - 3)
#define AUDIO_OPUS_ENCODER_STATE_MAX 49152U
#define AUDIO_OPUS_DECODER_STATE_MAX 24576U
#ifndef CONFIG_M61_OPUS_PACKET_AUDIT
#define CONFIG_M61_OPUS_PACKET_AUDIT 0
#endif
#define AUDIO_CODEC_DIAG_PERIOD_MS 1000U
#define AUDIO_CODEC_ERR_STATE_TOO_SMALL -1001
#define HAPTICS_DOWNSAMPLE_FACTOR 16
#define HAPTICS_RESAMPLE_MODE_PHASE_DECIMATE 4
#ifndef HAPTICS_GAIN_Q8
#define HAPTICS_GAIN_Q8 256
#endif
#define HAPTICS_QUEUE_DEPTH 2
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

#if CONFIG_M61_HPM_PROFILE
/* Real 48 kHz mono, 10 ms, 71-byte CBR packet from the mic stream fixture. */
static const uint8_t decoder_benchmark_opus[M61_DS5_MIC_OPUS_LEN] = {
    0xb0, 0x53, 0xfc, 0xaa, 0xa5, 0x93, 0x9b, 0x45, 0x44, 0x04, 0x53, 0x15,
    0x32, 0x58, 0xc9, 0xde, 0x7b, 0x57, 0x19, 0x98, 0xc8, 0x59, 0x1a, 0x20,
    0x89, 0x4e, 0x94, 0xc9, 0xc3, 0x15, 0xc3, 0x59, 0xe2, 0xce, 0x6b, 0x83,
    0xc4, 0x01, 0x44, 0x7f, 0x81, 0x6e, 0xdb, 0x92, 0xe0, 0xaf, 0x48, 0x6f,
    0x4e, 0xb0, 0x06, 0x2a, 0xb8, 0x06, 0x94, 0x5e, 0xcf, 0x0e, 0x75, 0x09,
    0xda, 0xa1, 0x8a, 0x42, 0x79, 0x64, 0x15, 0xb8, 0x1a, 0x20, 0xfc,
};
#endif

static volatile bool usb_ready;
static volatile bool usb_configured;
static volatile bool usb_busy;
static volatile bool usb_input_pending;
static volatile bool usb_suspended;
static volatile bool usb_out_armed;
static volatile bool audio_out_open;
static volatile bool audio_out_armed;
static volatile bool audio_in_open;
static volatile bool audio_in_busy;
static volatile bool audio_codec_task_started;
#if CONFIG_M61_HPM_PROFILE
static volatile bool decoder_benchmark_requested;
static volatile bool decoder_benchmark_active;
#endif
static volatile uint32_t audio_generation = 1;
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
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t audio_out_ring[AUDIO_INGRESS_DEPTH][AUDIO_OUT_SLOT_STRIDE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t audio_in_buffer[AUDIO_IN_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t audio_mic_packet_ring[AUDIO_MIC_PACKET_DEPTH][AUDIO_IN_STREAM_PACKET_SIZE];
static uint8_t last_input_payload[M61_DS5_USB_INPUT_PAYLOAD_LEN];
typedef enum {
    AUDIO_INGRESS_FREE = 0,
    AUDIO_INGRESS_DMA_ACTIVE,
    AUDIO_INGRESS_READY,
    AUDIO_INGRESS_READING,
} audio_ingress_state_t;

typedef struct {
    uint64_t captured_us;
    uint32_t generation;
    uint32_t sequence;
    uint16_t len;
    uint8_t speaker_enabled;
    uint8_t state;
} audio_ingress_slot_t;

typedef struct {
    const uint8_t *data;
    uint64_t captured_us;
    uint32_t generation;
    uint32_t sequence;
    uint16_t len;
    uint8_t speaker_enabled;
    uint8_t slot;
} audio_ingress_view_t;

typedef enum {
    AUDIO_MIC_PACKET_FREE = 0,
    AUDIO_MIC_PACKET_FILLING,
    AUDIO_MIC_PACKET_READY,
    AUDIO_MIC_PACKET_DMA_ACTIVE,
} audio_mic_packet_state_t;

static audio_ingress_slot_t audio_ingress[AUDIO_INGRESS_DEPTH];
static volatile uint8_t audio_ingress_producer_cursor;
static volatile uint8_t audio_ingress_consumer_cursor;
static volatile uint8_t audio_ingress_active_slot = AUDIO_INGRESS_INVALID_SLOT;
static volatile uint8_t audio_ingress_count;
static volatile uint8_t audio_ingress_high_water;
static volatile uint32_t audio_ingress_dropped;
static volatile uint32_t audio_ingress_sequence;
static volatile uint32_t audio_ingress_gaps;
#if CONFIG_M61_PIPELINE_PROFILE
static uint64_t audio_out_last_captured_us;
static uint64_t audio_out_interval_us_total;
static uint32_t audio_out_interval_samples;
static uint32_t audio_out_interval_us_last;
static uint32_t audio_out_interval_us_min;
static uint32_t audio_out_interval_us_max;
static uint32_t audio_out_frames_48_packets;
static uint32_t audio_out_frames_49_packets;
static uint32_t audio_out_frames_other_packets;
static bool audio_out_last_capture_valid;
#endif
static volatile uint8_t audio_mic_packet_state[AUDIO_MIC_PACKET_DEPTH];
static volatile uint8_t audio_mic_packet_nonzero[AUDIO_MIC_PACKET_DEPTH];
static volatile uint8_t audio_mic_packet_producer_cursor;
static volatile uint8_t audio_mic_packet_consumer_cursor;
static volatile uint8_t audio_mic_packet_active_slot =
    AUDIO_MIC_PACKET_INVALID_SLOT;
static volatile uint8_t audio_mic_packet_count;
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
static int16_t speaker_accum[AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM * AUDIO_SPEAKER_CHANNELS];
static uint16_t speaker_accum_frames;
static bool speaker_accum_nonzero;
static int16_t speaker_frame_queue[AUDIO_SPEAKER_QUEUE_DEPTH][AUDIO_SPEAKER_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS];
static volatile uint8_t speaker_frame_queue_head;
static volatile uint8_t speaker_frame_queue_tail;
static volatile uint8_t speaker_frame_queue_count;
static uint8_t speaker_opus_queue[AUDIO_SPEAKER_OPUS_QUEUE_DEPTH][M61_DS5_SPEAKER_OPUS_LEN];
static volatile uint8_t speaker_opus_queue_head;
static volatile uint8_t speaker_opus_queue_tail;
static volatile uint8_t speaker_opus_queue_count;
static uint8_t mic_opus_queue[AUDIO_MIC_OPUS_QUEUE_DEPTH][M61_DS5_MIC_OPUS_LEN];
static uint64_t mic_opus_created_us[AUDIO_MIC_OPUS_QUEUE_DEPTH]
    __attribute__((section(".wifi_ram.m61_mic_metadata"), aligned(16)));
static volatile uint8_t mic_opus_queue_head;
static volatile uint8_t mic_opus_queue_tail;
static volatile uint8_t mic_opus_queue_count;
static StaticTask_t audio_codec_task_tcb;
static StackType_t audio_codec_task_stack[AUDIO_CODEC_TASK_STACK_WORDS] __attribute__((aligned(16)));
static StaticTask_t audio_ingress_task_tcb;
static StackType_t audio_ingress_task_stack[AUDIO_INGRESS_TASK_STACK_WORDS]
    __attribute__((aligned(16)));
static uint32_t audio_opus_encoder_state[(AUDIO_OPUS_ENCODER_STATE_MAX + sizeof(uint32_t) - 1U) / sizeof(uint32_t)] __attribute__((aligned(32)));
static uint32_t audio_opus_decoder_state[(AUDIO_OPUS_DECODER_STATE_MAX + sizeof(uint32_t) - 1U) / sizeof(uint32_t)] __attribute__((aligned(16)));
static TaskHandle_t audio_codec_task_handle;

static TaskHandle_t audio_ingress_task_handle;

typedef struct {
    volatile uint32_t sequence;
    m61_opus_packet_audit_t value;
} opus_packet_audit_store_t;

static opus_packet_audit_store_t zz_opus_packet_audit;

#if CONFIG_M61_OPUS_PACKET_AUDIT
static void record_opus_packet_shape(m61_opus_packet_shape_t *shape,
                                     const uint8_t *packet,
                                     uint16_t length)
{
    uint8_t toc;

    if (shape == NULL || packet == NULL || length == 0U) return;
    toc = packet[0];
    if (shape->samples == 0U) {
        shape->length_min = length;
        shape->length_max = length;
    } else {
        if (toc != shape->last_toc) shape->toc_changes++;
        if (length != shape->last_length) shape->length_changes++;
        if (length < shape->length_min) shape->length_min = length;
        if (length > shape->length_max) shape->length_max = length;
    }
    shape->samples++;
    shape->config_mask |= 1UL << (toc >> 3);
    shape->frame_code_mask |= (uint8_t)(1U << (toc & 0x03U));
    if (toc & 0x04U) shape->stereo_packets++;
    shape->last_toc = toc;
    shape->last_length = length;
}

static void record_opus_packet(bool speaker, const uint8_t *packet,
                               uint16_t length)
{
    zz_opus_packet_audit.sequence++;
    __asm volatile("" : : : "memory");
    record_opus_packet_shape(speaker ? &zz_opus_packet_audit.value.speaker
                                     : &zz_opus_packet_audit.value.mic,
                             packet, length);
    __asm volatile("" : : : "memory");
    zz_opus_packet_audit.sequence++;
}
#endif
static m61_usb_gamepad_host_report_t pending_host_report;
static m61_usb_gamepad_host_report_t
    feature_set_queue[FEATURE_SET_QUEUE_DEPTH];
static volatile uint8_t feature_set_queue_head;
static volatile uint8_t feature_set_queue_tail;
static volatile uint8_t feature_set_queue_count;
static volatile uint8_t feature_set_queue_high_water;
static feature_cache_entry_t feature_cache[FEATURE_CACHE_SLOTS];
static uint8_t feature_cache_replace_index;
#if CONFIG_M61_PIPELINE_PROFILE
static uint8_t usb_lock_depth;
static uint32_t usb_lock_start_cycle;
#endif

typedef char ds5_report_desc_size_check[(sizeof(dualsense_report_desc) == HID_DUALSENSE_REPORT_DESC_SIZE) ? 1 : -1];
typedef char ds5_config_desc_size_check[(sizeof(config_descriptor) == USB_DUALSENSE_CONFIG_DESC_SIZE) ? 1 : -1];

static uintptr_t usb_lock(void)
{
    uintptr_t flags = bflb_irq_save();

#if CONFIG_M61_PIPELINE_PROFILE
    if (usb_lock_depth++ == 0U) {
        __asm volatile("csrr %0, mcycle"
                       : "=r"(usb_lock_start_cycle)
                       :
                       : "memory");
    }
#endif
    return flags;
}

static void resample_epoch_speaker_mono(int16_t *dst, const int16_t *src);
static void arm_audio_out(uint8_t busid);

static void usb_unlock(uintptr_t flags)
{
#if CONFIG_M61_PIPELINE_PROFILE
    uint32_t end_cycle;
    uint32_t masked_cycles = 0U;

    if (usb_lock_depth > 0U && --usb_lock_depth == 0U) {
        __asm volatile("csrr %0, mcycle"
                       : "=r"(end_cycle)
                       :
                       : "memory");
        masked_cycles = end_cycle - usb_lock_start_cycle;
    }
    bflb_irq_restore(flags);
    if (masked_cycles != 0U) {
        m61_perf_profile_record_irq_mask_cycles(masked_cycles);
    }
#else
    bflb_irq_restore(flags);
#endif
}

static void reset_audio_pipeline(bool cancel_active_dma)
{
    uintptr_t flags = usb_lock();

    audio_generation++;
    audio_ingress_count = 0U;
    for (uint8_t i = 0; i < AUDIO_INGRESS_DEPTH; i++) {
        audio_ingress_slot_t *slot = &audio_ingress[i];

        if (slot->state == AUDIO_INGRESS_READY) {
            slot->state = AUDIO_INGRESS_FREE;
        } else if (cancel_active_dma &&
                   slot->state == AUDIO_INGRESS_DMA_ACTIVE) {
            slot->state = AUDIO_INGRESS_FREE;
        } else if (slot->state == AUDIO_INGRESS_READING) {
            audio_ingress_count++;
        }
    }
    if (cancel_active_dma) {
        audio_ingress_active_slot = AUDIO_INGRESS_INVALID_SLOT;
        audio_out_armed = false;
    }
#if CONFIG_M61_PIPELINE_PROFILE
    audio_out_last_capture_valid = false;
#endif
    m61_audio_epoch_reset(audio_generation);
    usb_unlock(flags);
}

static int find_free_audio_ingress_slot_locked(void)
{
    for (uint8_t offset = 0; offset < AUDIO_INGRESS_DEPTH; offset++) {
        uint8_t index = (uint8_t)(
            (audio_ingress_producer_cursor + offset) % AUDIO_INGRESS_DEPTH);

        if (audio_ingress[index].state == AUDIO_INGRESS_FREE) {
            return index;
        }
    }
    return -1;
}

static bool take_audio_ingress(audio_ingress_view_t *view)
{
    uintptr_t flags;
    int selected = -1;

    if (!view) {
        return false;
    }
    flags = usb_lock();
    for (uint8_t offset = 0; offset < AUDIO_INGRESS_DEPTH; offset++) {
        uint8_t index = (uint8_t)(
            (audio_ingress_consumer_cursor + offset) % AUDIO_INGRESS_DEPTH);

        if (audio_ingress[index].state == AUDIO_INGRESS_READY) {
            selected = index;
            break;
        }
    }
    if (selected < 0) {
        usb_unlock(flags);
        return false;
    }
    audio_ingress_slot_t *slot = &audio_ingress[selected];
    slot->state = AUDIO_INGRESS_READING;
    view->data = audio_out_ring[selected];
    view->captured_us = slot->captured_us;
    view->generation = slot->generation;
    view->sequence = slot->sequence;
    view->len = slot->len;
    view->speaker_enabled = slot->speaker_enabled;
    view->slot = (uint8_t)selected;
    usb_unlock(flags);
    return true;
}

static void release_audio_ingress(const audio_ingress_view_t *view)
{
    uintptr_t flags;

    if (!view || view->slot >= AUDIO_INGRESS_DEPTH) {
        return;
    }
    flags = usb_lock();
    if (audio_ingress[view->slot].state == AUDIO_INGRESS_READING) {
        audio_ingress[view->slot].state = AUDIO_INGRESS_FREE;
        audio_ingress_consumer_cursor = (uint8_t)(
            (view->slot + 1U) % AUDIO_INGRESS_DEPTH);
        if (audio_ingress_count > 0U) {
            audio_ingress_count--;
        }
    }
    usb_unlock(flags);
    arm_audio_out(0);
}

static void usb_input_pump(void)
{
    uintptr_t flags;
    int ret;

    flags = usb_lock();
    if (!usb_input_pending || !usb_ready || !usb_configured || usb_busy ||
        usb_suspended || !usb_device_is_configured(0)) {
        usb_unlock(flags);
        return;
    }
    usb_in_buffer[0] = M61_DS5_USB_INPUT_REPORT_ID;
    memcpy(usb_in_buffer + 1, last_input_payload,
           M61_DS5_USB_INPUT_PAYLOAD_LEN);
    usb_input_pending = false;
    usb_busy = true;
    usb_unlock(flags);

    ret = usbd_ep_start_write(0, HID_IN_EP, usb_in_buffer,
                              M61_DS5_USB_INPUT_PAYLOAD_LEN + 1U);
    flags = usb_lock();
    if (ret == 0) {
        usb_sent_count++;
    } else {
        usb_busy = false;
        usb_input_pending = true;
        usb_drop_count++;
    }
    usb_unlock(flags);
}

static int16_t read_i16_le(const uint8_t *data)
{
    uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return (int16_t)value;
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
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

static bool audio_packet_has_nonzero_haptics(const uint8_t *data, uint32_t nbytes)
{
    uint32_t frames;

    if (!data || nbytes < AUDIO_FRAME_BYTES) {
        return false;
    }

    frames = nbytes / AUDIO_FRAME_BYTES;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *frame = data + (i * AUDIO_FRAME_BYTES);
        if (frame[4] || frame[5] || frame[6] || frame[7]) {
            return true;
        }
    }
    return false;
}

static bool audio_packet_has_nonzero_speaker(const uint8_t *data, uint32_t nbytes)
{
    uint32_t frames;

    if (!data || nbytes < AUDIO_FRAME_BYTES) {
        return false;
    }

    frames = nbytes / AUDIO_FRAME_BYTES;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *frame = data + (i * AUDIO_FRAME_BYTES);
        if (frame[0] || frame[1] || frame[2] || frame[3]) {
            return true;
        }
    }
    return false;
}

static void resample_speaker_upstream_frame(int16_t *dst, const int16_t *src)
{
    if (!dst || !src) {
        return;
    }

    for (uint32_t out_frame = 0; out_frame < AUDIO_SPEAKER_FRAME_SAMPLES; out_frame++) {
        uint32_t src_pos_num = out_frame * AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM;
        uint32_t src_frame = src_pos_num / AUDIO_SPEAKER_FRAME_SAMPLES;
        uint32_t frac = src_pos_num - (src_frame * AUDIO_SPEAKER_FRAME_SAMPLES);

        if (src_frame >= AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM - 1U) {
            src_frame = AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM - 1U;
            frac = 0;
        }

        for (uint32_t ch = 0; ch < AUDIO_SPEAKER_CHANNELS; ch++) {
            uint32_t src_index = (src_frame * AUDIO_SPEAKER_CHANNELS) + ch;
            int32_t a = src[src_index];
            int32_t b = (src_frame < AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM - 1U)
                            ? src[src_index + AUDIO_SPEAKER_CHANNELS]
                            : a;
            int32_t scaled_delta = (b - a) * (int32_t)frac;

            if (scaled_delta >= 0) {
                scaled_delta += AUDIO_SPEAKER_FRAME_SAMPLES / 2;
            } else {
                scaled_delta -= AUDIO_SPEAKER_FRAME_SAMPLES / 2;
            }

            dst[(out_frame * AUDIO_SPEAKER_CHANNELS) + ch] =
                clamp_i16(a + (scaled_delta / AUDIO_SPEAKER_FRAME_SAMPLES));
        }
    }
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

static void __attribute__((unused)) flush_haptics_queue(void)
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

static void __attribute__((unused)) flush_speaker_queues(void)
{
    uintptr_t flags = usb_lock();

    speaker_accum_frames = 0;
    speaker_accum_nonzero = false;
    speaker_frame_queue_head = 0;
    speaker_frame_queue_tail = 0;
    speaker_frame_queue_count = 0;
    speaker_opus_queue_head = 0;
    speaker_opus_queue_tail = 0;
    speaker_opus_queue_count = 0;
    usb_unlock(flags);
}

static void flush_mic_queues(void)
{
    uintptr_t flags = usb_lock();

    mic_opus_queue_head = 0;
    mic_opus_queue_tail = 0;
    mic_opus_queue_count = 0;
    audio_mic_packet_producer_cursor = 0;
    audio_mic_packet_consumer_cursor = 0;
    audio_mic_packet_active_slot = AUDIO_MIC_PACKET_INVALID_SLOT;
    audio_mic_packet_count = 0;
    for (uint8_t i = 0; i < AUDIO_MIC_PACKET_DEPTH; i++) {
        audio_mic_packet_state[i] = AUDIO_MIC_PACKET_FREE;
        audio_mic_packet_nonzero[i] = 0U;
    }
    usb_unlock(flags);
}

static void queue_speaker_frame(const int16_t *samples)
{
    uintptr_t flags;

    if (!samples) {
        return;
    }

    flags = usb_lock();
    if (speaker_frame_queue_count >= AUDIO_SPEAKER_QUEUE_DEPTH) {
        speaker_frame_queue_tail = (uint8_t)((speaker_frame_queue_tail + 1U) % AUDIO_SPEAKER_QUEUE_DEPTH);
        speaker_frame_queue_count--;
        usb_diag.audio_speaker_queue_dropped++;
    }
    memcpy(speaker_frame_queue[speaker_frame_queue_head],
           samples,
           AUDIO_SPEAKER_FRAME_BYTES);
    speaker_frame_queue_head = (uint8_t)((speaker_frame_queue_head + 1U) % AUDIO_SPEAKER_QUEUE_DEPTH);
    speaker_frame_queue_count++;
    usb_diag.audio_speaker_frames++;
    usb_unlock(flags);
}

static bool __attribute__((unused)) take_speaker_frame(int16_t *samples)
{
    bool valid;
    uintptr_t flags;

    if (!samples) {
        return false;
    }

    flags = usb_lock();
    valid = speaker_frame_queue_count > 0;
    if (valid) {
        memcpy(samples,
               speaker_frame_queue[speaker_frame_queue_tail],
               AUDIO_SPEAKER_FRAME_BYTES);
        speaker_frame_queue_tail = (uint8_t)((speaker_frame_queue_tail + 1U) % AUDIO_SPEAKER_QUEUE_DEPTH);
        speaker_frame_queue_count--;
    }
    usb_unlock(flags);
    return valid;
}

static void __attribute__((unused)) queue_speaker_opus(const uint8_t *data, size_t len)
{
    uintptr_t flags;

    if (!data || len == 0) {
        return;
    }
    if (len > M61_DS5_SPEAKER_OPUS_LEN) {
        len = M61_DS5_SPEAKER_OPUS_LEN;
    }

    flags = usb_lock();
    if (speaker_opus_queue_count >= AUDIO_SPEAKER_OPUS_QUEUE_DEPTH) {
        speaker_opus_queue_tail = (uint8_t)((speaker_opus_queue_tail + 1U) % AUDIO_SPEAKER_OPUS_QUEUE_DEPTH);
        speaker_opus_queue_count--;
        usb_diag.audio_speaker_opus_dropped++;
    }
    memset(speaker_opus_queue[speaker_opus_queue_head], 0, M61_DS5_SPEAKER_OPUS_LEN);
    memcpy(speaker_opus_queue[speaker_opus_queue_head], data, len);
    speaker_opus_queue_head = (uint8_t)((speaker_opus_queue_head + 1U) % AUDIO_SPEAKER_OPUS_QUEUE_DEPTH);
    speaker_opus_queue_count++;
    usb_diag.audio_speaker_encoded++;
    usb_unlock(flags);
}

static void record_speaker_encode_diag(uint32_t encode_us, int encoded_len)
{
    usb_diag.audio_speaker_encode_us_last = encode_us;
    usb_diag.audio_speaker_encode_us_total += encode_us;
    if (encode_us > usb_diag.audio_speaker_encode_us_max) {
        usb_diag.audio_speaker_encode_us_max = encode_us;
    }
    usb_diag.audio_speaker_last_opus_len =
        (encoded_len > 0) ? (uint16_t)encoded_len : 0;
}

static bool take_mic_opus(uint8_t *data, uint64_t *created_us)
{
    bool valid;
    uintptr_t flags;

    if (!data || !created_us) {
        return false;
    }

    flags = usb_lock();
    valid = mic_opus_queue_count > 0;
    if (valid) {
        memcpy(data, mic_opus_queue[mic_opus_queue_tail], M61_DS5_MIC_OPUS_LEN);
        *created_us = mic_opus_created_us[mic_opus_queue_tail];
        mic_opus_queue_tail = (uint8_t)((mic_opus_queue_tail + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
        mic_opus_queue_count--;
    }
    usb_unlock(flags);
    return valid;
}

static void push_mic_pcm_stereo(const int16_t *mono, uint16_t samples)
{
    uintptr_t flags;
    uint8_t selected;

    if (!mono || samples == 0) {
        return;
    }

    uint16_t sample_offset = 0;
    flags = usb_lock();
    selected = audio_mic_packet_producer_cursor;
    if (audio_mic_packet_state[selected] != AUDIO_MIC_PACKET_FREE) {
        usb_diag.audio_mic_opus_dropped++;
        usb_unlock(flags);
        return;
    }
    audio_mic_packet_state[selected] = AUDIO_MIC_PACKET_FILLING;
    usb_unlock(flags);

    while (sample_offset < samples) {
        uint32_t *packet = (uint32_t *)audio_mic_packet_ring[selected];
        uint16_t packet_samples = samples - sample_offset;
        uint16_t nonzero = 0;
        if (packet_samples > AUDIO_IN_STREAM_PACKET_SIZE / 4U) {
            packet_samples = AUDIO_IN_STREAM_PACKET_SIZE / 4U;
        }
        for (uint16_t i = 0; i < packet_samples; i++) {
            int16_t pcm = mono[sample_offset + i];
            uint16_t sample = (uint16_t)pcm;

            packet[i] = (uint32_t)sample | ((uint32_t)sample << 16);
            if (pcm != 0) {
                nonzero++;
            }
        }
        if (packet_samples < AUDIO_IN_STREAM_PACKET_SIZE / 4U) {
            memset((uint8_t *)(packet + packet_samples), 0,
                   AUDIO_IN_STREAM_PACKET_SIZE - packet_samples * 4U);
        }

        flags = usb_lock();
        if (audio_mic_packet_state[selected] != AUDIO_MIC_PACKET_FILLING) {
            usb_unlock(flags);
            return;
        }
        audio_mic_packet_nonzero[selected] = nonzero != 0U ? 1U : 0U;
        audio_mic_packet_state[selected] = AUDIO_MIC_PACKET_READY;
        audio_mic_packet_producer_cursor = (uint8_t)(
            (selected + 1U) % AUDIO_MIC_PACKET_DEPTH);
        audio_mic_packet_count++;
        usb_diag.audio_mic_pcm_bytes += AUDIO_IN_STREAM_PACKET_SIZE;
        usb_diag.audio_mic_pcm_nonzero_samples += nonzero;
        sample_offset = (uint16_t)(sample_offset + packet_samples);
        if (sample_offset < samples) {
            uint8_t next = audio_mic_packet_producer_cursor;

            if (audio_mic_packet_state[next] != AUDIO_MIC_PACKET_FREE) {
                usb_diag.audio_mic_opus_dropped++;
                usb_unlock(flags);
                return;
            }
            audio_mic_packet_state[next] = AUDIO_MIC_PACKET_FILLING;
            selected = next;
        }
        usb_unlock(flags);
    }
}

static void __attribute__((unused)) process_audio_speaker(const uint8_t *data, uint32_t nbytes)
{
    static int16_t resampled[AUDIO_SPEAKER_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS];
    uint32_t frames;

    if (!data || nbytes < AUDIO_FRAME_BYTES || audio_speaker_mute) {
        return;
    }

    frames = nbytes / AUDIO_FRAME_BYTES;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *frame = data + (i * AUDIO_FRAME_BYTES);
        uint32_t out = (uint32_t)speaker_accum_frames * AUDIO_SPEAKER_CHANNELS;
        int16_t left = read_i16_le(frame);
        int16_t right = read_i16_le(frame + 2);

        speaker_accum[out] = left;
        speaker_accum[out + 1] = right;
        if (left || right) {
            speaker_accum_nonzero = true;
        }
        speaker_accum_frames++;

        if (speaker_accum_frames >= AUDIO_SPEAKER_FRAME_SAMPLES_UPSTREAM) {
            if (speaker_accum_nonzero) {
                resample_speaker_upstream_frame(resampled, speaker_accum);
                queue_speaker_frame(resampled);
            }
            speaker_accum_frames = 0;
            speaker_accum_nonzero = false;
        }
    }
}

static void __attribute__((unused)) process_audio_haptics(const uint8_t *data, uint32_t nbytes)
{
    uint32_t frames;
    bool speaker_packet;
    bool force_zero_haptics;

    if (!data || nbytes < AUDIO_FRAME_BYTES) {
        return;
    }

    speaker_packet = audio_packet_has_nonzero_speaker(data, nbytes);
    force_zero_haptics = audio_out_open &&
                         (speaker_packet ||
                          speaker_accum_nonzero ||
                          speaker_frame_queue_count > 0 ||
                          speaker_opus_queue_count > 0);

    if (!audio_packet_has_nonzero_haptics(data, nbytes) &&
        haptics_accum_pos == 0 &&
        !force_zero_haptics) {
        reset_haptics_accumulator();
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
                if (haptics_accum_nonzero || force_zero_haptics) {
                    queue_haptics_block(haptics_accum,
                                        haptics_accum_nonzero,
                                        haptics_accum_peak);
                }
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

static void audio_ingress_task(void *pvParameters)
{
    audio_ingress_view_t packet;
    uint32_t ingress_generation = 0;
    uint32_t expected_sequence = 0;
    bool sequence_valid = false;

    (void)pvParameters;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (take_audio_ingress(&packet)) {
#if CONFIG_M61_HPM_PROFILE
            uint64_t now_us = bflb_mtimer_get_time_us();
            uint64_t age_us = now_us >= packet.captured_us
                                  ? now_us - packet.captured_us
                                  : 0U;

            m61_perf_profile_record_ingress_age(
                age_us > UINT32_MAX ? UINT32_MAX : (uint32_t)age_us);
#endif
            if (!sequence_valid || packet.generation != ingress_generation) {
                ingress_generation = packet.generation;
                expected_sequence = packet.sequence;
                sequence_valid = true;
            }
            if (packet.sequence != expected_sequence) {
                audio_ingress_gaps++;
                release_audio_ingress(&packet);
                reset_audio_pipeline(false);
                sequence_valid = false;
                break;
            }
            expected_sequence = packet.sequence + 1U;
#if CONFIG_M61_PIPELINE_PROFILE
            uint64_t ingress_work_start_us = bflb_mtimer_get_time_us();
#endif
            m61_audio_epoch_ingest_usb(packet.data,
                                       packet.len,
                                       packet.generation,
                                       packet.captured_us,
                                       packet.speaker_enabled != 0U,
                                       HAPTICS_GAIN_Q8);
#if CONFIG_M61_PIPELINE_PROFILE
            uint64_t ingress_work_elapsed_us =
                bflb_mtimer_get_time_us() - ingress_work_start_us;
            m61_perf_profile_record_timing(
                M61_PERF_TIMING_INGRESS_WORK,
                ingress_work_elapsed_us > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)ingress_work_elapsed_us);
#endif
            release_audio_ingress(&packet);
        }
    }
}

static void __attribute__((aligned(32)))
audio_codec_task(void *pvParameters)
{
    OpusEncoder *encoder = NULL;
    OpusDecoder *decoder = NULL;
    static m61_audio_epoch_encode_job_t speaker_job;
    static int16_t speaker_frame[AUDIO_SPEAKER_FRAME_SAMPLES *
                                 AUDIO_SPEAKER_OPUS_CHANNELS];
    static uint8_t speaker_opus[M61_DS5_SPEAKER_OPUS_LEN];
    static uint8_t mic_opus[M61_DS5_MIC_OPUS_LEN];
    static int16_t mic_pcm[AUDIO_MIC_FRAME_SAMPLES * AUDIO_MIC_CHANNELS];
    uint64_t mic_created_us = 0U;
    TickType_t next_diag_tick;

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
                                       OPUS_APPLICATION_RESTRICTED_LOWDELAY);
        usb_diag.audio_codec_encoder_error = opus_error;
        if (opus_error != OPUS_OK) {
            printf("M61 Opus encoder init failed: %d\r\n", opus_error);
            encoder = NULL;
        } else {
            opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(AUDIO_SPEAKER_OPUS_BITRATE));
            opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
            opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(AUDIO_SPEAKER_OPUS_FORCE_CHANNELS));
            opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(AUDIO_SPEAKER_OPUS_BANDWIDTH));
            usb_diag.audio_speaker_opus_force_mono =
                (AUDIO_SPEAKER_OPUS_FORCE_CHANNELS == 1) ? 1 : 0;
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
        uint8_t speaker_budget = 1U;
        bool speaker_encoded = false;
        uint8_t codec_stages_ran = 0U;
        TickType_t now;

#if CONFIG_M61_HPM_PROFILE
        if (decoder && decoder_benchmark_requested != decoder_benchmark_active) {
            int reset_error = opus_decoder_ctl(decoder, OPUS_RESET_STATE);

            if (reset_error == OPUS_OK) {
                decoder_benchmark_active = decoder_benchmark_requested;
            } else {
                decoder_benchmark_active = false;
                decoder_benchmark_requested = false;
                usb_diag.audio_decoder_benchmark_errors++;
            }
        }
#endif

        while (speaker_budget > 0 && encoder &&
               m61_audio_epoch_take_encode_job(&speaker_job)) {
            codec_stages_ran++;
            uint64_t encode_start_us;
            uint64_t encode_elapsed_us;
#if CONFIG_M61_HPM_PROFILE
            m61_perf_counter_sample_t perf_start;
#endif
            usb_diag.audio_codec_stage = 3;
#if CONFIG_M61_PIPELINE_PROFILE
            uint64_t resample_start_us = bflb_mtimer_get_time_us();
#endif
            resample_epoch_speaker_mono(speaker_frame,
                                        speaker_job.speaker_pcm);
#if CONFIG_M61_PIPELINE_PROFILE
            uint64_t resample_elapsed_us =
                bflb_mtimer_get_time_us() - resample_start_us;
            m61_perf_profile_record_timing(
                M61_PERF_TIMING_RESAMPLE,
                resample_elapsed_us > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)resample_elapsed_us);
#endif
            encode_start_us = bflb_mtimer_get_time_us();
#if CONFIG_M61_HPM_PROFILE
            m61_perf_profile_counter_begin(&perf_start);
#endif
            int encoded = opus_encode(encoder,
                                      speaker_frame,
                                      AUDIO_SPEAKER_FRAME_SAMPLES,
                                      speaker_opus,
                                      sizeof(speaker_opus));
            if (encoded > 0 && encoded < M61_DS5_SPEAKER_OPUS_LEN) {
                int pad_error = opus_packet_pad(speaker_opus,
                                                encoded,
                                                M61_DS5_SPEAKER_OPUS_LEN);
                encoded = pad_error == OPUS_OK
                              ? M61_DS5_SPEAKER_OPUS_LEN
                              : pad_error;
            }
            encode_elapsed_us = bflb_mtimer_get_time_us() - encode_start_us;
            if (encode_elapsed_us > UINT32_MAX) {
                encode_elapsed_us = UINT32_MAX;
            }
#if CONFIG_M61_HPM_PROFILE
            m61_perf_profile_counter_end(&perf_start,
                                         (uint32_t)encode_elapsed_us);
#endif
            record_speaker_encode_diag((uint32_t)encode_elapsed_us, encoded);
            if (encoded > 0) {
#if CONFIG_M61_OPUS_PACKET_AUDIT
                record_opus_packet(true, speaker_opus, (uint16_t)encoded);
#endif
                speaker_encoded = true;
                if (m61_audio_epoch_complete_encode(speaker_job.generation,
                                                    speaker_job.epoch,
                                                    speaker_opus,
                                                    (size_t)encoded)) {
                    usb_diag.audio_speaker_encoded++;
                }
            } else {
                (void)m61_audio_epoch_complete_encode(speaker_job.generation,
                                                      speaker_job.epoch,
                                                      NULL,
                                                      0);
                usb_diag.audio_speaker_encode_errors++;
            }
            speaker_budget--;
        }

#if CONFIG_M61_HPM_PROFILE
        if (decoder && decoder_benchmark_active && speaker_encoded) {
            uint64_t decode_start_us;
            uint64_t decode_elapsed_us;
            m61_perf_counter_sample_t perf_start;

            usb_diag.audio_codec_stage = 4;
            decode_start_us = bflb_mtimer_get_time_us();
            m61_perf_profile_counter_begin(&perf_start);
            int decoded = opus_decode(decoder,
                                      decoder_benchmark_opus,
                                      M61_DS5_MIC_OPUS_LEN,
                                      mic_pcm,
                                      AUDIO_MIC_FRAME_SAMPLES,
                                      0);
            decode_elapsed_us = bflb_mtimer_get_time_us() - decode_start_us;
            if (decode_elapsed_us > UINT32_MAX) {
                decode_elapsed_us = UINT32_MAX;
            }
            m61_perf_profile_counter_end_decode(&perf_start,
                                                (uint32_t)decode_elapsed_us);
            if (decoded == AUDIO_MIC_FRAME_SAMPLES) {
                usb_diag.audio_decoder_benchmark_frames++;
            } else {
                usb_diag.audio_decoder_benchmark_errors++;
            }
        }
#endif

        if (decoder && take_mic_opus(mic_opus, &mic_created_us)) {
            codec_stages_ran++;
            uint64_t mic_queue_age_us = bflb_mtimer_get_time_us() - mic_created_us;
            usb_diag.audio_mic_queue_age_us_last =
                mic_queue_age_us > UINT32_MAX ? UINT32_MAX : (uint32_t)mic_queue_age_us;
            if (usb_diag.audio_mic_queue_age_us_last >
                usb_diag.audio_mic_queue_age_us_max) {
                usb_diag.audio_mic_queue_age_us_max =
                    usb_diag.audio_mic_queue_age_us_last;
            }
#if CONFIG_M61_HPM_PROFILE
            uint64_t decode_start_us;
            uint64_t decode_elapsed_us;
            m61_perf_counter_sample_t perf_start;
#endif
            usb_diag.audio_codec_stage = 4;
#if CONFIG_M61_OPUS_PACKET_AUDIT
            record_opus_packet(false, mic_opus, M61_DS5_MIC_OPUS_LEN);
#endif
#if CONFIG_M61_HPM_PROFILE
            decode_start_us = bflb_mtimer_get_time_us();
            m61_perf_profile_counter_begin(&perf_start);
#endif
            int decoded = opus_decode(decoder,
                                      mic_opus,
                                      M61_DS5_MIC_OPUS_LEN,
                                      mic_pcm,
                                      AUDIO_MIC_FRAME_SAMPLES,
                                      0);
#if CONFIG_M61_HPM_PROFILE
            decode_elapsed_us = bflb_mtimer_get_time_us() - decode_start_us;
            if (decode_elapsed_us > UINT32_MAX) {
                decode_elapsed_us = UINT32_MAX;
            }
            m61_perf_profile_counter_end_decode(&perf_start,
                                                (uint32_t)decode_elapsed_us);
#endif
            if (decoded > 0) {
                usb_diag.audio_mic_decoded++;
                push_mic_pcm_stereo(mic_pcm, (uint16_t)decoded);
            } else {
                usb_diag.audio_mic_decode_errors++;
            }
        }

        usb_diag.audio_codec_stage = 5;
        now = xTaskGetTickCount();
        if ((int32_t)(now - next_diag_tick) >= 0) {
            update_audio_codec_runtime_diag();
            next_diag_tick = now + pdMS_TO_TICKS(AUDIO_CODEC_DIAG_PERIOD_MS);
        }

        /*
         * Preserve the proven 2 ms BT window after a paired encode+decode
         * iteration.  A single-stage iteration only needs 1 ms before the
         * scheduler is re-evaluated; this accelerates queue recovery without
         * increasing idle polling or allowing back-to-back heavy stages.
         */
        vTaskDelay(pdMS_TO_TICKS(codec_stages_ran == 1U ? 1U : 2U));
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

static void ensure_audio_ingress_task(void)
{
    if (audio_ingress_task_handle != NULL) {
        return;
    }
    audio_ingress_task_handle = xTaskCreateStatic(audio_ingress_task,
                                                  "ds5_audio_in",
                                                  AUDIO_INGRESS_TASK_STACK_WORDS,
                                                  NULL,
                                                  AUDIO_INGRESS_TASK_PRIORITY,
                                                  audio_ingress_task_stack,
                                                  &audio_ingress_task_tcb);
    if (audio_ingress_task_handle == NULL) {
        printf("M61 audio ingress task create failed\r\n");
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
    if (report_type == HID_REPORT_FEATURE) {
        m61_usb_gamepad_host_report_t *queued;

        if (feature_set_queue_count >= FEATURE_SET_QUEUE_DEPTH) {
            usb_diag.host_report_dropped++;
            usb_unlock(flags);
            return;
        }
        queued = &feature_set_queue[feature_set_queue_head];
        queued->report_id = report_id;
        queued->report_type = report_type;
        queued->len = len;
        memcpy(queued->data, data, len);
        feature_set_queue_head = (uint8_t)(
            (feature_set_queue_head + 1U) % FEATURE_SET_QUEUE_DEPTH);
        feature_set_queue_count++;
        if (feature_set_queue_count > feature_set_queue_high_water) {
            feature_set_queue_high_water = feature_set_queue_count;
        }
    } else {
        if (pending_host_report_valid) {
            usb_diag.host_report_dropped++;
        }
        pending_host_report.report_id = report_id;
        pending_host_report.report_type = report_type;
        pending_host_report.len = len;
        memcpy(pending_host_report.data, data, len);
        pending_host_report_valid = true;
    }
    usb_diag.last_out_report_id = report_id ? report_id : data[0];
    usb_diag.last_out_report_len = len;
    update_last_out_state_diag(report_id, data, len);
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
    uintptr_t flags;
    int selected;
    int ret;

    if (!usb_ready || !usb_configured || !audio_out_open || usb_suspended ||
        audio_out_armed) {
        return;
    }

    flags = usb_lock();
    if (audio_out_armed || audio_ingress_active_slot !=
                               AUDIO_INGRESS_INVALID_SLOT) {
        usb_unlock(flags);
        return;
    }
    selected = find_free_audio_ingress_slot_locked();
    if (selected < 0) {
        audio_ingress_dropped++;
        usb_unlock(flags);
        return;
    }
    audio_ingress_slot_t *slot = &audio_ingress[selected];
    slot->generation = audio_generation;
    slot->speaker_enabled =
        (audio_out_open && !audio_speaker_mute) ? 1U : 0U;
    slot->state = AUDIO_INGRESS_DMA_ACTIVE;
    audio_ingress_active_slot = (uint8_t)selected;
    audio_out_armed = true;
    usb_unlock(flags);

    ret = usbd_ep_start_read(busid, AUDIO_OUT_EP,
                             audio_out_ring[selected], AUDIO_OUT_PACKET_SIZE);
    if (ret != 0) {
        flags = usb_lock();
        if (audio_ingress_active_slot == (uint8_t)selected) {
            audio_ingress_active_slot = AUDIO_INGRESS_INVALID_SLOT;
            slot->state = AUDIO_INGRESS_FREE;
        }
        audio_out_armed = false;
        audio_ingress_dropped++;
        usb_unlock(flags);
    }
}

static void arm_audio_in(uint8_t busid)
{
    uintptr_t flags;
    uint8_t selected = AUDIO_MIC_PACKET_INVALID_SLOT;
    uint8_t *buffer = audio_in_buffer;
    bool nonzero = false;

    if (!usb_ready || !usb_configured || !audio_in_open || audio_in_busy) {
        return;
    }

    flags = usb_lock();
    selected = audio_mic_packet_consumer_cursor;
    if (audio_mic_packet_state[selected] != AUDIO_MIC_PACKET_READY) {
        selected = AUDIO_MIC_PACKET_INVALID_SLOT;
    }
    if (selected != AUDIO_MIC_PACKET_INVALID_SLOT) {
        audio_mic_packet_state[selected] = AUDIO_MIC_PACKET_DMA_ACTIVE;
        audio_mic_packet_active_slot = selected;
        buffer = audio_mic_packet_ring[selected];
        nonzero = audio_mic_packet_nonzero[selected] != 0U;
    } else {
        audio_mic_packet_active_slot = AUDIO_MIC_PACKET_INVALID_SLOT;
        usb_diag.audio_mic_underflow++;
    }
    audio_in_busy = true;
    usb_unlock(flags);

    if (usbd_ep_start_write(busid, AUDIO_IN_EP, buffer,
                            AUDIO_IN_STREAM_PACKET_SIZE) == 0) {
        if (nonzero) {
            usb_diag.audio_mic_usb_nonzero_packets++;
            usb_diag.audio_mic_usb_nonzero_bytes +=
                AUDIO_IN_STREAM_PACKET_SIZE;
        }
    } else {
        flags = usb_lock();
        if (selected != AUDIO_MIC_PACKET_INVALID_SLOT &&
            audio_mic_packet_state[selected] == AUDIO_MIC_PACKET_DMA_ACTIVE) {
            audio_mic_packet_state[selected] = AUDIO_MIC_PACKET_READY;
        }
        audio_mic_packet_active_slot = AUDIO_MIC_PACKET_INVALID_SLOT;
        audio_in_busy = false;
        usb_unlock(flags);
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
            usb_input_pending = false;
            pending_host_report_valid = false;
            feature_set_queue_head = 0;
            feature_set_queue_tail = 0;
            feature_set_queue_count = 0;
            usb_suspended = false;
            usb_out_armed = false;
            audio_out_open = false;
            audio_in_open = false;
            audio_in_busy = false;
            flush_mic_queues();
            reset_audio_pipeline(true);
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
            reset_audio_pipeline(true);
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
    uintptr_t flags = usb_lock();
    usb_busy = false;
    usb_unlock(flags);
    usb_input_pump();
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
    uint64_t captured_us = bflb_mtimer_get_time_us();
    BaseType_t higher_priority_task_woken = pdFALSE;
    uintptr_t flags;
    uint8_t selected;
    bool packet_ready = false;

    (void)ep;

    flags = usb_lock();
    selected = audio_ingress_active_slot;
    audio_ingress_active_slot = AUDIO_INGRESS_INVALID_SLOT;
    audio_out_armed = false;
    if (selected < AUDIO_INGRESS_DEPTH &&
        audio_ingress[selected].state == AUDIO_INGRESS_DMA_ACTIVE) {
        audio_ingress_slot_t *slot = &audio_ingress[selected];

        if (nbytes > AUDIO_OUT_PACKET_SIZE) {
            nbytes = AUDIO_OUT_PACKET_SIZE;
        }
        if (nbytes > 0U) {
#if CONFIG_M61_PIPELINE_PROFILE
            uint32_t frames = nbytes / AUDIO_FRAME_BYTES;

            if (frames == 48U) {
                audio_out_frames_48_packets++;
            } else if (frames == 49U) {
                audio_out_frames_49_packets++;
            } else {
                audio_out_frames_other_packets++;
            }
            if (audio_out_last_capture_valid &&
                captured_us >= audio_out_last_captured_us) {
                uint64_t interval_us =
                    captured_us - audio_out_last_captured_us;
                uint32_t interval_u32 = interval_us > UINT32_MAX
                                            ? UINT32_MAX
                                            : (uint32_t)interval_us;

                audio_out_interval_samples++;
                audio_out_interval_us_last = interval_u32;
                audio_out_interval_us_total += interval_u32;
                if (audio_out_interval_us_min == 0U ||
                    interval_u32 < audio_out_interval_us_min) {
                    audio_out_interval_us_min = interval_u32;
                }
                if (interval_u32 > audio_out_interval_us_max) {
                    audio_out_interval_us_max = interval_u32;
                }
            }
            audio_out_last_captured_us = captured_us;
            audio_out_last_capture_valid = true;
#endif
            slot->captured_us = captured_us;
            slot->sequence = audio_ingress_sequence++;
            slot->len = (uint16_t)nbytes;
            slot->state = AUDIO_INGRESS_READY;
            audio_ingress_producer_cursor = (uint8_t)(
                (selected + 1U) % AUDIO_INGRESS_DEPTH);
            audio_ingress_count++;
            if (audio_ingress_count > audio_ingress_high_water) {
                audio_ingress_high_water = audio_ingress_count;
            }
            packet_ready = true;
        } else {
            slot->state = AUDIO_INGRESS_FREE;
        }
    }
    usb_unlock(flags);

    if (nbytes > 0U) {
        usb_diag.audio_out_packets++;
        usb_diag.audio_out_bytes += nbytes;
    }
    arm_audio_out(busid);
    if (packet_ready && audio_ingress_task_handle != NULL) {
        vTaskNotifyGiveFromISR(audio_ingress_task_handle,
                               &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

static void usbd_audio_in_ep_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    uintptr_t flags;
    uint8_t selected;

    (void)ep;

    flags = usb_lock();
    selected = audio_mic_packet_active_slot;
    audio_mic_packet_active_slot = AUDIO_MIC_PACKET_INVALID_SLOT;
    if (selected < AUDIO_MIC_PACKET_DEPTH &&
        audio_mic_packet_state[selected] == AUDIO_MIC_PACKET_DMA_ACTIVE) {
        audio_mic_packet_state[selected] = AUDIO_MIC_PACKET_FREE;
        audio_mic_packet_nonzero[selected] = 0U;
        audio_mic_packet_consumer_cursor = (uint8_t)(
            (selected + 1U) % AUDIO_MIC_PACKET_DEPTH);
        if (audio_mic_packet_count > 0U) {
            audio_mic_packet_count--;
        }
    }
    audio_in_busy = false;
    usb_unlock(flags);
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
    m61_perf_profile_init();
    memcpy(last_input_payload, ds5_idle_payload, sizeof(last_input_payload));
    memset(audio_in_buffer, 0, sizeof(audio_in_buffer));
    memset(audio_ingress, 0, sizeof(audio_ingress));
    audio_ingress_producer_cursor = 0U;
    audio_ingress_consumer_cursor = 0U;
    audio_ingress_active_slot = AUDIO_INGRESS_INVALID_SLOT;
    audio_ingress_count = 0U;
    audio_out_armed = false;
    flush_mic_queues();
    m61_audio_epoch_init(audio_generation);
    ensure_audio_ingress_task();
    ensure_audio_codec_task();
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
        reset_audio_pipeline(true);
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
    usb_input_pending = true;
    usb_unlock(flags);
    usb_input_pump();
}

static void resample_epoch_speaker_mono(int16_t *dst, const int16_t *src)
{
    uint32_t src_frame = 0U;
    uint32_t frac = 0U;

    for (uint32_t out_frame = 0; out_frame < AUDIO_SPEAKER_FRAME_SAMPLES;
         out_frame++) {
        uint32_t next_frame = src_frame + 1U;
        int32_t a;
        int32_t b;
        int32_t delta;

        a = ((int32_t)src[src_frame * 2U] +
             (int32_t)src[src_frame * 2U + 1U]) / 2;
        b = ((int32_t)src[next_frame * 2U] +
             (int32_t)src[next_frame * 2U + 1U]) / 2;
        delta = (b - a) * (int32_t)frac;
        if (delta >= 0) {
            delta += AUDIO_SPEAKER_FRAME_SAMPLES / 2;
        } else {
            delta -= AUDIO_SPEAKER_FRAME_SAMPLES / 2;
        }
        dst[out_frame] = clamp_i16(
            a + delta / AUDIO_SPEAKER_FRAME_SAMPLES);

        /* Exact recurrence for floor(out_frame * 512 / 480) and its
         * remainder.  frac+512 is in [512, 991], so the source advances by
         * exactly one or two frames without a divide or modulo operation. */
        frac += M61_AUDIO_EPOCH_USB_FRAMES;
        if (frac >= 2U * AUDIO_SPEAKER_FRAME_SAMPLES) {
            src_frame += 2U;
            frac -= 2U * AUDIO_SPEAKER_FRAME_SAMPLES;
        } else {
            src_frame++;
            frac -= AUDIO_SPEAKER_FRAME_SAMPLES;
        }
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
    } else if (feature_set_queue_count > 0) {
        *report = feature_set_queue[feature_set_queue_tail];
        feature_set_queue_tail = (uint8_t)(
            (feature_set_queue_tail + 1U) % FEATURE_SET_QUEUE_DEPTH);
        feature_set_queue_count--;
        valid = true;
    }
    usb_unlock(flags);
    return valid;
}

void m61_usb_gamepad_submit_mic_opus(const uint8_t *data, size_t len)
{
    uintptr_t flags;
    bool nonzero;
    uint64_t created_us;

    if (!CONFIG_M61_DS5_MIC_DEFAULT_ENABLED ||
        !data || len < M61_DS5_MIC_OPUS_LEN || !audio_in_open || audio_mic_mute) {
        return;
    }

    nonzero = bytes_have_nonzero(data, M61_DS5_MIC_OPUS_LEN);
    created_us = bflb_mtimer_get_time_us();
    flags = usb_lock();
    if (mic_opus_queue_count >= AUDIO_MIC_OPUS_QUEUE_DEPTH) {
        mic_opus_queue_tail = (uint8_t)((mic_opus_queue_tail + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
        mic_opus_queue_count--;
        usb_diag.audio_mic_opus_dropped++;
    }
    memcpy(mic_opus_queue[mic_opus_queue_head], data, M61_DS5_MIC_OPUS_LEN);
    mic_opus_created_us[mic_opus_queue_head] = created_us;
    mic_opus_queue_head = (uint8_t)((mic_opus_queue_head + 1U) % AUDIO_MIC_OPUS_QUEUE_DEPTH);
    mic_opus_queue_count++;
    usb_diag.audio_mic_opus_packets++;
    if (nonzero) {
        usb_diag.audio_mic_opus_nonzero++;
    }
    usb_unlock(flags);
}

int m61_usb_gamepad_set_decoder_benchmark(bool enabled)
{
#if CONFIG_M61_HPM_PROFILE
    decoder_benchmark_requested = enabled;
    return 0;
#else
    (void)enabled;
    return -1;
#endif
}

bool m61_usb_gamepad_decoder_benchmark_enabled(void)
{
#if CONFIG_M61_HPM_PROFILE
    return decoder_benchmark_active;
#else
    return false;
#endif
}

bool m61_usb_gamepad_audio_mic_enabled(void)
{
    return CONFIG_M61_DS5_MIC_DEFAULT_ENABLED ? true : false;
}

bool m61_usb_gamepad_audio_in_active(void)
{
    if (!CONFIG_M61_DS5_MIC_DEFAULT_ENABLED) {
        return false;
    }
    return audio_in_open && !audio_mic_mute;
}

bool m61_usb_gamepad_audio_speaker_active(void)
{
    return audio_out_open && !audio_speaker_mute;
}

uint32_t m61_usb_gamepad_audio_generation(void)
{
    return audio_generation;
}

void m61_usb_gamepad_realtime_task(void)
{
    usb_input_pump();
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

void m61_usb_gamepad_get_opus_packet_audit(m61_opus_packet_audit_t *audit)
{
    uint32_t sequence;

    if (audit == NULL) return;
    do {
        sequence = zz_opus_packet_audit.sequence;
        if (sequence & 1U) continue;
        __asm volatile("" : : : "memory");
        *audit = zz_opus_packet_audit.value;
        __asm volatile("" : : : "memory");
    } while (sequence != zz_opus_packet_audit.sequence || (sequence & 1U));
}

void m61_usb_gamepad_get_diag(m61_usb_gamepad_diag_t *diag)
{
    m61_audio_epoch_stats_t epoch_stats;
    m61_perf_profile_snapshot_t perf;

    if (!diag) {
        return;
    }
    m61_audio_epoch_get_stats(&epoch_stats);
    m61_perf_profile_get_snapshot(&perf);

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
    diag->feature_set_queue_depth = feature_set_queue_count;
    diag->feature_set_queue_high_water = feature_set_queue_high_water;
    diag->audio_open = usb_diag.audio_open;
    diag->audio_close = usb_diag.audio_close;
    diag->audio_out_packets = usb_diag.audio_out_packets;
    diag->audio_out_bytes = usb_diag.audio_out_bytes;
#if CONFIG_M61_PIPELINE_PROFILE
    {
        uintptr_t flags = usb_lock();

        diag->audio_out_frames_48_packets = audio_out_frames_48_packets;
        diag->audio_out_frames_49_packets = audio_out_frames_49_packets;
        diag->audio_out_frames_other_packets =
            audio_out_frames_other_packets;
        diag->audio_out_interval_samples = audio_out_interval_samples;
        diag->audio_out_interval_us_last = audio_out_interval_us_last;
        diag->audio_out_interval_us_average =
            audio_out_interval_samples != 0U
                ? (uint32_t)(audio_out_interval_us_total /
                             audio_out_interval_samples)
                : 0U;
        diag->audio_out_interval_us_min = audio_out_interval_us_min;
        diag->audio_out_interval_us_max = audio_out_interval_us_max;
        usb_unlock(flags);
    }
    diag->audio_epoch_interval_samples = epoch_stats.epoch_interval_samples;
    diag->audio_epoch_interval_us_last = epoch_stats.epoch_interval_us_last;
    diag->audio_epoch_interval_us_average =
        epoch_stats.epoch_interval_us_average;
    diag->audio_epoch_interval_us_min = epoch_stats.epoch_interval_us_min;
    diag->audio_epoch_interval_us_max = epoch_stats.epoch_interval_us_max;
#else
    diag->audio_out_frames_48_packets = 0U;
    diag->audio_out_frames_49_packets = 0U;
    diag->audio_out_frames_other_packets = 0U;
    diag->audio_out_interval_samples = 0U;
    diag->audio_out_interval_us_last = 0U;
    diag->audio_out_interval_us_average = 0U;
    diag->audio_out_interval_us_min = 0U;
    diag->audio_out_interval_us_max = 0U;
    diag->audio_epoch_interval_samples = 0U;
    diag->audio_epoch_interval_us_last = 0U;
    diag->audio_epoch_interval_us_average = 0U;
    diag->audio_epoch_interval_us_min = 0U;
    diag->audio_epoch_interval_us_max = 0U;
#endif
    diag->audio_ingress_dropped = audio_ingress_dropped;
    diag->audio_ingress_gaps = audio_ingress_gaps;
    diag->audio_in_packets = usb_diag.audio_in_packets;
    diag->audio_in_bytes = usb_diag.audio_in_bytes;
    diag->audio_haptic_blocks = epoch_stats.epochs_completed;
    diag->audio_haptic_sample_pairs = epoch_stats.haptics_sample_pairs;
    diag->audio_haptic_nonzero_blocks = epoch_stats.haptics_nonzero_epochs;
    diag->audio_haptic_queue_dropped = epoch_stats.epochs_dropped;
    diag->audio_haptic_deadline_pairs = epoch_stats.deadline_fallback_pairs;
    diag->audio_speaker_frames = epoch_stats.epochs_started;
    diag->audio_speaker_encoded = usb_diag.audio_speaker_encoded;
    diag->audio_speaker_encode_errors = usb_diag.audio_speaker_encode_errors;
    diag->audio_speaker_queue_dropped = epoch_stats.epochs_dropped;
    diag->audio_speaker_opus_dropped = epoch_stats.encode_failures;
    diag->audio_speaker_encode_cancelled = epoch_stats.encode_jobs_cancelled;
    diag->audio_speaker_encode_us_total = usb_diag.audio_speaker_encode_us_total;
    diag->audio_speaker_encode_us_last = usb_diag.audio_speaker_encode_us_last;
    diag->audio_speaker_encode_us_max = usb_diag.audio_speaker_encode_us_max;
    diag->perf_profile_enabled = perf.enabled ? 1U : 0U;
    diag->perf_encode_samples = perf.encode_samples;
    diag->perf_encode_us_p50 = perf.encode_us_p50;
    diag->perf_encode_us_p95 = perf.encode_us_p95;
    diag->perf_encode_us_p99 = perf.encode_us_p99;
    diag->perf_encode_cycles_last = perf.cycles_last;
    diag->perf_encode_cycles_average = perf.cycles_average;
    diag->perf_encode_cycles_max = perf.cycles_max;
    diag->perf_encode_instret_average = perf.instret_average;
    diag->perf_icache_access_average = perf.icache_access_average;
    diag->perf_icache_miss_average = perf.icache_miss_average;
    diag->perf_icache_miss_ppm = perf.icache_miss_ppm;
    diag->perf_dcache_read_average = perf.dcache_read_average;
    diag->perf_dcache_read_miss_average = perf.dcache_read_miss_average;
    diag->perf_dcache_read_miss_ppm = perf.dcache_read_miss_ppm;
    diag->perf_decode_samples = perf.decode_samples;
    diag->perf_decode_us_last = perf.decode_us_last;
    diag->perf_decode_us_average = perf.decode_us_average;
    diag->perf_decode_us_max = perf.decode_us_max;
    diag->perf_decode_us_p50 = perf.decode_us_p50;
    diag->perf_decode_us_p95 = perf.decode_us_p95;
    diag->perf_decode_us_p99 = perf.decode_us_p99;
    diag->perf_decode_cycles_last = perf.decode_cycles_last;
    diag->perf_decode_cycles_average = perf.decode_cycles_average;
    diag->perf_decode_cycles_max = perf.decode_cycles_max;
    diag->perf_decode_instret_average = perf.decode_instret_average;
    diag->perf_decode_icache_access_average =
        perf.decode_icache_access_average;
    diag->perf_decode_icache_miss_average = perf.decode_icache_miss_average;
    diag->perf_decode_icache_miss_ppm = perf.decode_icache_miss_ppm;
    diag->perf_decode_dcache_read_average =
        perf.decode_dcache_read_average;
    diag->perf_decode_dcache_read_miss_average =
        perf.decode_dcache_read_miss_average;
    diag->perf_decode_dcache_read_miss_ppm =
        perf.decode_dcache_read_miss_ppm;
    diag->perf_ingress_samples = perf.ingress_samples;
    diag->perf_ingress_age_us_last = perf.ingress_age_us_last;
    diag->perf_ingress_age_us_max = perf.ingress_age_us_max;
    diag->perf_ingress_age_us_p95 = perf.ingress_age_us_p95;
    diag->perf_ingress_age_us_p99 = perf.ingress_age_us_p99;
    diag->perf_irq_mask_cycles_max = perf.irq_mask_cycles_max;
    diag->audio_speaker_last_opus_len = usb_diag.audio_speaker_last_opus_len;
    diag->audio_mic_opus_packets = usb_diag.audio_mic_opus_packets;
    diag->audio_mic_opus_nonzero = usb_diag.audio_mic_opus_nonzero;
    diag->audio_mic_opus_dropped = usb_diag.audio_mic_opus_dropped;
    diag->audio_mic_decoded = usb_diag.audio_mic_decoded;
    diag->audio_mic_decode_errors = usb_diag.audio_mic_decode_errors;
    diag->audio_mic_queue_age_us_last = usb_diag.audio_mic_queue_age_us_last;
    diag->audio_mic_queue_age_us_max = usb_diag.audio_mic_queue_age_us_max;
    diag->audio_decoder_benchmark_frames =
        usb_diag.audio_decoder_benchmark_frames;
    diag->audio_decoder_benchmark_errors =
        usb_diag.audio_decoder_benchmark_errors;
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
#if CONFIG_M61_HPM_PROFILE
    diag->audio_decoder_benchmark_enabled = decoder_benchmark_active ? 1U : 0U;
#else
    diag->audio_decoder_benchmark_enabled = 0U;
#endif
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
    diag->audio_ingress_depth = audio_ingress_count;
    diag->audio_ingress_high_water = audio_ingress_high_water;
    diag->audio_haptic_queue_depth = epoch_stats.complete_slots;
    diag->audio_speaker_queue_depth = epoch_stats.encode_ready_slots;
    diag->audio_speaker_opus_queue_depth = epoch_stats.complete_slots;
    diag->audio_mic_opus_queue_depth = mic_opus_queue_count;
    diag->audio_mic_ring_bytes =
        (uint16_t)audio_mic_packet_count * AUDIO_IN_STREAM_PACKET_SIZE;
    diag->audio_codec_started = usb_diag.audio_codec_started;
    diag->audio_codec_stage = usb_diag.audio_codec_stage;
    diag->audio_codec_encoder_ready = usb_diag.audio_codec_encoder_ready;
    diag->audio_codec_decoder_ready = usb_diag.audio_codec_decoder_ready;
    diag->audio_speaker_opus_force_mono = usb_diag.audio_speaker_opus_force_mono;
    diag->audio_speaker_opus_bandwidth = usb_diag.audio_speaker_opus_bandwidth;
    diag->audio_speaker_opus_bitrate = usb_diag.audio_speaker_opus_bitrate;
    diag->audio_haptic_last_peak = epoch_stats.haptics_last_peak;
    diag->audio_haptic_downsample = HAPTICS_DOWNSAMPLE_FACTOR;
    diag->audio_haptic_resample_mode = HAPTICS_RESAMPLE_MODE_PHASE_DECIMATE;
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
        reset_audio_pipeline(true);
        arm_audio_out(busid);
    } else if (intf == ITF_NUM_AUDIO_STREAMING_IN) {
        audio_in_open = true;
        audio_in_busy = false;
        flush_mic_queues();
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
        reset_audio_pipeline(true);
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
        if (mute) {
            reset_audio_pipeline(false);
        }
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
bool m61_usb_gamepad_take_speaker_opus(uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}
void m61_usb_gamepad_submit_mic_opus(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}
int m61_usb_gamepad_set_decoder_benchmark(bool enabled)
{
    (void)enabled;
    return -1;
}
bool m61_usb_gamepad_decoder_benchmark_enabled(void) { return false; }
bool m61_usb_gamepad_audio_mic_enabled(void) { return false; }
bool m61_usb_gamepad_audio_in_active(void) { return false; }
bool m61_usb_gamepad_audio_speaker_active(void) { return false; }
uint32_t m61_usb_gamepad_audio_generation(void) { return 0; }
void m61_usb_gamepad_realtime_task(void) {}
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
void m61_usb_gamepad_get_opus_packet_audit(m61_opus_packet_audit_t *audit)
{
    if (audit) memset(audit, 0, sizeof(*audit));
}
#endif
