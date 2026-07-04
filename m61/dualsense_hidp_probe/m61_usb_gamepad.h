#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dualsense_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

void m61_usb_gamepad_init(void);
void m61_usb_gamepad_send_state(const dualsense_state_t *state);
bool m61_usb_gamepad_ready(void);
bool m61_usb_gamepad_configured(void);
bool m61_usb_gamepad_busy(void);
uint32_t m61_usb_gamepad_sent_count(void);
uint32_t m61_usb_gamepad_drop_count(void);

#ifdef __cplusplus
}
#endif
