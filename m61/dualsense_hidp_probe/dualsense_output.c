#include "dualsense_output.h"

#include <string.h>

#define DS5_OUTPUT_CRC32_SEED 0xA2
#define DS5_OUTPUT_TAG 0x10
#define DS5_OUTPUT_AUDIO_TAG 0x91
#define DS5_OUTPUT_AUDIO_STATE_TAG 0x90
#define DS5_OUTPUT_HAPTICS_TAG 0x92

#ifndef CONFIG_M61_CRC32_NIBBLE_TABLE
#define CONFIG_M61_CRC32_NIBBLE_TABLE 1
#endif

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
    0xFD, 0xF7, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x09,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00,
    0x00, 0x02, 0x01, 0x00, 0xFF, 0xD7, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#if CONFIG_M61_CRC32_NIBBLE_TABLE
static const uint32_t crc32_nibble_table[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
    0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};
#endif

static uint32_t crc32_le_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
#if CONFIG_M61_CRC32_NIBBLE_TABLE
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0FU];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0FU];
#else
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
#endif
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

static bool apply_usb_set_state(dualsense_output_context_t *ctx,
                                const uint8_t *usb_payload,
                                size_t usb_payload_len)
{
    uint8_t *state;
    uint8_t flags0;
    uint8_t flags1;
    uint8_t flags2;

    if (ctx == NULL || usb_payload == NULL || usb_payload_len < DS5_USB_SET_STATE_LEN) {
        return false;
    }

    ensure_set_state(ctx);
    state = ctx->set_state;
    flags0 = usb_payload[DS5_STATE_FLAGS0];
    flags1 = usb_payload[DS5_STATE_FLAGS1];
    flags2 = usb_payload[DS5_STATE_FLAGS2];

    state[DS5_STATE_FLAGS0] = (uint8_t)((state[DS5_STATE_FLAGS0] &
                                         ~(DS5_STATE_ENABLE_RUMBLE_EMULATION |
                                           DS5_STATE_USE_RUMBLE_NOT_HAPTICS)) |
                                        (flags0 & (DS5_STATE_ENABLE_RUMBLE_EMULATION |
                                                   DS5_STATE_USE_RUMBLE_NOT_HAPTICS)));
    state[DS5_STATE_FLAGS2] = (uint8_t)((state[DS5_STATE_FLAGS2] &
                                         ~(DS5_STATE_ENABLE_IMPROVED_RUMBLE |
                                           DS5_STATE_USE_RUMBLE_NOT_HAPTICS2)) |
                                        (flags2 & (DS5_STATE_ENABLE_IMPROVED_RUMBLE |
                                                   DS5_STATE_USE_RUMBLE_NOT_HAPTICS2)));

    if ((state[DS5_STATE_FLAGS0] & DS5_STATE_USE_RUMBLE_NOT_HAPTICS) ||
        (state[DS5_STATE_FLAGS2] & DS5_STATE_USE_RUMBLE_NOT_HAPTICS2)) {
        state[DS5_STATE_RUMBLE_RIGHT] = usb_payload[DS5_STATE_RUMBLE_RIGHT];
        state[DS5_STATE_RUMBLE_LEFT] = usb_payload[DS5_STATE_RUMBLE_LEFT];
    } else {
        state[DS5_STATE_RUMBLE_RIGHT] = 0;
        state[DS5_STATE_RUMBLE_LEFT] = 0;
    }

    if (flags0 & DS5_STATE_ALLOW_HEADPHONE_VOLUME) {
        state[DS5_STATE_VOLUME_HEADPHONES] = usb_payload[DS5_STATE_VOLUME_HEADPHONES];
    }
    if (flags0 & DS5_STATE_ALLOW_SPEAKER_VOLUME) {
        state[DS5_STATE_VOLUME_SPEAKER] = usb_payload[DS5_STATE_VOLUME_SPEAKER];
    }
    if (flags0 & DS5_STATE_ALLOW_MIC_VOLUME) {
        state[DS5_STATE_VOLUME_MIC] = usb_payload[DS5_STATE_VOLUME_MIC];
    }
    if (flags0 & DS5_STATE_ALLOW_AUDIO_CONTROL) {
        state[DS5_STATE_AUDIO_CONTROL] = usb_payload[DS5_STATE_AUDIO_CONTROL];
    }
    if (flags1 & DS5_STATE_ALLOW_MUTE_LIGHT) {
        state[DS5_STATE_MUTE_LIGHT] = usb_payload[DS5_STATE_MUTE_LIGHT];
    }
    if (flags1 & DS5_STATE_ALLOW_AUDIO_MUTE) {
        state[DS5_STATE_AUDIO_MUTE] = usb_payload[DS5_STATE_AUDIO_MUTE];
    }
    if (flags0 & DS5_STATE_ALLOW_RIGHT_TRIGGER_FFB) {
        memcpy(state + DS5_STATE_RIGHT_TRIGGER_FFB,
               usb_payload + DS5_STATE_RIGHT_TRIGGER_FFB,
               11);
    }
    if (flags0 & DS5_STATE_ALLOW_LEFT_TRIGGER_FFB) {
        memcpy(state + DS5_STATE_LEFT_TRIGGER_FFB,
               usb_payload + DS5_STATE_LEFT_TRIGGER_FFB,
               11);
    }
    if (flags1 & DS5_STATE_ALLOW_MOTOR_POWER_LEVEL) {
        state[DS5_STATE_MOTOR_POWER_LEVEL] = usb_payload[DS5_STATE_MOTOR_POWER_LEVEL];
    }
    if (flags1 & DS5_STATE_ALLOW_AUDIO_CONTROL2) {
        state[DS5_STATE_AUDIO_CONTROL2] = usb_payload[DS5_STATE_AUDIO_CONTROL2];
    }
    if (flags1 & DS5_STATE_ALLOW_HAPTIC_LOW_PASS) {
        state[DS5_STATE_HAPTIC_LOW_PASS] = usb_payload[DS5_STATE_HAPTIC_LOW_PASS];
    }
    if (flags2 & DS5_STATE_ALLOW_LIGHT_FADE) {
        state[DS5_STATE_LIGHT_FADE] = usb_payload[DS5_STATE_LIGHT_FADE];
    }
    if (flags2 & DS5_STATE_ALLOW_LIGHT_BRIGHTNESS) {
        state[DS5_STATE_LIGHT_BRIGHTNESS] = usb_payload[DS5_STATE_LIGHT_BRIGHTNESS];
    }
    if (flags1 & DS5_STATE_ALLOW_PLAYER_INDICATORS) {
        state[DS5_STATE_PLAYER_LIGHTS] = usb_payload[DS5_STATE_PLAYER_LIGHTS];
    }
    if (flags1 & DS5_STATE_ALLOW_LED_COLOR) {
        memcpy(state + DS5_STATE_LED_RED, usb_payload + DS5_STATE_LED_RED, 3);
    }

    return true;
}

void dualsense_output_init(dualsense_output_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->sequence = 0;
        ctx->audio_packet_counter = 0;
        reset_set_state(ctx);
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
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = 0x90;
    report[3] = DS5_OUTPUT_SET_STATE_LEN;
    copy_set_state(report + 4, ctx);
    fill_crc(report, DS5_OUTPUT_REPORT32_BT_LEN);
    return true;
}

bool dualsense_output_make_report36_haptics(dualsense_output_context_t *ctx,
                                            const uint8_t *haptics,
                                            size_t haptics_len,
                                            bool mic_active,
                                            uint8_t audio_buffer_len,
                                            uint8_t *report,
                                            size_t report_len)
{
    return dualsense_output_make_report36_audio(ctx,
                                                haptics,
                                                haptics_len,
                                                NULL,
                                                0,
                                                DS5_OUTPUT_SPEAKER_BLOCK_ID,
                                                mic_active,
                                                audio_buffer_len,
                                                report,
                                                report_len);
}

bool dualsense_output_make_report36_audio(dualsense_output_context_t *ctx,
                                          const uint8_t *haptics,
                                          size_t haptics_len,
                                          const uint8_t *speaker_opus,
                                          size_t speaker_opus_len,
                                          uint8_t speaker_block_id,
                                          bool mic_active,
                                          uint8_t audio_buffer_len,
                                          uint8_t *report,
                                          size_t report_len)
{
    bool have_haptics = haptics != NULL && haptics_len >= DS5_OUTPUT_HAPTICS_BLOCK_LEN;
    bool have_speaker = speaker_opus != NULL && speaker_opus_len > 0;

    if (ctx == NULL || report == NULL || report_len < DS5_OUTPUT_REPORT36_BT_LEN ||
        (!have_haptics && !have_speaker)) {
        return false;
    }

    ensure_set_state(ctx);
    if (audio_buffer_len == 0) {
        audio_buffer_len = 64;
    }

    memset(report, 0, DS5_OUTPUT_REPORT36_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT36_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_AUDIO_TAG;
    report[3] = 7;
    report[4] = mic_active ? 0xFF : 0xFE;
    report[5] = audio_buffer_len;
    report[6] = audio_buffer_len;
    report[7] = audio_buffer_len;
    report[8] = audio_buffer_len;
    report[9] = audio_buffer_len;
    report[10] = ctx->audio_packet_counter++;

    report[11] = DS5_OUTPUT_AUDIO_STATE_TAG;
    report[12] = DS5_OUTPUT_SET_STATE_LEN;
    copy_set_state(report + 13, ctx);

    report[76] = DS5_OUTPUT_HAPTICS_TAG;
    report[77] = DS5_OUTPUT_HAPTICS_BLOCK_LEN;
    if (have_haptics) {
        memcpy(report + 78, haptics, DS5_OUTPUT_HAPTICS_BLOCK_LEN);
    }

    if (have_speaker) {
        size_t copy_len = speaker_opus_len;
        if (copy_len > DS5_OUTPUT_SPEAKER_OPUS_LEN) {
            copy_len = DS5_OUTPUT_SPEAKER_OPUS_LEN;
        }
        if (speaker_block_id == 0) {
            speaker_block_id = DS5_OUTPUT_SPEAKER_BLOCK_ID;
        }
        report[142] = (uint8_t)(speaker_block_id | 0x80);
        report[143] = DS5_OUTPUT_SPEAKER_OPUS_LEN;
        memcpy(report + 144, speaker_opus, copy_len);
    }

    fill_crc(report, DS5_OUTPUT_REPORT36_BT_LEN);
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

    if (ctx == NULL || report == NULL ||
        report_len < DS5_OUTPUT_AUDIO_RT_BT_LEN ||
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

    report[10] = 0xD2;
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
        if (speaker_opus0 != NULL) {
            memcpy(report + 142, speaker_opus0, copy_len);
        }
        if (speaker_opus1 != NULL) {
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

    if (audio_buffer_len == 0) {
        audio_buffer_len = 64;
    }

    memset(report, 0, DS5_OUTPUT_REPORT32_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT32_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_AUDIO_TAG;
    report[3] = 7;
    report[4] = mic_active ? 0xFF : 0xFE;
    report[5] = audio_buffer_len;
    report[6] = audio_buffer_len;
    report[7] = audio_buffer_len;
    report[8] = audio_buffer_len;
    report[9] = audio_buffer_len;
    report[10] = ctx->audio_packet_counter++;

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
    if (!apply_usb_set_state(ctx, usb_payload, usb_payload_len)) {
        return false;
    }

    memset(report, 0, DS5_OUTPUT_REPORT31_BT_LEN);
    report[0] = DS5_OUTPUT_REPORT31_BT_ID;
    report[1] = (uint8_t)((ctx->sequence++ & 0x0F) << 4);
    report[2] = DS5_OUTPUT_TAG;
    copy_set_state(report + 3, ctx);
    fill_crc(report, DS5_OUTPUT_REPORT31_BT_LEN);
    return true;
}
