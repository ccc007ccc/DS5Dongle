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

static void test_box_average_and_pair(void)
{
    uint8_t audio[2U * M61_AUDIO_EPOCH_USB_FRAMES * USB_FRAME_BYTES];
    m61_audio_epoch_pair_t pair;

    memset(audio, 0, sizeof(audio));
    for (size_t frame = 0; frame < 2U * M61_AUDIO_EPOCH_USB_FRAMES; frame++) {
        int16_t sample = (frame % 16U) == 15U ? 16000 : 0;
        set_frame(audio + frame * USB_FRAME_BYTES, 1000, sample,
                  (int16_t)-sample);
    }
    m61_audio_epoch_init(1);
    m61_audio_epoch_ingest_usb(audio, sizeof(audio), 1, 100, false, 256);
    assert(m61_audio_epoch_take_adjacent_pair(&pair));
    assert(pair.generation == 1 && pair.first_epoch == 0);
    assert(!pair.speaker_enabled);
    /* 16000 / 16 = 1000: verifies box averaging instead of decimation. */
    assert((int8_t)pair.haptics[0][0] == 3);
    assert((int8_t)pair.haptics[0][1] == -3);
    assert((int8_t)pair.haptics[1][62] == 3);
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
    assert(stats.epochs_started == 5);
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

int main(void)
{
    assert(sizeof(m61_audio_epoch_t) <= M61_AUDIO_EPOCH_RESERVED_SLOT_BYTES);
    test_box_average_and_pair();
    test_keyed_encode_pair();
    test_capacity_drops_oldest();
    test_generation_retires_encode_owner();
    puts("M61 dual-epoch audio tests passed.");
    return 0;
}
