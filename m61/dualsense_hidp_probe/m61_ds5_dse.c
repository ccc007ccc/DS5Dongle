#include "m61_ds5_dse.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bflb_core.h"
#include "bflb_irq.h"
#include "dualsense_output.h"
#include "task.h"

#define DSE_FEATURE_20_HANDSHAKE_LEN 61U
#define DSE_FEATURE_80_LEN 59U
#define DSE_FEATURE_REQUEST_LEN 64U
#define DSE_PROFILE_REPORT_FIRST 0x70U
#define DSE_PROFILE_REPORT_LAST 0x7BU
#define DSE_PROFILE_WRITE_FIRST 0x60U
#define DSE_PROFILE_WRITE_LAST 0x62U
#define DSE_PROFILE_STATUS_REPORT 0x81U
#define DSE_UNLOCK_WAIT_MS 4000U
#define DSE_PREFETCH_INTERVAL_MS 80U
#define DSE_POST_SAVE_UNLOCK_DELAY_MS 500U
#define DSE_POST_SAVE_STATUS_DELAY_MS 1000U
#define DSE_POST_SAVE_STATUS_INTERVAL_MS 250U
#define DSE_POST_SAVE_STATUS_POLLS 6U
#define DSE_POST_SAVE_REFETCH_DELAY_MS 5500U

typedef struct {
    m61_ds5_dse_ops_t ops;
    TickType_t unlock_wait_due;
    TickType_t prefetch_due;
    TickType_t post_save_started;
    uint8_t prefetch_next_report;
    uint8_t post_save_round;
    uint8_t feature20_body[DSE_FEATURE_20_HANDSHAKE_LEN];
    bool is_edge;
    bool profiles_ready;
    bool feature20_valid;
    bool unlock_pending;
    bool unlock_wait_active;
    bool prefetch_active;
    bool prefetch_mark_ready;
    bool post_save_active;
} m61_ds5_dse_state_t;

static m61_ds5_dse_state_t s_dse;

static uintptr_t dse_lock(void)
{
    return bflb_irq_save();
}

static void dse_unlock(uintptr_t flags)
{
    bflb_irq_restore(flags);
}

static bool tick_due(TickType_t now, TickType_t due)
{
    return (int32_t)(now - due) >= 0;
}

static void dse_reset_state_locked(void)
{
    s_dse.unlock_wait_due = 0;
    s_dse.prefetch_due = 0;
    s_dse.post_save_started = 0;
    s_dse.prefetch_next_report = DSE_PROFILE_REPORT_FIRST;
    s_dse.post_save_round = 0;
    memset(s_dse.feature20_body, 0, sizeof(s_dse.feature20_body));
    s_dse.is_edge = false;
    s_dse.profiles_ready = true;
    s_dse.feature20_valid = false;
    s_dse.unlock_pending = false;
    s_dse.unlock_wait_active = false;
    s_dse.prefetch_active = false;
    s_dse.prefetch_mark_ready = false;
    s_dse.post_save_active = false;
}

static void dse_start_prefetch_locked(TickType_t now, bool mark_ready_after)
{
    s_dse.prefetch_active = true;
    s_dse.prefetch_mark_ready = mark_ready_after;
    s_dse.prefetch_next_report = DSE_PROFILE_REPORT_FIRST;
    s_dse.prefetch_due = now;
}

static void dse_build_unlock_feature80(uint8_t *body)
{
    uint8_t packet[1 + DSE_FEATURE_80_LEN];

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x80;
    packet[1] = DSE_PROFILE_REPORT_FIRST;
    packet[2] = 0x01;
    dualsense_feature_fill_crc(packet, sizeof(packet));
    memcpy(body, packet + 1, DSE_FEATURE_80_LEN);
}

static bool dse_send_initial_unlock(TickType_t now)
{
    m61_ds5_dse_ops_t ops;
    uint8_t handshake[DSE_FEATURE_20_HANDSHAKE_LEN];
    uint8_t unlock80[DSE_FEATURE_80_LEN];
    uintptr_t flags;
    int err;

    flags = dse_lock();
    if (!s_dse.unlock_pending || !s_dse.feature20_valid ||
        s_dse.ops.set_feature_exact == NULL) {
        dse_unlock(flags);
        return false;
    }
    ops = s_dse.ops;
    memcpy(handshake, s_dse.feature20_body, sizeof(handshake));
    dse_unlock(flags);

    err = ops.set_feature_exact(ops.ctx, 0x65, handshake, sizeof(handshake));
    if (err != 0) {
        return false;
    }

    dse_build_unlock_feature80(unlock80);
    err = ops.set_feature_exact(ops.ctx, 0x80, unlock80, sizeof(unlock80));
    if (err != 0) {
        return false;
    }

    flags = dse_lock();
    if (s_dse.unlock_pending) {
        s_dse.unlock_pending = false;
        s_dse.unlock_wait_active = true;
        s_dse.unlock_wait_due = now + pdMS_TO_TICKS(DSE_UNLOCK_WAIT_MS);
    }
    dse_unlock(flags);

    printf("[DSE] Sent 0x65/0x80 unlock\r\n");
    return true;
}

static bool dse_finish_unlock_wait(TickType_t now)
{
    uintptr_t flags = dse_lock();

    if (!s_dse.unlock_wait_active || !tick_due(now, s_dse.unlock_wait_due)) {
        dse_unlock(flags);
        return false;
    }

    s_dse.unlock_wait_active = false;
    dse_start_prefetch_locked(now, true);
    dse_unlock(flags);

    printf("[DSE] Unlock wait done; prefetching 0x70-0x7B\r\n");
    return true;
}

static bool dse_run_prefetch(TickType_t now)
{
    m61_ds5_dse_ops_t ops;
    uint8_t report_id;
    bool mark_ready = false;
    bool ready_now = false;
    uintptr_t flags;
    int err;

    flags = dse_lock();
    if (!s_dse.prefetch_active || !tick_due(now, s_dse.prefetch_due) ||
        s_dse.ops.request_feature == NULL) {
        dse_unlock(flags);
        return false;
    }
    ops = s_dse.ops;
    report_id = s_dse.prefetch_next_report;
    mark_ready = s_dse.prefetch_mark_ready;
    dse_unlock(flags);

    err = ops.request_feature(ops.ctx, report_id, DSE_FEATURE_REQUEST_LEN);
    if (err != 0) {
        return false;
    }

    flags = dse_lock();
    if (s_dse.prefetch_active && s_dse.prefetch_next_report == report_id) {
        if (report_id >= DSE_PROFILE_REPORT_LAST) {
            s_dse.prefetch_active = false;
            s_dse.prefetch_mark_ready = false;
            if (mark_ready) {
                s_dse.profiles_ready = true;
                ready_now = true;
            }
        } else {
            s_dse.prefetch_next_report++;
            s_dse.prefetch_due = now + pdMS_TO_TICKS(DSE_PREFETCH_INTERVAL_MS);
        }
    }
    dse_unlock(flags);

    if (ready_now) {
        printf("[DSE] Profile snapshot ready\r\n");
    }
    return true;
}

static bool dse_run_post_save(TickType_t now)
{
    m61_ds5_dse_ops_t ops;
    uint8_t unlock80[DSE_FEATURE_80_LEN];
    uintptr_t flags;
    uint8_t round;
    TickType_t started;
    int err;

    flags = dse_lock();
    if (!s_dse.post_save_active || !s_dse.is_edge) {
        dse_unlock(flags);
        return false;
    }
    ops = s_dse.ops;
    round = s_dse.post_save_round;
    started = s_dse.post_save_started;

    if (round == 0 &&
        tick_due(now, started + pdMS_TO_TICKS(DSE_POST_SAVE_UNLOCK_DELAY_MS)) &&
        ops.set_feature_exact != NULL) {
        dse_unlock(flags);

        dse_build_unlock_feature80(unlock80);
        err = ops.set_feature_exact(ops.ctx, 0x80, unlock80, sizeof(unlock80));
        if (err != 0) {
            return false;
        }

        flags = dse_lock();
        if (s_dse.post_save_active && s_dse.post_save_round == 0) {
            s_dse.post_save_round = 1;
        }
        dse_unlock(flags);

        printf("[DSE] Post-save: re-sent 0x80\r\n");
        return true;
    }

    if (round >= 1 && round <= DSE_POST_SAVE_STATUS_POLLS &&
        tick_due(now,
                 started + pdMS_TO_TICKS(DSE_POST_SAVE_STATUS_DELAY_MS +
                                          DSE_POST_SAVE_STATUS_INTERVAL_MS * (uint32_t)(round - 1))) &&
        ops.request_feature != NULL) {
        dse_unlock(flags);

        err = ops.request_feature(ops.ctx, DSE_PROFILE_STATUS_REPORT, DSE_FEATURE_REQUEST_LEN);
        if (err != 0) {
            return false;
        }

        flags = dse_lock();
        if (s_dse.post_save_active && s_dse.post_save_round == round) {
            s_dse.post_save_round = (uint8_t)(round + 1);
        }
        dse_unlock(flags);
        return true;
    }

    if (round == (uint8_t)(DSE_POST_SAVE_STATUS_POLLS + 1U) &&
        tick_due(now, started + pdMS_TO_TICKS(DSE_POST_SAVE_REFETCH_DELAY_MS))) {
        dse_start_prefetch_locked(now, false);
        s_dse.post_save_round = (uint8_t)(DSE_POST_SAVE_STATUS_POLLS + 2U);
        s_dse.post_save_active = false;
        dse_unlock(flags);

        printf("[DSE] Post-save: profile snapshot refetch started\r\n");
        return true;
    }

    dse_unlock(flags);
    return false;
}

void m61_ds5_dse_init(const m61_ds5_dse_ops_t *ops)
{
    uintptr_t flags = dse_lock();

    memset(&s_dse, 0, sizeof(s_dse));
    if (ops != NULL) {
        s_dse.ops = *ops;
    }
    dse_reset_state_locked();
    dse_unlock(flags);
}

void m61_ds5_dse_reset(void)
{
    uintptr_t flags = dse_lock();

    dse_reset_state_locked();
    dse_unlock(flags);
}

void m61_ds5_dse_note_feature_report(uint8_t report_id, const uint8_t *data, size_t len)
{
    uintptr_t flags;
    bool detected_edge = false;

    if (data == NULL || len == 0) {
        return;
    }

    flags = dse_lock();
    if (report_id == 0x20) {
        size_t copy_len = len;

        if (copy_len > sizeof(s_dse.feature20_body)) {
            copy_len = sizeof(s_dse.feature20_body);
        }
        memset(s_dse.feature20_body, 0, sizeof(s_dse.feature20_body));
        memcpy(s_dse.feature20_body, data, copy_len);
        s_dse.feature20_valid = len >= sizeof(s_dse.feature20_body);
    }

    if (report_id == DSE_PROFILE_REPORT_FIRST && !s_dse.is_edge) {
        s_dse.is_edge = true;
        s_dse.profiles_ready = false;
        s_dse.unlock_pending = true;
        s_dse.unlock_wait_active = false;
        s_dse.prefetch_active = false;
        s_dse.prefetch_mark_ready = false;
        s_dse.post_save_active = false;
        s_dse.post_save_round = 0;
        detected_edge = true;
    }
    dse_unlock(flags);

    if (detected_edge) {
        printf("[DSE] DualSense Edge detected; unlock scheduled\r\n");
    }
}

void m61_ds5_dse_note_feature_set(uint8_t report_id)
{
    uintptr_t flags;

    if (report_id < DSE_PROFILE_WRITE_FIRST || report_id > DSE_PROFILE_WRITE_LAST) {
        return;
    }

    flags = dse_lock();
    if (s_dse.is_edge) {
        s_dse.post_save_active = true;
        s_dse.post_save_round = 0;
        s_dse.post_save_started = xTaskGetTickCount();
    }
    dse_unlock(flags);
}

bool m61_ds5_dse_is_profile_report(uint8_t report_id)
{
    return report_id >= DSE_PROFILE_REPORT_FIRST && report_id <= DSE_PROFILE_REPORT_LAST;
}

bool m61_ds5_dse_profiles_ready(void)
{
    bool ready;
    uintptr_t flags = dse_lock();

    ready = s_dse.profiles_ready;
    dse_unlock(flags);
    return ready;
}

bool m61_ds5_dse_is_edge(void)
{
    bool is_edge;
    uintptr_t flags = dse_lock();

    is_edge = s_dse.is_edge;
    dse_unlock(flags);
    return is_edge;
}

bool m61_ds5_dse_should_nak_profile(uint8_t report_id)
{
    bool should_nak;
    uintptr_t flags;

    if (!m61_ds5_dse_is_profile_report(report_id)) {
        return false;
    }

    flags = dse_lock();
    should_nak = s_dse.is_edge && !s_dse.profiles_ready;
    dse_unlock(flags);
    return should_nak;
}

void m61_ds5_dse_task(TickType_t now)
{
    if (dse_send_initial_unlock(now)) {
        return;
    }
    if (dse_finish_unlock_wait(now)) {
        return;
    }
    if (dse_run_prefetch(now)) {
        return;
    }
    (void)dse_run_post_save(now);
}
