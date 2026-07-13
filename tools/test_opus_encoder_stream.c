#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"

enum {
    SAMPLE_RATE = 48000,
    FRAME_SAMPLES = 480,
    DEFAULT_PACKET_BYTES = 200,
    MAX_PACKET_BYTES = 1500,
    DEFAULT_BITRATE = 160000,
    DEFAULT_FRAME_COUNT = 1200,
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

int main(int argc, char **argv)
{
    OpusEncoder *encoder;
    int error = OPUS_OK;
    int16_t pcm[FRAME_SAMPLES];
    unsigned char packet[MAX_PACKET_BYTES];
    unsigned frame_count = DEFAULT_FRAME_COUNT;
    unsigned packet_bytes = DEFAULT_PACKET_BYTES;
    int bitrate = DEFAULT_BITRATE;
    int discard_output = 0;
    unsigned frame;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--discard") == 0) {
            discard_output = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            unsigned long value = strtoul(argv[++i], NULL, 0);
            if (value == 0 || value > 10000000UL)
                return 5;
            frame_count = (unsigned)value;
        } else if (strcmp(argv[i], "--packet-bytes") == 0 && i + 1 < argc) {
            unsigned long value = strtoul(argv[++i], NULL, 0);
            if (value == 0 || value > MAX_PACKET_BYTES)
                return 5;
            packet_bytes = (unsigned)value;
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            long value = strtol(argv[++i], NULL, 0);
            if (value <= 0 || value > 512000)
                return 5;
            bitrate = (int)value;
        } else {
            return 5;
        }
    }

    encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    if (encoder == NULL || error != OPUS_OK)
        return 1;

    if (opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND)) != OPUS_OK) {
        opus_encoder_destroy(encoder);
        return 2;
    }

    for (frame = 0; frame < frame_count; frame++) {
        unsigned sample;
        int encoded;
        uint16_t encoded_le;

        for (sample = 0; sample < FRAME_SAMPLES; sample++)
            pcm[sample] = next_pcm_sample(frame, sample);

        encoded = opus_encode(encoder, pcm, FRAME_SAMPLES, packet, packet_bytes);
        if (encoded < 0) {
            opus_encoder_destroy(encoder);
            return 3;
        }
        encoded_le = (uint16_t)encoded;
        if (!discard_output &&
            (fwrite(&encoded_le, sizeof(encoded_le), 1, stdout) != 1 ||
             fwrite(packet, 1, (size_t)encoded, stdout) != (size_t)encoded)) {
            opus_encoder_destroy(encoder);
            return 4;
        }
    }

    opus_encoder_destroy(encoder);
    return 0;
}
