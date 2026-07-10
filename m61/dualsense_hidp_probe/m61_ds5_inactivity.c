#include "m61_ds5_inactivity.h"

#include <string.h>

#define DS5_INACTIVE_AXIS_MIN 120U
#define DS5_INACTIVE_AXIS_MAX 140U
#define DS5_INACTIVE_DPAD_NEUTRAL 8U
#define DS5_INACTIVE_MINUTE_MS 60000UL

void m61_ds5_inactivity_reset(m61_ds5_inactivity_t *tracker)
{
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

bool m61_ds5_inactivity_state_is_neutral(const dualsense_state_t *state)
{
    if (state == NULL || !state->is_full_report) {
        return false;
    }

    return state->left_x >= DS5_INACTIVE_AXIS_MIN &&
           state->left_x <= DS5_INACTIVE_AXIS_MAX &&
           state->left_y >= DS5_INACTIVE_AXIS_MIN &&
           state->left_y <= DS5_INACTIVE_AXIS_MAX &&
           state->right_x >= DS5_INACTIVE_AXIS_MIN &&
           state->right_x <= DS5_INACTIVE_AXIS_MAX &&
           state->right_y >= DS5_INACTIVE_AXIS_MIN &&
           state->right_y <= DS5_INACTIVE_AXIS_MAX &&
           state->l2 == 0U &&
           state->r2 == 0U &&
           state->dpad == DS5_INACTIVE_DPAD_NEUTRAL &&
           state->buttons == 0U;
}

bool m61_ds5_inactivity_note_report(m61_ds5_inactivity_t *tracker,
                                    const dualsense_state_t *state,
                                    uint32_t now_ms,
                                    uint8_t inactive_minutes)
{
    uint32_t timeout_ms;

    if (tracker == NULL || state == NULL || !state->is_full_report) {
        return false;
    }

    if (inactive_minutes == 0U) {
        m61_ds5_inactivity_reset(tracker);
        return false;
    }

    if (!tracker->armed || tracker->inactive_minutes != inactive_minutes) {
        tracker->last_activity_ms = now_ms;
        tracker->inactive_minutes = inactive_minutes;
        tracker->armed = true;
        tracker->disconnect_requested = false;
        return false;
    }

    if (!m61_ds5_inactivity_state_is_neutral(state)) {
        tracker->last_activity_ms = now_ms;
        tracker->disconnect_requested = false;
        return false;
    }

    if (tracker->disconnect_requested) {
        return false;
    }

    timeout_ms = (uint32_t)inactive_minutes * DS5_INACTIVE_MINUTE_MS;
    if ((uint32_t)(now_ms - tracker->last_activity_ms) > timeout_ms) {
        tracker->last_activity_ms = now_ms;
        tracker->disconnect_requested = true;
        return true;
    }

    return false;
}

void m61_ds5_inactivity_retry(m61_ds5_inactivity_t *tracker)
{
    if (tracker != NULL) {
        tracker->disconnect_requested = false;
    }
}
