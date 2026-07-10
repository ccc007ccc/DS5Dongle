#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dualsense_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t last_activity_ms;
    uint8_t inactive_minutes;
    bool armed;
    bool disconnect_requested;
} m61_ds5_inactivity_t;

void m61_ds5_inactivity_reset(m61_ds5_inactivity_t *tracker);
bool m61_ds5_inactivity_state_is_neutral(const dualsense_state_t *state);
bool m61_ds5_inactivity_note_report(m61_ds5_inactivity_t *tracker,
                                    const dualsense_state_t *state,
                                    uint32_t now_ms,
                                    uint8_t inactive_minutes);
void m61_ds5_inactivity_retry(m61_ds5_inactivity_t *tracker);

#ifdef __cplusplus
}
#endif
