#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_OUTPUT_REPORT31_BT_ID 0x31
#define DS5_OUTPUT_REPORT31_BT_LEN 78
#define DS5_OUTPUT_REPORT32_BT_ID 0x32
#define DS5_OUTPUT_REPORT32_BT_LEN 142
#define DS5_OUTPUT_SET_STATE_LEN 63

typedef struct {
    uint8_t sequence;
} dualsense_output_context_t;

void dualsense_output_init(dualsense_output_context_t *ctx);
uint32_t dualsense_output_crc32(const uint8_t *data, size_t len_without_crc);
bool dualsense_output_make_report31(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len);
bool dualsense_output_make_report32(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len);

#ifdef __cplusplus
}
#endif
