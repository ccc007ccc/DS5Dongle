#include "m61_web_config.h"

#include <string.h>

static const uint8_t m61_web_magic[4] = {'M', '6', '1', 'C'};
static const uint8_t m61_web_persistent_magic[4] = {'M', '6', '1', 'W'};
#define M61_WEB_CONFIG_SCHEMA_V1 1U
#define M61_WEB_CONFIG_BODY_SIZE_V1 16U
#define M61_WEB_PERSISTENT_RECORD_V1 1U
#define M61_WEB_PERSISTENT_RECORD_SIZE_V1 26U
static m61_web_config_t m61_web_runtime_config;
static bool m61_web_runtime_initialized;
static volatile uint32_t m61_web_runtime_sequence;

static void put_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16_le(const uint8_t *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8);
}

static void put_u32_le(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *input)
{
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8U; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);

            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

void m61_web_config_defaults(m61_web_config_t *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->capabilities = M61_WEB_CAP_MICROPHONE |
                           M61_WEB_CAP_SPEAKER_GATE |
                           M61_WEB_CAP_SPEAKER_ROUTE |
                           M61_WEB_CAP_AUTO_RECONNECT |
                           M61_WEB_CAP_STATUS_LED |
                           M61_WEB_CAP_HAPTICS_GAIN |
                           M61_WEB_CAP_DVFS |
                           M61_WEB_CAP_TELEMETRY_V1 |
                           M61_WEB_CAP_IDLE_POWEROFF |
                           M61_WEB_CAP_CONTROLLER_POWEROFF |
                           M61_WEB_CAP_SUSPEND_POWEROFF;
    config->speaker_enabled = true;
    config->auto_reconnect_enabled = true;
    config->status_led_enabled = true;
    config->manual_cpu_mhz = 320U;
    config->haptics_gain_q8 = 0x0100U;
}

bool m61_web_config_valid(const m61_web_config_t *config)
{
    if (config == NULL) return false;
    if (config->speaker_route > 2U) return false;
    if (config->cpu_governor > 1U) return false;
    if (config->cpu_profile > 3U) return false;
    if (config->manual_cpu_mhz < 320U || config->manual_cpu_mhz > 400U) {
        return false;
    }
    if (config->haptics_gain_q8 < 0x0100U ||
        config->haptics_gain_q8 > 0x0200U) {
        return false;
    }
    if (config->idle_timeout_minutes > 60U) return false;
    return true;
}

int m61_web_config_encode(const m61_web_config_t *config,
                          uint8_t *output,
                          size_t output_size)
{
    uint8_t flags = 0U;

    if (!m61_web_config_valid(config) || output == NULL ||
        output_size < M61_WEB_CONFIG_BODY_SIZE) {
        return -1;
    }
    memset(output, 0, M61_WEB_CONFIG_BODY_SIZE);
    memcpy(output, m61_web_magic, sizeof(m61_web_magic));
    output[4] = M61_WEB_CONFIG_SCHEMA_VERSION;
    output[5] = M61_WEB_CONFIG_BODY_SIZE;
    put_u16_le(&output[6], config->capabilities);
    if (config->microphone_enabled) flags |= 0x01U;
    if (config->speaker_enabled) flags |= 0x02U;
    if (config->auto_reconnect_enabled) flags |= 0x04U;
    if (config->status_led_enabled) flags |= 0x08U;
    output[8] = flags;
    output[9] = config->speaker_route;
    output[10] = config->cpu_governor;
    output[11] = config->cpu_profile;
    put_u16_le(&output[12], config->manual_cpu_mhz);
    put_u16_le(&output[14], config->haptics_gain_q8);
    output[16] = config->idle_timeout_minutes;
    if (config->power_off_on_usb_suspend) output[17] = 0x01U;
    return M61_WEB_CONFIG_BODY_SIZE;
}

int m61_web_config_decode(const uint8_t *input,
                          size_t input_size,
                          m61_web_config_t *config)
{
    uint8_t flags;

    uint8_t version;
    uint8_t body_size;

    if (input == NULL || config == NULL || input_size < M61_WEB_CONFIG_BODY_SIZE_V1) {
        return -1;
    }
    if (memcmp(input, m61_web_magic, sizeof(m61_web_magic)) != 0) return -2;
    version = input[4];
    body_size = input[5];
    if (version != M61_WEB_CONFIG_SCHEMA_V1 &&
        version != M61_WEB_CONFIG_SCHEMA_VERSION) return -3;
    if ((version == M61_WEB_CONFIG_SCHEMA_V1 && body_size != M61_WEB_CONFIG_BODY_SIZE_V1) ||
        (version == M61_WEB_CONFIG_SCHEMA_VERSION && body_size != M61_WEB_CONFIG_BODY_SIZE) ||
        input_size < body_size) return -1;
    memset(config, 0, sizeof(*config));
    config->capabilities = get_u16_le(&input[6]);
    flags = input[8];
    config->microphone_enabled = (flags & 0x01U) != 0U;
    config->speaker_enabled = (flags & 0x02U) != 0U;
    config->auto_reconnect_enabled = (flags & 0x04U) != 0U;
    config->status_led_enabled = (flags & 0x08U) != 0U;
    config->speaker_route = input[9];
    config->cpu_governor = input[10];
    config->cpu_profile = input[11];
    config->manual_cpu_mhz = get_u16_le(&input[12]);
    config->haptics_gain_q8 = get_u16_le(&input[14]);
    if (version >= M61_WEB_CONFIG_SCHEMA_VERSION) {
        config->idle_timeout_minutes = input[16];
        config->power_off_on_usb_suspend = (input[17] & 0x01U) != 0U;
    }
    return m61_web_config_valid(config) ? (int)body_size : -4;
}

int m61_web_command_encode(uint8_t command,
                           const m61_web_config_t *config,
                           uint8_t *output,
                           size_t output_size)
{
    int encoded;

    if (output == NULL || output_size < M61_WEB_FEATURE_PAYLOAD_SIZE) return -1;
    memset(output, 0, M61_WEB_FEATURE_PAYLOAD_SIZE);
    output[0] = command;
    if (command == M61_WEB_COMMAND_APPLY_CONFIG) {
        encoded = m61_web_config_encode(config,
                                        &output[1],
                                        M61_WEB_FEATURE_PAYLOAD_SIZE - 1U);
        return encoded < 0 ? encoded : (int)M61_WEB_FEATURE_PAYLOAD_SIZE;
    }
    if (command != M61_WEB_COMMAND_SAVE_CONFIG &&
        command != M61_WEB_COMMAND_RECONNECT_USB &&
        command != M61_WEB_COMMAND_POWER_OFF_CONTROLLER) {
        return -4;
    }
    if (config != NULL) return -4;
    return M61_WEB_FEATURE_PAYLOAD_SIZE;
}

int m61_web_telemetry_encode(const m61_web_telemetry_t *telemetry,
                             uint8_t *output,
                             size_t output_size)
{
    uint8_t state = 0U;

    if (telemetry == NULL || output == NULL || output_size < 8U) return -1;
    memset(output, 0, output_size);
    output[0] = telemetry->rssi_valid ? (uint8_t)telemetry->rssi : 0x7FU;
    output[1] = 0x80U;
    if (telemetry->speaker_active) output[1] |= 0x02U;
    if (telemetry->microphone_active) output[1] |= 0x01U;
    output[2] = M61_WEB_TELEMETRY_VERSION;
    if (telemetry->bluetooth_connected) state |= 0x01U;
    if (telemetry->usb_configured) state |= 0x02U;
    if (telemetry->headphones_connected) state |= 0x04U;
    if (telemetry->speaker_stereo) state |= 0x08U;
    output[3] = state;
    put_u16_le(&output[4], telemetry->current_cpu_mhz);
    put_u16_le(&output[6], telemetry->requested_cpu_mhz);
    return 8;
}

int m61_web_persistent_encode(const m61_web_config_t *config,
                              uint8_t *output,
                              size_t output_size)
{
    int encoded;
    uint32_t crc;

    if (output == NULL || output_size < M61_WEB_PERSISTENT_RECORD_SIZE) {
        return -1;
    }
    memset(output, 0, M61_WEB_PERSISTENT_RECORD_SIZE);
    memcpy(output, m61_web_persistent_magic, sizeof(m61_web_persistent_magic));
    output[4] = M61_WEB_PERSISTENT_RECORD_VERSION;
    output[5] = M61_WEB_CONFIG_BODY_SIZE;
    encoded = m61_web_config_encode(config,
                                    &output[6],
                                    M61_WEB_CONFIG_BODY_SIZE);
    if (encoded < 0) return encoded;
    crc = crc32_ieee(output, M61_WEB_PERSISTENT_RECORD_SIZE - 4U);
    put_u32_le(&output[M61_WEB_PERSISTENT_RECORD_SIZE - 4U], crc);
    return (int)M61_WEB_PERSISTENT_RECORD_SIZE;
}

int m61_web_persistent_decode(const uint8_t *input,
                              size_t input_size,
                              m61_web_config_t *config)
{
    uint32_t expected_crc;
    uint32_t actual_crc;

    uint8_t record_version;
    uint8_t body_size;
    size_t expected_size;

    if (input == NULL || config == NULL ||
        (input_size != M61_WEB_PERSISTENT_RECORD_SIZE &&
         input_size != M61_WEB_PERSISTENT_RECORD_SIZE_V1)) {
        return -1;
    }
    if (memcmp(input,
               m61_web_persistent_magic,
               sizeof(m61_web_persistent_magic)) != 0) {
        return -2;
    }
    record_version = input[4];
    body_size = input[5];
    if (record_version == M61_WEB_PERSISTENT_RECORD_V1 &&
        body_size == M61_WEB_CONFIG_BODY_SIZE_V1) {
        expected_size = M61_WEB_PERSISTENT_RECORD_SIZE_V1;
    } else if (record_version == M61_WEB_PERSISTENT_RECORD_VERSION &&
               body_size == M61_WEB_CONFIG_BODY_SIZE) {
        expected_size = M61_WEB_PERSISTENT_RECORD_SIZE;
    } else {
        return -3;
    }
    if (input_size != expected_size) return -1;
    expected_crc = get_u32_le(&input[expected_size - 4U]);
    actual_crc = crc32_ieee(input, expected_size - 4U);
    if (actual_crc != expected_crc) return -5;
    if (m61_web_config_decode(&input[6], body_size, config) < 0) {
        return -4;
    }
    return (int)expected_size;
}

void m61_web_runtime_set(const m61_web_config_t *config)
{
    if (!m61_web_config_valid(config)) return;
    m61_web_runtime_sequence++;
    __asm volatile("" : : : "memory");
    m61_web_runtime_config = *config;
    m61_web_runtime_initialized = true;
    __asm volatile("" : : : "memory");
    m61_web_runtime_sequence++;
}

void m61_web_runtime_get(m61_web_config_t *config)
{
    uint32_t before;
    uint32_t after;

    if (config == NULL) return;
    if (!m61_web_runtime_initialized) {
        m61_web_config_defaults(&m61_web_runtime_config);
        m61_web_runtime_initialized = true;
    }
    do {
        before = m61_web_runtime_sequence;
        __asm volatile("" : : : "memory");
        *config = m61_web_runtime_config;
        __asm volatile("" : : : "memory");
        after = m61_web_runtime_sequence;
    } while ((before & 1U) != 0U || before != after);
}
