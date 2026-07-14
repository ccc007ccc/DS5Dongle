#include "m61_audio_epoch.h"

#include <limits.h>
#include <string.h>

#ifndef CONFIG_M61_PIPELINE_PROFILE
#define CONFIG_M61_PIPELINE_PROFILE 0
#endif

#ifndef M61_AUDIO_EPOCH_HOST_TEST
#include "bflb_irq.h"
#endif

#define USB_CHANNELS 4U
#define USB_FRAME_BYTES (USB_CHANNELS * sizeof(int16_t))
#define HAPTICS_DECIMATION_FRAMES 16U
#define INVALID_SLOT UINT8_MAX
#define M61_CACHE_LINE_BYTES 32U

_Static_assert(sizeof(m61_audio_epoch_t) <= M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES,
               "M61 audio epoch slot budget drift");
_Static_assert(sizeof(m61_audio_epoch_t) % M61_CACHE_LINE_BYTES == 0U,
               "M61 audio epoch slot cache-line stride drift");

typedef struct {
    m61_audio_epoch_t slots[M61_AUDIO_EPOCH_SLOT_COUNT];
    m61_audio_epoch_stats_t stats;
#if CONFIG_M61_PIPELINE_PROFILE
    uint64_t epoch_interval_us_total;
    uint64_t last_epoch_captured_us;
#endif
    uint8_t filling_slot;
#if CONFIG_M61_PIPELINE_PROFILE
    bool last_epoch_capture_valid;
#endif
} audio_epoch_store_t;

static audio_epoch_store_t s_store __attribute__((aligned(M61_CACHE_LINE_BYTES)));

#ifdef M61_AUDIO_EPOCH_HOST_TEST
static uint32_t s_host_lock_count;
#endif

static uintptr_t epoch_lock(void)
{
#ifdef M61_AUDIO_EPOCH_HOST_TEST
    s_host_lock_count++;
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

static int8_t pcm16_to_i8(int32_t sample, uint16_t gain_q8)
{
    int32_t scaled = (sample * gain_q8) / 256;
    int32_t value;

    if (scaled > INT16_MAX) scaled = INT16_MAX;
    if (scaled < INT16_MIN) scaled = INT16_MIN;
    value = (scaled * 127) / 32768;
    if (value > INT8_MAX) value = INT8_MAX;
    if (value < INT8_MIN) value = INT8_MIN;
    return (int8_t)value;
}

static int find_slot_locked(uint32_t generation, uint32_t epoch)
{
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_store.slots[i];
        if (slot->state != M61_AUDIO_EPOCH_FREE &&
            slot->generation == generation && slot->epoch == epoch) {
            return i;
        }
    }
    return -1;
}

static int allocate_slot_locked(uint64_t captured_us, bool speaker_enabled)
{
    int selected = -1;
    uint32_t oldest = UINT32_MAX;

    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        if (s_store.slots[i].state == M61_AUDIO_EPOCH_FREE) {
            selected = i;
            break;
        }
    }
    if (selected < 0) {
        for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
            m61_audio_epoch_t *slot = &s_store.slots[i];
            if ((slot->state == M61_AUDIO_EPOCH_READY_ENCODE ||
                 slot->state == M61_AUDIO_EPOCH_COMPLETE) &&
                slot->epoch < oldest) {
                oldest = slot->epoch;
                selected = i;
            }
        }
        if (selected < 0) {
            s_store.stats.epochs_dropped++;
            return -1;
        }
        s_store.stats.epochs_dropped++;
    }

#if CONFIG_M61_PIPELINE_PROFILE
    if (s_store.last_epoch_capture_valid &&
        captured_us >= s_store.last_epoch_captured_us) {
        uint64_t interval_us = captured_us - s_store.last_epoch_captured_us;
        uint32_t interval_u32 = interval_us > UINT32_MAX
                                    ? UINT32_MAX
                                    : (uint32_t)interval_us;

        s_store.stats.epoch_interval_samples++;
        s_store.stats.epoch_interval_us_last = interval_u32;
        s_store.epoch_interval_us_total += interval_u32;
        if (s_store.stats.epoch_interval_us_min == 0U ||
            interval_u32 < s_store.stats.epoch_interval_us_min) {
            s_store.stats.epoch_interval_us_min = interval_u32;
        }
        if (interval_u32 > s_store.stats.epoch_interval_us_max) {
            s_store.stats.epoch_interval_us_max = interval_u32;
        }
    }
    s_store.last_epoch_captured_us = captured_us;
    s_store.last_epoch_capture_valid = true;
#endif

    m61_audio_epoch_t *slot = &s_store.slots[selected];
    slot->generation = s_store.stats.generation;
    slot->epoch = s_store.stats.next_epoch++;
    slot->captured_us = captured_us;
    slot->pcm_frames = 0U;
    slot->speaker_enabled = speaker_enabled ? 1U : 0U;
    slot->state = M61_AUDIO_EPOCH_FILLING;
    s_store.filling_slot = (uint8_t)selected;
    s_store.stats.epochs_started++;
    return selected;
}

void m61_audio_epoch_init(uint32_t generation)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.filling_slot = INVALID_SLOT;
    s_store.stats.generation = generation;
}

void m61_audio_epoch_reset(uint32_t generation)
{
    uintptr_t flags = epoch_lock();

    s_store.stats.generation = generation;
    s_store.stats.next_epoch = 0;
    s_store.stats.generation_resets++;
    s_store.filling_slot = INVALID_SLOT;
#if CONFIG_M61_PIPELINE_PROFILE
    s_store.last_epoch_capture_valid = false;
#endif
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_store.slots[i];
        if (slot->state == M61_AUDIO_EPOCH_ENCODING ||
            slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING) {
            slot->state = M61_AUDIO_EPOCH_CANCELLED_ENCODING;
        } else if (slot->state == M61_AUDIO_EPOCH_READING ||
                   slot->state == M61_AUDIO_EPOCH_CANCELLED_READING) {
            slot->state = M61_AUDIO_EPOCH_CANCELLED_READING;
        } else if (slot->state != M61_AUDIO_EPOCH_FREE) {
            slot->state = M61_AUDIO_EPOCH_FREE;
            s_store.stats.epochs_stale++;
        }
    }
    epoch_unlock(flags);
}

#ifdef M61_AUDIO_EPOCH_HOST_TEST
void m61_audio_epoch_host_reset_lock_count(void)
{
    s_host_lock_count = 0U;
}

uint32_t m61_audio_epoch_host_lock_count(void)
{
    return s_host_lock_count;
}

__attribute__((weak)) void m61_audio_epoch_host_before_encode_copy(void)
{
}

__attribute__((weak)) void m61_audio_epoch_host_after_read_claim(void)
{
}
#endif

void m61_audio_epoch_ingest_usb(const uint8_t *data, size_t len,
                                uint32_t generation, uint64_t captured_us,
                                bool speaker_enabled, uint16_t haptics_gain_q8)
{
    size_t frame = 0;
    size_t frames;

    if (data == NULL || len < USB_FRAME_BYTES) return;
    frames = len / USB_FRAME_BYTES;
    while (frame < frames) {
        uintptr_t flags = epoch_lock();
        int selected;
        uint16_t pcm_frame;
        size_t chunk_frames;
        uint8_t peak = 0U;
        bool nonzero = false;
        bool epoch_complete;

        if (generation != s_store.stats.generation) {
            epoch_unlock(flags);
            return;
        }
        if (s_store.filling_slot == INVALID_SLOT) {
            selected = allocate_slot_locked(captured_us, speaker_enabled);
            if (selected < 0) {
                epoch_unlock(flags);
                return;
            }
        } else {
            selected = s_store.filling_slot;
        }
        m61_audio_epoch_t *slot = &s_store.slots[selected];
        if (slot->state != M61_AUDIO_EPOCH_FILLING) {
            s_store.filling_slot = INVALID_SLOT;
            epoch_unlock(flags);
            continue;
        }
        pcm_frame = slot->pcm_frames;
        chunk_frames = frames - frame;
        if (chunk_frames > M61_AUDIO_EPOCH_USB_FRAMES - pcm_frame) {
            chunk_frames = M61_AUDIO_EPOCH_USB_FRAMES - pcm_frame;
        }
        epoch_unlock(flags);

        for (size_t i = 0; i < chunk_frames; i++) {
            const uint8_t *usb = data + (frame + i) * USB_FRAME_BYTES;
            uint16_t dst_frame = (uint16_t)(pcm_frame + i);

            slot->speaker_pcm[dst_frame * 2U] = read_i16_le(usb);
            slot->speaker_pcm[dst_frame * 2U + 1U] = read_i16_le(usb + 2U);
            if ((dst_frame % HAPTICS_DECIMATION_FRAMES) == 0U) {
                uint8_t out =
                    (uint8_t)((dst_frame / HAPTICS_DECIMATION_FRAMES) * 2U);
                slot->haptics[out] = (uint8_t)pcm16_to_i8(
                    read_i16_le(usb + 4U),
                    haptics_gain_q8);
                slot->haptics[out + 1U] = (uint8_t)pcm16_to_i8(
                    read_i16_le(usb + 6U),
                    haptics_gain_q8);
            }
        }
        slot->pcm_frames = (uint16_t)(pcm_frame + chunk_frames);
        frame += chunk_frames;
        epoch_complete = slot->pcm_frames == M61_AUDIO_EPOCH_USB_FRAMES;
        if (epoch_complete) {
            for (uint8_t i = 0; i < M61_AUDIO_EPOCH_HAPTICS_LEN; i++) {
                int8_t sample = (int8_t)slot->haptics[i];
                uint8_t magnitude = sample == INT8_MIN ? 128U :
                    (uint8_t)(sample < 0 ? -sample : sample);

                if (magnitude != 0U) nonzero = true;
                if (magnitude > peak) peak = magnitude;
            }
        }

        flags = epoch_lock();
        if (generation != s_store.stats.generation ||
            s_store.filling_slot != (uint8_t)selected ||
            slot->generation != generation ||
            slot->state != M61_AUDIO_EPOCH_FILLING) {
            epoch_unlock(flags);
            return;
        }
        if (epoch_complete) {
            s_store.stats.haptics_sample_pairs += 32U;
            if (nonzero) s_store.stats.haptics_nonzero_epochs++;
            s_store.stats.haptics_last_peak = peak;
            slot->state = slot->speaker_enabled ?
                M61_AUDIO_EPOCH_READY_ENCODE : M61_AUDIO_EPOCH_COMPLETE;
            if (!slot->speaker_enabled) s_store.stats.epochs_completed++;
            s_store.filling_slot = INVALID_SLOT;
        }
        epoch_unlock(flags);
    }
}

bool m61_audio_epoch_take_encode_job(m61_audio_epoch_encode_job_t *job)
{
    int selected = -1;
    uint32_t oldest = UINT32_MAX;
    uintptr_t flags;

    if (job == NULL) return false;
    flags = epoch_lock();
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_store.slots[i];
        if (slot->state == M61_AUDIO_EPOCH_READY_ENCODE &&
            slot->generation == s_store.stats.generation && slot->epoch < oldest) {
            oldest = slot->epoch;
            selected = i;
        }
    }
    if (selected < 0) {
        epoch_unlock(flags);
        return false;
    }
    m61_audio_epoch_t *slot = &s_store.slots[selected];
    slot->state = M61_AUDIO_EPOCH_ENCODING;
    job->generation = slot->generation;
    job->epoch = slot->epoch;
    job->captured_us = slot->captured_us;
    job->speaker_pcm = slot->speaker_pcm;
    s_store.stats.encode_jobs++;
    epoch_unlock(flags);
    return true;
}

bool m61_audio_epoch_complete_encode(uint32_t generation, uint32_t epoch,
                                     const uint8_t *opus, size_t opus_len)
{
    uintptr_t flags;
    int selected;
    m61_audio_epoch_t *slot;

    flags = epoch_lock();
    selected = find_slot_locked(generation, epoch);

    if (selected < 0) {
        epoch_unlock(flags);
        return false;
    }
    slot = &s_store.slots[selected];
    if (slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING ||
        generation != s_store.stats.generation) {
        slot->state = M61_AUDIO_EPOCH_FREE;
        s_store.stats.epochs_stale++;
        epoch_unlock(flags);
        return false;
    }
    if (slot->state != M61_AUDIO_EPOCH_ENCODING || opus == NULL ||
        opus_len != M61_AUDIO_EPOCH_OPUS_LEN) {
        if (slot->state == M61_AUDIO_EPOCH_ENCODING) {
            slot->state = M61_AUDIO_EPOCH_FREE;
            s_store.stats.encode_failures++;
            s_store.stats.epochs_dropped++;
        }
        epoch_unlock(flags);
        return false;
    }
    epoch_unlock(flags);

#ifdef M61_AUDIO_EPOCH_HOST_TEST
    m61_audio_epoch_host_before_encode_copy();
#endif
    memcpy(slot->speaker_opus, opus, M61_AUDIO_EPOCH_OPUS_LEN);

    flags = epoch_lock();
    if (slot->state == M61_AUDIO_EPOCH_CANCELLED_ENCODING ||
        generation != s_store.stats.generation) {
        slot->state = M61_AUDIO_EPOCH_FREE;
        s_store.stats.epochs_stale++;
        epoch_unlock(flags);
        return false;
    }
    if (slot->state != M61_AUDIO_EPOCH_ENCODING ||
        slot->generation != generation || slot->epoch != epoch) {
        epoch_unlock(flags);
        return false;
    }
    slot->state = M61_AUDIO_EPOCH_COMPLETE;
    s_store.stats.epochs_completed++;
    epoch_unlock(flags);
    return true;
}

bool m61_audio_epoch_take_adjacent_pair(m61_audio_epoch_pair_t *pair)
{
    int first = -1;
    int second = -1;
    uint32_t oldest = UINT32_MAX;
    uintptr_t flags;

    if (pair == NULL) return false;
    flags = epoch_lock();
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_store.slots[i];
        if (slot->state == M61_AUDIO_EPOCH_COMPLETE &&
            slot->generation == s_store.stats.generation && slot->epoch < oldest) {
            oldest = slot->epoch;
            first = i;
        }
    }
    if (first < 0) {
        epoch_unlock(flags);
        return false;
    }
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        m61_audio_epoch_t *slot = &s_store.slots[i];
        if (slot->state == M61_AUDIO_EPOCH_COMPLETE &&
            slot->generation == s_store.stats.generation &&
            slot->epoch == oldest + 1U) {
            second = i;
            break;
        }
    }
    if (second < 0) {
        bool newer_complete = false;
        for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
            m61_audio_epoch_t *slot = &s_store.slots[i];
            if (slot->state == M61_AUDIO_EPOCH_COMPLETE && slot->epoch > oldest) {
                newer_complete = true;
                break;
            }
        }
        if (newer_complete) {
            s_store.slots[first].state = M61_AUDIO_EPOCH_FREE;
            s_store.stats.epochs_dropped++;
            s_store.stats.epoch_gaps++;
            s_store.stats.epoch_discontinuities++;
        }
        epoch_unlock(flags);
        return false;
    }
    m61_audio_epoch_t *first_slot = &s_store.slots[first];
    m61_audio_epoch_t *second_slot = &s_store.slots[second];
    if (first_slot->speaker_enabled != second_slot->speaker_enabled) {
        first_slot->state = M61_AUDIO_EPOCH_FREE;
        s_store.stats.epochs_dropped++;
        s_store.stats.speaker_state_discontinuities++;
        s_store.stats.epoch_discontinuities++;
        epoch_unlock(flags);
        return false;
    }
    uint32_t generation = first_slot->generation;
    uint32_t first_epoch = first_slot->epoch;
    uint64_t first_captured_us = first_slot->captured_us;
    uint64_t second_captured_us = second_slot->captured_us;
    bool speaker_enabled = first_slot->speaker_enabled != 0U;

    first_slot->state = M61_AUDIO_EPOCH_READING;
    second_slot->state = M61_AUDIO_EPOCH_READING;
    epoch_unlock(flags);

#ifdef M61_AUDIO_EPOCH_HOST_TEST
    m61_audio_epoch_host_after_read_claim();
#endif
    pair->generation = generation;
    pair->first_epoch = first_epoch;
    pair->first_captured_us = first_captured_us;
    pair->second_captured_us = second_captured_us;
    pair->speaker_enabled = speaker_enabled;
    memcpy(pair->haptics[0], first_slot->haptics, M61_AUDIO_EPOCH_HAPTICS_LEN);
    memcpy(pair->haptics[1], second_slot->haptics, M61_AUDIO_EPOCH_HAPTICS_LEN);
    if (speaker_enabled) {
        memcpy(pair->speaker_opus[0], first_slot->speaker_opus,
               M61_AUDIO_EPOCH_OPUS_LEN);
        memcpy(pair->speaker_opus[1], second_slot->speaker_opus,
               M61_AUDIO_EPOCH_OPUS_LEN);
    } else {
        memset(pair->speaker_opus, 0, sizeof(pair->speaker_opus));
    }

    flags = epoch_lock();
    if (first_slot->state == M61_AUDIO_EPOCH_CANCELLED_READING ||
        second_slot->state == M61_AUDIO_EPOCH_CANCELLED_READING ||
        generation != s_store.stats.generation) {
        first_slot->state = M61_AUDIO_EPOCH_FREE;
        second_slot->state = M61_AUDIO_EPOCH_FREE;
        s_store.stats.epochs_stale += 2U;
        epoch_unlock(flags);
        return false;
    }
    if (first_slot->state != M61_AUDIO_EPOCH_READING ||
        second_slot->state != M61_AUDIO_EPOCH_READING ||
        first_slot->generation != generation ||
        second_slot->generation != generation ||
        first_slot->epoch != first_epoch ||
        second_slot->epoch != first_epoch + 1U) {
        epoch_unlock(flags);
        return false;
    }
    first_slot->state = M61_AUDIO_EPOCH_FREE;
    second_slot->state = M61_AUDIO_EPOCH_FREE;
    s_store.stats.epochs_emitted += 2U;
    s_store.stats.adjacent_pairs++;
    epoch_unlock(flags);
    return true;
}

void m61_audio_epoch_get_stats(m61_audio_epoch_stats_t *stats)
{
    uintptr_t flags;

    if (stats == NULL) return;
    flags = epoch_lock();
    *stats = s_store.stats;
#if CONFIG_M61_PIPELINE_PROFILE
    if (stats->epoch_interval_samples != 0U) {
        stats->epoch_interval_us_average =
            (uint32_t)(s_store.epoch_interval_us_total /
                       stats->epoch_interval_samples);
    }
#endif
    stats->filling_slots = 0;
    stats->encode_ready_slots = 0;
    stats->encoding_slots = 0;
    stats->complete_slots = 0;
    for (uint8_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT; i++) {
        switch (s_store.slots[i].state) {
            case M61_AUDIO_EPOCH_FILLING: stats->filling_slots++; break;
            case M61_AUDIO_EPOCH_READY_ENCODE: stats->encode_ready_slots++; break;
            case M61_AUDIO_EPOCH_ENCODING: stats->encoding_slots++; break;
            case M61_AUDIO_EPOCH_COMPLETE: stats->complete_slots++; break;
            case M61_AUDIO_EPOCH_READING: stats->complete_slots++; break;
            default: break;
        }
    }
    epoch_unlock(flags);
}
