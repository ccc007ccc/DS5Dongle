#ifndef DS5_BRIDGE_AUDIO_EPOCH_PAIRER_H
#define DS5_BRIDGE_AUDIO_EPOCH_PAIRER_H

#include <array>
#include <cstddef>
#include <cstdint>

constexpr std::size_t AUDIO_EPOCH_HAPTICS_SIZE = 64;
constexpr std::size_t AUDIO_EPOCH_OPUS_SIZE = 200;

struct AudioEpochComplete {
    uint32_t generation = 0;
    uint32_t epoch = 0;
    std::array<uint8_t, AUDIO_EPOCH_HAPTICS_SIZE> haptics{};
    std::array<uint8_t, AUDIO_EPOCH_OPUS_SIZE> speaker_opus{};
    bool speaker_enabled = false;
};

class AudioEpochPairer {
public:
    void reset() { pending_valid_ = false; }

    bool push(const AudioEpochComplete &current, AudioEpochComplete *first,
              AudioEpochComplete *second) {
        if (!pending_valid_) {
            pending_ = current;
            pending_valid_ = true;
            return false;
        }
        if (current.generation != pending_.generation ||
            current.epoch != pending_.epoch + 1U ||
            current.speaker_enabled != pending_.speaker_enabled) {
            pending_ = current;
            return false;
        }
        if (first != nullptr) *first = pending_;
        if (second != nullptr) *second = current;
        pending_valid_ = false;
        return true;
    }

private:
    AudioEpochComplete pending_{};
    bool pending_valid_ = false;
};

#endif
