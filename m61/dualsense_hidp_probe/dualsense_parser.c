#include "dualsense_parser.h"

#include <stdio.h>
#include <string.h>

#define DS5_BT_HIDP_INPUT 0xA1
#define DS5_BT_INPUT_REPORT_ID 0x31
#define DS5_MIN_PAYLOAD_LEN 55
#define DS5_BASIC_INPUT_REPORT_ID 0x01
#define DS5_BASIC_PAYLOAD_LEN 9

static int16_t read_i16_le(const uint8_t *p)
{
    return (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t) p[0] |
           ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static void set_button(uint32_t *buttons, uint8_t value, uint8_t bit,
                       dualsense_button_mask_t mask)
{
    if ((value & bit) != 0) {
        *buttons |= (uint32_t) mask;
    }
}

bool dualsense_parse_report(const uint8_t *data, size_t len,
                            dualsense_state_t *state,
                            dualsense_parse_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->kind = DS5_REPORT_UNKNOWN;
    }
    if (data == NULL || state == NULL || len == 0) {
        return false;
    }

    size_t payload_offset = 0;
    size_t payload_len = 0;
    uint8_t report_id = 0;
    uint8_t sequence = 0;
    bool is_full_report = false;

    if (len >= 3 && data[0] == DS5_BT_HIDP_INPUT && data[1] == DS5_BT_INPUT_REPORT_ID) {
        report_id = data[1];
        sequence = data[2];
        payload_offset = 3;
        payload_len = len - payload_offset;
        is_full_report = true;
        if ((data[2] & 0x02) != 0) {
            if (result != NULL) {
                result->kind = DS5_REPORT_MIC_AUDIO;
                result->report_id = report_id;
                result->payload_offset = payload_offset;
                result->payload_len = len - payload_offset;
            }
            return false;
        }
    } else if (len >= 2 && data[0] == DS5_BT_INPUT_REPORT_ID) {
        report_id = data[0];
        sequence = data[1];
        payload_offset = 2;
        payload_len = len - payload_offset;
        is_full_report = true;
        if ((data[1] & 0x02) != 0) {
            if (result != NULL) {
                result->kind = DS5_REPORT_MIC_AUDIO;
                result->report_id = report_id;
                result->payload_offset = payload_offset;
                result->payload_len = len - payload_offset;
            }
            return false;
        }
    } else if (len >= 2 + DS5_BASIC_PAYLOAD_LEN &&
               data[0] == DS5_BT_HIDP_INPUT &&
               data[1] == DS5_BASIC_INPUT_REPORT_ID) {
        report_id = data[1];
        sequence = 0;
        payload_offset = 2;
        payload_len = len - payload_offset;
    } else if (len >= 1 + DS5_BASIC_PAYLOAD_LEN && data[0] == DS5_BASIC_INPUT_REPORT_ID) {
        report_id = data[0];
        sequence = 0;
        payload_offset = 1;
        payload_len = len - payload_offset;
    } else if (len >= DS5_MIN_PAYLOAD_LEN) {
        report_id = DS5_BT_INPUT_REPORT_ID;
        sequence = 0;
        payload_offset = 0;
        payload_len = len;
        is_full_report = true;
    } else {
        return false;
    }

    if (len < payload_offset ||
        (is_full_report && payload_len < DS5_MIN_PAYLOAD_LEN) ||
        (!is_full_report && payload_len < DS5_BASIC_PAYLOAD_LEN)) {
        return false;
    }

    const uint8_t *p = data + payload_offset;
    memset(state, 0, sizeof(*state));

    state->report_id = report_id;
    state->sequence = sequence;
    state->is_full_report = is_full_report;
    state->has_motion = is_full_report;
    state->has_battery = is_full_report;
    state->left_x = p[0];
    state->left_y = p[1];
    state->right_x = p[2];
    state->right_y = p[3];
    state->l2 = p[4];
    state->r2 = p[5];
    state->dpad = p[7] & 0x0F;

    uint32_t buttons = 0;
    set_button(&buttons, p[7], 0x10, DS5_BUTTON_SQUARE);
    set_button(&buttons, p[7], 0x20, DS5_BUTTON_CROSS);
    set_button(&buttons, p[7], 0x40, DS5_BUTTON_CIRCLE);
    set_button(&buttons, p[7], 0x80, DS5_BUTTON_TRIANGLE);

    set_button(&buttons, p[8], 0x01, DS5_BUTTON_L1);
    set_button(&buttons, p[8], 0x02, DS5_BUTTON_R1);
    set_button(&buttons, p[8], 0x04, DS5_BUTTON_L2);
    set_button(&buttons, p[8], 0x08, DS5_BUTTON_R2);
    set_button(&buttons, p[8], 0x10, DS5_BUTTON_CREATE);
    set_button(&buttons, p[8], 0x20, DS5_BUTTON_OPTIONS);
    set_button(&buttons, p[8], 0x40, DS5_BUTTON_L3);
    set_button(&buttons, p[8], 0x80, DS5_BUTTON_R3);

    if (payload_len > 9) {
        set_button(&buttons, p[9], 0x01, DS5_BUTTON_PS);
        set_button(&buttons, p[9], 0x02, DS5_BUTTON_TOUCHPAD);
        set_button(&buttons, p[9], 0x04, DS5_BUTTON_MUTE);
        set_button(&buttons, p[9], 0x10, DS5_BUTTON_EDGE_FN_L);
        set_button(&buttons, p[9], 0x20, DS5_BUTTON_EDGE_FN_R);
        set_button(&buttons, p[9], 0x40, DS5_BUTTON_EDGE_PADDLE_L);
        set_button(&buttons, p[9], 0x80, DS5_BUTTON_EDGE_PADDLE_R);
    }
    state->buttons = buttons;

    if (!is_full_report) {
        if (result != NULL) {
            result->kind = DS5_REPORT_INPUT;
            result->report_id = report_id;
            result->payload_offset = payload_offset;
            result->payload_len = payload_len;
        }
        return true;
    }

    state->gyro_x = read_i16_le(p + 15);
    state->gyro_z = read_i16_le(p + 17);
    state->gyro_y = read_i16_le(p + 19);
    state->accel_x = read_i16_le(p + 21);
    state->accel_y = read_i16_le(p + 23);
    state->accel_z = read_i16_le(p + 25);
    state->sensor_timestamp = read_u32_le(p + 27);
    state->temperature = (int8_t) p[31];
    state->battery_percent = p[52] & 0x0F;
    state->power_state = p[52] >> 4;
    state->headphones = (p[53] & 0x01) != 0;
    state->mic_present = (p[53] & 0x02) != 0;
    state->mic_muted = (p[53] & 0x04) != 0;
    state->usb_data = (p[53] & 0x08) != 0;
    state->usb_power = (p[53] & 0x10) != 0;

    if (result != NULL) {
        result->kind = DS5_REPORT_INPUT;
        result->report_id = report_id;
        result->payload_offset = payload_offset;
        result->payload_len = payload_len;
    }
    return true;
}

bool dualsense_state_equal(const dualsense_state_t *lhs,
                           const dualsense_state_t *rhs)
{
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
}

bool dualsense_user_input_active(const dualsense_state_t *state)
{
    /* Idle detection deliberately ignores the inner 25% of stick travel so
     * ordinary center drift cannot keep the controller awake.  This does not
     * alter the input report sent to the host. */
    const uint8_t stick_min = 96U;
    const uint8_t stick_max = 160U;

    if (state == NULL) return false;
    return state->left_x < stick_min || state->left_x > stick_max ||
           state->left_y < stick_min || state->left_y > stick_max ||
           state->right_x < stick_min || state->right_x > stick_max ||
           state->right_y < stick_min || state->right_y > stick_max ||
           state->l2 > 8U || state->r2 > 8U || state->dpad != 8U ||
           state->buttons != 0U;
}

const char *dualsense_dpad_name(uint8_t dpad)
{
    switch (dpad) {
    case 0: return "N";
    case 1: return "NE";
    case 2: return "E";
    case 3: return "SE";
    case 4: return "S";
    case 5: return "SW";
    case 6: return "W";
    case 7: return "NW";
    case 8: return "idle";
    default: return "invalid";
    }
}

typedef struct {
    uint32_t mask;
    const char *name;
} button_name_t;

void dualsense_format_buttons(uint32_t buttons, char *out, size_t out_len)
{
    static const button_name_t names[] = {
        {DS5_BUTTON_SQUARE, "square"},
        {DS5_BUTTON_CROSS, "cross"},
        {DS5_BUTTON_CIRCLE, "circle"},
        {DS5_BUTTON_TRIANGLE, "triangle"},
        {DS5_BUTTON_L1, "L1"},
        {DS5_BUTTON_R1, "R1"},
        {DS5_BUTTON_L2, "L2"},
        {DS5_BUTTON_R2, "R2"},
        {DS5_BUTTON_CREATE, "create"},
        {DS5_BUTTON_OPTIONS, "options"},
        {DS5_BUTTON_L3, "L3"},
        {DS5_BUTTON_R3, "R3"},
        {DS5_BUTTON_PS, "PS"},
        {DS5_BUTTON_TOUCHPAD, "touchpad"},
        {DS5_BUTTON_MUTE, "mute"},
        {DS5_BUTTON_EDGE_FN_L, "FnL"},
        {DS5_BUTTON_EDGE_FN_R, "FnR"},
        {DS5_BUTTON_EDGE_PADDLE_L, "PaddleL"},
        {DS5_BUTTON_EDGE_PADDLE_R, "PaddleR"},
    };

    if (out == NULL || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (buttons == 0) {
        snprintf(out, out_len, "none");
        return;
    }

    size_t used = 0;
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((buttons & names[i].mask) == 0) {
            continue;
        }
        int written = snprintf(out + used, out_len - used, "%s%s",
                               first ? "" : "+", names[i].name);
        if (written < 0 || (size_t) written >= out_len - used) {
            out[out_len - 1] = '\0';
            return;
        }
        used += (size_t) written;
        first = false;
    }
}

void dualsense_format_state(const dualsense_state_t *state,
                            char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (state == NULL) {
        snprintf(out, out_len, "no state");
        return;
    }

    char buttons[160];
    dualsense_format_buttons(state->buttons, buttons, sizeof(buttons));

    if (state->has_motion && state->has_battery) {
        snprintf(out, out_len,
             "report=0x%02X mode=%s seq=0x%02X LX=%3u LY=%3u RX=%3u RY=%3u "
             "L2=%3u R2=%3u dpad=%s buttons=%s gyro=(%d,%d,%d) "
             "accel=(%d,%d,%d) battery=%u0%% power=0x%X hp=%u mic=%u muted=%u",
             state->report_id,
             state->is_full_report ? "full" : "basic",
             state->sequence,
             state->left_x,
             state->left_y,
             state->right_x,
             state->right_y,
             state->l2,
             state->r2,
             dualsense_dpad_name(state->dpad),
             buttons,
             state->gyro_x,
             state->gyro_y,
             state->gyro_z,
             state->accel_x,
             state->accel_y,
             state->accel_z,
             state->battery_percent,
             state->power_state,
             state->headphones ? 1 : 0,
             state->mic_present ? 1 : 0,
             state->mic_muted ? 1 : 0);
        return;
    }

    snprintf(out, out_len,
             "report=0x%02X mode=%s seq=0x%02X LX=%3u LY=%3u RX=%3u RY=%3u "
             "L2=%3u R2=%3u dpad=%s buttons=%s gyro=n/a accel=n/a battery=n/a",
             state->report_id,
             state->is_full_report ? "full" : "basic",
             state->sequence,
             state->left_x,
             state->left_y,
             state->right_x,
             state->right_y,
             state->l2,
             state->r2,
             dualsense_dpad_name(state->dpad),
             buttons);
}
