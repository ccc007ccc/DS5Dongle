#include "m61_realtime_scheduler.h"

#include <limits.h>
#include <stddef.h>

static uint32_t job_slack_us(uint64_t now_us, uint64_t created_us,
                             uint32_t estimated_us)
{
    uint64_t deadline_us = created_us + M61_RT_CODEC_DEADLINE_US;
    uint64_t finish_us = now_us + estimated_us;

    if (finish_us >= deadline_us) {
        return 0U;
    }
    uint64_t slack_us = deadline_us - finish_us;
    return slack_us > UINT32_MAX ? UINT32_MAX : (uint32_t)slack_us;
}

void m61_realtime_scheduler_select(const m61_rt_snapshot_t *snapshot,
                                   m61_rt_decision_t *decision)
{
    uint32_t encode_slack = UINT32_MAX;
    uint32_t decode_slack = UINT32_MAX;
    bool encode_ready;
    bool decode_ready;

    if (decision == NULL) {
        return;
    }
    decision->action = M61_RT_ACTION_IDLE;
    decision->ready_consumed = 0U;
    decision->slack_us = UINT32_MAX;
    decision->deadline_forced = false;
    if (snapshot == NULL) {
        return;
    }

    encode_ready = (snapshot->ready & M61_RT_READY_ENCODE) != 0U;
    decode_ready = (snapshot->ready & M61_RT_READY_DECODE) != 0U;
    if (encode_ready) {
        encode_slack = job_slack_us(snapshot->now_us,
                                    snapshot->encode_created_us,
                                    snapshot->encode_estimated_us);
    }
    if (decode_ready) {
        decode_slack = job_slack_us(snapshot->now_us,
                                    snapshot->decode_created_us,
                                    snapshot->decode_estimated_us);
    }

    if ((snapshot->ready & M61_RT_READY_USB_MIC_LOW) != 0U) {
        decision->action = M61_RT_ACTION_USB_MIC;
        decision->ready_consumed = M61_RT_READY_USB_MIC_LOW;
        decision->slack_us = 0U;
        decision->deadline_forced = true;
        return;
    }

    if ((snapshot->ready & M61_RT_READY_BT_REALTIME) != 0U &&
        snapshot->bt_window_elapsed_us < M61_RT_BT_MIN_WINDOW_US &&
        encode_slack > M61_RT_BT_MIN_WINDOW_US &&
        decode_slack > M61_RT_BT_MIN_WINDOW_US) {
        decision->action = M61_RT_ACTION_BT_WINDOW;
        decision->ready_consumed = M61_RT_READY_BT_REALTIME;
        decision->slack_us = encode_slack < decode_slack
                                 ? encode_slack : decode_slack;
        return;
    }

    if (encode_ready || decode_ready) {
        bool choose_encode = encode_ready &&
            (!decode_ready || encode_slack < decode_slack);

        if (encode_ready && decode_ready && encode_slack == decode_slack) {
            /* Avoid an unnecessary immediate return to the same large codec
             * working set when both deadlines are equivalent. */
            choose_encode = snapshot->last_codec != M61_RT_CODEC_ENCODE;
        }
        if (snapshot->encode_depth > 1U && encode_ready) choose_encode = true;
        if (snapshot->decode_depth > 1U && decode_ready) choose_encode = false;

        decision->action = choose_encode ? M61_RT_ACTION_ENCODE
                                         : M61_RT_ACTION_DECODE;
        decision->ready_consumed = choose_encode ? M61_RT_READY_ENCODE
                                                  : M61_RT_READY_DECODE;
        decision->slack_us = choose_encode ? encode_slack : decode_slack;
        decision->deadline_forced = decision->slack_us <= M61_RT_CACHE_GUARD_US;
        return;
    }

    if ((snapshot->ready & M61_RT_READY_BT_REALTIME) != 0U) {
        decision->action = M61_RT_ACTION_BT_WINDOW;
        decision->ready_consumed = M61_RT_READY_BT_REALTIME;
        return;
    }
    if ((snapshot->ready & (M61_RT_READY_HID_INPUT |
                            M61_RT_READY_HID_OUTPUT |
                            M61_RT_READY_CONTROL)) != 0U) {
        decision->action = M61_RT_ACTION_CONTROL;
        decision->ready_consumed = snapshot->ready &
            (M61_RT_READY_HID_INPUT | M61_RT_READY_HID_OUTPUT |
             M61_RT_READY_CONTROL);
    }
}
