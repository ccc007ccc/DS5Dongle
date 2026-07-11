#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_SCHED_RT_ACTUATION = 1,
    DS5_SCHED_LATEST_STATE_31,
    DS5_SCHED_LATEST_STATE_32,
    DS5_SCHED_RELIABLE_CONTROL,
    DS5_SCHED_INPUT_REALTIME,
    DS5_SCHED_MIC_STREAM,
    DS5_SCHED_LINK_STATE,
    DS5_SCHED_TELEMETRY,
} ds5_sched_class_t;

#define DS5_SCHED_CLASS_COUNT 8U

typedef enum {
    DS5_SCHED_SLOT_FREE = 0,
    DS5_SCHED_SLOT_WRITING,
    DS5_SCHED_SLOT_READY,
    DS5_SCHED_SLOT_SENDING,
    DS5_SCHED_SLOT_EVICTED,
} ds5_sched_slot_state_t;

typedef struct {
    uint64_t created_us;
    uint32_t generation;
    uint32_t sequence;
    uint32_t reserved;
    uint16_t length;
    uint8_t sched_class;
    uint8_t flags;
} ds5_sched_meta_t;

/* The byte-sized state keeps the slot header independent of enum ABI flags. */
typedef struct {
    uint32_t version;
    uint8_t state;
    uint8_t reserved[3];
} ds5_sched_slot_control_t;

typedef struct {
    uint64_t accepted;
    uint64_t transmitted;
    uint64_t pending;
    uint64_t highwater;
    uint64_t coalesced;
    uint64_t replaced;
    uint64_t stale;
    uint64_t admission_rejected;
    uint64_t transport_failed;
    uint64_t age_last_us;
    uint64_t age_max_us;
    uint64_t age_sum_us;
    uint64_t age_samples;
} ds5_sched_class_metrics_t;

typedef struct {
    uint64_t gap_last_us;
    uint64_t gap_max_us;
    uint64_t gap_over_25ms;
    uint64_t gap_over_40ms;
    uint64_t epoch_discontinuity;
} ds5_sched_rt_metrics_t;

typedef struct {
    ds5_sched_class_metrics_t classes[DS5_SCHED_CLASS_COUNT];
    ds5_sched_rt_metrics_t realtime;
} ds5_sched_metrics_t;

/* Normative M61 capacities from the realtime scheduler specification. */
#define DS5_SCHED_M61_AUDIO_EPOCH_CAPACITY 8U
#define DS5_SCHED_M61_RT_REPORT_CAPACITY 2U
#define DS5_SCHED_M61_STATE31_CAPACITY 1U
#define DS5_SCHED_M61_STATE32_CAPACITY 1U
#define DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY 8U
#define DS5_SCHED_M61_INPUT_REALTIME_CAPACITY 4U
#define DS5_SCHED_M61_MIC_OPUS_CAPACITY 4U
#define DS5_SCHED_M61_FEATURE_TRANSACTION_CAPACITY 8U
#define DS5_SCHED_M61_LINK_STATE_CAPACITY 1U
#define DS5_SCHED_M61_SCHED_STATUS_CAPACITY 1U
#define DS5_SCHED_M61_TELEMETRY_CAPACITY 1U
#define DS5_SCHED_M61_USB_AUDIO_INGRESS_CAPACITY 4U
#define DS5_SCHED_M61_USB_CONTROL_CAPACITY 8U
#define DS5_SCHED_M61_CONTROL_SMALL_POOL_CAPACITY 8U
#define DS5_SCHED_M61_CONTROL_LARGE_POOL_CAPACITY 2U

/* Normative ESP32 capacities from the realtime scheduler specification. */
#define DS5_SCHED_ESP_RT_REPORT_CAPACITY 2U
#define DS5_SCHED_ESP_STATE31_CAPACITY 1U
#define DS5_SCHED_ESP_STATE32_CAPACITY 1U
#define DS5_SCHED_ESP_RELIABLE_CONTROL_CAPACITY 8U
#define DS5_SCHED_ESP_INPUT_RESPONSE_CAPACITY 4U
#define DS5_SCHED_ESP_MIC_RESPONSE_CAPACITY 4U
#define DS5_SCHED_ESP_FEATURE_RESPONSE_CAPACITY 8U
#define DS5_SCHED_ESP_LINK_STATE_CAPACITY 1U
#define DS5_SCHED_ESP_SCHED_STATUS_CAPACITY 1U
#define DS5_SCHED_ESP_TELEMETRY_CAPACITY 1U
#define DS5_SCHED_ESP_CONTROL_SMALL_POOL_CAPACITY 8U
#define DS5_SCHED_ESP_CONTROL_LARGE_POOL_CAPACITY 2U
#define DS5_SCHED_ESP_FEATURE_SMALL_POOL_CAPACITY 8U
#define DS5_SCHED_ESP_FEATURE_LARGE_POOL_CAPACITY 2U

/* Budget constants are shared with tools/scheduler_ram_budget.py. */
#define DS5_SCHED_M61_TYPED_STORAGE_LIMIT_BYTES (32U * 1024U)
#define DS5_SCHED_ESP_TYPED_STORAGE_LIMIT_BYTES (16U * 1024U)
#define DS5_SCHED_M61_PLANNED_TYPED_STORAGE_BYTES 29416U
#define DS5_SCHED_ESP_PLANNED_TYPED_STORAGE_BYTES 10952U
#define DS5_SCHED_METRICS_STORAGE_BYTES 2048U

void ds5_sched_meta_init(ds5_sched_meta_t *meta,
                         uint64_t created_us,
                         uint32_t generation,
                         uint32_t sequence,
                         uint16_t length,
                         ds5_sched_class_t sched_class,
                         uint8_t flags);

#ifdef __cplusplus
}
#endif
