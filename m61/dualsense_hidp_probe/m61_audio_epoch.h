#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M61_AUDIO_EPOCH_SLOT_COUNT 8U
#define M61_AUDIO_EPOCH_USB_FRAMES 512U
#define M61_AUDIO_EPOCH_SPEAKER_CHANNELS 2U
#define M61_AUDIO_EPOCH_HAPTICS_LEN 64U
#define M61_AUDIO_EPOCH_OPUS_LEN 200U
#define M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES 2344U

typedef enum {
    M61_AUDIO_EPOCH_FREE = 0,
    M61_AUDIO_EPOCH_FILLING,
    M61_AUDIO_EPOCH_READY_ENCODE,
    M61_AUDIO_EPOCH_ENCODING,
    M61_AUDIO_EPOCH_COMPLETE,
    M61_AUDIO_EPOCH_READING,
    M61_AUDIO_EPOCH_CANCELLED_ENCODING,
    M61_AUDIO_EPOCH_CANCELLED_READING,
} m61_audio_epoch_state_t;

typedef struct {
    uint32_t generation;
    uint32_t epoch;
    uint64_t captured_us;
    uint16_t pcm_frames;
    uint8_t state;
    uint8_t speaker_enabled;
    uint8_t haptics[M61_AUDIO_EPOCH_HAPTICS_LEN];
    int16_t speaker_pcm[M61_AUDIO_EPOCH_USB_FRAMES *
                        M61_AUDIO_EPOCH_SPEAKER_CHANNELS];
    uint8_t speaker_opus[M61_AUDIO_EPOCH_OPUS_LEN];
    uint8_t haptics_peak;
    uint8_t haptics_nonzero;
} m61_audio_epoch_t;

typedef struct {
    uint32_t generation;
    uint32_t epoch;
    uint64_t captured_us;
    const int16_t *speaker_pcm;
} m61_audio_epoch_encode_job_t;

typedef struct {
    uint32_t generation;
    uint32_t first_epoch;
    uint64_t first_captured_us;
    uint64_t second_captured_us;
    bool speaker_enabled;
    uint8_t haptics[2][M61_AUDIO_EPOCH_HAPTICS_LEN];
    uint8_t speaker_opus[2][M61_AUDIO_EPOCH_OPUS_LEN];
} m61_audio_epoch_pair_t;

typedef struct {
    uint32_t generation;
    uint32_t next_epoch;
    uint32_t generation_resets;
    uint32_t epochs_started;
    uint32_t epochs_completed;
    uint32_t epochs_emitted;
    uint32_t epochs_dropped;
    uint32_t epochs_stale;
    uint32_t encode_jobs;
    uint32_t encode_failures;
    uint32_t adjacent_pairs;
    uint32_t epoch_gaps;
    uint32_t epoch_discontinuities;
    uint32_t speaker_state_discontinuities;
    uint32_t epoch_interval_samples;
    uint32_t epoch_interval_us_last;
    uint32_t epoch_interval_us_average;
    uint32_t epoch_interval_us_min;
    uint32_t epoch_interval_us_max;
    /* Legacy compatibility counters. The deadline fallback was removed and
     * these must remain zero in production. */
    uint32_t deadline_fallback_pairs;
    uint32_t encode_jobs_cancelled;
    uint32_t haptics_nonzero_epochs;
    uint32_t haptics_sample_pairs;
    uint8_t haptics_last_peak;
    uint8_t filling_slots;
    uint8_t encode_ready_slots;
    uint8_t encoding_slots;
    uint8_t complete_slots;
} m61_audio_epoch_stats_t;

void m61_audio_epoch_init(uint32_t generation);
void m61_audio_epoch_reset(uint32_t generation);
void m61_audio_epoch_ingest_usb(const uint8_t *data,
                                size_t len,
                                uint32_t generation,
                                uint64_t captured_us,
                                bool speaker_enabled,
                                uint16_t haptics_gain_q8);
bool m61_audio_epoch_take_encode_job(m61_audio_epoch_encode_job_t *job);
bool m61_audio_epoch_complete_encode(uint32_t generation,
                                     uint32_t epoch,
                                     const uint8_t *opus,
                                     size_t opus_len);
bool m61_audio_epoch_take_adjacent_pair(m61_audio_epoch_pair_t *pair);
void m61_audio_epoch_get_stats(m61_audio_epoch_stats_t *stats);

#ifdef M61_AUDIO_EPOCH_HOST_TEST
void m61_audio_epoch_host_reset_lock_count(void);
uint32_t m61_audio_epoch_host_lock_count(void);
int8_t m61_audio_epoch_host_pcm16_to_i8(int16_t sample, uint16_t gain_q8);
#endif

#ifdef __cplusplus
}
#endif
