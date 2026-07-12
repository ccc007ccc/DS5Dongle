#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opus.h"

enum {
    SAMPLE_RATE = 48000,
    FRAME_SAMPLES = 480,
    PACKET_BYTES = 200,
    FRAME_COUNT = 1200,
};

static uint32_t lcg_state = 0x61d55eedU;

static int16_t next_pcm_sample(unsigned frame, unsigned sample)
{
    unsigned section = frame / 200U;

    switch (section) {
    case 0:
        return 0;
    case 1:
        return (sample == 0U) ? 30000 : 0;
    case 2:
        return (sample & 1U) ? 20000 : -20000;
    case 3:
        return (int16_t)(((sample * 257U + frame * 97U) & 0xffffU) - 32768);
    case 4:
        lcg_state = lcg_state * 1664525U + 1013904223U;
        return (int16_t)(lcg_state >> 16);
    default:
        return (int16_t)((((sample + frame) % 96U) * 640U) - 30720);
    }
}

int main(void)
{
    OpusEncoder *encoder;
    int error = OPUS_OK;
    int16_t pcm[FRAME_SAMPLES];
    unsigned char packet[PACKET_BYTES];
    unsigned frame;

    encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    if (encoder == NULL || error != OPUS_OK)
        return 1;

    if (opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(160000)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND)) != OPUS_OK) {
        opus_encoder_destroy(encoder);
        return 2;
    }

    for (frame = 0; frame < FRAME_COUNT; frame++) {
        unsigned sample;
        int encoded;
        uint16_t encoded_le;

        for (sample = 0; sample < FRAME_SAMPLES; sample++)
            pcm[sample] = next_pcm_sample(frame, sample);

        encoded = opus_encode(encoder, pcm, FRAME_SAMPLES, packet, sizeof(packet));
        if (encoded < 0) {
            opus_encoder_destroy(encoder);
            return 3;
        }
        encoded_le = (uint16_t)encoded;
        if (fwrite(&encoded_le, sizeof(encoded_le), 1, stdout) != 1 ||
            fwrite(packet, 1, (size_t)encoded, stdout) != (size_t)encoded) {
            opus_encoder_destroy(encoder);
            return 4;
        }
    }

    opus_encoder_destroy(encoder);
    return 0;
}
