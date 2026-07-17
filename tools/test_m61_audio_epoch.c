#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m61_audio_epoch.h"

#ifndef CONFIG_M61_PIPELINE_PROFILE
#define CONFIG_M61_PIPELINE_PROFILE 0
#endif

#define USB_FRAME_BYTES 8U

static bool reset_before_encode_copy;
static bool reset_after_read_claim;

void m61_audio_epoch_host_before_encode_copy(void)
{
    if (reset_before_encode_copy) {
        reset_before_encode_copy = false;
        m61_audio_epoch_reset(11);
    }
}

void m61_audio_epoch_host_after_read_claim(void)
{
    if (reset_after_read_claim) {
        reset_after_read_claim = false;
        m61_audio_epoch_reset(13);
    }
}

static void write_i16_le(uint8_t *dst, int16_t value)
{
    uint16_t raw = (uint16_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
}

static void set_frame(uint8_t *frame, int16_t speaker, int16_t left,
                      int16_t right)
{
    write_i16_le(frame, speaker);
    write_i16_le(frame + 2, (int16_t)-speaker);
    write_i16_le(frame + 4, left);
    write_i16_le(frame + 6, right);
}

static void fill_constant(uint8_t *data, size_t frames, int16_t speaker,
                          int16_t haptics)
{
    for (size_t i = 0; i < frames; i++) {
        set_frame(data + i * USB_FRAME_BYTES, speaker, haptics,
                  (int16_t)-haptics);
    }
}

static int8_t reference_pcm16_to_i8(int16_t sample, uint16_t gain_q8)
{
    int32_t scaled = ((int32_t)sample * gain_q8) / 256;
    int32_t value;

    if (scaled > INT16_MAX) scaled = INT16_MAX;
    if (scaled < INT16_MIN) scaled = INT16_MIN;
    value = (scaled * 127) / 32768;
    if (value > INT8_MAX) value = INT8_MAX;
    if (value < INT8_MIN) value = INT8_MIN;
    return (int8_t)value;
}

static void test_pcm_conversion_is_exhaustively_bit_exact(void)
{
    static const uint16_t gains[] = {
        0U, 1U, 127U, 255U, 256U, 257U, 512U, UINT16_MAX
    };

    for (size_t gain = 0; gain < sizeof(gains) / sizeof(gains[0]); gain++) {
        for (int32_t sample = INT16_MIN; sample <= INT16_MAX; sample++) {
            assert(m61_audio_epoch_host_pcm16_to_i8(
                       (int16_t)sample, gains[gain]) ==
                   reference_pcm16_to_i8((int16_t)sample, gains[gain]));
        }
    }
}

static void test_unaligned_usb_input_matches_aligned_path(void)
{
    uint8_t aligned[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t storage[sizeof(aligned) + 1U];
    uint8_t *unaligned = storage + 1U;
    m61_audio_epoch_pair_t aligned_pair;
    m61_audio_epoch_pair_t unaligned_pair;

    fill_constant(aligned, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 2345, 16000);
    memcpy(unaligned, aligned, sizeof(aligned));
    m61_audio_epoch_init(20);
    m61_audio_epoch_ingest_usb(aligned, sizeof(aligned), 20, 100, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&aligned_pair));
    m61_audio_epoch_init(21);
    m61_audio_epoch_ingest_usb(unaligned, sizeof(aligned), 21, 100,
                               false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&unaligned_pair));
    assert(memcmp(aligned_pair.haptics, unaligned_pair.haptics,
                  sizeof(aligned_pair.haptics)) == 0);
}

static void test_phase_decimation_and_pair(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_pair_t pair;

    memset(audio, 0, sizeof(audio));
    for (size_t frame = 0; frame < 2U * M61_AUDIO_EPOCH_USB_FRAMES; frame++) {
        int16_t sample = (frame % 16U) == 0U ? 16000 : 0;
        set_frame(audio + frame * USB_FRAME_BYTES, 1000, sample,
                  (int16_t)-sample);
    }
    m61_audio_epoch_init(1);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 1, 100, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 1 && pair.first_epoch == 0);
    assert(!pair.speaker_enabled);
    /* 48 kHz -> 3 kHz uses one phase-continuous sample every 16 frames. */
    assert((int8_t)pair.haptics[0][0] == 62);
    assert((int8_t)pair.haptics[0][1] == -62);
    assert((int8_t)pair.haptics[1][62] == 62);
    assert(!m61_audio_epoch_take_adjacent_pair(&pair));
}

static void test_keyed_encode_pair(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN];
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    fill_constant(audio, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 2345, 0);
    m61_audio_epoch_init(2);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 2, 200, true, 256);
    memset(opus, 0x5a, sizeof(opus));
    assert(m61_audio_epoch_take_encode_job(&job));
    assert(job.generation == 2 && job.epoch == 0 && job.speaker_pcm[0] == 2345);
    assert(!m61_audio_epoch_complete_encode(2, 7, opus, sizeof(opus)));
    assert(m61_audio_epoch_complete_encode(2, 0, opus, sizeof(opus)));
    memset(opus, 0xa5, sizeof(opus));
    assert(m61_audio_epoch_take_encode_job(&job));
    assert(job.epoch == 1);
    assert(m61_audio_epoch_complete_encode(2, 1, opus, sizeof(opus)));
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.speaker_enabled);
    assert(pair.speaker_opus[0][0] == 0x5a);
    assert(pair.speaker_opus[1][0] == 0xa5);
}

static void test_capacity_drops_oldest(void)
{
    uint8_t audio[(M61_AUDIO_EPOCH_SLOT_COUNT + 1U) *
                  M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_pair_t pair;
    m61_audio_epoch_stats_t stats;

    fill_constant(audio,
                  (M61_AUDIO_EPOCH_SLOT_COUNT + 1U) *
                      M61_AUDIO_EPOCH_USB_FRAMES,
                  0, 0);
    m61_audio_epoch_init(3);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 3, 300, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.first_epoch == 1);
    m61_audio_epoch_get_stats(&stats);
    assert(stats.epochs_started == M61_AUDIO_EPOCH_SLOT_COUNT + 1U);
    assert(stats.epochs_dropped == 1);
    assert(stats.adjacent_pairs == 1);
}

static void test_generation_retires_encode_owner(void)
{
    uint8_t one[M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t two[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN] = {0};
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    fill_constant(one, M61_AUDIO_EPOCH_USB_FRAMES, 1, 1);
    fill_constant(two, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 0, 0);
    m61_audio_epoch_init(4);
    m61_audio_epoch_ingest_usb(one, sizeof(one), 4, 400, true, 256);
    assert(m61_audio_epoch_take_encode_job(&job));
    m61_audio_epoch_reset(5);
    assert(!m61_audio_epoch_complete_encode(job.generation, job.epoch,
                                            opus, sizeof(opus)));
    m61_audio_epoch_ingest_usb(two, sizeof(two), 5, 500, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 5 && pair.first_epoch == 0);
}

static void test_reset_during_encode_copy_does_not_reuse_slot(void)
{
    uint8_t one[M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t two[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN] = {0x7a};
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    fill_constant(one, M61_AUDIO_EPOCH_USB_FRAMES, 1000, 1000);
    fill_constant(two, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 0, 0);
    m61_audio_epoch_init(10);
    m61_audio_epoch_ingest_usb(one, sizeof(one), 10, 100, true, 256);
    assert(m61_audio_epoch_take_encode_job(&job));
    reset_before_encode_copy = true;
    assert(!m61_audio_epoch_complete_encode(10, 0, opus, sizeof(opus)));
    m61_audio_epoch_ingest_usb(two, sizeof(two), 11, 200, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 11);
}

static void test_reset_during_pair_copy_does_not_publish_stale_pair(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_pair_t pair;

    fill_constant(audio, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 0, 1000);
    m61_audio_epoch_init(12);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 12, 100, false, 256);
    reset_after_read_claim = true;
    assert(!m61_audio_epoch_take_adjacent_pair(&pair));
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 13, 200, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 13);
}

static void test_ingress_lock_count_is_per_chunk_not_per_frame(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];

    fill_constant(audio, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 1000, 500);
    m61_audio_epoch_init(9);
    m61_audio_epoch_host_reset_lock_count();
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 9, 100, false, 256);
    assert(m61_audio_epoch_host_lock_count() == 4U);
}

static void test_epoch_cadence_and_no_deadline_fallback(void)
{
    uint8_t audio[M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_stats_t stats;

    fill_constant(audio, M61_AUDIO_EPOCH_USB_FRAMES, 1234, 1000);
    m61_audio_epoch_init(6);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 6, 1000, true, 256);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 6, 11667, true, 256);
    assert(m61_audio_epoch_take_encode_job(&job));
    m61_audio_epoch_get_stats(&stats);
#if CONFIG_M61_PIPELINE_PROFILE
    assert(stats.epoch_interval_samples == 1);
    assert(stats.epoch_interval_us_last == 10667);
    assert(stats.epoch_interval_us_average == 10667);
    assert(stats.epoch_interval_us_min == 10667);
    assert(stats.epoch_interval_us_max == 10667);
#else
    assert(stats.epoch_interval_samples == 0);
#endif
    assert(stats.deadline_fallback_pairs == 0);
    assert(stats.encode_jobs_cancelled == 0);
}

int main(void)
{
    assert(sizeof(m61_audio_epoch_t) <= M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES);
    test_pcm_conversion_is_exhaustively_bit_exact();
    test_unaligned_usb_input_matches_aligned_path();
    test_phase_decimation_and_pair();
    test_ingress_lock_count_is_per_chunk_not_per_frame();
    test_keyed_encode_pair();
    test_capacity_drops_oldest();
    test_generation_retires_encode_owner();
    test_reset_during_encode_copy_does_not_reuse_slot();
    test_reset_during_pair_copy_does_not_publish_stale_pair();
    test_epoch_cadence_and_no_deadline_fallback();
    puts("M61 dual-epoch audio tests passed.");
    return 0;
}
