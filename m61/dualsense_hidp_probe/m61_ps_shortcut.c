#include "m61_ps_shortcut.h"

#include <stddef.h>
#include <string.h>

#define PS_SHORTCUT_RELEASE_DEBOUNCE_MS 50U
#define PS_SHORTCUT_LONG_PRESS_MS 750U

void m61_ps_shortcut_reset(m61_ps_shortcut_t *shortcut)
{
    if (shortcut != NULL) {
        memset(shortcut, 0, sizeof(*shortcut));
    }
}

void m61_ps_shortcut_note(m61_ps_shortcut_t *shortcut,
                          bool raw_ps_pressed,
                          uint32_t now_ms,
                          bool enabled)
{
    if (shortcut == NULL) {
        return;
    }
    if (!enabled) {
        m61_ps_shortcut_reset(shortcut);
        return;
    }

    if (raw_ps_pressed) {
        shortcut->stable_pressed = true;
        shortcut->last_high_time_ms = now_ms;
    } else if ((uint32_t)(now_ms - shortcut->last_high_time_ms) >
               PS_SHORTCUT_RELEASE_DEBOUNCE_MS) {
        shortcut->stable_pressed = false;
    }

    if (shortcut->stable_pressed && !shortcut->was_pressed) {
        shortcut->press_time_ms = now_ms;
        shortcut->was_pressed = true;
        shortcut->long_press_fired = false;
    } else if (shortcut->stable_pressed && shortcut->was_pressed) {
        if (!shortcut->long_press_fired &&
            (uint32_t)(now_ms - shortcut->press_time_ms) >=
                PS_SHORTCUT_LONG_PRESS_MS) {
            shortcut->pending_action = M61_PS_SHORTCUT_WIN_TAB;
            shortcut->long_press_fired = true;
        }
    } else if (!shortcut->stable_pressed && shortcut->was_pressed) {
        if (!shortcut->long_press_fired) {
            shortcut->pending_action = M61_PS_SHORTCUT_WIN_G;
        }
        shortcut->was_pressed = false;
    }
}

m61_ps_shortcut_action_t m61_ps_shortcut_take_action(m61_ps_shortcut_t *shortcut)
{
    m61_ps_shortcut_action_t action;

    if (shortcut == NULL) {
        return M61_PS_SHORTCUT_NONE;
    }
    action = shortcut->pending_action;
    shortcut->pending_action = M61_PS_SHORTCUT_NONE;
    return action;
}
