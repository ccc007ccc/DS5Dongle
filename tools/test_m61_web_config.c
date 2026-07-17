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
    return config;
}

int main(void)
{
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
    assert(m61_web_config_decode(body, sizeof(body), &decoded) ==
           M61_WEB_CONFIG_BODY_SIZE);
    assert(memcmp(&input, &decoded, sizeof(input)) == 0);

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
    assert(m61_web_telemetry_encode(&telemetry, report, sizeof(report)) == 8);
    assert(report[0] == 0xD6U && report[1] == 0x83U);
    assert(report[2] == 1U && report[3] == 0x0FU);
    assert(report[4] == 0x90U && report[5] == 0x01U);
    assert(report[6] == 0x80U && report[7] == 0x01U);

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

    m61_web_runtime_set(&input);
    memset(&decoded, 0, sizeof(decoded));
    m61_web_runtime_get(&decoded);
    assert(memcmp(&input, &decoded, sizeof(input)) == 0);

    puts("M61 Web config protocol tests passed.");
    return 0;
}
