#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M61_DS5_BRIDGE_CONFIG_VERSION 5U
#define M61_DS5_BRIDGE_CONFIG_MAGIC 0x66CCFF00UL

#ifndef CONFIG_M61_DS5_BRIDGE_HAPTICS_GAIN_Q8
#define CONFIG_M61_DS5_BRIDGE_HAPTICS_GAIN_Q8 256
#endif

#ifndef HAPTICS_GAIN_Q8
#define HAPTICS_GAIN_Q8 CONFIG_M61_DS5_BRIDGE_HAPTICS_GAIN_Q8
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_SPEAKER_VOLUME
#define CONFIG_M61_DS5_BRIDGE_SPEAKER_VOLUME 100
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_HEADSET_VOLUME
#define CONFIG_M61_DS5_BRIDGE_HEADSET_VOLUME 100
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_SPEAKER_GAIN
#define CONFIG_M61_DS5_BRIDGE_SPEAKER_GAIN 2
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_INACTIVE_TIME_MIN
#define CONFIG_M61_DS5_BRIDGE_INACTIVE_TIME_MIN 30
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_POLLING_RATE_MODE
#define CONFIG_M61_DS5_BRIDGE_POLLING_RATE_MODE 1
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_AUDIO_BUFFER_LENGTH
#define CONFIG_M61_DS5_BRIDGE_AUDIO_BUFFER_LENGTH 64
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_CONTROLLER_MODE
#define CONFIG_M61_DS5_BRIDGE_CONTROLLER_MODE 2
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_ENABLE_USB_SN
#define CONFIG_M61_DS5_BRIDGE_ENABLE_USB_SN 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_PS_SHORTCUT_ENABLED
#define CONFIG_M61_DS5_BRIDGE_PS_SHORTCUT_ENABLED 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_MIC_SELECT
#define CONFIG_M61_DS5_BRIDGE_MIC_SELECT 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_SPEAKER_SELECT
#define CONFIG_M61_DS5_BRIDGE_SPEAKER_SELECT 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_ENABLE_WAKE
#define CONFIG_M61_DS5_BRIDGE_ENABLE_WAKE 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_TRIGGER_REDUCE
#define CONFIG_M61_DS5_BRIDGE_TRIGGER_REDUCE 0
#endif

#ifndef CONFIG_M61_DS5_BRIDGE_LOCK_VOLUME
#define CONFIG_M61_DS5_BRIDGE_LOCK_VOLUME 0
#endif

typedef enum {
    M61_DS5_CONTROLLER_MODE_DS5 = 0,
    M61_DS5_CONTROLLER_MODE_DSE = 1,
    M61_DS5_CONTROLLER_MODE_AUTO = 2,
} m61_ds5_controller_mode_t;

typedef enum {
    M61_DS5_AUDIO_SELECT_AUTO = 0,
    M61_DS5_AUDIO_SELECT_BUILTIN = 1,
    M61_DS5_AUDIO_SELECT_HEADSET = 2,
    M61_DS5_AUDIO_SELECT_DISABLED = 3,
} m61_ds5_audio_select_t;

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
    uint8_t mic_select;
    uint8_t speaker_select;
    uint8_t enable_wake;
    uint8_t trigger_reduce;
    uint8_t lock_volume;
} m61_ds5_bridge_config_body_t;

void m61_ds5_bridge_config_init(void);
void m61_ds5_bridge_config_reset_defaults(void);
const m61_ds5_bridge_config_body_t *m61_ds5_bridge_config_get(void);
void m61_ds5_bridge_config_set_raw(const uint8_t *data, size_t len);
bool m61_ds5_bridge_config_save(void);

uint16_t m61_ds5_bridge_config_haptics_gain_q8(void);
uint8_t m61_ds5_bridge_config_audio_buffer_length(void);
uint8_t m61_ds5_bridge_config_polling_interval(void);
bool m61_ds5_bridge_config_mic_enabled(void);
bool m61_ds5_bridge_config_speaker_enabled(void);
uint8_t m61_ds5_bridge_config_mic_select(void);
uint8_t m61_ds5_bridge_config_speaker_select(void);
bool m61_ds5_bridge_config_speaker_uses_headset(bool headset_connected);
bool m61_ds5_bridge_config_usb_serial_enabled(void);
bool m61_ds5_bridge_config_ps_shortcut_enabled(void);
bool m61_ds5_bridge_config_wake_enabled(void);
bool m61_ds5_bridge_config_dse_enabled(void);
void m61_ds5_bridge_config_apply_usb_set_state(uint8_t *payload, size_t len);
void m61_ds5_bridge_config_make_controller_state(uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
