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
#define DS5_OUTPUT_REPORT36_BT_ID 0x36
#define DS5_OUTPUT_REPORT36_BT_LEN 398
#define DS5_OUTPUT_SET_STATE_LEN 63
#define DS5_USB_SET_STATE_LEN 47
#define DS5_OUTPUT_HAPTICS_BLOCK_LEN 64
#define DS5_OUTPUT_SPEAKER_OPUS_LEN 200
#define DS5_OUTPUT_SPEAKER_BLOCK_ID 0x13
#define DS5_OUTPUT_HEADSET_BLOCK_ID 0x16
#define DS5_FEATURE_CRC32_SEED 0x53

typedef struct {
    uint8_t sequence;
    uint8_t audio_packet_counter;
    uint8_t set_state[DS5_OUTPUT_SET_STATE_LEN];
} dualsense_output_context_t;

void dualsense_output_init(dualsense_output_context_t *ctx);
void dualsense_output_apply_audio_controls(dualsense_output_context_t *ctx,
                                           uint8_t speaker_volume,
                                           uint8_t headphone_volume,
                                           uint8_t mic_volume,
                                           bool speaker_mute,
                                           bool headphone_mute,
                                           bool mic_mute);
uint32_t dualsense_output_crc32(const uint8_t *data, size_t len_without_crc);
uint32_t dualsense_feature_crc32(const uint8_t *data, size_t len_without_crc);
void dualsense_feature_fill_crc(uint8_t *data, size_t len);
bool dualsense_output_make_report31(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len);
bool dualsense_output_make_report32(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len);
bool dualsense_output_make_report36_haptics(dualsense_output_context_t *ctx,
                                            const uint8_t *haptics,
                                            size_t haptics_len,
                                            bool mic_active,
                                            uint8_t audio_buffer_len,
                                            uint8_t *report,
                                            size_t report_len);
bool dualsense_output_make_report36_audio(dualsense_output_context_t *ctx,
                                          const uint8_t *haptics,
                                          size_t haptics_len,
                                          const uint8_t *speaker_opus,
                                          size_t speaker_opus_len,
                                          uint8_t speaker_block_id,
                                          bool mic_active,
                                          uint8_t audio_buffer_len,
                                          uint8_t *report,
                                          size_t report_len);
bool dualsense_output_make_report32_audio_status(dualsense_output_context_t *ctx,
                                                 bool mic_active,
                                                 uint8_t audio_buffer_len,
                                                 uint8_t *report,
                                                 size_t report_len);
bool dualsense_output_make_report31_from_usb(dualsense_output_context_t *ctx,
                                             const uint8_t *usb_payload,
                                             size_t usb_payload_len,
                                             uint8_t *report,
                                             size_t report_len);

#ifdef __cplusplus
}
#endif
