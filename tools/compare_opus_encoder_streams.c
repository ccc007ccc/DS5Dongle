#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "opus.h"

/* Compares streams produced by test_opus_encoder_stream.c. */

enum {
    SAMPLE_RATE = 48000,
    FRAME_SAMPLES = 480,
    MAX_PACKET_BYTES = 1500,
    OPUS_LOOKAHEAD_SAMPLES = 312,
};

static int16_t generated_sample(unsigned frame, unsigned sample, uint32_t *lcg_state)
{
    unsigned section = frame / 200U;

    switch (section) {
    case 0:
        return 0;
    case 1:
        return sample == 0U ? 30000 : 0;
    case 2:
        return (sample & 1U) ? 20000 : -20000;
    case 3:
        return (int16_t)(((sample * 257U + frame * 97U) & 0xffffU) - 32768);
    case 4:
        *lcg_state = *lcg_state * 1664525U + 1013904223U;
        return (int16_t)(*lcg_state >> 16);
    default:
        return (int16_t)((((sample + frame) % 96U) * 640U) - 30720);
    }
}

static int read_packet(FILE *file, unsigned char *packet, int *length)
{
    uint16_t length_le;
    if (fread(&length_le, sizeof(length_le), 1, file) != 1)
        return feof(file) ? 0 : -1;
    if (length_le > MAX_PACKET_BYTES)
        return -1;
    if (fread(packet, 1, length_le, file) != length_le)
        return -1;
    *length = length_le;
    return 1;
}

int main(int argc, char **argv)
{
    FILE *left_file;
    FILE *right_file;
    OpusDecoder *left_decoder;
    OpusDecoder *right_decoder;
    unsigned char left_packet[MAX_PACKET_BYTES];
    unsigned char right_packet[MAX_PACKET_BYTES];
    opus_int16 left_pcm[FRAME_SAMPLES];
    opus_int16 right_pcm[FRAME_SAMPLES];
    uint64_t frames = 0;
    uint64_t packet_differences = 0;
    uint64_t sample_differences = 0;
    long double error_energy = 0;
    long double signal_energy = 0;
    long double left_source_error = 0;
    long double right_source_error = 0;
    long double source_energy = 0;
    int16_t *source_pcm = NULL;
    size_t source_capacity = 0;
    uint32_t lcg_state = 0x61d55eedU;
    int max_abs_error = 0;
    int error;

    if (argc != 3)
        return 2;
    left_file = fopen(argv[1], "rb");
    right_file = fopen(argv[2], "rb");
    if (left_file == NULL || right_file == NULL)
        return 3;
    left_decoder = opus_decoder_create(SAMPLE_RATE, 1, &error);
    if (left_decoder == NULL || error != OPUS_OK)
        return 4;
    right_decoder = opus_decoder_create(SAMPLE_RATE, 1, &error);
    if (right_decoder == NULL || error != OPUS_OK)
        return 4;

    for (;;) {
        int left_length;
        int right_length;
        int left_status = read_packet(left_file, left_packet, &left_length);
        int right_status = read_packet(right_file, right_packet, &right_length);
        int left_samples;
        int right_samples;
        int i;
        size_t source_offset = (size_t)frames * FRAME_SAMPLES;

        if (source_offset + FRAME_SAMPLES > source_capacity) {
            size_t new_capacity = source_capacity == 0 ? FRAME_SAMPLES * 1024U
                                                       : source_capacity * 2U;
            int16_t *new_pcm;
            while (new_capacity < source_offset + FRAME_SAMPLES)
                new_capacity *= 2U;
            new_pcm = realloc(source_pcm, new_capacity * sizeof(*source_pcm));
            if (new_pcm == NULL)
                return 7;
            source_pcm = new_pcm;
            source_capacity = new_capacity;
        }
        for (i = 0; i < FRAME_SAMPLES; i++)
            source_pcm[source_offset + (size_t)i] =
                generated_sample((unsigned)frames, (unsigned)i, &lcg_state);

        if (left_status == 0 && right_status == 0)
            break;
        if (left_status <= 0 || right_status <= 0)
            return 5;
        if (left_length != right_length)
            packet_differences++;
        else {
            for (i = 0; i < left_length; i++) {
                if (left_packet[i] != right_packet[i]) {
                    packet_differences++;
                    break;
                }
            }
        }
        left_samples = opus_decode(left_decoder, left_packet, left_length,
                                   left_pcm, FRAME_SAMPLES, 0);
        right_samples = opus_decode(right_decoder, right_packet, right_length,
                                    right_pcm, FRAME_SAMPLES, 0);
        if (left_samples != FRAME_SAMPLES || right_samples != FRAME_SAMPLES)
            return 6;
        for (i = 0; i < FRAME_SAMPLES; i++) {
            int difference = (int)left_pcm[i] - right_pcm[i];
            int absolute = difference < 0 ? -difference : difference;
            if (difference != 0)
                sample_differences++;
            if (absolute > max_abs_error)
                max_abs_error = absolute;
            error_energy += (long double)difference * difference;
            signal_energy += (long double)left_pcm[i] * left_pcm[i];
            if (source_offset + (size_t)i >= OPUS_LOOKAHEAD_SAMPLES) {
                int source = source_pcm[source_offset + (size_t)i - OPUS_LOOKAHEAD_SAMPLES];
                int left_error = (int)left_pcm[i] - source;
                int right_error = (int)right_pcm[i] - source;
                source_energy += (long double)source * source;
                left_source_error += (long double)left_error * left_error;
                right_source_error += (long double)right_error * right_error;
            }
        }
        frames++;
    }

    printf("frames=%llu packet_differences=%llu sample_differences=%llu "
           "max_abs_error=%d snr_db=%.3Lf left_source_snr_db=%.3Lf "
           "right_source_snr_db=%.3Lf source_snr_delta_db=%.6Lf\n",
           (unsigned long long)frames,
           (unsigned long long)packet_differences,
           (unsigned long long)sample_differences,
           max_abs_error,
           error_energy == 0 ? INFINITY : 10 * log10l(signal_energy / error_energy),
           10 * log10l(source_energy / left_source_error),
           10 * log10l(source_energy / right_source_error),
           10 * log10l(left_source_error / right_source_error));
    free(source_pcm);
    opus_decoder_destroy(left_decoder);
    opus_decoder_destroy(right_decoder);
    fclose(left_file);
    fclose(right_file);
    return 0;
}
