#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m61_audio_epoch.h"

#define USB_FRAME_BYTES 8U

static void write_i16_le(uint8_t *dst, int16_t value)
{
    uint16_t raw = (uint16_t)value;
    dst[0] = (uint8_t)raw;
    dst[1] = (uint8_t)(raw >> 8);
}

static void make_audio(uint8_t *data, size_t frames, int16_t speaker,
                       int16_t haptics)
{
    for (size_t i = 0; i < frames; i++) {
        uint8_t *frame = data + i * USB_FRAME_BYTES;
        write_i16_le(frame, speaker);
        write_i16_le(frame + 2, (int16_t)-speaker);
        write_i16_le(frame + 4, haptics);
        write_i16_le(frame + 6, (int16_t)-haptics);
    }
}

static void test_inactive_speaker_pair(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    make_audio(audio, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 1000, 16000);
    m61_audio_epoch_init(1);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 1, 100, false, 256);
    assert(!m61_audio_epoch_take_encode_job(&job));
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 1);
    assert(pair.first_epoch == 0);
    assert(!pair.speaker_enabled);
    assert(pair.haptics[0][0] != 0);
    assert(pair.haptics[0][0] == pair.haptics[1][0]);
    assert(!m61_audio_epoch_take_adjacent_pair(&pair));
}

static void test_keyed_encode_completion(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN];
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    make_audio(audio, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 2000, 0);
    memset(opus, 0x5a, sizeof(opus));
    m61_audio_epoch_init(2);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 2, 200, true, 256);
    assert(m61_audio_epoch_take_encode_job(&job));
    assert(job.generation == 2 && job.epoch == 0);
    assert(job.speaker_pcm[0] == 2000);
    assert(m61_audio_epoch_complete_encode(job.generation, job.epoch,
                                           opus, sizeof(opus)));
    memset(opus, 0xa5, sizeof(opus));
    assert(m61_audio_epoch_take_encode_job(&job));
    assert(job.epoch == 1);
    assert(m61_audio_epoch_complete_encode(job.generation, job.epoch,
                                           opus, sizeof(opus)));
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.speaker_enabled);
    assert(pair.speaker_opus[0][0] == 0x5a);
    assert(pair.speaker_opus[1][0] == 0xa5);
}

static void test_gap_and_capacity_accounting(void)
{
    uint8_t audio[(M61_AUDIO_EPOCH_SLOT_COUNT + 1U) *
                  M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_pair_t pair;
    m61_audio_epoch_stats_t stats;

    make_audio(audio,
               (M61_AUDIO_EPOCH_SLOT_COUNT + 1U) *
                   M61_AUDIO_EPOCH_USB_FRAMES,
               0, 0);
    m61_audio_epoch_init(3);
    m61_audio_epoch_ingest_usb(audio,
                               2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES,
                               3, 300, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.first_epoch == 0);

    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 3, 400, false, 256);
    for (uint32_t i = 0; i < M61_AUDIO_EPOCH_SLOT_COUNT / 2U; i++) {
        assert(m61_audio_epoch_take_adjacent_pair(&pair));
        assert(pair.first_epoch == 3U + i * 2U);
    }
    m61_audio_epoch_get_stats(&stats);
    assert(stats.epochs_dropped == 1);
    assert(stats.epoch_gaps == 1);
    assert(stats.epoch_discontinuities == 1);
    assert(stats.adjacent_pairs == 1U + M61_AUDIO_EPOCH_SLOT_COUNT / 2U);
}

static void test_generation_reset_retires_encode_owner(void)
{
    uint8_t one[M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t two[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN] = {0};
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;

    make_audio(one, M61_AUDIO_EPOCH_USB_FRAMES, 1, 1);
    make_audio(two, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 0, 0);
    m61_audio_epoch_init(4);
    m61_audio_epoch_ingest_usb(one, sizeof(one), 4, 500, true, 256);
    assert(m61_audio_epoch_take_encode_job(&job));
    m61_audio_epoch_reset(5);
    assert(!m61_audio_epoch_complete_encode(job.generation, job.epoch,
                                            opus, sizeof(opus)));
    m61_audio_epoch_ingest_usb(two, sizeof(two), 5, 600, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 5 && pair.first_epoch == 0);
}

static void test_speaker_transition_never_cross_pairs(void)
{
    uint8_t one[M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t two[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    uint8_t opus[M61_AUDIO_EPOCH_OPUS_LEN] = {0};
    m61_audio_epoch_encode_job_t job;
    m61_audio_epoch_pair_t pair;
    m61_audio_epoch_stats_t stats;

    make_audio(one, M61_AUDIO_EPOCH_USB_FRAMES, 0, 0);
    make_audio(two, 2U * M61_AUDIO_EPOCH_USB_FRAMES, 2, 0);
    m61_audio_epoch_init(6);
    m61_audio_epoch_ingest_usb(one, sizeof(one), 6, 700, false, 256);
    m61_audio_epoch_ingest_usb(two, sizeof(two), 6, 800, true, 256);
    while (m61_audio_epoch_take_encode_job(&job)) {
        assert(m61_audio_epoch_complete_encode(job.generation, job.epoch,
                                               opus, sizeof(opus)));
    }
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.first_epoch == 1);
    assert(pair.speaker_enabled);
    m61_audio_epoch_get_stats(&stats);
    assert(stats.speaker_state_discontinuities == 1);
    assert(stats.epochs_dropped == 1);
}

int main(void)
{
    assert(sizeof(m61_audio_epoch_t) <= M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES);
    test_inactive_speaker_pair();
    test_keyed_encode_completion();
    test_gap_and_capacity_accounting();
    test_generation_reset_retires_encode_owner();
    test_speaker_transition_never_cross_pairs();
    puts("M61 audio epoch tests passed.");
    return 0;
}
