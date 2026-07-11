#include "bt_tx_scheduler.h"

#include <cassert>
#include <cstdint>

static void publish(BtTxScheduler &scheduler, uint8_t report_id,
                    uint8_t marker, uint32_t now_ms) {
    uint8_t packet[4] = {0xa2, report_id, marker, 0};
    assert(scheduler.publish(report_id, packet, sizeof(packet), now_ms));
}

int main() {
    BtTxScheduler scheduler;
    BtTxFrame frame;

    publish(scheduler, 0x39, 1, 0);
    publish(scheduler, 0x39, 2, 10);
    publish(scheduler, 0x39, 3, 20);
    assert(scheduler.metrics().classes[0].replaced == 1);
    assert(scheduler.select(20, &frame));
    assert(frame.data[2] == 2);
    scheduler.finish(frame, true);
    assert(scheduler.select(20, &frame));
    assert(frame.data[2] == 3);
    scheduler.finish(frame, true);

    scheduler.reset();
    publish(scheduler, 0x39, 4, 0);
    assert(!scheduler.select(BT_TX_RT_MAX_AGE_MS + 1, &frame));
    assert(scheduler.metrics().classes[0].stale == 1);

    scheduler.reset();
    publish(scheduler, 0x31, 1, 0);
    publish(scheduler, 0x31, 2, 1);
    publish(scheduler, 0x32, 3, 2);
    for (uint8_t marker = 10; marker < 14; marker++) {
        publish(scheduler, 0x39, marker, marker);
        assert(scheduler.select(marker, &frame));
        scheduler.finish(frame, true);
    }
    assert(frame.tx_class == BtTxClass::State31 ||
           frame.tx_class == BtTxClass::State32);

    scheduler.reset();
    publish(scheduler, 0x31, 7, 0);
    assert(scheduler.select(0, &frame));
    scheduler.finish(frame, false);
    assert(scheduler.pending());
    assert(scheduler.select(1, &frame));
    scheduler.finish(frame, true);
    assert(!scheduler.pending());
    return 0;
}
