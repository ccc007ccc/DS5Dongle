#pragma once

#include <stdbool.h>
#include <stdint.h>

#define M61_OPUS_STAGE_COUNT 7U

typedef struct {
    uint32_t samples;
    uint32_t cycles_average;
    uint32_t instret_average;
    uint32_t icache_access_average;
    uint32_t icache_miss_average;
    uint32_t dcache_read_average;
    uint32_t dcache_read_miss_average;
} m61_opus_stage_result_t;

typedef struct {
    bool enabled;
    m61_opus_stage_result_t stages[M61_OPUS_STAGE_COUNT];
} m61_opus_stage_snapshot_t;

void m61_opus_stage_mark(uint32_t boundary);
void m61_opus_stage_profile_reset(void);
void m61_opus_stage_profile_get_snapshot(m61_opus_stage_snapshot_t *snapshot);
