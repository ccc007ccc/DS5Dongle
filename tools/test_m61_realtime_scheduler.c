#include "m61_realtime_scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static m61_rt_decision_t select_action(m61_rt_snapshot_t snapshot)
{
    m61_rt_decision_t decision;
    memset(&decision, 0, sizeof(decision));
    m61_realtime_scheduler_select(&snapshot, &decision);
    return decision;
}

int main(void)
{
    m61_rt_snapshot_t snapshot = {0};
    m61_rt_decision_t decision;

    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_IDLE);

    snapshot.ready = M61_RT_READY_ENCODE;
    snapshot.now_us = 5000;
    snapshot.encode_deadline_us = 10000;
    snapshot.encode_estimated_us = 4000;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_ENCODE);
    assert(decision.slack_us == 1000);

    snapshot.ready = M61_RT_READY_ENCODE | M61_RT_READY_DECODE;
    snapshot.decode_deadline_us = 13000;
    snapshot.decode_estimated_us = 3500;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_ENCODE);

    snapshot.encode_deadline_us = 13000;
    snapshot.decode_deadline_us = 10000;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_DECODE);

    snapshot.encode_deadline_us = 10000;
    snapshot.decode_deadline_us = 10500;
    snapshot.ready |= M61_RT_READY_BT_REALTIME;
    snapshot.bt_window_elapsed_us = 0;
    snapshot.now_us = 1000;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_BT_WINDOW);

    snapshot.ready |= M61_RT_READY_USB_MIC_LOW;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_USB_MIC);
    assert(decision.deadline_forced);

    snapshot = (m61_rt_snapshot_t){0};
    snapshot.ready = M61_RT_READY_ENCODE | M61_RT_READY_DECODE;
    snapshot.now_us = 1000;
    snapshot.encode_deadline_us = 10000;
    snapshot.decode_deadline_us = 10000;
    snapshot.encode_estimated_us = 4000;
    snapshot.decode_estimated_us = 4000;
    snapshot.last_codec = M61_RT_CODEC_ENCODE;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_DECODE);

    snapshot.encode_depth = 2;
    decision = select_action(snapshot);
    assert(decision.action == M61_RT_ACTION_ENCODE);

    puts("M61 realtime scheduler tests passed.");
    return 0;
}
