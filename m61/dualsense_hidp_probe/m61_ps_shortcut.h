#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    M61_PS_SHORTCUT_NONE = 0,
    M61_PS_SHORTCUT_WIN_G,
    M61_PS_SHORTCUT_WIN_TAB,
} m61_ps_shortcut_action_t;

typedef struct {
    uint32_t press_time_ms;
    uint32_t last_high_time_ms;
    m61_ps_shortcut_action_t pending_action;
    bool stable_pressed;
    bool was_pressed;
    bool long_press_fired;
} m61_ps_shortcut_t;

void m61_ps_shortcut_reset(m61_ps_shortcut_t *shortcut);
void m61_ps_shortcut_note(m61_ps_shortcut_t *shortcut,
                          bool raw_ps_pressed,
                          uint32_t now_ms,
                          bool enabled);
m61_ps_shortcut_action_t m61_ps_shortcut_take_action(m61_ps_shortcut_t *shortcut);

#ifdef __cplusplus
}
#endif
