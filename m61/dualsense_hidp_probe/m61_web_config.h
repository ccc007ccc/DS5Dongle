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
#define M61_WEB_CONFIG_SCHEMA_VERSION 5U
#define M61_WEB_CONFIG_BODY_SIZE 23U
#define M61_WEB_TELEMETRY_VERSION 2U
#define M61_WEB_PERSISTENT_RECORD_VERSION 5U
#define M61_WEB_PERSISTENT_RECORD_SIZE 33U
#define M61_WEB_STATUS_LED_BRIGHTNESS_DEFAULT 12U
#define M61_WEB_AUDIO_BUFFER_LENGTH_DEFAULT 48U

enum {
    M61_WEB_COMMAND_APPLY_CONFIG = 0x01U,
    M61_WEB_COMMAND_SAVE_CONFIG = 0x02U,
    M61_WEB_COMMAND_RECONNECT_USB = 0x03U,
    M61_WEB_COMMAND_POWER_OFF_CONTROLLER = 0x04U,
    M61_WEB_COMMAND_PAIR_CONTROLLER = 0x05U,
    M61_WEB_COMMAND_DISCONNECT_CONTROLLER = 0x06U,
    M61_WEB_COMMAND_FORGET_CONTROLLER = 0x07U,
};

enum {
    M61_WEB_CAP_MICROPHONE = 1U << 0,
    M61_WEB_CAP_SPEAKER_GATE = 1U << 1,
    M61_WEB_CAP_SPEAKER_ROUTE = 1U << 2,
    M61_WEB_CAP_AUTO_RECONNECT = 1U << 3,
    M61_WEB_CAP_STATUS_LED = 1U << 4,
    M61_WEB_CAP_HAPTICS_GAIN = 1U << 5,
    M61_WEB_CAP_DVFS = 1U << 6,
    M61_WEB_CAP_TELEMETRY = 1U << 7,
    M61_WEB_CAP_IDLE_POWEROFF = 1U << 8,
    M61_WEB_CAP_CONTROLLER_POWEROFF = 1U << 9,
    M61_WEB_CAP_SUSPEND_POWEROFF = 1U << 10,
    M61_WEB_CAP_STICK_DEADZONE = 1U << 11,
    M61_WEB_CAP_USB_POLLING_RATE = 1U << 12,
    M61_WEB_CAP_STATUS_LED_BRIGHTNESS = 1U << 13,
    M61_WEB_CAP_AUDIO_BUFFER_LENGTH = 1U << 14,
};

enum {
    M61_WEB_USB_POLL_REALTIME = 0U,
    M61_WEB_USB_POLL_250_HZ = 1U,
    M61_WEB_USB_POLL_500_HZ = 2U,
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
    uint8_t idle_timeout_minutes;
    bool power_off_on_usb_suspend;
    uint8_t left_stick_deadzone_percent;
    uint8_t right_stick_deadzone_percent;
    uint8_t usb_polling_rate_mode;
    uint8_t status_led_brightness_percent;
    uint8_t audio_buffer_length;
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
    bool pairing_active;
    bool discovery_active;
    bool saved_controller;
    bool config_loaded;
    bool usb_suspended;
    uint8_t last_management_command;
    int16_t last_management_error;
    uint32_t management_sequence;
    uint32_t usb_input_dropped;
    uint32_t host_report_dropped;
    uint32_t audio_ingress_dropped;
    uint32_t haptics_queue_dropped;
    uint32_t speaker_errors;
    uint32_t microphone_errors;
    uint8_t feature_get_queue_depth;
    uint8_t feature_set_queue_depth;
    uint8_t haptics_queue_depth;
    uint8_t speaker_queue_depth;
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
