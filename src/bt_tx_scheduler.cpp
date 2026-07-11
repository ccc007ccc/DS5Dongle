#include "bt_tx_scheduler.h"

#include <cstring>
#include <limits>

std::size_t BtTxScheduler::index(BtTxClass tx_class) {
    return static_cast<std::size_t>(tx_class);
}

BtTxClass BtTxScheduler::classify(uint8_t report_id) {
    if (report_id == 0x39) return BtTxClass::Realtime;
    if (report_id == 0x32) return BtTxClass::State32;
    return BtTxClass::State31;
}

void BtTxScheduler::reset() {
    realtime_ = {};
    state31_ = {};
    state32_ = {};
    realtime_burst_ = 0;
    prefer_state32_ = false;
}

bool BtTxScheduler::publish(uint8_t report_id, const uint8_t *data,
                            std::size_t len, uint32_t now_ms) {
    if (data == nullptr || len == 0 || len > BT_TX_MAX_PACKET_SIZE) return false;

    const BtTxClass tx_class = classify(report_id);
    Slot *slot = nullptr;
    if (tx_class == BtTxClass::Realtime) {
        Slot *oldest = nullptr;
        for (auto &candidate : realtime_) {
            if (!candidate.ready) {
                slot = &candidate;
                break;
            }
            if (oldest == nullptr ||
                candidate.frame.sequence < oldest->frame.sequence) {
                oldest = &candidate;
            }
        }
        if (slot == nullptr) {
            slot = oldest;
            metrics_.classes[index(tx_class)].replaced++;
        }
    } else if (tx_class == BtTxClass::State31) {
        slot = &state31_;
        if (slot->ready) metrics_.classes[index(tx_class)].replaced++;
        state31_version_++;
    } else {
        slot = &state32_;
        if (slot->ready) metrics_.classes[index(tx_class)].replaced++;
        state32_version_++;
    }

    slot->frame = {};
    std::memcpy(slot->frame.data.data(), data, len);
    slot->frame.len = static_cast<uint16_t>(len);
    slot->frame.tx_class = tx_class;
    slot->frame.created_ms = now_ms;
    slot->frame.sequence = ++publish_sequence_;
    slot->frame.version = tx_class == BtTxClass::State31
                              ? state31_version_
                              : (tx_class == BtTxClass::State32
                                     ? state32_version_
                                     : 0);
    slot->ready = true;
    metrics_.classes[index(tx_class)].accepted++;
    return true;
}

bool BtTxScheduler::select_realtime(uint32_t now_ms, BtTxFrame *frame) {
    while (true) {
        Slot *oldest = nullptr;
        for (auto &candidate : realtime_) {
            if (candidate.ready &&
                (oldest == nullptr ||
                 candidate.frame.sequence < oldest->frame.sequence)) {
                oldest = &candidate;
            }
        }
        if (oldest == nullptr) return false;
        if (now_ms - oldest->frame.created_ms > BT_TX_RT_MAX_AGE_MS) {
            oldest->ready = false;
            metrics_.classes[index(BtTxClass::Realtime)].stale++;
            continue;
        }
        *frame = oldest->frame;
        oldest->ready = false;
        return true;
    }
}

bool BtTxScheduler::select_state(BtTxFrame *frame) {
    Slot *first = prefer_state32_ ? &state32_ : &state31_;
    Slot *second = prefer_state32_ ? &state31_ : &state32_;
    if (!first->ready) first = second;
    if (!first->ready) return false;
    *frame = first->frame;
    prefer_state32_ = first->frame.tx_class == BtTxClass::State31;
    return true;
}

bool BtTxScheduler::select(uint32_t now_ms, BtTxFrame *frame) {
    if (frame == nullptr) return false;
    const bool state_pending = state31_.ready || state32_.ready;
    if (realtime_burst_ < BT_TX_RT_MAX_BURST || !state_pending) {
        if (select_realtime(now_ms, frame)) {
            realtime_burst_++;
            return true;
        }
    }
    if (select_state(frame)) {
        realtime_burst_ = 0;
        return true;
    }
    if (select_realtime(now_ms, frame)) {
        realtime_burst_++;
        return true;
    }
    return false;
}

void BtTxScheduler::finish(const BtTxFrame &frame, bool success) {
    auto &class_metrics = metrics_.classes[index(frame.tx_class)];
    if (success) {
        class_metrics.transmitted++;
        Slot *mailbox = frame.tx_class == BtTxClass::State31
                            ? &state31_
                            : (frame.tx_class == BtTxClass::State32
                                   ? &state32_
                                   : nullptr);
        if (mailbox != nullptr && mailbox->ready &&
            mailbox->frame.version == frame.version) {
            mailbox->ready = false;
        }
    } else {
        class_metrics.failed++;
    }
}

bool BtTxScheduler::pending() const {
    if (state31_.ready || state32_.ready) return true;
    for (const auto &slot : realtime_) {
        if (slot.ready) return true;
    }
    return false;
}
