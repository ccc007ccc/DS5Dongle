#ifndef DS5_BRIDGE_BT_TX_SCHEDULER_H
#define DS5_BRIDGE_BT_TX_SCHEDULER_H

#include <array>
#include <cstddef>
#include <cstdint>

constexpr std::size_t BT_TX_MAX_PACKET_SIZE = 672;
constexpr std::size_t BT_TX_RT_CAPACITY = 2;
constexpr uint32_t BT_TX_RT_MAX_AGE_MS = 64;
constexpr uint8_t BT_TX_RT_MAX_BURST = 3;

enum class BtTxClass : uint8_t {
    Realtime,
    State31,
    State32,
    Count,
};

struct BtTxFrame {
    std::array<uint8_t, BT_TX_MAX_PACKET_SIZE> data{};
    uint16_t len = 0;
    BtTxClass tx_class = BtTxClass::Realtime;
    uint32_t created_ms = 0;
    uint32_t sequence = 0;
    uint32_t version = 0;
};

struct BtTxClassMetrics {
    uint32_t accepted = 0;
    uint32_t transmitted = 0;
    uint32_t replaced = 0;
    uint32_t stale = 0;
    uint32_t failed = 0;
};

struct BtTxMetrics {
    std::array<BtTxClassMetrics, static_cast<std::size_t>(BtTxClass::Count)> classes{};
};

class BtTxScheduler {
public:
    void reset();
    bool publish(uint8_t report_id, const uint8_t *data, std::size_t len,
                 uint32_t now_ms);
    bool select(uint32_t now_ms, BtTxFrame *frame);
    void finish(const BtTxFrame &frame, bool success);
    bool pending() const;
    const BtTxMetrics &metrics() const { return metrics_; }

private:
    struct Slot {
        BtTxFrame frame{};
        bool ready = false;
    };

    bool select_realtime(uint32_t now_ms, BtTxFrame *frame);
    bool select_state(BtTxFrame *frame);
    static BtTxClass classify(uint8_t report_id);
    static std::size_t index(BtTxClass tx_class);

    std::array<Slot, BT_TX_RT_CAPACITY> realtime_{};
    Slot state31_{};
    Slot state32_{};
    BtTxMetrics metrics_{};
    uint32_t publish_sequence_ = 0;
    uint32_t state31_version_ = 0;
    uint32_t state32_version_ = 0;
    uint8_t realtime_burst_ = 0;
    bool prefer_state32_ = false;
};

#endif
