#ifndef M61_STICK_DEADZONE_H
#define M61_STICK_DEADZONE_H

#include <stdint.h>

void m61_stick_deadzone_apply(uint8_t *x, uint8_t *y, uint8_t percent);

#endif
