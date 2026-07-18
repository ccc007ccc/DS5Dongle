#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m61_web_config.h"

static m61_web_config_t fixture(void)
{
    m61_web_config_t config = {0};

    config.capabilities = M61_WEB_CAP_MICROPHONE |
                          M61_WEB_CAP_SPEAKER_ROUTE |
                          M61_WEB_CAP_AUTO_RECONNECT |
                          M61_WEB_CAP_DVFS;
    config.speaker_enabled = true;
    config.auto_reconnect_enabled = true;
    config.status_led_enabled = true;
    config.speaker_route = 2U;
    config.cpu_governor = 1U;
    config.cpu_profile = 2U;
    config.manual_cpu_mhz = 400U;
    config.haptics_gain_q8 = 0x0180U;
    config.idle_timeout_minutes = 30U;
    config.power_off_on_usb_suspend = true;
    config.left_stick_deadzone_percent = 8U;
    config.right_stick_deadzone_percent = 12U;
    config.usb_polling_rate_mode = M61_WEB_USB_POLL_500_HZ;
    config.status_led_brightness_percent = 35U;
    config.audio_buffer_length = 64U;
    return config;
}

int main(void)
{
    static const uint8_t persistent_v1[26] = {
        0x4d, 0x36, 0x31, 0x57, 0x01, 0x10, 0x4d, 0x36, 0x31, 0x43,
        0x01, 0x10, 0xff, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x40, 0x01,
        0x00, 0x01, 0xb1, 0x14, 0x2e, 0x1a,
    };
    static const uint8_t persistent_v2[28] = {
        0x4d, 0x36, 0x31, 0x57, 0x02, 0x12, 0x4d, 0x36, 0x31, 0x43,
        0x02, 0x12, 0xff, 0x07, 0x0e, 0x00, 0x00, 0x00, 0x40, 0x01,
        0x00, 0x01, 0x00, 0x00, 0x7d, 0x8d, 0xc6, 0x48,
    };
    static const uint8_t persistent_v3[30] = {
        0x4d, 0x36, 0x31, 0x57, 0x03, 0x14, 0x4d, 0x36, 0x31, 0x43,
        0x03, 0x14, 0x4d, 0x00, 0x0e, 0x02, 0x01, 0x02, 0x90, 0x01,
        0x80, 0x01, 0x1e, 0x01, 0x08, 0x0c, 0xdc, 0xfb, 0xe0, 0x60,
    };
    static const uint8_t persistent_v4_retired_rate[31] = {
        0x4d, 0x36, 0x31, 0x57, 0x04, 0x15, 0x4d, 0x36, 0x31, 0x43,
        0x04, 0x15, 0x4d, 0x00, 0x0e, 0x02, 0x01, 0x02, 0x90, 0x01,
        0x80, 0x01, 0x1e, 0x01, 0x08, 0x0c, 0x03, 0x87, 0xd1, 0x65,
        0x2b,
    };
    m61_web_config_t input = fixture();
    m61_web_config_t decoded;
    m61_web_telemetry_t telemetry = {0};
    uint8_t body[M61_WEB_CONFIG_BODY_SIZE];
    uint8_t command[M61_WEB_FEATURE_PAYLOAD_SIZE];
    uint8_t report[M61_WEB_FEATURE_PAYLOAD_SIZE];
    uint8_t persistent[M61_WEB_PERSISTENT_RECORD_SIZE];

    assert(m61_web_config_encode(&input, body, sizeof(body)) ==
           M61_WEB_CONFIG_BODY_SIZE);
    assert(memcmp(body, "M61C", 4) == 0);
    assert(body[4] == M61_WEB_CONFIG_SCHEMA_VERSION);
    assert(body[6] == 0x4DU && body[7] == 0U);
    assert(body[8] == 0x0EU);
    assert(body[12] == 0x90U && body[13] == 0x01U);
    assert(body[14] == 0x80U && body[15] == 0x01U);
    assert(body[16] == 30U && body[17] == 1U);
    assert(body[18] == 8U && body[19] == 12U);
    assert(body[20] == M61_WEB_USB_POLL_500_HZ);
    assert(body[21] == 35U && body[22] == 64U);
    assert(m61_web_config_decode(body, sizeof(body), &decoded) ==
           M61_WEB_CONFIG_BODY_SIZE);
    assert(memcmp(&input, &decoded, sizeof(input)) == 0);

    decoded = input;
    decoded.status_led_brightness_percent = 0U;
    assert(!m61_web_config_valid(&decoded));
    decoded.status_led_brightness_percent = 101U;
    assert(!m61_web_config_valid(&decoded));
    decoded = input;
    decoded.audio_buffer_length = 15U;
    assert(!m61_web_config_valid(&decoded));
    decoded.audio_buffer_length = 128U;
    assert(!m61_web_config_valid(&decoded));

    body[20] = 3U;
    assert(m61_web_config_decode(body, sizeof(body), &decoded) ==
           M61_WEB_CONFIG_BODY_SIZE);
    assert(decoded.usb_polling_rate_mode == M61_WEB_USB_POLL_500_HZ);
    body[20] = M61_WEB_USB_POLL_500_HZ;

    body[0] = 5U;
    assert(m61_web_config_decode(body, sizeof(body), &decoded) == -2);
    assert(m61_web_command_encode(M61_WEB_COMMAND_APPLY_CONFIG,
                                  &input,
                                  command,
                                  sizeof(command)) ==
           M61_WEB_FEATURE_PAYLOAD_SIZE);
    assert(command[0] == M61_WEB_COMMAND_APPLY_CONFIG);
    assert(memcmp(&command[1], "M61C", 4) == 0);

    telemetry.rssi_valid = true;
    telemetry.rssi = -42;
    telemetry.speaker_active = true;
    telemetry.microphone_active = true;
    telemetry.bluetooth_connected = true;
    telemetry.usb_configured = true;
    telemetry.headphones_connected = true;
    telemetry.speaker_stereo = true;
    telemetry.current_cpu_mhz = 400U;
    telemetry.requested_cpu_mhz = 384U;
    telemetry.pairing_active = true;
    telemetry.discovery_active = true;
    telemetry.saved_controller = true;
    telemetry.config_loaded = true;
    telemetry.usb_suspended = true;
    telemetry.last_management_command = M61_WEB_COMMAND_FORGET_CONTROLLER;
    telemetry.last_management_error = -107;
    telemetry.management_sequence = 7U;
    telemetry.usb_input_dropped = 11U;
    telemetry.host_report_dropped = 13U;
    telemetry.audio_ingress_dropped = 17U;
    telemetry.haptics_queue_dropped = 19U;
    telemetry.speaker_errors = 23U;
    telemetry.microphone_errors = 29U;
    telemetry.feature_get_queue_depth = 2U;
    telemetry.feature_set_queue_depth = 3U;
    telemetry.haptics_queue_depth = 4U;
    telemetry.speaker_queue_depth = 5U;
    assert(m61_web_telemetry_encode(&telemetry, report, sizeof(report)) == 44);
    assert(report[0] == 0xD6U && report[1] == 0x83U);
    assert(report[2] == 2U && report[3] == 0xFFU);
    assert(report[4] == 0x90U && report[5] == 0x01U);
    assert(report[6] == 0x80U && report[7] == 0x01U);
    assert(report[8] == 1U && report[9] == M61_WEB_COMMAND_FORGET_CONTROLLER);
    assert(report[10] == 0x95U && report[11] == 0xFFU);
    assert(report[12] == 7U && report[16] == 11U && report[20] == 13U);
    assert(report[24] == 17U && report[28] == 19U && report[32] == 23U);
    assert(report[36] == 29U);
    assert(report[40] == 2U && report[41] == 3U &&
           report[42] == 4U && report[43] == 5U);

    assert(m61_web_persistent_encode(&input,
                                     persistent,
                                     sizeof(persistent)) ==
           M61_WEB_PERSISTENT_RECORD_SIZE);
    assert(memcmp(persistent, "M61W", 4) == 0);
    memset(&decoded, 0, sizeof(decoded));
    assert(m61_web_persistent_decode(persistent,
                                     sizeof(persistent),
                                     &decoded) ==
           M61_WEB_PERSISTENT_RECORD_SIZE);
    assert(memcmp(&input, &decoded, sizeof(input)) == 0);
    persistent[12] ^= 0x01U;
    assert(m61_web_persistent_decode(persistent,
                                     sizeof(persistent),
                                     &decoded) == -5);

    memset(&decoded, 0, sizeof(decoded));
    assert(m61_web_persistent_decode(persistent_v1,
                                     sizeof(persistent_v1),
                                     &decoded) == (int)sizeof(persistent_v1));
    assert(decoded.microphone_enabled == false);
    assert(decoded.manual_cpu_mhz == 320U);
    assert(decoded.idle_timeout_minutes == 0U);
    assert(decoded.power_off_on_usb_suspend == false);
    assert(decoded.left_stick_deadzone_percent == 0U);
    assert(decoded.right_stick_deadzone_percent == 0U);
    assert(decoded.usb_polling_rate_mode == M61_WEB_USB_POLL_REALTIME);
    assert(decoded.status_led_brightness_percent ==
           M61_WEB_STATUS_LED_BRIGHTNESS_DEFAULT);
    assert(decoded.audio_buffer_length == M61_WEB_AUDIO_BUFFER_LENGTH_DEFAULT);

    memset(&decoded, 0, sizeof(decoded));
    assert(m61_web_persistent_decode(persistent_v4_retired_rate,
                                     sizeof(persistent_v4_retired_rate),
                                     &decoded) ==
           (int)sizeof(persistent_v4_retired_rate));
    assert(decoded.left_stick_deadzone_percent == 8U);
    assert(decoded.right_stick_deadzone_percent == 12U);
    assert(decoded.usb_polling_rate_mode == M61_WEB_USB_POLL_500_HZ);
    assert(decoded.status_led_brightness_percent ==
           M61_WEB_STATUS_LED_BRIGHTNESS_DEFAULT);
    assert(decoded.audio_buffer_length == M61_WEB_AUDIO_BUFFER_LENGTH_DEFAULT);

    memset(&decoded, 0, sizeof(decoded));
    assert(m61_web_persistent_decode(persistent_v2,
                                     sizeof(persistent_v2),
                                     &decoded) == (int)sizeof(persistent_v2));
    assert(decoded.idle_timeout_minutes == 0U);
    assert(decoded.power_off_on_usb_suspend == false);
    assert(decoded.left_stick_deadzone_percent == 0U);
    assert(decoded.right_stick_deadzone_percent == 0U);
    assert(decoded.usb_polling_rate_mode == M61_WEB_USB_POLL_REALTIME);
    assert(decoded.status_led_brightness_percent ==
           M61_WEB_STATUS_LED_BRIGHTNESS_DEFAULT);
    assert(decoded.audio_buffer_length == M61_WEB_AUDIO_BUFFER_LENGTH_DEFAULT);

    memset(&decoded, 0, sizeof(decoded));
    assert(m61_web_persistent_decode(persistent_v3,
                                     sizeof(persistent_v3),
                                     &decoded) == (int)sizeof(persistent_v3));
    assert(decoded.left_stick_deadzone_percent == 8U);
    assert(decoded.right_stick_deadzone_percent == 12U);
    assert(decoded.usb_polling_rate_mode == M61_WEB_USB_POLL_REALTIME);
    assert(decoded.status_led_brightness_percent ==
           M61_WEB_STATUS_LED_BRIGHTNESS_DEFAULT);
    assert(decoded.audio_buffer_length == M61_WEB_AUDIO_BUFFER_LENGTH_DEFAULT);

    assert(m61_web_command_encode(M61_WEB_COMMAND_POWER_OFF_CONTROLLER,
                                  NULL,
                                  command,
                                  sizeof(command)) ==
           M61_WEB_FEATURE_PAYLOAD_SIZE);
    assert(m61_web_command_encode(M61_WEB_COMMAND_PAIR_CONTROLLER,
                                  NULL,
                                  command,
                                  sizeof(command)) == M61_WEB_FEATURE_PAYLOAD_SIZE);
    assert(m61_web_command_encode(M61_WEB_COMMAND_DISCONNECT_CONTROLLER,
                                  NULL,
                                  command,
                                  sizeof(command)) == M61_WEB_FEATURE_PAYLOAD_SIZE);
    assert(m61_web_command_encode(M61_WEB_COMMAND_FORGET_CONTROLLER,
                                  NULL,
                                  command,
                                  sizeof(command)) == M61_WEB_FEATURE_PAYLOAD_SIZE);

    m61_web_runtime_set(&input);
    memset(&decoded, 0, sizeof(decoded));
    m61_web_runtime_get(&decoded);
    assert(memcmp(&input, &decoded, sizeof(input)) == 0);

    puts("M61 Web config protocol tests passed.");
    return 0;
}
