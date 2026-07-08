#include "dualsense_output.h"

#include <string.h>

#define DS5_OUTPUT_CRC32_SEED 0xA2
#define DS5_OUTPUT_TAG 0x10
#define DS5_OUTPUT_AUDIO_TAG 0x91
#define DS5_OUTPUT_AUDIO_STATE_TAG 0x90
#define DS5_OUTPUT_HAPTICS_TAG 0xD2

#define DS5_STATE_ENABLE_RUMBLE_EMULATION 0x01
#define DS5_STATE_USE_RUMBLE_NOT_HAPTICS 0x02
#define DS5_STATE_ALLOW_RIGHT_TRIGGER_FFB 0x04
#define DS5_STATE_ALLOW_LEFT_TRIGGER_FFB 0x08
#define DS5_STATE_ALLOW_HEADPHONE_VOLUME 0x10
#define DS5_STATE_ALLOW_SPEAKER_VOLUME 0x20
#define DS5_STATE_ALLOW_MIC_VOLUME 0x40
#define DS5_STATE_ALLOW_AUDIO_CONTROL 0x80
#define DS5_STATE_ALLOW_MUTE_LIGHT 0x01
#define DS5_STATE_ALLOW_AUDIO_MUTE 0x02
#define DS5_STATE_ALLOW_LED_COLOR 0x04
#define DS5_STATE_ALLOW_PLAYER_INDICATORS 0x10
#define DS5_STATE_ALLOW_HAPTIC_LOW_PASS 0x20
#define DS5_STATE_ALLOW_MOTOR_POWER_LEVEL 0x40
#define DS5_STATE_ALLOW_AUDIO_CONTROL2 0x80
#define DS5_STATE_ALLOW_LIGHT_BRIGHTNESS 0x01
#define DS5_STATE_ALLOW_LIGHT_FADE 0x02
#define DS5_STATE_ENABLE_IMPROVED_RUMBLE 0x04
#define DS5_STATE_USE_RUMBLE_NOT_HAPTICS2 0x08

#define DS5_STATE_FLAGS0 0
#define DS5_STATE_FLAGS1 1
#define DS5_STATE_RUMBLE_RIGHT 2
#define DS5_STATE_RUMBLE_LEFT 3
#define DS5_STATE_VOLUME_HEADPHONES 4
#define DS5_STATE_VOLUME_SPEAKER 5
#define DS5_STATE_VOLUME_MIC 6
#define DS5_STATE_AUDIO_CONTROL 7
#define DS5_STATE_MUTE_LIGHT 8
#define DS5_STATE_AUDIO_MUTE 9
#define DS5_STATE_RIGHT_TRIGGER_FFB 10
#define DS5_STATE_LEFT_TRIGGER_FFB 21
#define DS5_STATE_MOTOR_POWER_LEVEL 36
#define DS5_STATE_AUDIO_CONTROL2 37
#define DS5_STATE_FLAGS2 38
#define DS5_STATE_HAPTIC_LOW_PASS 39
#define DS5_STATE_LIGHT_FADE 41
#define DS5_STATE_LIGHT_BRIGHTNESS 42
#define DS5_STATE_PLAYER_LIGHTS 43
#define DS5_STATE_LED_RED 44

static const uint8_t s_ds5_set_state_default[DS5_OUTPUT_SET_STATE_LEN] = {
    0x00, 0x04, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x02, 0x00, 0x00, 0xFF, 0xD7, 0x00, 0x00,
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

static void reset_set_state(dualsense_output_context_t *ctx)
{
    memcpy(ctx->set_state, s_ds5_set_state_default, sizeof(ctx->set_state));
}

static void ensure_set_state(dualsense_output_context_t *ctx)
{
    if (ctx->set_state[DS5_STATE_FLAGS0] == 0 &&
        ctx->set_state[DS5_STATE_FLAGS1] == 0) {
        reset_set_state(ctx);
    }
}

static void copy_set_state(uint8_t *dst, const dualsense_output_context_t *ctx)
{
    memcpy(dst, ctx->set_state, DS5_OUTPUT_SET_STATE_LEN);
}

void dualsense_output_init(dualsense_output_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->sequence = 0;
        ctx->audio_packet_counter = 0;
        reset_set_state(ctx);
    }
}

void dualsense_output_apply_audio_controls(dualsense_output_context_t *ctx,
                                           uint8_t speaker_volume,
                                           uint8_t headphone_volume,
                                           uint8_t mic_volume,
                                           bool speaker_mute,
                                           bool headphone_mute,
                                           bool mic_mute)
{
    uint8_t mute;

    if (ctx == NULL) {
        return;
    }

    memset(ctx->set_state, 0, sizeof(ctx->set_state));
    ctx->set_state[DS5_STATE_FLAGS0] = DS5_STATE_ALLOW_HEADPHONE_VOLUME |
                                       DS5_STATE_ALLOW_SPEAKER_VOLUME |
                                       DS5_STATE_ALLOW_MIC_VOLUME;
    ctx->set_state[DS5_STATE_FLAGS1] = DS5_STATE_ALLOW_AUDIO_MUTE;
    ctx->set_state[DS5_STATE_VOLUME_SPEAKER] = speaker_volume;
    ctx->set_state[DS5_STATE_VOLUME_HEADPHONES] = headphone_volume;
    ctx->set_state[DS5_STATE_VOLUME_MIC] = mic_volume;

    mute = 0;
    if (mic_mute) {
        mute |= 0x10U;
    }
    if (speaker_mute) {
        mute |= 0x20U;
    }
    if (headphone_mute) {
        mute |= 0x40U;
    }
    ctx->set_state[DS5_STATE_AUDIO_MUTE] = mute;
}

uint32_t dualsense_output_crc32(const uint8_t *data, size_t len_without_crc)
{
    uint8_t seed = DS5_OUTPUT_CRC32_SEED;
    uint32_t crc = 0xFFFFFFFFU;

    crc = crc32_le_update(crc, &seed, 1);
    crc = crc32_le_update(crc, data, len_without_crc);
    return ~crc;
}

uint32_t dualsense_feature_crc32(const uint8_t *data, size_t len_without_crc)
{
    uint8_t seed = DS5_FEATURE_CRC32_SEED;
    uint32_t crc = 0xFFFFFFFFU;

    crc = crc32_le_update(crc, &seed, 1);
    crc = crc32_le_update(crc, data, len_without_crc);
    return ~crc;
}

void dualsense_feature_fill_crc(uint8_t *data, size_t len)
{
    uint32_t crc;

    if (data == NULL || len < 4) {
        return;
    }

    crc = dualsense_feature_crc32(data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xFF);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xFF);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xFF);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xFF);
}

bool dualsense_output_make_report31(dualsense_output_context_t *ctx,
                                    uint8_t *report,
                                    size_t report_len)
{
    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_REPORT31_BT_LEN) {
        return false;
    }

    ensure_set_state(ctx);
    memset(report, 0, DS5_OUTPUT_REPORT31_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT31_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_TAG;
    copy_set_state(report + 3, ctx);
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

    ensure_set_state(ctx);
    memset(report, 0, DS5_OUTPUT_REPORT32_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT32_BT_ID;
    report[1] = 0x10;
    report[2] = 0x90;
    report[3] = DS5_OUTPUT_SET_STATE_LEN;
    copy_set_state(report + 4, ctx);
    fill_crc(report, DS5_OUTPUT_REPORT32_BT_LEN);
    return true;
}

bool dualsense_output_make_audio_rt(dualsense_output_context_t *ctx,
                                    const uint8_t *haptics0,
                                    const uint8_t *haptics1,
                                    const uint8_t *speaker_opus0,
                                    const uint8_t *speaker_opus1,
                                    size_t speaker_opus_len,
                                    uint8_t speaker_block_id,
                                    bool mic_active,
                                    uint8_t audio_buffer_len,
                                    uint8_t *report,
                                    size_t report_len)
{
    bool have_haptics = haptics0 != NULL || haptics1 != NULL;
    bool have_speaker = speaker_opus0 != NULL || speaker_opus1 != NULL;
    size_t copy_len = speaker_opus_len;

    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_AUDIO_RT_BT_LEN ||
        (!have_haptics && !have_speaker)) {
        return false;
    }

    if (audio_buffer_len == 0) {
        audio_buffer_len = 48;
    }
    if (copy_len > DS5_OUTPUT_SPEAKER_OPUS_LEN) {
        copy_len = DS5_OUTPUT_SPEAKER_OPUS_LEN;
    }

    memset(report, 0, DS5_OUTPUT_AUDIO_RT_BT_LEN);
    report[0] = DS5_OUTPUT_AUDIO_RT_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_AUDIO_TAG;
    report[3] = 6;
    report[4] = mic_active ? 0x7F : 0x7E;
    report[5] = audio_buffer_len;
    report[6] = audio_buffer_len;
    report[7] = audio_buffer_len;
    report[8] = audio_buffer_len;
    ctx->audio_packet_counter = (uint8_t)(ctx->audio_packet_counter + 2U);
    report[9] = ctx->audio_packet_counter;

    report[10] = DS5_OUTPUT_HAPTICS_TAG;
    report[11] = DS5_OUTPUT_HAPTICS_BLOCK_LEN;
    if (haptics0 != NULL) {
        memcpy(report + 12, haptics0, DS5_OUTPUT_HAPTICS_BLOCK_LEN);
    }
    if (haptics1 != NULL) {
        memcpy(report + 12 + DS5_OUTPUT_HAPTICS_BLOCK_LEN,
               haptics1,
               DS5_OUTPUT_HAPTICS_BLOCK_LEN);
    }

    if (have_speaker) {
        if (speaker_block_id == 0) {
            speaker_block_id = DS5_OUTPUT_SPEAKER_BLOCK_ID;
        }
        report[140] = (uint8_t)(speaker_block_id | 0xC0);
        report[141] = DS5_OUTPUT_SPEAKER_OPUS_LEN;
        if (speaker_opus0 != NULL && copy_len != 0) {
            memcpy(report + 142, speaker_opus0, copy_len);
        }
        if (speaker_opus1 != NULL && copy_len != 0) {
            memcpy(report + 142 + DS5_OUTPUT_SPEAKER_OPUS_LEN,
                   speaker_opus1,
                   copy_len);
        }
    }

    fill_crc(report, DS5_OUTPUT_AUDIO_RT_BT_LEN);
    return true;
}

bool dualsense_output_make_report32_audio_status(dualsense_output_context_t *ctx,
                                                 bool mic_active,
                                                 uint8_t audio_buffer_len,
                                                 uint8_t *report,
                                                 size_t report_len)
{
    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_REPORT32_BT_LEN) {
        return false;
    }

    (void)audio_buffer_len;

    memset(report, 0, DS5_OUTPUT_REPORT32_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT32_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_AUDIO_TAG;
    report[3] = 1;
    report[4] = mic_active ? 0x03 : 0x02;

    fill_crc(report, DS5_OUTPUT_REPORT32_BT_LEN);
    return true;
}

bool dualsense_output_make_report31_from_usb(dualsense_output_context_t *ctx,
                                             const uint8_t *usb_payload,
                                             size_t usb_payload_len,
                                             uint8_t *report,
                                             size_t report_len)
{
    if (ctx == NULL || usb_payload == NULL || report == NULL ||
        report_len < DS5_OUTPUT_REPORT31_BT_LEN) {
        return false;
    }
    if (usb_payload_len < DS5_USB_SET_STATE_LEN) {
        return false;
    }

    memset(report, 0, DS5_OUTPUT_REPORT31_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT31_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_TAG;
    memcpy(report + 3, usb_payload, DS5_USB_SET_STATE_LEN);
    memset(ctx->set_state, 0, sizeof(ctx->set_state));
    memcpy(ctx->set_state, usb_payload, DS5_USB_SET_STATE_LEN);
    fill_crc(report, DS5_OUTPUT_REPORT31_BT_LEN);
    return true;
}
