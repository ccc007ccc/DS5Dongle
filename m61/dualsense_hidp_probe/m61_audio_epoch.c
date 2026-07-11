#include "m61_audio_epoch.h"

#include <limits.h>
#include <string.h>

#ifndef M61_AUDIO_EPOCH_HOST_TEST
#include "bflb_irq.h"
#endif

#define M61_AUDIO_USB_CHANNELS 4U
#define M61_AUDIO_BYTES_PER_SAMPLE 2U
#define M61_AUDIO_USB_FRAME_BYTES \
    (M61_AUDIO_USB_CHANNELS * M61_AUDIO_BYTES_PER_SAMPLE)
#define M61_AUDIO_HAPTICS_DOWNSAMPLE 16U
#define M61_AUDIO_INVALID_SLOT UINT8_MAX

typedef char m61_audio_epoch_slot_budget_check[
    (sizeof(m61_audio_epoch_t) <= M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES) ? 1 : -1];

typedef struct {
    m61_audio_epoch_t slots[M61_AUDIO_EPOCH_SLOT_COUNT];
    m61_audio_epoch_stats_t stats;
    uint8_t filling_slot;
    bool last_emitted_valid;
    uint32_t last_emitted_epoch;
} m61_audio_epoch_store_t;

static m61_audio_epoch_store_t s_audio_epochs;
uint8_t g_ds5_owner_m61_audio_epochs;

static uintptr_t epoch_lock(void)
{
#ifdef M61_AUDIO_EPOCH_HOST_TEST
    return 0;
#else
    return bflb_irq_save();
#endif
}

static void epoch_unlock(uintptr_t flags)
{
#ifdef M61_AUDIO_EPOCH_HOST_TEST
    (void)flags;
#else
    bflb_irq_restore(flags);
#endif
}

static int16_t read_i16_le(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint8_t abs_i8(int8_t value)
{
    if (value == INT8_MIN) {
        return 128U;
    }
    return (uint8_t)(value < 0 ? -value : value);
}

static int8_t haptic_pcm16_to_i8(int16_t sample, uint16_t gain_q8)
{
    int32_t scaled = ((int32_t)sample * gain_q8) / 256;
    int32_t value;

    if (scaled > 32768) {
        scaled = 32768;
    } else if (scaled < -32768) {
        scaled = -32768;
    }
    value = (scaled * 127) / 32768;
    if (value > 127) {
        value = 127;
    } else if (value < -128) {
        value = -128;
    }
    return (int8_t)value;
}

static void release_cancelled_slot(uint8_t index, uint8_t cancelled_state)
{
    uintptr_t flags = epoch_lock();
    m61_audio_epoch_t *slot = &s_audio_epochs.slots[index];

    if (slot->state == cancelled_state) {
        slot->state = M61_AUDIO_EPOCH_FREE;
        s_audio_epochs.stats.epochs_stale++;
    }
    epoch_unlock(flags);
}

static int find_slot_locked(uint32_t generation, uint32_t epoch)
{
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        const m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
        if (slot->state != M61_AUDIO_EPOCH_FREE &&
            slot->generation == generation && slot->epoch == epoch) {
            return i;
        }
    }
    return -1;
}

static int allocate_slot_locked(uint64_t captured_us, bool speaker_enabled)
{
    int target = -1;
    uint32_t oldest_epoch = UINT32_MAX;

    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        if (s_audio_epochs.slots[i].state == M61_AUDIO_EPOCH_FREE) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
            const m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
            if ((slot->state == M61_AUDIO_EPOCH_READY_ENCODE ||
                 slot->state == M61_AUDIO_EPOCH_COMPLETE) &&
                slot->epoch < oldest_epoch) {
                oldest_epoch = slot->epoch;
                target = i;
            }
        }
        if (target < 0) {
            s_audio_epochs.stats.epochs_dropped++;
            return -1;
        }
        s_audio_epochs.stats.epochs_dropped++;
    }

    m61_audio_epoch_t *slot = &s_audio_epochs.slots[target];
    slot->generation = s_audio_epochs.stats.generation;
    slot->epoch = s_audio_epochs.stats.next_epoch++;
    slot->captured_us = captured_us;
    slot->pcm_frames = 0;
    slot->speaker_enabled = speaker_enabled ? 1U : 0U;
    slot->state = M61_AUDIO_EPOCH_FILLING;
    memset(slot->haptics, 0, sizeof(slot->haptics));
    s_audio_epochs.filling_slot = (uint8_t)target;
    s_audio_epochs.stats.next_epoch = slot->epoch + 1U;
    s_audio_epochs.stats.epochs_started++;
    return target;
}

void m61_audio_epoch_init(uint32_t generation)
{
    g_ds5_owner_m61_audio_epochs = 1U;
    memset(&s_audio_epochs, 0, sizeof(s_audio_epochs));
    s_audio_epochs.filling_slot = M61_AUDIO_INVALID_SLOT;
    s_audio_epochs.stats.generation = generation;
}

void m61_audio_epoch_reset(uint32_t generation)
{
    uintptr_t flags = epoch_lock();
    bool generation_changed = generation != s_audio_epochs.stats.generation;

    s_audio_epochs.stats.generation = generation;
    if (generation_changed) {
        s_audio_epochs.stats.next_epoch = 0;
    }
    s_audio_epochs.stats.generation_resets++;
    s_audio_epochs.filling_slot = M61_AUDIO_INVALID_SLOT;
    s_audio_epochs.last_emitted_valid = false;
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
        switch (slot->state) {
            case M61_AUDIO_EPOCH_FILLING:
                slot->state = M61_AUDIO_EPOCH_CANCELLED_FILLING;
                break;
            case M61_AUDIO_EPOCH_ENCODING:
                slot->state = M61_AUDIO_EPOCH_CANCELLED_ENCODING;
                break;
            case M61_AUDIO_EPOCH_ASSEMBLING:
                slot->state = M61_AUDIO_EPOCH_CANCELLED_ASSEMBLING;
                break;
            case M61_AUDIO_EPOCH_READY_ENCODE:
            case M61_AUDIO_EPOCH_COMPLETE:
                slot->state = M61_AUDIO_EPOCH_FREE;
                s_audio_epochs.stats.epochs_stale++;
                break;
            default:
                break;
        }
    }
    epoch_unlock(flags);
}

void m61_audio_epoch_ingest_usb(const uint8_t *data,
                                size_t len,
                                uint32_t generation,
                                uint64_t captured_us,
                                bool speaker_enabled,
                                uint16_t haptics_gain_q8)
{
    size_t frames;
    size_t input_frame = 0;

    if (data == NULL || len < M61_AUDIO_USB_FRAME_BYTES) {
        return;
    }
    frames = len / M61_AUDIO_USB_FRAME_BYTES;
    while (input_frame < frames) {
        m61_audio_epoch_t *slot;
        uint16_t first_frame;
        uint16_t chunk_frames;
        uint8_t slot_index;
        uintptr_t flags = epoch_lock();

        if (generation != s_audio_epochs.stats.generation) {
            epoch_unlock(flags);
            return;
        }
        if (s_audio_epochs.filling_slot == M61_AUDIO_INVALID_SLOT) {
            int allocated = allocate_slot_locked(captured_us, speaker_enabled);
            if (allocated < 0) {
                epoch_unlock(flags);
                return;
            }
        }
        slot_index = s_audio_epochs.filling_slot;
        slot = &s_audio_epochs.slots[slot_index];
        if (slot->state != M61_AUDIO_EPOCH_FILLING ||
            slot->generation != generation) {
            s_audio_epochs.filling_slot = M61_AUDIO_INVALID_SLOT;
            epoch_unlock(flags);
            release_cancelled_slot(slot_index,
                                   M61_AUDIO_EPOCH_CANCELLED_FILLING);
            return;
        }
        first_frame = slot->pcm_frames;
        chunk_frames = (uint16_t)(M61_AUDIO_EPOCH_USB_FRAMES - first_frame);
        if (chunk_frames > frames - input_frame) {
            chunk_frames = (uint16_t)(frames - input_frame);
        }
        epoch_unlock(flags);

        for (uint16_t i = 0; i < chunk_frames; i++) {
            uint16_t frame_index = (uint16_t)(first_frame + i);
            const uint8_t *frame =
                data + (input_frame + i) * M61_AUDIO_USB_FRAME_BYTES;

            slot->speaker_pcm[frame_index * 2U] = read_i16_le(frame);
            slot->speaker_pcm[frame_index * 2U + 1U] = read_i16_le(frame + 2U);
            if ((frame_index % M61_AUDIO_HAPTICS_DOWNSAMPLE) == 0U) {
                uint8_t haptic_index =
                    (uint8_t)((frame_index / M61_AUDIO_HAPTICS_DOWNSAMPLE) * 2U);
                int8_t left = haptic_pcm16_to_i8(read_i16_le(frame + 4U),
                                                 haptics_gain_q8);
                int8_t right = haptic_pcm16_to_i8(read_i16_le(frame + 6U),
                                                  haptics_gain_q8);
                slot->haptics[haptic_index] = (uint8_t)left;
                slot->haptics[haptic_index + 1U] = (uint8_t)right;
            }
        }

        flags = epoch_lock();
        if (slot->state == M61_AUDIO_EPOCH_CANCELLED_FILLING ||
            slot->generation != s_audio_epochs.stats.generation) {
            slot->state = M61_AUDIO_EPOCH_FREE;
            s_audio_epochs.stats.epochs_stale++;
            epoch_unlock(flags);
            return;
        }
        slot->pcm_frames = (uint16_t)(slot->pcm_frames + chunk_frames);
        input_frame += chunk_frames;
        if (slot->pcm_frames >= M61_AUDIO_EPOCH_USB_FRAMES) {
            bool nonzero = false;
            uint8_t peak = 0;

            for (uint8_t i = 0; i < M61_AUDIO_EPOCH_HAPTICS_LEN; i++) {
                uint8_t magnitude = abs_i8((int8_t)slot->haptics[i]);
                if (magnitude != 0U) {
                    nonzero = true;
                }
                if (magnitude > peak) {
                    peak = magnitude;
                }
            }
            s_audio_epochs.stats.haptics_sample_pairs +=
                M61_AUDIO_EPOCH_HAPTICS_LEN / 2U;
            if (nonzero) {
                s_audio_epochs.stats.haptics_nonzero_epochs++;
            }
            s_audio_epochs.stats.haptics_last_peak = peak;
            s_audio_epochs.filling_slot = M61_AUDIO_INVALID_SLOT;
            if (slot->speaker_enabled) {
                slot->state = M61_AUDIO_EPOCH_READY_ENCODE;
            } else {
                slot->state = M61_AUDIO_EPOCH_COMPLETE;
                s_audio_epochs.stats.epochs_completed++;
            }
        }
        epoch_unlock(flags);
    }
}

bool m61_audio_epoch_take_encode_job(m61_audio_epoch_encode_job_t *job)
{
    int selected = -1;
    uint32_t oldest_epoch = UINT32_MAX;
    uintptr_t flags;

    if (job == NULL) {
        return false;
    }
    flags = epoch_lock();
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        const m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
        if (slot->state == M61_AUDIO_EPOCH_READY_ENCODE &&
            slot->generation == s_audio_epochs.stats.generation &&
            slot->epoch < oldest_epoch) {
            selected = i;
            oldest_epoch = slot->epoch;
        }
    }
    if (selected < 0) {
        epoch_unlock(flags);
        return false;
    }
    m61_audio_epoch_t *slot = &s_audio_epochs.slots[selected];
    slot->state = M61_AUDIO_EPOCH_ENCODING;
    job->generation = slot->generation;
    job->epoch = slot->epoch;
    job->captured_us = slot->captured_us;
    job->speaker_pcm = slot->speaker_pcm;
    s_audio_epochs.stats.encode_jobs++;
    epoch_unlock(flags);
    return true;
}

bool m61_audio_epoch_complete_encode(uint32_t generation,
                                     uint32_t epoch,
                                     const uint8_t *opus,
                                     size_t opus_len)
{
    int index;
    m61_audio_epoch_t *slot;
    uintptr_t flags;

    flags = epoch_lock();
    index = find_slot_locked(generation, epoch);
    if (index < 0) {
        epoch_unlock(flags);
        return false;
    }
    slot = &s_audio_epochs.slots[index];
    if (slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING ||
        generation != s_audio_epochs.stats.generation) {
        slot->state = M61_AUDIO_EPOCH_FREE;
        s_audio_epochs.stats.epochs_stale++;
        epoch_unlock(flags);
        return false;
    }
    if (slot->state != M61_AUDIO_EPOCH_ENCODING) {
        epoch_unlock(flags);
        return false;
    }
    epoch_unlock(flags);

    if (opus == NULL || opus_len != M61_AUDIO_EPOCH_OPUS_LEN) {
        flags = epoch_lock();
        if (slot->state == M61_AUDIO_EPOCH_ENCODING) {
            slot->state = M61_AUDIO_EPOCH_FREE;
            s_audio_epochs.stats.encode_failures++;
            s_audio_epochs.stats.epochs_dropped++;
        } else if (slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING) {
            slot->state = M61_AUDIO_EPOCH_FREE;
            s_audio_epochs.stats.epochs_stale++;
        }
        epoch_unlock(flags);
        return false;
    }
    memcpy(slot->speaker_opus, opus, M61_AUDIO_EPOCH_OPUS_LEN);

    flags = epoch_lock();
    if (slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING ||
        generation != s_audio_epochs.stats.generation) {
        slot->state = M61_AUDIO_EPOCH_FREE;
        s_audio_epochs.stats.epochs_stale++;
        epoch_unlock(flags);
        return false;
    }
    if (slot->state != M61_AUDIO_EPOCH_ENCODING) {
        epoch_unlock(flags);
        return false;
    }
    slot->state = M61_AUDIO_EPOCH_COMPLETE;
    s_audio_epochs.stats.epochs_completed++;
    epoch_unlock(flags);
    return true;
}

bool m61_audio_epoch_take_adjacent_pair(m61_audio_epoch_pair_t *pair)
{
    while (pair != NULL) {
        int first = -1;
        int second = -1;
        uint32_t first_epoch = UINT32_MAX;
        uint32_t next_complete = UINT32_MAX;
        uintptr_t flags = epoch_lock();

        for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
            const m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
            if (slot->state != M61_AUDIO_EPOCH_FREE &&
                slot->generation == s_audio_epochs.stats.generation &&
                slot->epoch < first_epoch) {
                first = i;
                first_epoch = slot->epoch;
            }
        }
        if (first < 0) {
            epoch_unlock(flags);
            return false;
        }
        if (s_audio_epochs.slots[first].state != M61_AUDIO_EPOCH_COMPLETE) {
            epoch_unlock(flags);
            return false;
        }
        for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
            const m61_audio_epoch_t *slot = &s_audio_epochs.slots[i];
            if (slot->state != M61_AUDIO_EPOCH_COMPLETE || i == first ||
                slot->generation != s_audio_epochs.stats.generation) {
                continue;
            }
            if (slot->epoch == first_epoch + 1U) {
                second = i;
                break;
            }
            if (slot->epoch > first_epoch && slot->epoch < next_complete) {
                next_complete = slot->epoch;
            }
        }
        if (second < 0) {
            if (next_complete != UINT32_MAX) {
                s_audio_epochs.slots[first].state = M61_AUDIO_EPOCH_FREE;
                s_audio_epochs.stats.epochs_dropped++;
                s_audio_epochs.stats.epoch_gaps +=
                    next_complete - first_epoch - 1U;
                s_audio_epochs.stats.epoch_discontinuities++;
                epoch_unlock(flags);
                continue;
            }
            epoch_unlock(flags);
            return false;
        }

        m61_audio_epoch_t *first_slot = &s_audio_epochs.slots[first];
        m61_audio_epoch_t *second_slot = &s_audio_epochs.slots[second];
        if (first_slot->speaker_enabled != second_slot->speaker_enabled) {
            first_slot->state = M61_AUDIO_EPOCH_FREE;
            s_audio_epochs.stats.epochs_dropped++;
            s_audio_epochs.stats.speaker_state_discontinuities++;
            s_audio_epochs.stats.epoch_discontinuities++;
            epoch_unlock(flags);
            continue;
        }
        if (s_audio_epochs.last_emitted_valid &&
            first_epoch != s_audio_epochs.last_emitted_epoch + 1U) {
            s_audio_epochs.stats.epoch_discontinuities++;
            if (first_epoch > s_audio_epochs.last_emitted_epoch + 1U) {
                s_audio_epochs.stats.epoch_gaps +=
                    first_epoch - s_audio_epochs.last_emitted_epoch - 1U;
            }
        }
        first_slot->state = M61_AUDIO_EPOCH_ASSEMBLING;
        second_slot->state = M61_AUDIO_EPOCH_ASSEMBLING;
        pair->generation = first_slot->generation;
        pair->first_epoch = first_slot->epoch;
        pair->first_captured_us = first_slot->captured_us;
        pair->second_captured_us = second_slot->captured_us;
        pair->speaker_enabled = first_slot->speaker_enabled != 0U;
        epoch_unlock(flags);

        memcpy(pair->haptics[0], first_slot->haptics,
               M61_AUDIO_EPOCH_HAPTICS_LEN);
        memcpy(pair->haptics[1], second_slot->haptics,
               M61_AUDIO_EPOCH_HAPTICS_LEN);
        if (pair->speaker_enabled) {
            memcpy(pair->speaker_opus[0], first_slot->speaker_opus,
                   M61_AUDIO_EPOCH_OPUS_LEN);
            memcpy(pair->speaker_opus[1], second_slot->speaker_opus,
                   M61_AUDIO_EPOCH_OPUS_LEN);
        } else {
            memset(pair->speaker_opus, 0, sizeof(pair->speaker_opus));
        }

        flags = epoch_lock();
        if (first_slot->state == M61_AUDIO_EPOCH_CANCELLED_ASSEMBLING ||
            second_slot->state == M61_AUDIO_EPOCH_CANCELLED_ASSEMBLING ||
            pair->generation != s_audio_epochs.stats.generation) {
            if (first_slot->state == M61_AUDIO_EPOCH_CANCELLED_ASSEMBLING) {
                first_slot->state = M61_AUDIO_EPOCH_FREE;
                s_audio_epochs.stats.epochs_stale++;
            }
            if (second_slot->state == M61_AUDIO_EPOCH_CANCELLED_ASSEMBLING) {
                second_slot->state = M61_AUDIO_EPOCH_FREE;
                s_audio_epochs.stats.epochs_stale++;
            }
            epoch_unlock(flags);
            return false;
        }
        first_slot->state = M61_AUDIO_EPOCH_FREE;
        second_slot->state = M61_AUDIO_EPOCH_FREE;
        s_audio_epochs.last_emitted_valid = true;
        s_audio_epochs.last_emitted_epoch = first_epoch + 1U;
        s_audio_epochs.stats.adjacent_pairs++;
        epoch_unlock(flags);
        return true;
    }
    return false;
}

void m61_audio_epoch_get_stats(m61_audio_epoch_stats_t *stats)
{
    uintptr_t flags;

    if (stats == NULL) {
        return;
    }
    flags = epoch_lock();
    *stats = s_audio_epochs.stats;
    stats->filling_slots = 0;
    stats->encode_ready_slots = 0;
    stats->encoding_slots = 0;
    stats->complete_slots = 0;
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        switch (s_audio_epochs.slots[i].state) {
            case M61_AUDIO_EPOCH_FILLING:
                stats->filling_slots++;
                break;
            case M61_AUDIO_EPOCH_READY_ENCODE:
                stats->encode_ready_slots++;
                break;
            case M61_AUDIO_EPOCH_ENCODING:
                stats->encoding_slots++;
                break;
            case M61_AUDIO_EPOCH_COMPLETE:
                stats->complete_slots++;
                break;
            default:
                break;
        }
    }
    epoch_unlock(flags);
}
