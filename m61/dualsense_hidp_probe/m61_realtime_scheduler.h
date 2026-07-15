#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M61_RT_READY_ENCODE        (1UL << 0)
#define M61_RT_READY_DECODE        (1UL << 1)
#define M61_RT_READY_BT_REALTIME   (1UL << 2)
#define M61_RT_READY_USB_MIC_LOW   (1UL << 3)
#define M61_RT_READY_HID_INPUT     (1UL << 4)
#define M61_RT_READY_HID_OUTPUT    (1UL << 5)
#define M61_RT_READY_CONTROL       (1UL << 6)

#define M61_RT_BT_MIN_WINDOW_US    1000U
#define M61_RT_CACHE_GUARD_US      500U

typedef enum {
    M61_RT_ACTION_IDLE = 0,
    M61_RT_ACTION_ENCODE,
    M61_RT_ACTION_DECODE,
    M61_RT_ACTION_BT_WINDOW,
    M61_RT_ACTION_USB_MIC,
    M61_RT_ACTION_CONTROL,
} m61_rt_action_t;

typedef enum {
    M61_RT_CODEC_NONE = 0,
    M61_RT_CODEC_ENCODE,
    M61_RT_CODEC_DECODE,
} m61_rt_codec_phase_t;

typedef struct {
    uint32_t ready;
    uint64_t now_us;
    uint64_t encode_deadline_us;
    uint64_t decode_deadline_us;
    uint32_t encode_estimated_us;
    uint32_t decode_estimated_us;
    uint32_t bt_window_elapsed_us;
    uint8_t encode_depth;
    uint8_t decode_depth;
    m61_rt_codec_phase_t last_codec;
} m61_rt_snapshot_t;

typedef struct {
    m61_rt_action_t action;
    uint32_t ready_consumed;
    uint32_t slack_us;
    bool deadline_forced;
} m61_rt_decision_t;

void m61_realtime_scheduler_select(const m61_rt_snapshot_t *snapshot,
                                   m61_rt_decision_t *decision);

#ifdef __cplusplus
}
#endif
