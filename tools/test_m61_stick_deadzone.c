#include <assert.h>
#include <stdint.h>

#include "m61_stick_deadzone.h"

int main(void)
{
    uint8_t x;
    uint8_t y;

    x = 128U; y = 128U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x == 128U && y == 128U);
    x = 138U; y = 128U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x == 128U && y == 128U);
    x = 150U; y = 128U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x > 128U && x < 150U && y == 128U);
    x = 255U; y = 128U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x == 255U && y == 128U);
    x = 0U; y = 128U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x == 0U && y == 128U);
    x = 255U; y = 255U; m61_stick_deadzone_apply(&x, &y, 10U);
    assert(x == 255U && y == 255U);
    x = 150U; y = 150U; m61_stick_deadzone_apply(&x, &y, 0U);
    assert(x == 150U && y == 150U);
    return 0;
}
