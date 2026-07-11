#include "bt_ds5_tx_scheduler.h"

#include "btstack_defines.h"
#include "btstack_run_loop.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "l2cap.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define DS5_TX_RT_REPORT_MAX 547U
#define DS5_TX_STATE31_MAX 78U
#define DS5_TX_STATE32_MAX 142U
#define DS5_TX_RT_STALE_US 64000ULL
#define DS5_TX_RT_MAX_BURST 3U
#define DS5_TX_REPORT_31 0x31U
#define DS5_TX_REPORT_32 0x32U
#define DS5_TX_REPORT_39 0x39U
#define DS5_HIDP_DATA_OUTPUT 0xA2U

typedef struct {
    ds5_sched_meta_t meta;
    uint32_t version;
    uint8_t state;
    uint8_t reserved[3];
    uint8_t payload[DS5_TX_RT_REPORT_MAX];
} bt_ds5_rt_slot_t;

typedef struct {
    ds5_sched_meta_t meta;
    uint32_t version;
    bool dirty;
    uint8_t reserved[3];
} bt_ds5_state_mailbox_header_t;

typedef struct {
    bt_ds5_state_mailbox_header_t header;
    uint8_t payload[DS5_TX_STATE31_MAX];
} bt_ds5_state31_mailbox_t;

typedef struct {
    bt_ds5_state_mailbox_header_t header;
    uint8_t payload[DS5_TX_STATE32_MAX];
} bt_ds5_state32_mailbox_t;

typedef struct {
    uint8_t sched_class;
    uint8_t rt_slot;
    uint32_t version;
    uint32_t generation;
    uint16_t report_len;
    uint64_t created_us;
    uint8_t report[DS5_TX_RT_REPORT_MAX];
} bt_ds5_tx_selection_t;

_Static_assert(sizeof(bt_ds5_rt_slot_t) == 584U,
               "ESP realtime slot exceeds scheduler budget");
_Static_assert(sizeof(bt_ds5_state31_mailbox_t) == 112U,
               "ESP state31 mailbox exceeds scheduler budget");
_Static_assert(sizeof(bt_ds5_state32_mailbox_t) == 176U,
               "ESP state32 mailbox exceeds scheduler budget");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bt_ds5_rt_slot_t s_rt[DS5_SCHED_ESP_RT_REPORT_CAPACITY];
static bt_ds5_state31_mailbox_t s_state31;
static bt_ds5_state32_mailbox_t s_state32;
static ds5_sched_metrics_t s_metrics;
static btstack_context_callback_registration_t s_kick_registration;
static bt_ds5_tx_finalize_report_cb_t s_finalize_report;
static bt_ds5_tx_note_result_cb_t s_note_result;
static uint16_t s_interrupt_cid;
static uint32_t s_generation;
static uint32_t s_sequence;
static uint8_t s_rt_burst;
static bool s_can_send_requested;
static bool s_kick_pending;
static uint64_t s_last_rt_send_us;
uint8_t g_ds5_owner_esp_tx_scheduler;

static ds5_sched_class_metrics_t *class_metrics(ds5_sched_class_t sched_class)
{
    if (sched_class < DS5_SCHED_RT_ACTUATION ||
        sched_class > DS5_SCHED_TELEMETRY) {
        return NULL;
    }
    return &s_metrics.classes[(uint8_t)sched_class - 1U];
}

static uint8_t pending_count_locked(void)
{
    uint8_t pending = 0;

    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (s_rt[i].state == DS5_SCHED_SLOT_READY ||
            s_rt[i].state == DS5_SCHED_SLOT_SENDING) {
            pending++;
        }
    }
    pending += s_state31.header.dirty ? 1U : 0U;
    pending += s_state32.header.dirty ? 1U : 0U;
    return pending;
}

static bool has_pending_locked(void)
{
    return pending_count_locked() != 0U;
}

static void clear_pending_locked(bool account_replaced)
{
    ds5_sched_class_metrics_t *rt_metrics =
        class_metrics(DS5_SCHED_RT_ACTUATION);

    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (account_replaced && s_rt[i].state != DS5_SCHED_SLOT_FREE &&
            rt_metrics != NULL) {
            rt_metrics->replaced++;
        }
        s_rt[i].state = DS5_SCHED_SLOT_FREE;
    }
    for (ds5_sched_class_t sched_class = DS5_SCHED_LATEST_STATE_31;
         sched_class <= DS5_SCHED_LATEST_STATE_32;
         sched_class++) {
        ds5_sched_class_metrics_t *metrics = class_metrics(sched_class);
        bool dirty = sched_class == DS5_SCHED_LATEST_STATE_31 ?
            s_state31.header.dirty : s_state32.header.dirty;

        if (account_replaced && dirty && metrics != NULL) {
            metrics->replaced++;
        }
        if (metrics != NULL) {
            metrics->pending = 0;
        }
    }
    if (rt_metrics != NULL) {
        rt_metrics->pending = 0;
    }
    s_state31.header.dirty = false;
    s_state32.header.dirty = false;
    s_rt_burst = 0;
    s_last_rt_send_us = 0;
}

static void request_can_send_on_main_thread(void)
{
    uint16_t cid = 0;

    portENTER_CRITICAL(&s_mux);
    s_kick_pending = false;
    if (s_interrupt_cid != 0U && has_pending_locked() &&
        !s_can_send_requested) {
        s_can_send_requested = true;
        cid = s_interrupt_cid;
    }
    portEXIT_CRITICAL(&s_mux);

    if (cid != 0U) {
        l2cap_request_can_send_now_event(cid);
    }
}

static void kick_callback(void *context)
{
    (void)context;
    request_can_send_on_main_thread();
}

static void schedule_kick(void)
{
    bool schedule = false;

    portENTER_CRITICAL(&s_mux);
    if (!s_kick_pending && s_interrupt_cid != 0U && has_pending_locked()) {
        s_kick_pending = true;
        schedule = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (schedule) {
        btstack_run_loop_execute_on_main_thread(&s_kick_registration);
    }
}

static int submit_rt(const uint8_t *report,
                     size_t report_len,
                     uint64_t created_us,
                     uint32_t generation)
{
    int selected = -1;
    int oldest_ready = -1;
    uint32_t oldest_sequence = UINT32_MAX;
    bool replaced = false;
    ds5_sched_class_metrics_t *metrics =
        class_metrics(DS5_SCHED_RT_ACTUATION);

    portENTER_CRITICAL(&s_mux);
    if (generation != s_generation) {
        portEXIT_CRITICAL(&s_mux);
        return -ESTALE;
    }
    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (s_rt[i].state == DS5_SCHED_SLOT_FREE) {
            selected = (int)i;
            break;
        }
        if (s_rt[i].state == DS5_SCHED_SLOT_READY &&
            s_rt[i].meta.sequence < oldest_sequence) {
            oldest_sequence = s_rt[i].meta.sequence;
            oldest_ready = (int)i;
        }
    }
    if (selected < 0) {
        selected = oldest_ready;
        if (selected >= 0 && metrics != NULL) {
            metrics->replaced++;
            replaced = true;
        }
    }
    if (selected >= 0) {
        bt_ds5_rt_slot_t *slot = &s_rt[selected];

        slot->state = DS5_SCHED_SLOT_WRITING;
        slot->version++;
        memcpy(slot->payload, report, report_len);
        ds5_sched_meta_init(&slot->meta,
                            created_us,
                            generation,
                            s_sequence++,
                            (uint16_t)report_len,
                            DS5_SCHED_RT_ACTUATION,
                            0);
        slot->state = DS5_SCHED_SLOT_READY;
        if (metrics != NULL) {
            metrics->accepted++;
            if (!replaced) {
                metrics->pending++;
                if (metrics->pending > metrics->highwater) {
                    metrics->highwater = metrics->pending;
                }
            }
        }
    } else if (metrics != NULL) {
        metrics->admission_rejected++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (selected < 0) {
        return -ENOBUFS;
    }
    schedule_kick();
    return 0;
}

static int publish_state(bt_ds5_state_mailbox_header_t *mailbox,
                         uint8_t *payload_storage,
                         size_t payload_capacity,
                         ds5_sched_class_t sched_class,
                         const uint8_t *report,
                         size_t report_len,
                         uint64_t created_us,
                         uint32_t generation)
{
    ds5_sched_class_metrics_t *metrics = class_metrics(sched_class);

    if (report_len > payload_capacity) {
        return -EMSGSIZE;
    }

    portENTER_CRITICAL(&s_mux);
    if (generation != s_generation) {
        portEXIT_CRITICAL(&s_mux);
        return -ESTALE;
    }
    if (mailbox->dirty && metrics != NULL) {
        metrics->coalesced++;
    }
    memcpy(payload_storage, report, report_len);
    mailbox->version++;
    ds5_sched_meta_init(&mailbox->meta,
                        created_us,
                        generation,
                        s_sequence++,
                        (uint16_t)report_len,
                        sched_class,
                        0);
    if (!mailbox->dirty && metrics != NULL) {
        metrics->pending++;
        if (metrics->pending > metrics->highwater) {
            metrics->highwater = metrics->pending;
        }
    }
    mailbox->dirty = true;
    if (metrics != NULL) {
        metrics->accepted++;
    }
    portEXIT_CRITICAL(&s_mux);
    schedule_kick();
    return 0;
}

static int oldest_ready_rt_locked(void)
{
    int selected = -1;
    uint32_t oldest_sequence = UINT32_MAX;

    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (s_rt[i].state == DS5_SCHED_SLOT_READY &&
            s_rt[i].meta.sequence < oldest_sequence) {
            selected = (int)i;
            oldest_sequence = s_rt[i].meta.sequence;
        }
    }
    return selected;
}

static void drop_stale_rt_locked(uint64_t now_us)
{
    ds5_sched_class_metrics_t *metrics =
        class_metrics(DS5_SCHED_RT_ACTUATION);

    while (true) {
        int selected = oldest_ready_rt_locked();
        if (selected < 0) {
            return;
        }
        bt_ds5_rt_slot_t *slot = &s_rt[selected];
        uint64_t age_us = now_us - slot->meta.created_us;
        if (age_us <= DS5_TX_RT_STALE_US) {
            return;
        }
        slot->state = DS5_SCHED_SLOT_FREE;
        if (metrics != NULL) {
            metrics->stale++;
            if (metrics->pending > 0U) {
                metrics->pending--;
            }
            metrics->age_last_us = age_us;
            if (age_us > metrics->age_max_us) {
                metrics->age_max_us = age_us;
            }
        }
    }
}

static bool select_next(uint64_t now_us, bt_ds5_tx_selection_t *selection)
{
    int rt_slot;
    bool state31_dirty;
    bool state32_dirty;

    memset(selection, 0, sizeof(*selection));
    portENTER_CRITICAL(&s_mux);
    drop_stale_rt_locked(now_us);
    rt_slot = oldest_ready_rt_locked();
    state31_dirty = s_state31.header.dirty;
    state32_dirty = s_state32.header.dirty;

    if (rt_slot >= 0 &&
        (s_rt_burst < DS5_TX_RT_MAX_BURST ||
         (!state31_dirty && !state32_dirty))) {
        bt_ds5_rt_slot_t *slot = &s_rt[rt_slot];

        slot->state = DS5_SCHED_SLOT_SENDING;
        selection->sched_class = DS5_SCHED_RT_ACTUATION;
        selection->rt_slot = (uint8_t)rt_slot;
        selection->version = slot->version;
        selection->generation = slot->meta.generation;
        selection->report_len = slot->meta.length;
        selection->created_us = slot->meta.created_us;
        memcpy(selection->report, slot->payload, slot->meta.length);
    } else if (state31_dirty) {
        selection->sched_class = DS5_SCHED_LATEST_STATE_31;
        selection->version = s_state31.header.version;
        selection->generation = s_state31.header.meta.generation;
        selection->report_len = s_state31.header.meta.length;
        selection->created_us = s_state31.header.meta.created_us;
        memcpy(selection->report,
               s_state31.payload,
               s_state31.header.meta.length);
    } else if (state32_dirty) {
        selection->sched_class = DS5_SCHED_LATEST_STATE_32;
        selection->version = s_state32.header.version;
        selection->generation = s_state32.header.meta.generation;
        selection->report_len = s_state32.header.meta.length;
        selection->created_us = s_state32.header.meta.created_us;
        memcpy(selection->report,
               s_state32.payload,
               s_state32.header.meta.length);
    } else if (rt_slot >= 0) {
        bt_ds5_rt_slot_t *slot = &s_rt[rt_slot];

        slot->state = DS5_SCHED_SLOT_SENDING;
        selection->sched_class = DS5_SCHED_RT_ACTUATION;
        selection->rt_slot = (uint8_t)rt_slot;
        selection->version = slot->version;
        selection->generation = slot->meta.generation;
        selection->report_len = slot->meta.length;
        selection->created_us = slot->meta.created_us;
        memcpy(selection->report, slot->payload, slot->meta.length);
    }
    portEXIT_CRITICAL(&s_mux);
    return selection->sched_class != 0U;
}

static void record_age_locked(ds5_sched_class_t sched_class,
                              uint64_t created_us,
                              uint64_t now_us)
{
    ds5_sched_class_metrics_t *metrics = class_metrics(sched_class);
    uint64_t age_us = now_us - created_us;

    if (metrics == NULL) {
        return;
    }
    metrics->age_last_us = age_us;
    if (age_us > metrics->age_max_us) {
        metrics->age_max_us = age_us;
    }
    metrics->age_sum_us += age_us;
    metrics->age_samples++;
}

static void complete_selection(const bt_ds5_tx_selection_t *selection,
                               bool success,
                               uint64_t now_us)
{
    ds5_sched_class_t sched_class =
        (ds5_sched_class_t)selection->sched_class;
    ds5_sched_class_metrics_t *metrics;

    portENTER_CRITICAL(&s_mux);
    metrics = class_metrics(sched_class);
    if (sched_class == DS5_SCHED_RT_ACTUATION) {
        bt_ds5_rt_slot_t *slot = &s_rt[selection->rt_slot];

        if (slot->state == DS5_SCHED_SLOT_SENDING &&
            slot->version == selection->version) {
            record_age_locked(sched_class, slot->meta.created_us, now_us);
            slot->state = DS5_SCHED_SLOT_FREE;
            if (metrics != NULL && metrics->pending > 0U) {
                metrics->pending--;
            }
        }
        if (success) {
            s_rt_burst = s_rt_burst < UINT8_MAX ? s_rt_burst + 1U : UINT8_MAX;
            if (s_last_rt_send_us != 0U) {
                uint64_t gap_us = now_us - s_last_rt_send_us;
                s_metrics.realtime.gap_last_us = gap_us;
                if (gap_us > s_metrics.realtime.gap_max_us) {
                    s_metrics.realtime.gap_max_us = gap_us;
                }
                if (gap_us > 25000U) {
                    s_metrics.realtime.gap_over_25ms++;
                }
                if (gap_us > 40000U) {
                    s_metrics.realtime.gap_over_40ms++;
                }
            }
            s_last_rt_send_us = now_us;
        }
    } else {
        bt_ds5_state_mailbox_header_t *mailbox =
            sched_class == DS5_SCHED_LATEST_STATE_31 ?
                &s_state31.header : &s_state32.header;

        record_age_locked(sched_class, selection->created_us, now_us);
        if (success && mailbox->dirty && mailbox->version == selection->version) {
            mailbox->dirty = false;
            if (metrics != NULL && metrics->pending > 0U) {
                metrics->pending--;
            }
        }
        if (success) {
            s_rt_burst = 0;
        }
    }
    if (metrics != NULL) {
        if (success) {
            metrics->transmitted++;
        } else {
            metrics->transport_failed++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static void rollback_selection(const bt_ds5_tx_selection_t *selection)
{
    if (selection->sched_class != DS5_SCHED_RT_ACTUATION ||
        selection->rt_slot >= DS5_SCHED_ESP_RT_REPORT_CAPACITY) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    bt_ds5_rt_slot_t *slot = &s_rt[selection->rt_slot];
    if (slot->state == DS5_SCHED_SLOT_SENDING &&
        slot->version == selection->version &&
        slot->meta.generation == selection->generation) {
        slot->state = selection->generation == s_generation ?
            DS5_SCHED_SLOT_READY : DS5_SCHED_SLOT_FREE;
        if (slot->state == DS5_SCHED_SLOT_FREE) {
            ds5_sched_class_metrics_t *metrics =
                class_metrics(DS5_SCHED_RT_ACTUATION);
            if (metrics != NULL) {
                metrics->stale++;
                if (metrics->pending > 0U) {
                    metrics->pending--;
                }
            }
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void bt_ds5_tx_scheduler_init(bt_ds5_tx_finalize_report_cb_t finalize_report,
                              bt_ds5_tx_note_result_cb_t note_result)
{
    g_ds5_owner_esp_tx_scheduler = 1U;
    portENTER_CRITICAL(&s_mux);
    memset(s_rt, 0, sizeof(s_rt));
    memset(&s_state31, 0, sizeof(s_state31));
    memset(&s_state32, 0, sizeof(s_state32));
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_finalize_report = finalize_report;
    s_note_result = note_result;
    s_interrupt_cid = 0;
    s_generation = 0;
    s_sequence = 0;
    s_rt_burst = 0;
    s_can_send_requested = false;
    s_kick_pending = false;
    s_last_rt_send_us = 0;
    portEXIT_CRITICAL(&s_mux);
    s_kick_registration.callback = kick_callback;
}

void bt_ds5_tx_scheduler_set_interrupt_channel(uint16_t cid)
{
    portENTER_CRITICAL(&s_mux);
    s_interrupt_cid = cid;
    s_can_send_requested = false;
    if (cid == 0U) {
        clear_pending_locked(true);
    }
    portEXIT_CRITICAL(&s_mux);
    if (cid != 0U) {
        schedule_kick();
    }
}

int bt_ds5_tx_scheduler_submit(const uint8_t *report,
                               size_t report_len,
                               bool realtime,
                               uint64_t created_us,
                               uint32_t generation)
{
    if (report == NULL || report_len < 1U ||
        report_len > DS5_TX_RT_REPORT_MAX) {
        return -EINVAL;
    }
    if (created_us == 0U) {
        created_us = (uint64_t)esp_timer_get_time();
    }
    portENTER_CRITICAL(&s_mux);
    bool generation_matches = generation == s_generation;
    portEXIT_CRITICAL(&s_mux);
    if (!generation_matches) {
        return -ESTALE;
    }
    if ((realtime || report[0] == DS5_TX_REPORT_39) &&
        report[0] == DS5_TX_REPORT_39) {
        if (report_len != DS5_TX_RT_REPORT_MAX) {
            return -EMSGSIZE;
        }
        return submit_rt(report,
                         report_len,
                         created_us,
                         generation);
    }
    if (report[0] == DS5_TX_REPORT_31) {
        if (report_len != DS5_TX_STATE31_MAX) {
            return -EMSGSIZE;
        }
        return publish_state(&s_state31.header,
                             s_state31.payload,
                             sizeof(s_state31.payload),
                             DS5_SCHED_LATEST_STATE_31,
                             report,
                             report_len,
                             created_us,
                             generation);
    }
    if (report[0] == DS5_TX_REPORT_32) {
        if (report_len != DS5_TX_STATE32_MAX) {
            return -EMSGSIZE;
        }
        return publish_state(&s_state32.header,
                             s_state32.payload,
                             sizeof(s_state32.payload),
                             DS5_SCHED_LATEST_STATE_32,
                             report,
                             report_len,
                             created_us,
                             generation);
    }
    return -ENOTSUP;
}

void bt_ds5_tx_scheduler_handle_can_send_now(void)
{
    bt_ds5_tx_selection_t selection;
    uint8_t packet[DS5_TX_RT_REPORT_MAX + 1U];
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint16_t cid;

    portENTER_CRITICAL(&s_mux);
    s_can_send_requested = false;
    cid = s_interrupt_cid;
    portEXIT_CRITICAL(&s_mux);

    if (cid == 0U || !select_next(now_us, &selection)) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    bool selection_current = cid == s_interrupt_cid &&
                             selection.generation == s_generation;
    portEXIT_CRITICAL(&s_mux);
    if (!selection_current) {
        rollback_selection(&selection);
        request_can_send_on_main_thread();
        return;
    }

    packet[0] = DS5_HIDP_DATA_OUTPUT;
    memcpy(packet + 1, selection.report, selection.report_len);
    if (s_finalize_report != NULL) {
        s_finalize_report(packet + 1, selection.report_len);
    }

    uint8_t status = l2cap_send(cid, packet, selection.report_len + 1U);
    bool success = status == ERROR_CODE_SUCCESS;
    if (s_note_result != NULL) {
        s_note_result(packet,
                      selection.report_len + 1U,
                      success ? 0 : -EIO);
    }
    complete_selection(&selection,
                       success,
                       (uint64_t)esp_timer_get_time());
    request_can_send_on_main_thread();
}

void bt_ds5_tx_scheduler_reset_generation(uint32_t generation)
{
    portENTER_CRITICAL(&s_mux);
    s_generation = generation;
    clear_pending_locked(true);
    portEXIT_CRITICAL(&s_mux);
}

void bt_ds5_tx_scheduler_reset_metrics(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t rt_pending = 0;
    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (s_rt[i].state == DS5_SCHED_SLOT_READY ||
            s_rt[i].state == DS5_SCHED_SLOT_SENDING) {
            rt_pending++;
        }
    }
    bool state31_pending = s_state31.header.dirty;
    bool state32_pending = s_state32.header.dirty;

    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].pending = rt_pending;
    s_metrics.classes[DS5_SCHED_RT_ACTUATION - 1U].highwater = rt_pending;
    s_metrics.classes[DS5_SCHED_LATEST_STATE_31 - 1U].pending =
        state31_pending ? 1U : 0U;
    s_metrics.classes[DS5_SCHED_LATEST_STATE_31 - 1U].highwater =
        state31_pending ? 1U : 0U;
    s_metrics.classes[DS5_SCHED_LATEST_STATE_32 - 1U].pending =
        state32_pending ? 1U : 0U;
    s_metrics.classes[DS5_SCHED_LATEST_STATE_32 - 1U].highwater =
        state32_pending ? 1U : 0U;
    s_last_rt_send_us = 0;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t bt_ds5_tx_scheduler_generation(void)
{
    uint32_t generation;

    portENTER_CRITICAL(&s_mux);
    generation = s_generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}

void bt_ds5_tx_scheduler_get_metrics(ds5_sched_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *metrics = s_metrics;
    portEXIT_CRITICAL(&s_mux);
}

uint8_t bt_ds5_tx_scheduler_pending_count(void)
{
    uint8_t pending;

    portENTER_CRITICAL(&s_mux);
    pending = pending_count_locked();
    portEXIT_CRITICAL(&s_mux);
    return pending;
}

uint8_t bt_ds5_tx_scheduler_free_count(void)
{
    uint8_t free_count = 0;

    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < DS5_SCHED_ESP_RT_REPORT_CAPACITY; i++) {
        if (s_rt[i].state == DS5_SCHED_SLOT_FREE) {
            free_count++;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return free_count;
}
