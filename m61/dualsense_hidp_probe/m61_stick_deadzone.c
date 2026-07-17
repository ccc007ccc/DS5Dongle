#include "m61_stick_deadzone.h"

static uint32_t isqrt_u32(uint32_t value)
{
    uint32_t result = 0U;
    uint32_t bit = 1UL << 30;

    while (bit > value) bit >>= 2;
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static int32_t div_round_signed(int32_t numerator, int32_t denominator)
{
    if (numerator >= 0) return (numerator + denominator / 2) / denominator;
    return (numerator - denominator / 2) / denominator;
}

static int32_t clamp_axis(int32_t value)
{
    if (value < -128) return -128;
    if (value > 127) return 127;
    return value;
}

void m61_stick_deadzone_apply(uint8_t *x, uint8_t *y, uint8_t percent)
{
    int32_t dx;
    int32_t dy;
    int32_t abs_x;
    int32_t abs_y;
    uint32_t magnitude;
    uint32_t deadzone;
    uint32_t max_magnitude = UINT32_MAX;
    uint32_t output_magnitude;

    if (x == 0 || y == 0 || percent == 0U) return;
    if (percent > 30U) percent = 30U;
    dx = (int32_t)*x - 128;
    dy = (int32_t)*y - 128;
    abs_x = dx < 0 ? -dx : dx;
    abs_y = dy < 0 ? -dy : dy;
    magnitude = isqrt_u32((uint32_t)(dx * dx + dy * dy));
    deadzone = ((uint32_t)percent * 128U + 50U) / 100U;
    if (magnitude <= deadzone || magnitude == 0U) {
        *x = 128U;
        *y = 128U;
        return;
    }

    if (abs_x != 0) {
        uint32_t limit = dx < 0 ? 128U : 127U;
        max_magnitude = magnitude * limit / (uint32_t)abs_x;
    }
    if (abs_y != 0) {
        uint32_t limit = dy < 0 ? 128U : 127U;
        uint32_t candidate = magnitude * limit / (uint32_t)abs_y;
        if (candidate < max_magnitude) max_magnitude = candidate;
    }
    if (max_magnitude <= deadzone) {
        *x = 128U;
        *y = 128U;
        return;
    }
    output_magnitude = (magnitude - deadzone) * max_magnitude /
                       (max_magnitude - deadzone);
    dx = div_round_signed(dx * (int32_t)output_magnitude, (int32_t)magnitude);
    dy = div_round_signed(dy * (int32_t)output_magnitude, (int32_t)magnitude);
    *x = (uint8_t)(clamp_axis(dx) + 128);
    *y = (uint8_t)(clamp_axis(dy) + 128);
}
