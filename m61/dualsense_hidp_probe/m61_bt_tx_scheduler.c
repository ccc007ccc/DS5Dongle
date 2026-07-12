#include "m61_bt_tx_scheduler.h"

#include <limits.h>
#include <string.h>

static m61_bt_tx_class_metrics_t *class_metrics(
    m61_bt_tx_scheduler_t *scheduler,
    m61_bt_tx_class_t tx_class)
{
    if (tx_class <= M61_BT_TX_CLASS_NONE ||
        tx_class >= M61_BT_TX_CLASS_COUNT) {
        return NULL;
    }
    return &scheduler->metrics.classes[tx_class];
}

static uint32_t next_version(m61_bt_tx_scheduler_t *scheduler)
{
    scheduler->next_version++;
    if (scheduler->next_version == 0U) {
        scheduler->next_version++;
    }
    return scheduler->next_version;
}

static void retire_metric(m61_bt_tx_scheduler_t *scheduler,
                          m61_bt_tx_class_t tx_class,
                          bool stale)
{
    m61_bt_tx_class_metrics_t *metrics = class_metrics(scheduler, tx_class);

    if (metrics == NULL) {
        return;
    }
    if (stale) {
        metrics->stale++;
    }
    if (metrics->pending > 0U) {
        metrics->pending--;
    }
}

static void clear_pending(m61_bt_tx_scheduler_t *scheduler, bool stale)
{
    for (size_t i = 0; i < M61_BT_TX_RT_DEPTH; i++) {
        if (scheduler->realtime[i].pending) {
            scheduler->realtime[i].pending = false;
            retire_metric(scheduler, M61_BT_TX_CLASS_REALTIME, stale);
        }
    }
    if (scheduler->state31.pending) {
        scheduler->state31.pending = false;
        retire_metric(scheduler, M61_BT_TX_CLASS_STATE31, stale);
    }
    if (scheduler->state32.pending) {
        scheduler->state32.pending = false;
        retire_metric(scheduler, M61_BT_TX_CLASS_STATE32, stale);
    }
    scheduler->rt_burst = 0U;
    scheduler->next_state_class = M61_BT_TX_CLASS_STATE31;
}

static int oldest_realtime(const m61_bt_tx_scheduler_t *scheduler)
{
    int selected = -1;
    uint64_t oldest = UINT64_MAX;

    for (size_t i = 0; i < M61_BT_TX_RT_DEPTH; i++) {
        if (scheduler->realtime[i].pending &&
            scheduler->realtime[i].sequence < oldest) {
            selected = (int)i;
            oldest = scheduler->realtime[i].sequence;
        }
    }
    return selected;
}

static void drop_stale_realtime(m61_bt_tx_scheduler_t *scheduler,
                                uint64_t now_us)
{
    for (size_t i = 0; i < M61_BT_TX_RT_DEPTH; i++) {
        m61_bt_tx_rt_slot_t *slot = &scheduler->realtime[i];
        uint64_t age_us;

        if (!slot->pending) {
            continue;
        }
        age_us = now_us >= slot->created_us ? now_us - slot->created_us : 0U;
        if (slot->payload.generation != scheduler->generation ||
            age_us > M61_BT_TX_RT_MAX_AGE_US) {
            slot->pending = false;
            retire_metric(scheduler, M61_BT_TX_CLASS_REALTIME, true);
        }
    }
}

static void drop_wrong_generation_state(m61_bt_tx_scheduler_t *scheduler)
{
    if (scheduler->state31.pending &&
        scheduler->state31.generation != scheduler->generation) {
        scheduler->state31.pending = false;
        retire_metric(scheduler, M61_BT_TX_CLASS_STATE31, true);
    }
    if (scheduler->state32.pending &&
        scheduler->state32.generation != scheduler->generation) {
        scheduler->state32.pending = false;
        retire_metric(scheduler, M61_BT_TX_CLASS_STATE32, true);
    }
}

static m61_bt_tx_class_t select_state_class(
    const m61_bt_tx_scheduler_t *scheduler,
    uint32_t eligible_classes)
{
    if (scheduler->next_state_class == M61_BT_TX_CLASS_STATE32) {
        if (scheduler->state32.pending &&
            (eligible_classes & M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE32))) {
            return M61_BT_TX_CLASS_STATE32;
        }
        return scheduler->state31.pending &&
                       (eligible_classes &
                        M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE31))
                   ? M61_BT_TX_CLASS_STATE31
                   : M61_BT_TX_CLASS_NONE;
    }
    if (scheduler->state31.pending &&
        (eligible_classes & M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE31))) {
        return M61_BT_TX_CLASS_STATE31;
    }
    return scheduler->state32.pending &&
                   (eligible_classes &
                    M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_STATE32))
               ? M61_BT_TX_CLASS_STATE32
               : M61_BT_TX_CLASS_NONE;
}

void m61_bt_tx_scheduler_init(m61_bt_tx_scheduler_t *scheduler,
                              uint32_t generation)
{
    if (scheduler == NULL) {
        return;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->generation = generation;
    scheduler->next_state_class = M61_BT_TX_CLASS_STATE31;
}

void m61_bt_tx_scheduler_reset_generation(m61_bt_tx_scheduler_t *scheduler,
                                          uint32_t generation)
{
    if (scheduler == NULL) {
        return;
    }
    clear_pending(scheduler, true);
    scheduler->generation = generation;
}

bool m61_bt_tx_scheduler_publish_realtime(
    m61_bt_tx_scheduler_t *scheduler,
    const m61_audio_epoch_pair_t *pair)
{
    int selected = -1;
    m61_bt_tx_class_metrics_t *metrics;

    if (scheduler == NULL || pair == NULL) {
        return false;
    }
    metrics = class_metrics(scheduler, M61_BT_TX_CLASS_REALTIME);
    if (pair->generation != scheduler->generation) {
        metrics->rejected++;
        return false;
    }
    for (size_t i = 0; i < M61_BT_TX_RT_DEPTH; i++) {
        if (!scheduler->realtime[i].pending) {
            selected = (int)i;
            break;
        }
    }
    if (selected < 0) {
        selected = oldest_realtime(scheduler);
        metrics->replaced++;
    } else {
        metrics->pending++;
    }

    m61_bt_tx_rt_slot_t *slot = &scheduler->realtime[selected];
    slot->pending = false;
    slot->version = next_version(scheduler);
    slot->sequence = scheduler->next_sequence++;
    slot->created_us = pair->first_captured_us;
    slot->payload = *pair;
    slot->pending = true;
    metrics->accepted++;
    return true;
}

bool m61_bt_tx_scheduler_ingest_epoch_pair(
    m61_bt_tx_scheduler_t *scheduler)
{
    int selected = -1;
    m61_bt_tx_class_metrics_t *metrics;
    m61_bt_tx_rt_slot_t *slot;

    if (scheduler == NULL) {
        return false;
    }
    metrics = class_metrics(scheduler, M61_BT_TX_CLASS_REALTIME);
    for (size_t i = 0; i < M61_BT_TX_RT_DEPTH; i++) {
        if (!scheduler->realtime[i].pending) {
            selected = (int)i;
            break;
        }
    }
    if (selected < 0) {
        return false;
    }

    slot = &scheduler->realtime[selected];
    if (!m61_audio_epoch_take_adjacent_pair(&slot->payload)) {
        return false;
    }
    if (slot->payload.generation != scheduler->generation) {
        metrics->rejected++;
        return false;
    }

    slot->version = next_version(scheduler);
    slot->sequence = scheduler->next_sequence++;
    slot->created_us = slot->payload.first_captured_us;
    slot->pending = true;
    metrics->pending++;
    metrics->accepted++;
    return true;
}

bool m61_bt_tx_scheduler_publish_state31(m61_bt_tx_scheduler_t *scheduler,
                                         const uint8_t *report,
                                         size_t len,
                                         bool includes_id,
                                         uint32_t generation,
                                         uint64_t created_us)
{
    m61_bt_tx_class_metrics_t *metrics;

    if (scheduler == NULL || (report == NULL && len != 0U) ||
        len > M61_BT_TX_STATE31_REPORT_MAX) {
        return false;
    }
    metrics = class_metrics(scheduler, M61_BT_TX_CLASS_STATE31);
    if (generation != scheduler->generation) {
        metrics->rejected++;
        return false;
    }
    if (scheduler->state31.pending) {
        metrics->replaced++;
    } else {
        metrics->pending++;
    }
    if (len != 0U) {
        memcpy(scheduler->state31.payload.report, report, len);
    }
    scheduler->state31.payload.len = len;
    scheduler->state31.payload.includes_id = includes_id;
    scheduler->state31.generation = generation;
    scheduler->state31.created_us = created_us;
    scheduler->state31.version = next_version(scheduler);
    scheduler->state31.pending = true;
    metrics->accepted++;
    return true;
}

bool m61_bt_tx_scheduler_publish_state32(m61_bt_tx_scheduler_t *scheduler,
                                         bool mic_active,
                                         uint32_t generation,
                                         uint64_t created_us)
{
    m61_bt_tx_class_metrics_t *metrics;

    if (scheduler == NULL) {
        return false;
    }
    metrics = class_metrics(scheduler, M61_BT_TX_CLASS_STATE32);
    if (generation != scheduler->generation) {
        metrics->rejected++;
        return false;
    }
    if (scheduler->state32.pending) {
        metrics->replaced++;
    } else {
        metrics->pending++;
    }
    scheduler->state32.payload.mic_active = mic_active;
    scheduler->state32.generation = generation;
    scheduler->state32.created_us = created_us;
    scheduler->state32.version = next_version(scheduler);
    scheduler->state32.pending = true;
    metrics->accepted++;
    return true;
}

bool m61_bt_tx_scheduler_select(m61_bt_tx_scheduler_t *scheduler,
                                uint64_t now_us,
                                uint32_t eligible_classes,
                                m61_bt_tx_selection_t *selection)
{
    int rt_slot;
    m61_bt_tx_class_t state_class;

    if (scheduler == NULL || selection == NULL) {
        return false;
    }
    selection->tx_class = M61_BT_TX_CLASS_NONE;
    selection->slot = 0U;
    selection->generation = 0U;
    selection->version = 0U;
    selection->created_us = 0U;
    selection->payload.realtime = NULL;
    drop_stale_realtime(scheduler, now_us);
    drop_wrong_generation_state(scheduler);
    rt_slot = oldest_realtime(scheduler);
    state_class = select_state_class(scheduler, eligible_classes);

    if (rt_slot >= 0 &&
        (eligible_classes & M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_REALTIME)) &&
        (scheduler->rt_burst < M61_BT_TX_MAX_RT_BURST ||
         state_class == M61_BT_TX_CLASS_NONE)) {
        const m61_bt_tx_rt_slot_t *slot = &scheduler->realtime[rt_slot];

        selection->tx_class = M61_BT_TX_CLASS_REALTIME;
        selection->slot = (uint8_t)rt_slot;
        selection->generation = slot->payload.generation;
        selection->version = slot->version;
        selection->created_us = slot->created_us;
        selection->payload.realtime = &slot->payload;
        return true;
    }

    if (state_class == M61_BT_TX_CLASS_STATE31) {
        selection->tx_class = state_class;
        selection->generation = scheduler->state31.generation;
        selection->version = scheduler->state31.version;
        selection->created_us = scheduler->state31.created_us;
        selection->payload.state31 = &scheduler->state31.payload;
        return true;
    }
    if (state_class == M61_BT_TX_CLASS_STATE32) {
        selection->tx_class = state_class;
        selection->generation = scheduler->state32.generation;
        selection->version = scheduler->state32.version;
        selection->created_us = scheduler->state32.created_us;
        selection->payload.state32 = &scheduler->state32.payload;
        return true;
    }

    if (rt_slot >= 0 &&
        (eligible_classes & M61_BT_TX_CLASS_MASK(M61_BT_TX_CLASS_REALTIME))) {
        const m61_bt_tx_rt_slot_t *slot = &scheduler->realtime[rt_slot];

        selection->tx_class = M61_BT_TX_CLASS_REALTIME;
        selection->slot = (uint8_t)rt_slot;
        selection->generation = slot->payload.generation;
        selection->version = slot->version;
        selection->created_us = slot->created_us;
        selection->payload.realtime = &slot->payload;
        return true;
    }
    return false;
}

bool m61_bt_tx_scheduler_selection_is_current(
    const m61_bt_tx_scheduler_t *scheduler,
    const m61_bt_tx_selection_t *selection)
{
    if (scheduler == NULL || selection == NULL ||
        selection->generation != scheduler->generation) {
        return false;
    }
    switch (selection->tx_class) {
    case M61_BT_TX_CLASS_REALTIME:
        return selection->slot < M61_BT_TX_RT_DEPTH &&
               scheduler->realtime[selection->slot].pending &&
               scheduler->realtime[selection->slot].version ==
                   selection->version;
    case M61_BT_TX_CLASS_STATE31:
        return scheduler->state31.pending &&
               scheduler->state31.version == selection->version;
    case M61_BT_TX_CLASS_STATE32:
        return scheduler->state32.pending &&
               scheduler->state32.version == selection->version;
    default:
        return false;
    }
}

bool m61_bt_tx_scheduler_finish(m61_bt_tx_scheduler_t *scheduler,
                                const m61_bt_tx_selection_t *selection,
                                m61_bt_tx_finish_t result)
{
    bool current;
    m61_bt_tx_class_metrics_t *metrics;

    if (scheduler == NULL || selection == NULL ||
        (int)result < (int)M61_BT_TX_FINISH_SUCCESS ||
        result > M61_BT_TX_FINISH_DROP) {
        return false;
    }
    metrics = class_metrics(scheduler, selection->tx_class);
    if (metrics == NULL || selection->generation != scheduler->generation) {
        return false;
    }
    current = m61_bt_tx_scheduler_selection_is_current(scheduler, selection);
    if (result == M61_BT_TX_FINISH_RETRY) {
        if (current) {
            metrics->retried++;
        }
        return current;
    }
    if (!current) {
        return false;
    }
    if (result == M61_BT_TX_FINISH_DROP) {
        metrics->dropped++;
    } else {
        metrics->transmitted++;
    }

    switch (selection->tx_class) {
    case M61_BT_TX_CLASS_REALTIME:
        scheduler->realtime[selection->slot].pending = false;
        retire_metric(scheduler, selection->tx_class, false);
        if (scheduler->rt_burst < UINT8_MAX) {
            scheduler->rt_burst++;
        }
        break;
    case M61_BT_TX_CLASS_STATE31:
        scheduler->state31.pending = false;
        retire_metric(scheduler, selection->tx_class, false);
        if (result == M61_BT_TX_FINISH_SUCCESS) {
            scheduler->rt_burst = 0U;
            scheduler->next_state_class = M61_BT_TX_CLASS_STATE32;
        }
        break;
    case M61_BT_TX_CLASS_STATE32:
        scheduler->state32.pending = false;
        retire_metric(scheduler, selection->tx_class, false);
        if (result == M61_BT_TX_FINISH_SUCCESS) {
            scheduler->rt_burst = 0U;
            scheduler->next_state_class = M61_BT_TX_CLASS_STATE31;
        }
        break;
    default:
        return false;
    }
    return true;
}

void m61_bt_tx_scheduler_get_metrics(const m61_bt_tx_scheduler_t *scheduler,
                                     m61_bt_tx_metrics_t *metrics)
{
    if (scheduler == NULL || metrics == NULL) {
        return;
    }
    *metrics = scheduler->metrics;
}
