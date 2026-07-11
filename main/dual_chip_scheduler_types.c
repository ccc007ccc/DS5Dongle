#include "dual_chip_scheduler_types.h"

#include <stddef.h>

#if defined(__GNUC__)
#define DS5_SCHED_ALIGN8 __attribute__((aligned(8)))
#else
#define DS5_SCHED_ALIGN8
#endif

#define DS5_SCHED_AUDIO_HAPTICS_BYTES 64U
#define DS5_SCHED_AUDIO_PCM_SAMPLES (512U * 2U)
#define DS5_SCHED_AUDIO_OPUS_BYTES 200U
#define DS5_SCHED_RT_REPORT_BYTES 547U
#define DS5_SCHED_STATE31_BYTES 78U
#define DS5_SCHED_STATE32_BYTES 142U
#define DS5_SCHED_MIC_OPUS_BYTES 71U
#define DS5_SCHED_CONTROL_DESCRIPTOR_BYTES 40U
#define DS5_SCHED_SMALL_PAYLOAD_BYTES 160U
#define DS5_SCHED_LARGE_PAYLOAD_BYTES 672U
#define DS5_SCHED_FEATURE_TRANSACTION_BYTES 32U
#define DS5_SCHED_LINK_STATE_BYTES 56U
#define DS5_SCHED_STATUS_MAILBOX_BYTES 64U
#define DS5_SCHED_TELEMETRY_BYTES 512U
#define DS5_SCHED_USB_AUDIO_PACKET_BYTES 392U
#define DS5_SCHED_PENDING_DESCRIPTOR_BYTES 64U

typedef struct {
    uint8_t haptics[DS5_SCHED_AUDIO_HAPTICS_BYTES];
    int16_t speaker_pcm[DS5_SCHED_AUDIO_PCM_SAMPLES];
    uint8_t speaker_opus[DS5_SCHED_AUDIO_OPUS_BYTES];
} ds5_sched_audio_epoch_payload_t;

typedef struct DS5_SCHED_ALIGN8 {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    ds5_sched_audio_epoch_payload_t payload;
} ds5_sched_audio_epoch_slot_t;

typedef struct DS5_SCHED_ALIGN8 {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[DS5_SCHED_RT_REPORT_BYTES];
} ds5_sched_rt_report_slot_t;

typedef struct DS5_SCHED_ALIGN8 {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[DS5_SCHED_STATE31_BYTES];
} ds5_sched_state31_slot_t;

typedef struct DS5_SCHED_ALIGN8 {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[DS5_SCHED_STATE32_BYTES];
} ds5_sched_state32_slot_t;

typedef struct DS5_SCHED_ALIGN8 {
    ds5_sched_meta_t meta;
    ds5_sched_slot_control_t control;
    uint8_t payload[DS5_SCHED_MIC_OPUS_BYTES];
} ds5_sched_mic_opus_slot_t;

typedef struct DS5_SCHED_ALIGN8 {
    uint64_t captured_us;
    uint16_t length;
    uint16_t flags;
    uint8_t payload[DS5_SCHED_USB_AUDIO_PACKET_BYTES];
} ds5_sched_usb_audio_ingress_slot_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_CONTROL_DESCRIPTOR_BYTES];
} ds5_sched_control_descriptor_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_SMALL_PAYLOAD_BYTES];
} ds5_sched_small_payload_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_LARGE_PAYLOAD_BYTES];
} ds5_sched_large_payload_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_FEATURE_TRANSACTION_BYTES];
} ds5_sched_feature_transaction_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_LINK_STATE_BYTES];
} ds5_sched_link_state_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_STATUS_MAILBOX_BYTES];
} ds5_sched_status_mailbox_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_TELEMETRY_BYTES];
} ds5_sched_telemetry_storage_t;

typedef struct {
    uint8_t bytes[DS5_SCHED_PENDING_DESCRIPTOR_BYTES];
} ds5_sched_pending_descriptor_storage_t;

typedef union {
    ds5_sched_metrics_t typed;
    uint8_t reserved[DS5_SCHED_METRICS_STORAGE_BYTES];
} ds5_sched_metrics_storage_t;

typedef struct {
    ds5_sched_audio_epoch_slot_t
        audio_epochs[DS5_SCHED_M61_AUDIO_EPOCH_CAPACITY];
    ds5_sched_rt_report_slot_t
        rt_reports[DS5_SCHED_M61_RT_REPORT_CAPACITY];
    ds5_sched_state31_slot_t state31[DS5_SCHED_M61_STATE31_CAPACITY];
    ds5_sched_state32_slot_t state32[DS5_SCHED_M61_STATE32_CAPACITY];
    ds5_sched_control_descriptor_storage_t
        control_descriptors[DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY];
    ds5_sched_small_payload_storage_t
        control_small_payloads[DS5_SCHED_M61_CONTROL_SMALL_POOL_CAPACITY];
    ds5_sched_large_payload_storage_t
        control_large_payloads[DS5_SCHED_M61_CONTROL_LARGE_POOL_CAPACITY];
    ds5_sched_state31_slot_t
        input_realtime[DS5_SCHED_M61_INPUT_REALTIME_CAPACITY];
    ds5_sched_mic_opus_slot_t mic_opus[DS5_SCHED_M61_MIC_OPUS_CAPACITY];
    ds5_sched_feature_transaction_storage_t
        feature_transactions[DS5_SCHED_M61_FEATURE_TRANSACTION_CAPACITY];
    ds5_sched_link_state_storage_t
        link_state[DS5_SCHED_M61_LINK_STATE_CAPACITY];
    ds5_sched_status_mailbox_storage_t
        scheduler_status[DS5_SCHED_M61_SCHED_STATUS_CAPACITY];
    ds5_sched_telemetry_storage_t
        telemetry[DS5_SCHED_M61_TELEMETRY_CAPACITY];
    ds5_sched_usb_audio_ingress_slot_t
        usb_audio_ingress[DS5_SCHED_M61_USB_AUDIO_INGRESS_CAPACITY];
    ds5_sched_control_descriptor_storage_t
        usb_control_descriptors[DS5_SCHED_M61_USB_CONTROL_CAPACITY];
    uint8_t usb_control_payloads[DS5_SCHED_M61_USB_CONTROL_CAPACITY][64];
    ds5_sched_metrics_storage_t metrics;
} ds5_sched_m61_storage_plan_t;

typedef struct {
    ds5_sched_rt_report_slot_t
        rt_reports[DS5_SCHED_ESP_RT_REPORT_CAPACITY];
    ds5_sched_state31_slot_t state31[DS5_SCHED_ESP_STATE31_CAPACITY];
    ds5_sched_state32_slot_t state32[DS5_SCHED_ESP_STATE32_CAPACITY];
    ds5_sched_control_descriptor_storage_t
        control_descriptors[DS5_SCHED_ESP_RELIABLE_CONTROL_CAPACITY];
    ds5_sched_small_payload_storage_t
        control_small_payloads[DS5_SCHED_ESP_CONTROL_SMALL_POOL_CAPACITY];
    ds5_sched_large_payload_storage_t
        control_large_payloads[DS5_SCHED_ESP_CONTROL_LARGE_POOL_CAPACITY];
    ds5_sched_state31_slot_t
        input_responses[DS5_SCHED_ESP_INPUT_RESPONSE_CAPACITY];
    ds5_sched_mic_opus_slot_t
        mic_responses[DS5_SCHED_ESP_MIC_RESPONSE_CAPACITY];
    ds5_sched_control_descriptor_storage_t
        feature_descriptors[DS5_SCHED_ESP_FEATURE_RESPONSE_CAPACITY];
    ds5_sched_small_payload_storage_t
        feature_small_payloads[DS5_SCHED_ESP_FEATURE_SMALL_POOL_CAPACITY];
    ds5_sched_large_payload_storage_t
        feature_large_payloads[DS5_SCHED_ESP_FEATURE_LARGE_POOL_CAPACITY];
    ds5_sched_link_state_storage_t
        link_state[DS5_SCHED_ESP_LINK_STATE_CAPACITY];
    ds5_sched_status_mailbox_storage_t
        scheduler_status[DS5_SCHED_ESP_SCHED_STATUS_CAPACITY];
    ds5_sched_telemetry_storage_t
        telemetry[DS5_SCHED_ESP_TELEMETRY_CAPACITY];
    ds5_sched_pending_descriptor_storage_t pending_descriptor;
    ds5_sched_metrics_storage_t metrics;
} ds5_sched_esp_storage_plan_t;

_Static_assert(DS5_SCHED_CLASS_COUNT == DS5_SCHED_TELEMETRY,
               "scheduler class count drift");
_Static_assert(sizeof(ds5_sched_meta_t) == 24U,
               "scheduler metadata must be 24 bytes");
_Static_assert(offsetof(ds5_sched_meta_t, created_us) == 0U,
               "scheduler metadata timestamp offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, generation) == 8U,
               "scheduler metadata generation offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, sequence) == 12U,
               "scheduler metadata sequence offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, reserved) == 16U,
               "scheduler metadata reserved offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, length) == 20U,
               "scheduler metadata length offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, sched_class) == 22U,
               "scheduler metadata class offset drift");
_Static_assert(offsetof(ds5_sched_meta_t, flags) == 23U,
               "scheduler metadata flags offset drift");
_Static_assert(sizeof(ds5_sched_slot_control_t) == 8U,
               "scheduler slot control budget drift");
_Static_assert(sizeof(ds5_sched_class_metrics_t) == 104U,
               "scheduler class metrics layout drift");
_Static_assert(sizeof(ds5_sched_rt_metrics_t) == 40U,
               "scheduler realtime metrics layout drift");
_Static_assert(sizeof(ds5_sched_metrics_t) <= DS5_SCHED_METRICS_STORAGE_BYTES,
               "scheduler metrics exceed reserved storage");
_Static_assert(sizeof(ds5_sched_audio_epoch_payload_t) == 2312U,
               "audio epoch payload budget drift");
_Static_assert(sizeof(ds5_sched_audio_epoch_slot_t) == 2344U,
               "audio epoch slot budget drift");
_Static_assert(sizeof(ds5_sched_rt_report_slot_t) == 584U,
               "realtime report slot budget drift");
_Static_assert(sizeof(ds5_sched_state31_slot_t) == 112U,
               "state31 slot budget drift");
_Static_assert(sizeof(ds5_sched_state32_slot_t) == 176U,
               "state32 slot budget drift");
_Static_assert(sizeof(ds5_sched_mic_opus_slot_t) == 104U,
               "mic Opus slot budget drift");
_Static_assert(sizeof(ds5_sched_usb_audio_ingress_slot_t) == 408U,
               "USB audio ingress slot budget drift");
_Static_assert(sizeof(ds5_sched_metrics_storage_t) ==
                   DS5_SCHED_METRICS_STORAGE_BYTES,
               "scheduler metrics reservation drift");
_Static_assert(sizeof(ds5_sched_m61_storage_plan_t) ==
                   DS5_SCHED_M61_PLANNED_TYPED_STORAGE_BYTES,
               "M61 scheduler typed storage budget drift");
_Static_assert(sizeof(ds5_sched_esp_storage_plan_t) ==
                   DS5_SCHED_ESP_PLANNED_TYPED_STORAGE_BYTES,
               "ESP32 scheduler typed storage budget drift");
_Static_assert(sizeof(ds5_sched_m61_storage_plan_t) <=
                   DS5_SCHED_M61_TYPED_STORAGE_LIMIT_BYTES,
               "M61 scheduler typed storage exceeds 32 KiB");
_Static_assert(sizeof(ds5_sched_esp_storage_plan_t) <=
                   DS5_SCHED_ESP_TYPED_STORAGE_LIMIT_BYTES,
               "ESP32 scheduler typed storage exceeds 16 KiB");

#define DS5_SCHED_M61_TOTAL_RAM_BYTES (415U * 1024U)
#define DS5_SCHED_M61_CURRENT_STATIC_BYTES 268800U
#define DS5_SCHED_M61_REPLACED_STATIC_BYTES 14542U
#define DS5_SCHED_M61_STATIC_CONTINGENCY_BYTES (8U * 1024U)
#define DS5_SCHED_M61_CURRENT_MIN_HEAP_BYTES 154544U
#define DS5_SCHED_M61_RUNTIME_HEAP_DELTA_BYTES 6400U
#define DS5_SCHED_M61_HEAP_CONTINGENCY_BYTES (8U * 1024U)
#define DS5_SCHED_ESP_DRAM_TOTAL_BYTES 124580U
#define DS5_SCHED_ESP_CURRENT_STATIC_BYTES 71604U
#define DS5_SCHED_ESP_REPLACED_STATIC_BYTES 12560U
#define DS5_SCHED_ESP_STATIC_CONTINGENCY_BYTES (8U * 1024U)

#define DS5_SCHED_M61_PROJECTED_STATIC_BYTES                                \
    (DS5_SCHED_M61_CURRENT_STATIC_BYTES -                                   \
     DS5_SCHED_M61_REPLACED_STATIC_BYTES +                                  \
     sizeof(ds5_sched_m61_storage_plan_t) +                                 \
     DS5_SCHED_M61_STATIC_CONTINGENCY_BYTES)
#define DS5_SCHED_M61_PROJECTED_MIN_HEAP_BYTES                              \
    (DS5_SCHED_M61_CURRENT_MIN_HEAP_BYTES -                                 \
     DS5_SCHED_M61_RUNTIME_HEAP_DELTA_BYTES -                               \
     (sizeof(ds5_sched_m61_storage_plan_t) -                                \
      DS5_SCHED_M61_REPLACED_STATIC_BYTES) -                                \
     DS5_SCHED_M61_STATIC_CONTINGENCY_BYTES -                               \
     DS5_SCHED_M61_HEAP_CONTINGENCY_BYTES)
#define DS5_SCHED_ESP_PROJECTED_STATIC_BYTES                                \
    (DS5_SCHED_ESP_CURRENT_STATIC_BYTES -                                   \
     DS5_SCHED_ESP_REPLACED_STATIC_BYTES +                                  \
     sizeof(ds5_sched_esp_storage_plan_t) +                                 \
     DS5_SCHED_ESP_STATIC_CONTINGENCY_BYTES)

_Static_assert(DS5_SCHED_M61_PROJECTED_STATIC_BYTES == 291866U,
               "M61 projected static RAM model drift");
_Static_assert(DS5_SCHED_M61_PROJECTED_STATIC_BYTES <
                   (DS5_SCHED_M61_TOTAL_RAM_BYTES * 75U) / 100U,
               "M61 projected static RAM exceeds 75 percent");
_Static_assert(DS5_SCHED_M61_PROJECTED_MIN_HEAP_BYTES == 116886U,
               "M61 projected minimum heap model drift");
_Static_assert(DS5_SCHED_M61_PROJECTED_MIN_HEAP_BYTES >= (100U * 1024U),
               "M61 projected minimum heap is below 100 KiB");
_Static_assert(DS5_SCHED_ESP_PROJECTED_STATIC_BYTES == 78188U,
               "ESP32 projected static DRAM model drift");
_Static_assert(DS5_SCHED_ESP_PROJECTED_STATIC_BYTES <
                   (DS5_SCHED_ESP_DRAM_TOTAL_BYTES * 70U) / 100U,
               "ESP32 projected static DRAM exceeds 70 percent");

void ds5_sched_meta_init(ds5_sched_meta_t *meta,
                         uint64_t created_us,
                         uint32_t generation,
                         uint32_t sequence,
                         uint16_t length,
                         ds5_sched_class_t sched_class,
                         uint8_t flags)
{
    if (meta == NULL) {
        return;
    }

    *meta = (ds5_sched_meta_t){
        .created_us = created_us,
        .generation = generation,
        .sequence = sequence,
        .reserved = 0U,
        .length = length,
        .sched_class = (uint8_t)sched_class,
        .flags = flags,
    };
}
