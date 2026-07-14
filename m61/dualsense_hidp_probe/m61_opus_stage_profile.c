#include "m61_opus_stage_profile.h"

#include <limits.h>
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
static stage_total_t s_totals[M61_OPUS_STAGE_COUNT];
static stage_counter_t s_previous;
static uint32_t s_next_boundary;
static bool s_active;

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

void __attribute__((section(".tcm_code.m61_opus_stage_mark")))
m61_opus_stage_mark(uint32_t boundary)
{
    stage_counter_t current = read_counters();
    stage_total_t *total;

    if (boundary == 0U) {
        s_previous = current;
        s_next_boundary = 1U;
        s_active = true;
        return;
    }
    if (!s_active || boundary != s_next_boundary ||
        boundary > M61_OPUS_STAGE_COUNT) {
        s_active = false;
        return;
    }

    total = &s_totals[boundary - 1U];
    s_sequence++;
    profile_barrier();
    total->samples++;
    total->cycles += current.cycle - s_previous.cycle;
    total->instret += current.instret - s_previous.instret;
    total->icache_access += current.icache_access - s_previous.icache_access;
    total->icache_miss += current.icache_miss - s_previous.icache_miss;
    total->dcache_read += current.dcache_read - s_previous.dcache_read;
    total->dcache_read_miss +=
        current.dcache_read_miss - s_previous.dcache_read_miss;
    profile_barrier();
    s_sequence++;

    s_previous = current;
    s_next_boundary++;
    if (boundary == M61_OPUS_STAGE_COUNT) {
        s_active = false;
    }
}

void m61_opus_stage_profile_reset(void)
{
    uintptr_t flags = bflb_irq_save();

    s_sequence++;
    profile_barrier();
    memset(s_totals, 0, sizeof(s_totals));
    s_active = false;
    s_next_boundary = 0U;
    profile_barrier();
    s_sequence++;
    bflb_irq_restore(flags);
}

void m61_opus_stage_profile_get_snapshot(m61_opus_stage_snapshot_t *snapshot)
{
    stage_total_t totals[M61_OPUS_STAGE_COUNT];
    uint32_t sequence_before;
    uint32_t sequence_after;

    if (!snapshot) {
        return;
    }
    do {
        sequence_before = s_sequence;
        profile_barrier();
        memcpy(totals, s_totals, sizeof(totals));
        profile_barrier();
        sequence_after = s_sequence;
    } while ((sequence_before & 1U) != 0U || sequence_before != sequence_after);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = true;
    for (uint32_t i = 0; i < M61_OPUS_STAGE_COUNT; i++) {
        m61_opus_stage_result_t *result = &snapshot->stages[i];

        result->samples = totals[i].samples;
        result->cycles_average = average_u64(totals[i].cycles,
                                             totals[i].samples);
        result->instret_average = average_u64(totals[i].instret,
                                              totals[i].samples);
        result->icache_access_average = average_u64(totals[i].icache_access,
                                                    totals[i].samples);
        result->icache_miss_average = average_u64(totals[i].icache_miss,
                                                  totals[i].samples);
        result->dcache_read_average = average_u64(totals[i].dcache_read,
                                                  totals[i].samples);
        result->dcache_read_miss_average = average_u64(
            totals[i].dcache_read_miss,
            totals[i].samples);
    }
}
