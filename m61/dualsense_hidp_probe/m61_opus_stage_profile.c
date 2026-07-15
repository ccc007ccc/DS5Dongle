#include "m61_opus_stage_profile.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "bflb_irq.h"

typedef struct {
    uint32_t cycle;
    uint32_t instret;
    uint32_t icache_access;
    uint32_t icache_miss;
    uint32_t dcache_read;
    uint32_t dcache_read_miss;
} stage_counter_t;

typedef struct {
    uint32_t samples;
    uint64_t cycles;
    uint64_t instret;
    uint64_t icache_access;
    uint64_t icache_miss;
    uint64_t dcache_read;
    uint64_t dcache_read_miss;
} stage_total_t;

static volatile uint32_t s_sequence;
static stage_total_t
    s_totals[M61_OPUS_STAGE_KIND_COUNT][M61_OPUS_STAGE_COUNT];
static stage_counter_t s_previous[M61_OPUS_STAGE_KIND_COUNT];
static uint32_t s_next_boundary[M61_OPUS_STAGE_KIND_COUNT];
static bool s_active[M61_OPUS_STAGE_KIND_COUNT];
static uint32_t s_pvq_counts[M61_OPUS_STAGE_KIND_COUNT]
                            [M61_OPUS_PVQ_N_BINS][M61_OPUS_PVQ_K_BINS];

#define READ_COUNTER_LOW(name, value) \
    __asm volatile("csrr %0, " #name : "=r"(value) : : "memory")

static void profile_barrier(void)
{
    __asm volatile("" : : : "memory");
}

static stage_counter_t read_counters(void)
{
    stage_counter_t counter;

    READ_COUNTER_LOW(mcycle, counter.cycle);
    READ_COUNTER_LOW(minstret, counter.instret);
    READ_COUNTER_LOW(mhpmcounter3, counter.icache_access);
    READ_COUNTER_LOW(mhpmcounter4, counter.icache_miss);
    READ_COUNTER_LOW(mhpmcounter14, counter.dcache_read);
    READ_COUNTER_LOW(mhpmcounter15, counter.dcache_read_miss);
    return counter;
}

static uint32_t average_u64(uint64_t total, uint32_t samples)
{
    uint64_t average;

    if (samples == 0U) {
        return 0U;
    }
    average = total / samples;
    return average > UINT32_MAX ? UINT32_MAX : (uint32_t)average;
}

static void __attribute__((section(".tcm_code.m61_opus_stage_mark")))
stage_mark(m61_opus_stage_kind_t kind, uint32_t boundary)
{
    stage_counter_t current = read_counters();
    stage_total_t *total;

    if ((uint32_t)kind >= M61_OPUS_STAGE_KIND_COUNT) {
        return;
    }
    if (boundary == 0U) {
        s_previous[kind] = current;
        s_next_boundary[kind] = 1U;
        s_active[kind] = true;
        return;
    }
    if (!s_active[kind] || boundary != s_next_boundary[kind] ||
        boundary > M61_OPUS_STAGE_COUNT) {
        s_active[kind] = false;
        return;
    }

    total = &s_totals[kind][boundary - 1U];
    s_sequence++;
    profile_barrier();
    total->samples++;
    total->cycles += current.cycle - s_previous[kind].cycle;
    total->instret += current.instret - s_previous[kind].instret;
    total->icache_access +=
        current.icache_access - s_previous[kind].icache_access;
    total->icache_miss += current.icache_miss - s_previous[kind].icache_miss;
    total->dcache_read += current.dcache_read - s_previous[kind].dcache_read;
    total->dcache_read_miss +=
        current.dcache_read_miss - s_previous[kind].dcache_read_miss;
    profile_barrier();
    s_sequence++;

    s_previous[kind] = current;
    s_next_boundary[kind]++;
    if (boundary == M61_OPUS_STAGE_COUNT) {
        s_active[kind] = false;
    }
}

void __attribute__((section(".tcm_code.m61_opus_stage_mark")))
m61_opus_stage_mark(uint32_t boundary)
{
    stage_mark(M61_OPUS_STAGE_KIND_ENCODE, boundary);
}

void __attribute__((section(".tcm_code.m61_opus_stage_mark")))
m61_opus_decode_stage_mark(uint32_t boundary)
{
    stage_mark(M61_OPUS_STAGE_KIND_DECODE, boundary);
}

void __attribute__((section(".tcm_code.m61_opus_stage_mark")))
m61_opus_pvq_shape_mark(uint32_t kind, uint32_t n, uint32_t k)
{
    if (kind >= M61_OPUS_STAGE_KIND_COUNT) {
        return;
    }
    if (n >= M61_OPUS_PVQ_N_BINS) {
        n = M61_OPUS_PVQ_N_BINS - 1U;
    }
    if (k >= M61_OPUS_PVQ_K_BINS) {
        k = M61_OPUS_PVQ_K_BINS - 1U;
    }
    s_pvq_counts[kind][n][k]++;
}

void m61_opus_stage_profile_reset(void)
{
    uintptr_t flags = bflb_irq_save();

    s_sequence++;
    profile_barrier();
    memset(s_totals, 0, sizeof(s_totals));
    memset(s_active, 0, sizeof(s_active));
    memset(s_next_boundary, 0, sizeof(s_next_boundary));
    memset(s_pvq_counts, 0, sizeof(s_pvq_counts));
    profile_barrier();
    s_sequence++;
    bflb_irq_restore(flags);
}

void m61_opus_stage_profile_get_snapshot(m61_opus_stage_snapshot_t *snapshot)
{
    stage_total_t
        totals[M61_OPUS_STAGE_KIND_COUNT][M61_OPUS_STAGE_COUNT];
    uint32_t sequence_before;
    uint32_t sequence_after;

    if (!snapshot) {
        return;
    }
    do {
        sequence_before = s_sequence;
        profile_barrier();
        memcpy(totals, s_totals, sizeof(totals));
        memcpy(snapshot->pvq_counts, s_pvq_counts,
               sizeof(snapshot->pvq_counts));
        profile_barrier();
        sequence_after = s_sequence;
    } while ((sequence_before & 1U) != 0U || sequence_before != sequence_after);

    memset(&snapshot->enabled, 0,
           offsetof(m61_opus_stage_snapshot_t, pvq_counts));
    snapshot->enabled = true;
    for (uint32_t kind = 0; kind < M61_OPUS_STAGE_KIND_COUNT; kind++) {
        for (uint32_t i = 0; i < M61_OPUS_STAGE_COUNT; i++) {
            m61_opus_stage_result_t *result = &snapshot->stages[kind][i];
            const stage_total_t *total = &totals[kind][i];

            result->samples = total->samples;
            result->cycles_average = average_u64(total->cycles,
                                                 total->samples);
            result->instret_average = average_u64(total->instret,
                                                  total->samples);
            result->icache_access_average = average_u64(
                total->icache_access, total->samples);
            result->icache_miss_average = average_u64(
                total->icache_miss, total->samples);
            result->dcache_read_average = average_u64(
                total->dcache_read, total->samples);
            result->dcache_read_miss_average = average_u64(
                total->dcache_read_miss, total->samples);
        }
    }
}
