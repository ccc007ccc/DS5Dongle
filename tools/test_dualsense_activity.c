#include <assert.h>
#include <string.h>

#include "dualsense_parser.h"

static dualsense_state_t idle_state(void)
{
    dualsense_state_t state;
    memset(&state, 0, sizeof(state));
    state.left_x = 128U;
    state.left_y = 128U;
    state.right_x = 128U;
    state.right_y = 128U;
    state.dpad = 8U;
    return state;
}

int main(void)
{
    dualsense_state_t state = idle_state();

    assert(!dualsense_user_input_active(&state));

    state.left_x = 96U;
    assert(!dualsense_user_input_active(&state));
    state.left_x = 160U;
    assert(!dualsense_user_input_active(&state));
    state.left_x = 95U;
    assert(dualsense_user_input_active(&state));
    state.left_x = 161U;
    assert(dualsense_user_input_active(&state));

    state = idle_state();
    state.gyro_x = 32767;
    state.accel_z = -32768;
    assert(!dualsense_user_input_active(&state));

    state.buttons = 1U;
    assert(dualsense_user_input_active(&state));
    return 0;
}
