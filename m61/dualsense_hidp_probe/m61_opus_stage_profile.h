#pragma once

#include <stdbool.h>
#include <stdint.h>

#define M61_OPUS_STAGE_COUNT 7U
#define M61_OPUS_PVQ_N_BINS 32U
#define M61_OPUS_PVQ_K_BINS 32U

typedef enum {
    M61_OPUS_STAGE_KIND_ENCODE = 0,
    M61_OPUS_STAGE_KIND_DECODE,
    M61_OPUS_STAGE_KIND_COUNT,
} m61_opus_stage_kind_t;

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
    m61_opus_stage_result_t
        stages[M61_OPUS_STAGE_KIND_COUNT][M61_OPUS_STAGE_COUNT];
    uint32_t pvq_counts[M61_OPUS_STAGE_KIND_COUNT]
                       [M61_OPUS_PVQ_N_BINS][M61_OPUS_PVQ_K_BINS];
} m61_opus_stage_snapshot_t;

void m61_opus_stage_mark(uint32_t boundary);
void m61_opus_decode_stage_mark(uint32_t boundary);
void m61_opus_pvq_shape_mark(uint32_t kind, uint32_t n, uint32_t k);
void m61_opus_stage_profile_reset(void);
void m61_opus_stage_profile_get_snapshot(m61_opus_stage_snapshot_t *snapshot);
