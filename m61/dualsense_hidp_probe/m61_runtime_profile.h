#ifndef M61_RUNTIME_PROFILE_H
#define M61_RUNTIME_PROFILE_H

#include <stdint.h>

typedef struct {
    uint64_t wall_cycles;
    uint64_t idle_cycles;
    uint64_t task_cycles;
    uint32_t context_switches;
    uint32_t idle_percent_ppm;
    uint32_t task_percent_ppm;
} m61_runtime_profile_snapshot_t;

#if defined(CONFIG_M61_RUNTIME_PROFILE) && CONFIG_M61_RUNTIME_PROFILE
void m61_runtime_profile_reset(void);
void m61_runtime_profile_get_snapshot(m61_runtime_profile_snapshot_t *snapshot);
void m61_runtime_profile_task_switched_in(void *task);
void m61_runtime_profile_task_switched_out(void *task);
#endif

#endif
