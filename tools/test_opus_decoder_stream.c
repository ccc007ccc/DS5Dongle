#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"

enum { SAMPLE_RATE = 48000, FRAME_SAMPLES = 480, MAX_PACKET_BYTES = 1500 };

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned repeat = 1;
    FILE *stream;
    OpusDecoder *decoder;
    unsigned char packet[MAX_PACKET_BYTES];
    opus_int16 pcm[FRAME_SAMPLES];
    uint64_t frames = 0;
    uint32_t checksum = 0;
    int error;
    unsigned pass;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            unsigned long value = strtoul(argv[++i], NULL, 0);
            if (value == 0 || value > 1000000UL)
                return 2;
            repeat = (unsigned)value;
        } else {
            return 2;
        }
    }
    if (path == NULL)
        return 2;
    stream = fopen(path, "rb");
    if (stream == NULL)
        return 3;
    decoder = opus_decoder_create(SAMPLE_RATE, 1, &error);
    if (decoder == NULL || error != OPUS_OK)
        return 4;

    for (pass = 0; pass < repeat; pass++) {
        if (pass != 0) {
            rewind(stream);
            if (opus_decoder_ctl(decoder, OPUS_RESET_STATE) != OPUS_OK)
                return 5;
        }
        for (;;) {
            uint16_t packet_bytes;
            int decoded;
            unsigned i;

            if (fread(&packet_bytes, sizeof(packet_bytes), 1, stream) != 1) {
                if (feof(stream))
                    break;
                return 6;
            }
            if (packet_bytes == 0 || packet_bytes > MAX_PACKET_BYTES ||
                fread(packet, 1, packet_bytes, stream) != packet_bytes)
                return 6;
            decoded = opus_decode(decoder, packet, packet_bytes,
                                  pcm, FRAME_SAMPLES, 0);
            if (decoded != FRAME_SAMPLES)
                return 7;
            for (i = 0; i < FRAME_SAMPLES; i += 32)
                checksum = checksum * 33U + (uint16_t)pcm[i];
            frames++;
        }
    }

    printf("frames=%llu checksum=%08x\n",
           (unsigned long long)frames, checksum);
    opus_decoder_destroy(decoder);
    fclose(stream);
    return 0;
}
