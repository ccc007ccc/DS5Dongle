#include "dualsense_output.h"

#include <string.h>

#define DS5_OUTPUT_CRC32_SEED 0xA2
#define DS5_OUTPUT_TAG 0x10

static const uint8_t s_ds5_set_state_default[DS5_OUTPUT_SET_STATE_LEN] = {
    0xFD, 0xF7, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x09,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00,
    0x00, 0x02, 0x01, 0x00, 0xFF, 0xD7, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint32_t crc32_le_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static void fill_crc(uint8_t *report, size_t len)
{
    uint32_t crc = dualsense_output_crc32(report, len - 4);

    report[len - 4] = (uint8_t)(crc & 0xFF);
    report[len - 3] = (uint8_t)((crc >> 8) & 0xFF);
    report[len - 2] = (uint8_t)((crc >> 16) & 0xFF);
    report[len - 1] = (uint8_t)((crc >> 24) & 0xFF);
}

void dualsense_output_init(dualsense_output_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->sequence = 0;
    }
}

uint32_t dualsense_output_crc32(const uint8_t *data, size_t len_without_crc)
{
    uint8_t seed = DS5_OUTPUT_CRC32_SEED;
    uint32_t crc = 0xFFFFFFFFU;

    crc = crc32_le_update(crc, &seed, 1);
    crc = crc32_le_update(crc, data, len_without_crc);
    return ~crc;
}

bool dualsense_output_make_report31(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len)
{
    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_REPORT31_BT_LEN) {
        return false;
    }

    memset(report, 0, DS5_OUTPUT_REPORT31_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT31_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_TAG;
    memcpy(report + 3, s_ds5_set_state_default, DS5_OUTPUT_SET_STATE_LEN);
    fill_crc(report, DS5_OUTPUT_REPORT31_BT_LEN);
    return true;
}

bool dualsense_output_make_report32(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len)
{
    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_REPORT32_BT_LEN) {
        return false;
    }

    memset(report, 0, DS5_OUTPUT_REPORT32_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT32_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = 0x90;
    report[3] = DS5_OUTPUT_SET_STATE_LEN;
    memcpy(report + 4, s_ds5_set_state_default, DS5_OUTPUT_SET_STATE_LEN);
    fill_crc(report, DS5_OUTPUT_REPORT32_BT_LEN);
    return true;
}
