#include "m61_ds5_bridge_config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "dualsense_output.h"

#if defined(CONFIG_BT_SETTINGS)
#include "easyflash.h"
#endif

#define M61_DS5_BRIDGE_CONFIG_KEY "ds5_bridge_cfg"

#define DS5_STATE_FLAGS0 0
#define DS5_STATE_FLAGS1 1
#define DS5_STATE_MOTOR_POWER_LEVEL 36
#define DS5_STATE_AUDIO_CONTROL2 37
#define DS5_STATE_ALLOW_HEADPHONE_VOLUME 0x10
#define DS5_STATE_ALLOW_SPEAKER_VOLUME 0x20
#define DS5_STATE_ALLOW_MIC_VOLUME 0x40
#define DS5_STATE_ALLOW_MUTE_LIGHT 0x01
#define DS5_STATE_ALLOW_AUDIO_MUTE 0x02
#define DS5_STATE_ALLOW_MOTOR_POWER_LEVEL 0x40
#define DS5_STATE_ALLOW_AUDIO_CONTROL2 0x80

static m61_ds5_bridge_config_body_t s_config;
static uint16_t s_haptics_gain_q8 = HAPTICS_GAIN_Q8;

static uint8_t clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value, uint8_t fallback)
{
    return (value < min_value || value > max_value) ? fallback : value;
}

static uint8_t bool_u8(uint8_t value, uint8_t fallback)
{
    return value > 1U ? fallback : value;
}

static float read_haptics_gain(const m61_ds5_bridge_config_body_t *body)
{
    float value;

    memcpy(&value,
           ((const uint8_t *)body) + offsetof(m61_ds5_bridge_config_body_t, haptics_gain),
           sizeof(value));
    return value;
}

static void write_haptics_gain(m61_ds5_bridge_config_body_t *body, float value)
{
    memcpy(((uint8_t *)body) + offsetof(m61_ds5_bridge_config_body_t, haptics_gain),
           &value,
           sizeof(value));
}

static float default_haptics_gain(void)
{
    uint16_t q8 = CONFIG_M61_DS5_BRIDGE_HAPTICS_GAIN_Q8;

    if (q8 < 256U || q8 > 512U) {
        q8 = 256U;
    }
    return (float)q8 / 256.0f;
}

static uint16_t haptics_gain_to_q8(float value)
{
    int scaled = (int)(value * 256.0f + 0.5f);

    if (scaled < 256) {
        scaled = 256;
    } else if (scaled > 512) {
        scaled = 512;
    }
    return (uint16_t)scaled;
}

static void config_defaults(m61_ds5_bridge_config_body_t *body)
{
    memset(body, 0, sizeof(*body));
    body->config_version = M61_DS5_BRIDGE_CONFIG_VERSION;
    write_haptics_gain(body, default_haptics_gain());
    body->speaker_volume = CONFIG_M61_DS5_BRIDGE_SPEAKER_VOLUME;
    body->headset_volume = CONFIG_M61_DS5_BRIDGE_HEADSET_VOLUME;
    body->speaker_gain = CONFIG_M61_DS5_BRIDGE_SPEAKER_GAIN;
    body->inactive_time = CONFIG_M61_DS5_BRIDGE_INACTIVE_TIME_MIN;
    body->polling_rate_mode = CONFIG_M61_DS5_BRIDGE_POLLING_RATE_MODE;
    body->audio_buffer_length = CONFIG_M61_DS5_BRIDGE_AUDIO_BUFFER_LENGTH;
    body->controller_mode = CONFIG_M61_DS5_BRIDGE_CONTROLLER_MODE;
    body->enable_usb_sn = CONFIG_M61_DS5_BRIDGE_ENABLE_USB_SN;
    body->ps_shortcut_enabled = CONFIG_M61_DS5_BRIDGE_PS_SHORTCUT_ENABLED;
    body->disable_mic = CONFIG_M61_DS5_BRIDGE_DISABLE_MIC;
    body->disable_speaker = CONFIG_M61_DS5_BRIDGE_DISABLE_SPEAKER;
    body->enable_wake = CONFIG_M61_DS5_BRIDGE_ENABLE_WAKE;
    body->trigger_reduce = CONFIG_M61_DS5_BRIDGE_TRIGGER_REDUCE;
    body->lock_volume = CONFIG_M61_DS5_BRIDGE_LOCK_VOLUME;
}

static void config_validate(m61_ds5_bridge_config_body_t *body)
{
    float haptics_gain = read_haptics_gain(body);

    if (body->config_version != M61_DS5_BRIDGE_CONFIG_VERSION) {
        config_defaults(body);
        printf("[Config] bridge config version reset to default\r\n");
        return;
    }
    if (!(haptics_gain >= 1.0f && haptics_gain <= 2.0f)) {
        haptics_gain = default_haptics_gain();
        write_haptics_gain(body, haptics_gain);
        printf("[Config] haptics_gain reset to default\r\n");
    }
    body->speaker_volume = clamp_u8(body->speaker_volume, 0, 127, CONFIG_M61_DS5_BRIDGE_SPEAKER_VOLUME);
    body->headset_volume = clamp_u8(body->headset_volume, 0, 127, CONFIG_M61_DS5_BRIDGE_HEADSET_VOLUME);
    body->speaker_gain = clamp_u8(body->speaker_gain, 0, 7, CONFIG_M61_DS5_BRIDGE_SPEAKER_GAIN);
    body->inactive_time = clamp_u8(body->inactive_time, 0, 60, CONFIG_M61_DS5_BRIDGE_INACTIVE_TIME_MIN);
    body->disable_pico_led = bool_u8(body->disable_pico_led, 0);
    body->polling_rate_mode = clamp_u8(body->polling_rate_mode, 0, 2, CONFIG_M61_DS5_BRIDGE_POLLING_RATE_MODE);
    body->audio_buffer_length = clamp_u8(body->audio_buffer_length, 16, 128, CONFIG_M61_DS5_BRIDGE_AUDIO_BUFFER_LENGTH);
    body->controller_mode = clamp_u8(body->controller_mode, 0, 2, CONFIG_M61_DS5_BRIDGE_CONTROLLER_MODE);
    body->enable_usb_sn = bool_u8(body->enable_usb_sn, CONFIG_M61_DS5_BRIDGE_ENABLE_USB_SN);
    body->ps_shortcut_enabled = bool_u8(body->ps_shortcut_enabled, CONFIG_M61_DS5_BRIDGE_PS_SHORTCUT_ENABLED);
    body->disable_mic = bool_u8(body->disable_mic, CONFIG_M61_DS5_BRIDGE_DISABLE_MIC);
    body->disable_speaker = bool_u8(body->disable_speaker, CONFIG_M61_DS5_BRIDGE_DISABLE_SPEAKER);
    body->enable_wake = bool_u8(body->enable_wake, CONFIG_M61_DS5_BRIDGE_ENABLE_WAKE);
    body->trigger_reduce = clamp_u8(body->trigger_reduce, 0, 10, CONFIG_M61_DS5_BRIDGE_TRIGGER_REDUCE);
    body->lock_volume = bool_u8(body->lock_volume, CONFIG_M61_DS5_BRIDGE_LOCK_VOLUME);
    s_haptics_gain_q8 = haptics_gain_to_q8(haptics_gain);
}

void m61_ds5_bridge_config_reset_defaults(void)
{
    config_defaults(&s_config);
    config_validate(&s_config);
}

void m61_ds5_bridge_config_init(void)
{
    m61_ds5_bridge_config_reset_defaults();

#if defined(CONFIG_BT_SETTINGS)
    size_t saved_len = 0;
    m61_ds5_bridge_config_body_t saved = s_config;
    size_t read_len = ef_get_env_blob(M61_DS5_BRIDGE_CONFIG_KEY,
                                      (uint8_t *)&saved,
                                      sizeof(saved),
                                      &saved_len);
    if (read_len == sizeof(saved) && saved_len == sizeof(saved)) {
        s_config = saved;
        config_validate(&s_config);
        printf("[Config] bridge config loaded len=%u\r\n", (unsigned int)read_len);
    } else if (read_len > 0U || saved_len > 0U) {
        printf("[Config] bridge config ignored len=%u saved=%u expected=%u\r\n",
               (unsigned int)read_len,
               (unsigned int)saved_len,
               (unsigned int)sizeof(saved));
    }
#endif
}

const m61_ds5_bridge_config_body_t *m61_ds5_bridge_config_get(void)
{
    return &s_config;
}

void m61_ds5_bridge_config_set_raw(const uint8_t *data, size_t len)
{
    size_t copy_len;

    if (data == NULL || len == 0U) {
        return;
    }

    copy_len = len < sizeof(s_config) ? len : sizeof(s_config);
    memcpy(&s_config, data, copy_len);
    if (copy_len < sizeof(s_config)) {
        m61_ds5_bridge_config_body_t defaults;

        config_defaults(&defaults);
        memcpy(((uint8_t *)&s_config) + copy_len,
               ((const uint8_t *)&defaults) + copy_len,
               sizeof(s_config) - copy_len);
    }
    config_validate(&s_config);
}

bool m61_ds5_bridge_config_save(void)
{
#if defined(CONFIG_BT_SETTINGS)
    EfErrCode err = ef_set_env_blob(M61_DS5_BRIDGE_CONFIG_KEY,
                                    (const uint8_t *)&s_config,
                                    sizeof(s_config));
    if (err == EF_NO_ERR) {
        err = ef_save_env();
    }
    if (err != EF_NO_ERR) {
        printf("[Config] bridge config save failed: %d\r\n", err);
        return false;
    }
    printf("[Config] bridge config saved\r\n");
    return true;
#else
    printf("[Config] bridge config save unavailable: EasyFlash disabled\r\n");
    return false;
#endif
}

uint16_t m61_ds5_bridge_config_haptics_gain_q8(void)
{
    return s_haptics_gain_q8;
}

uint8_t m61_ds5_bridge_config_audio_buffer_length(void)
{
    return s_config.audio_buffer_length;
}

bool m61_ds5_bridge_config_mic_enabled(void)
{
    return s_config.disable_mic == 0U;
}

bool m61_ds5_bridge_config_speaker_enabled(void)
{
    return s_config.disable_speaker == 0U;
}

bool m61_ds5_bridge_config_dse_enabled(void)
{
    return s_config.controller_mode != M61_DS5_CONTROLLER_MODE_DS5;
}

void m61_ds5_bridge_config_apply_usb_set_state(uint8_t *payload, size_t len)
{
    if (payload == NULL || len < DS5_USB_SET_STATE_LEN) {
        return;
    }

    if (s_config.trigger_reduce > 0U) {
        payload[DS5_STATE_FLAGS1] |= DS5_STATE_ALLOW_MOTOR_POWER_LEVEL;
        payload[DS5_STATE_MOTOR_POWER_LEVEL] =
            (uint8_t)((payload[DS5_STATE_MOTOR_POWER_LEVEL] & 0x0FU) |
                      ((s_config.trigger_reduce & 0x0FU) << 4));
    }
    if (s_config.speaker_gain > 0U) {
        payload[DS5_STATE_FLAGS1] |= DS5_STATE_ALLOW_AUDIO_CONTROL2;
        payload[DS5_STATE_AUDIO_CONTROL2] =
            (uint8_t)((payload[DS5_STATE_AUDIO_CONTROL2] & 0xF8U) |
                      (s_config.speaker_gain & 0x07U));
    }
    if (s_config.lock_volume) {
        payload[DS5_STATE_FLAGS0] &= (uint8_t)~(DS5_STATE_ALLOW_HEADPHONE_VOLUME |
                                                DS5_STATE_ALLOW_SPEAKER_VOLUME |
                                                DS5_STATE_ALLOW_MIC_VOLUME);
        payload[DS5_STATE_FLAGS1] &= (uint8_t)~(DS5_STATE_ALLOW_AUDIO_MUTE |
                                                DS5_STATE_ALLOW_MUTE_LIGHT);
    }
}
