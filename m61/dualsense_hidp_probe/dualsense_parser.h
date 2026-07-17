#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS5_REPORT_UNKNOWN = 0,
    DS5_REPORT_INPUT,
    DS5_REPORT_MIC_AUDIO,
} dualsense_report_kind_t;

typedef enum {
    DS5_BUTTON_SQUARE       = 1u << 0,
    DS5_BUTTON_CROSS        = 1u << 1,
    DS5_BUTTON_CIRCLE       = 1u << 2,
    DS5_BUTTON_TRIANGLE     = 1u << 3,
    DS5_BUTTON_L1           = 1u << 4,
    DS5_BUTTON_R1           = 1u << 5,
    DS5_BUTTON_L2           = 1u << 6,
    DS5_BUTTON_R2           = 1u << 7,
    DS5_BUTTON_CREATE       = 1u << 8,
    DS5_BUTTON_OPTIONS      = 1u << 9,
    DS5_BUTTON_L3           = 1u << 10,
    DS5_BUTTON_R3           = 1u << 11,
    DS5_BUTTON_PS           = 1u << 12,
    DS5_BUTTON_TOUCHPAD     = 1u << 13,
    DS5_BUTTON_MUTE         = 1u << 14,
    DS5_BUTTON_EDGE_FN_L    = 1u << 15,
    DS5_BUTTON_EDGE_FN_R    = 1u << 16,
    DS5_BUTTON_EDGE_PADDLE_L = 1u << 17,
    DS5_BUTTON_EDGE_PADDLE_R = 1u << 18,
} dualsense_button_mask_t;

typedef struct {
    uint8_t report_id;
    uint8_t sequence;
    bool is_full_report;
    bool has_motion;
    bool has_battery;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t l2;
    uint8_t r2;
    uint8_t dpad;
    uint32_t buttons;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    uint32_t sensor_timestamp;
    int8_t temperature;
    uint8_t battery_percent;
    uint8_t power_state;
    bool headphones;
    bool mic_present;
    bool mic_muted;
    bool usb_data;
    bool usb_power;
} dualsense_state_t;

typedef struct {
    dualsense_report_kind_t kind;
    uint8_t report_id;
    size_t payload_offset;
    size_t payload_len;
} dualsense_parse_result_t;

bool dualsense_parse_report(const uint8_t *data, size_t len,
                            dualsense_state_t *state,
                            dualsense_parse_result_t *result);
bool dualsense_state_equal(const dualsense_state_t *lhs,
                           const dualsense_state_t *rhs);
bool dualsense_user_input_active(const dualsense_state_t *state);
const char *dualsense_dpad_name(uint8_t dpad);
void dualsense_format_buttons(uint32_t buttons, char *out, size_t out_len);
void dualsense_format_state(const dualsense_state_t *state,
                            char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
