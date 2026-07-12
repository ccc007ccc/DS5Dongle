#pragma once

#include "m61_audio_epoch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M61_BT_TX_RT_DEPTH 2U
#define M61_BT_TX_RT_MAX_AGE_US 64000ULL
#define M61_BT_TX_MAX_RT_BURST 3U
#define M61_BT_TX_STATE31_REPORT_MAX 64U
#define M61_BT_TX_CLASS_MASK(tx_class) (1UL << (uint32_t)(tx_class))
#define M61_BT_TX_ELIGIBLE_ALL \
    (M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_REALTIME) | \
     M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE31) | \
     M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE32))

typedef enum {
    M61_BT_TX_CLASS_NONE = 0,
    M61_BT_TX_CLASS_REALTIME,
    M61_BT_TX_CLASS_STATE31,
    M61_BT_TX_CLASS_STATE32,
    M61_BT_TX_CLASS_COUNT,
} m61_bt_tx_class_t;

typedef enum {
    M61_BT_TX_FINISH_SUCCESS = 0,
    M61_BT_TX_FINISH_RETRY,
    M61_BT_TX_FINISH_DROP,
} m61_bt_tx_finish_t;

typedef struct {
    uint32_t accepted;
    uint32_t transmitted;
    uint32_t replaced;
    uint32_t stale;
    uint32_t retried;
    uint32_t dropped;
    uint32_t rejected;
    uint32_t pending;
} m61_bt_tx_class_metrics_t;

typedef struct {
    m61_bt_tx_class_metrics_t classes[M61_BT_TX_CLASS_COUNT];
} m61_bt_tx_metrics_t;

typedef struct {
    size_t len;
    bool includes_id;
    uint8_t report[M61_BT_TX_STATE31_REPORT_MAX];
} m61_bt_tx_state31_payload_t;

typedef struct {
    bool mic_active;
} m61_bt_tx_state32_payload_t;

typedef struct {
    m61_bt_tx_class_t tx_class;
    uint8_t slot;
    uint32_t generation;
    uint32_t version;
    uint64_t created_us;
    union {
        const m61_audio_epoch_pair_t *realtime;
        const m61_bt_tx_state31_payload_t *state31;
        const m61_bt_tx_state32_payload_t *state32;
    } payload;
} m61_bt_tx_selection_t;

typedef struct {
    bool pending;
    uint32_t version;
    uint64_t sequence;
    uint64_t created_us;
    m61_audio_epoch_pair_t payload;
} m61_bt_tx_rt_slot_t;

typedef struct {
    bool pending;
    uint32_t generation;
    uint32_t version;
    uint64_t created_us;
    m61_bt_tx_state31_payload_t payload;
} m61_bt_tx_state31_slot_t;

typedef struct {
    bool pending;
    uint32_t generation;
    uint32_t version;
    uint64_t created_us;
    m61_bt_tx_state32_payload_t payload;
} m61_bt_tx_state32_slot_t;

typedef struct {
    uint32_t generation;
    uint32_t next_version;
    uint64_t next_sequence;
    uint8_t rt_burst;
    m61_bt_tx_class_t next_state_class;
    m61_bt_tx_rt_slot_t realtime[M61_BT_TX_RT_DEPTH];
    m61_bt_tx_state31_slot_t state31;
    m61_bt_tx_state32_slot_t state32;
    m61_bt_tx_metrics_t metrics;
} m61_bt_tx_scheduler_t;

/* Callers serialize access; selections borrow immutable slot payloads. */
void m61_bt_tx_scheduler_init(m61_bt_tx_scheduler_t *scheduler,
                              uint32_t generation);
void m61_bt_tx_scheduler_reset_generation(m61_bt_tx_scheduler_t *scheduler,
                                          uint32_t generation);
bool m61_bt_tx_scheduler_publish_realtime(
    m61_bt_tx_scheduler_t *scheduler,
    const m61_audio_epoch_pair_t *pair);
bool m61_bt_tx_scheduler_ingest_epoch_pair(
    m61_bt_tx_scheduler_t *scheduler);
bool m61_bt_tx_scheduler_publish_state31(m61_bt_tx_scheduler_t *scheduler,
                                         const uint8_t *report,
                                         size_t len,
                                         bool includes_id,
                                         uint32_t generation,
                                         uint64_t created_us);
bool m61_bt_tx_scheduler_publish_state32(m61_bt_tx_scheduler_t *scheduler,
                                         bool mic_active,
                                         uint32_t generation,
                                         uint64_t created_us);
bool m61_bt_tx_scheduler_select(m61_bt_tx_scheduler_t *scheduler,
                                uint64_t now_us,
                                uint32_t eligible_classes,
                                m61_bt_tx_selection_t *selection);
bool m61_bt_tx_scheduler_selection_is_current(
    const m61_bt_tx_scheduler_t *scheduler,
    const m61_bt_tx_selection_t *selection);
bool m61_bt_tx_scheduler_finish(m61_bt_tx_scheduler_t *scheduler,
                                const m61_bt_tx_selection_t *selection,
                                m61_bt_tx_finish_t result);
void m61_bt_tx_scheduler_get_metrics(const m61_bt_tx_scheduler_t *scheduler,
                                     m61_bt_tx_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
