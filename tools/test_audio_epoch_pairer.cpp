#include "audio_epoch_pairer.h"

#include <cassert>

static AudioEpochComplete epoch(uint32_t sequence, bool speaker = true) {
    AudioEpochComplete value{};
    value.generation = 1;
    value.epoch = sequence;
    value.speaker_enabled = speaker;
    value.haptics[0] = static_cast<uint8_t>(sequence);
    return value;
}

int main() {
    AudioEpochPairer pairer;
    AudioEpochComplete first;
    AudioEpochComplete second;

    assert(!pairer.push(epoch(0), &first, &second));
    assert(pairer.push(epoch(1), &first, &second));
    assert(first.epoch == 0 && second.epoch == 1);

    assert(!pairer.push(epoch(3), &first, &second));
    assert(!pairer.push(epoch(5), &first, &second));
    assert(pairer.push(epoch(6), &first, &second));
    assert(first.epoch == 5 && second.epoch == 6);

    assert(!pairer.push(epoch(7, false), &first, &second));
    assert(!pairer.push(epoch(8, true), &first, &second));
    assert(pairer.push(epoch(9, true), &first, &second));
    auto next_generation = epoch(10, true);
    next_generation.generation = 2;
    assert(!pairer.push(epoch(11, true), &first, &second));
    assert(!pairer.push(next_generation, &first, &second));
    return 0;
}
