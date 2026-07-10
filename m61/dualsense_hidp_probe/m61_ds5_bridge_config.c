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
#define DS5_STATE_AUDIO_CONTROL 7
#define DS5_STATE_MOTOR_POWER_LEVEL 36
#define DS5_STATE_AUDIO_CONTROL2 37
#define DS5_STATE_ALLOW_AUDIO_CONTROL 0x80
#define DS5_STATE_ALLOW_HEADPHONE_VOLUME 0x10
#define DS5_STATE_ALLOW_SPEAKER_VOLUME 0x20
#define DS5_STATE_ALLOW_MIC_VOLUME 0x40
#define DS5_STATE_ALLOW_MUTE_LIGHT 0x01
#define DS5_STATE_ALLOW_AUDIO_MUTE 0x02
#define DS5_STATE_ALLOW_MOTOR_POWER_LEVEL 0x40
#define DS5_STATE_ALLOW_AUDIO_CONTROL2 0x80

typedef struct {
    uint32_t magic;
    uint32_t crc32;
    uint16_t size;
    m61_ds5_bridge_config_body_t body;
} m61_ds5_bridge_config_storage_t;

typedef struct __attribute__((packed)) {
    uint8_t config_version;
    float haptics_gain;
    uint8_t speaker_volume;
    uint8_t headset_volume;
    uint8_t speaker_gain;
    uint8_t inactive_time;
    uint8_t disable_pico_led;
    uint8_t polling_rate_mode;
    uint8_t audio_buffer_length;
    uint8_t controller_mode;
    uint8_t enable_usb_sn;
    uint8_t ps_shortcut_enabled;
    uint8_t disable_mic;
    uint8_t disable_speaker;
    uint8_t enable_wake;
    uint8_t trigger_reduce;
    uint8_t lock_volume;
} legacy_bridge_config_body_t;

_Static_assert(sizeof(m61_ds5_bridge_config_body_t) == 20U,
               "Config_body must stay byte-compatible with awalol/DS5Dongle");
_Static_assert(offsetof(m61_ds5_bridge_config_storage_t, body) == 10U,
               "Config body offset must match awalol/DS5Dongle");
_Static_assert(sizeof(m61_ds5_bridge_config_storage_t) == 32U,
               "Config storage layout must include the upstream alignment");

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

static uint32_t config_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = ~0xEADA2D49UL;

    while (len-- > 0U) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8U; bit++) {
            crc = (crc >> 1) ^
                  (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return ~crc;
}

static uint32_t config_body_crc(const m61_ds5_bridge_config_body_t *body)
{
    return config_crc32((const uint8_t *)body, sizeof(*body));
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
    body->mic_select = CONFIG_M61_DS5_BRIDGE_MIC_SELECT;
    body->speaker_select = CONFIG_M61_DS5_BRIDGE_SPEAKER_SELECT;
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
    body->mic_select = clamp_u8(body->mic_select, 0, 3, CONFIG_M61_DS5_BRIDGE_MIC_SELECT);
    body->speaker_select = clamp_u8(body->speaker_select, 0, 3, CONFIG_M61_DS5_BRIDGE_SPEAKER_SELECT);
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
    m61_ds5_bridge_config_storage_t saved = {0};
    size_t read_len = ef_get_env_blob(M61_DS5_BRIDGE_CONFIG_KEY,
                                      (uint8_t *)&saved,
                                      sizeof(saved),
                                      &saved_len);
    if (read_len == sizeof(saved) && saved_len == sizeof(saved) &&
        saved.magic == M61_DS5_BRIDGE_CONFIG_MAGIC &&
        saved.size == sizeof(saved.body) &&
        saved.crc32 == config_body_crc(&saved.body)) {
        s_config = saved.body;
        config_validate(&s_config);
        printf("[Config] upstream config loaded len=%u crc=%08lx\r\n",
               (unsigned int)read_len,
               (unsigned long)saved.crc32);
    } else if (read_len == sizeof(legacy_bridge_config_body_t) &&
               saved_len == sizeof(legacy_bridge_config_body_t)) {
        const legacy_bridge_config_body_t *legacy =
            (const legacy_bridge_config_body_t *)&saved;

        memcpy(&s_config, legacy, sizeof(s_config));
        s_config.mic_select = legacy->disable_mic
                                  ? M61_DS5_AUDIO_SELECT_DISABLED
                                  : M61_DS5_AUDIO_SELECT_AUTO;
        s_config.speaker_select = legacy->disable_speaker
                                      ? M61_DS5_AUDIO_SELECT_DISABLED
                                      : M61_DS5_AUDIO_SELECT_AUTO;
        config_validate(&s_config);
        printf("[Config] migrated legacy body-only config len=%u\r\n",
               (unsigned int)read_len);
        (void)m61_ds5_bridge_config_save();
    } else if (read_len > 0U || saved_len > 0U) {
        printf("[Config] invalid config ignored len=%u saved=%u expected=%u magic=%08lx size=%u crc=%08lx/%08lx\r\n",
               (unsigned int)read_len,
               (unsigned int)saved_len,
               (unsigned int)sizeof(saved),
               (unsigned long)saved.magic,
               (unsigned int)saved.size,
               (unsigned long)saved.crc32,
               (unsigned long)config_body_crc(&saved.body));
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
    config_validate(&s_config);
}

bool m61_ds5_bridge_config_save(void)
{
#if defined(CONFIG_BT_SETTINGS)
    m61_ds5_bridge_config_storage_t stored = {0};
    m61_ds5_bridge_config_storage_t verify = {0};
    size_t verify_saved_len = 0;
    size_t verify_read_len;

    stored.magic = M61_DS5_BRIDGE_CONFIG_MAGIC;
    stored.size = sizeof(stored.body);
    stored.body = s_config;
    stored.crc32 = config_body_crc(&stored.body);
    EfErrCode err = ef_set_env_blob(M61_DS5_BRIDGE_CONFIG_KEY,
                                    (const uint8_t *)&stored,
                                    sizeof(stored));
    if (err == EF_NO_ERR) {
        err = ef_save_env();
    }
    if (err != EF_NO_ERR) {
        printf("[Config] bridge config save failed: %d\r\n", err);
        return false;
    }
    verify_read_len = ef_get_env_blob(M61_DS5_BRIDGE_CONFIG_KEY,
                                      (uint8_t *)&verify,
                                      sizeof(verify),
                                      &verify_saved_len);
    if (verify_read_len != sizeof(verify) ||
        verify_saved_len != sizeof(verify) ||
        verify.magic != stored.magic ||
        verify.size != stored.size ||
        verify.crc32 != stored.crc32 ||
        verify.crc32 != config_body_crc(&verify.body) ||
        memcmp(&verify.body, &stored.body, sizeof(stored.body)) != 0) {
        printf("[Config] bridge config verify failed read=%u saved=%u crc=%08lx/%08lx\r\n",
               (unsigned int)verify_read_len,
               (unsigned int)verify_saved_len,
               (unsigned long)verify.crc32,
               (unsigned long)stored.crc32);
        return false;
    }
    printf("[Config] upstream config saved len=%u crc=%08lx\r\n",
           (unsigned int)sizeof(stored),
           (unsigned long)stored.crc32);
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

uint8_t m61_ds5_bridge_config_polling_interval(void)
{
    switch (s_config.polling_rate_mode) {
    case 0:
        return 4;
    case 1:
        return 2;
    default:
        return 1;
    }
}

bool m61_ds5_bridge_config_mic_enabled(void)
{
    return s_config.mic_select != M61_DS5_AUDIO_SELECT_DISABLED;
}

bool m61_ds5_bridge_config_speaker_enabled(void)
{
    return s_config.speaker_select != M61_DS5_AUDIO_SELECT_DISABLED;
}

uint8_t m61_ds5_bridge_config_mic_select(void)
{
    return s_config.mic_select;
}

uint8_t m61_ds5_bridge_config_speaker_select(void)
{
    return s_config.speaker_select;
}

bool m61_ds5_bridge_config_speaker_uses_headset(bool headset_connected)
{
    return s_config.speaker_select == M61_DS5_AUDIO_SELECT_HEADSET ||
           (s_config.speaker_select == M61_DS5_AUDIO_SELECT_AUTO &&
            headset_connected);
}

bool m61_ds5_bridge_config_usb_serial_enabled(void)
{
    return s_config.enable_usb_sn != 0U;
}

bool m61_ds5_bridge_config_ps_shortcut_enabled(void)
{
    return s_config.ps_shortcut_enabled != 0U;
}

bool m61_ds5_bridge_config_wake_enabled(void)
{
    return s_config.enable_wake != 0U;
}

bool m61_ds5_bridge_config_dse_enabled(void)
{
    return s_config.controller_mode != M61_DS5_CONTROLLER_MODE_DS5;
}

static void apply_controller_state(uint8_t *payload, size_t len,
                                   bool force_mic_select)
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
    if (force_mic_select || s_config.mic_select != M61_DS5_AUDIO_SELECT_AUTO) {
        payload[DS5_STATE_FLAGS0] |= DS5_STATE_ALLOW_AUDIO_CONTROL;
        payload[DS5_STATE_AUDIO_CONTROL] =
            (uint8_t)((payload[DS5_STATE_AUDIO_CONTROL] & 0xFCU) |
                      (s_config.mic_select & 0x03U));
    }
    if (s_config.lock_volume) {
        payload[DS5_STATE_FLAGS0] &= (uint8_t)~(DS5_STATE_ALLOW_HEADPHONE_VOLUME |
                                                DS5_STATE_ALLOW_SPEAKER_VOLUME |
                                                DS5_STATE_ALLOW_MIC_VOLUME);
        payload[DS5_STATE_FLAGS1] &= (uint8_t)~(DS5_STATE_ALLOW_AUDIO_MUTE |
                                                DS5_STATE_ALLOW_MUTE_LIGHT);
    }
}

void m61_ds5_bridge_config_apply_usb_set_state(uint8_t *payload, size_t len)
{
    apply_controller_state(payload, len, false);
}

void m61_ds5_bridge_config_make_controller_state(uint8_t *payload, size_t len)
{
    if (payload == NULL || len < DS5_USB_SET_STATE_LEN) {
        return;
    }
    memset(payload, 0, len);
    apply_controller_state(payload, len, true);
}
