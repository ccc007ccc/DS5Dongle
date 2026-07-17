#ifndef M61_WEB_CONFIG_H
#define M61_WEB_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M61_WEB_COMMAND_REPORT_ID 0xF6U
#define M61_WEB_CONFIG_REPORT_ID 0xF7U
#define M61_WEB_FIRMWARE_REPORT_ID 0xF8U
#define M61_WEB_TELEMETRY_REPORT_ID 0xF9U
#define M61_WEB_FEATURE_PAYLOAD_SIZE 63U
#define M61_WEB_CONFIG_SCHEMA_VERSION 1U
#define M61_WEB_CONFIG_BODY_SIZE 16U
#define M61_WEB_TELEMETRY_VERSION 1U
#define M61_WEB_PERSISTENT_RECORD_VERSION 1U
#define M61_WEB_PERSISTENT_RECORD_SIZE 26U

enum {
    M61_WEB_COMMAND_APPLY_CONFIG = 0x01U,
    M61_WEB_COMMAND_SAVE_CONFIG = 0x02U,
    M61_WEB_COMMAND_RECONNECT_USB = 0x03U,
};

enum {
    M61_WEB_CAP_MICROPHONE = 1U << 0,
    M61_WEB_CAP_SPEAKER_GATE = 1U << 1,
    M61_WEB_CAP_SPEAKER_ROUTE = 1U << 2,
    M61_WEB_CAP_AUTO_RECONNECT = 1U << 3,
    M61_WEB_CAP_STATUS_LED = 1U << 4,
    M61_WEB_CAP_HAPTICS_GAIN = 1U << 5,
    M61_WEB_CAP_DVFS = 1U << 6,
    M61_WEB_CAP_TELEMETRY_V1 = 1U << 7,
};

typedef struct {
    uint16_t capabilities;
    bool microphone_enabled;
    bool speaker_enabled;
    bool auto_reconnect_enabled;
    bool status_led_enabled;
    uint8_t speaker_route;
    uint8_t cpu_governor;
    uint8_t cpu_profile;
    uint16_t manual_cpu_mhz;
    uint16_t haptics_gain_q8;
} m61_web_config_t;

typedef struct {
    bool rssi_valid;
    int8_t rssi;
    bool speaker_active;
    bool microphone_active;
    bool bluetooth_connected;
    bool usb_configured;
    bool headphones_connected;
    bool speaker_stereo;
    uint16_t current_cpu_mhz;
    uint16_t requested_cpu_mhz;
} m61_web_telemetry_t;

void m61_web_config_defaults(m61_web_config_t *config);
bool m61_web_config_valid(const m61_web_config_t *config);
int m61_web_config_encode(const m61_web_config_t *config,
                          uint8_t *output,
                          size_t output_size);
int m61_web_config_decode(const uint8_t *input,
                          size_t input_size,
                          m61_web_config_t *config);
int m61_web_command_encode(uint8_t command,
                           const m61_web_config_t *config,
                           uint8_t *output,
                           size_t output_size);
int m61_web_telemetry_encode(const m61_web_telemetry_t *telemetry,
                             uint8_t *output,
                             size_t output_size);
int m61_web_persistent_encode(const m61_web_config_t *config,
                              uint8_t *output,
                              size_t output_size);
int m61_web_persistent_decode(const uint8_t *input,
                              size_t input_size,
                              m61_web_config_t *config);
void m61_web_runtime_set(const m61_web_config_t *config);
void m61_web_runtime_get(m61_web_config_t *config);

#endif
