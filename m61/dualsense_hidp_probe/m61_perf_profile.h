#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef CONFIG_M61_HPM_PROFILE
#define CONFIG_M61_HPM_PROFILE 0
#endif

#ifndef CONFIG_M61_PIPELINE_PROFILE
#define CONFIG_M61_PIPELINE_PROFILE 0
#endif

#ifndef CONFIG_M61_HPM_SAMPLE_SHIFT
#define CONFIG_M61_HPM_SAMPLE_SHIFT 4
#endif

#define M61_PERF_HISTOGRAM_BUCKET_US 250U
#define M61_PERF_HISTOGRAM_BUCKETS 64U

typedef enum {
    M61_PERF_TIMING_INGRESS_AGE = 0,
    M61_PERF_TIMING_INGRESS_WORK,
    M61_PERF_TIMING_RESAMPLE,
    M61_PERF_TIMING_BT_ALLOC,
    M61_PERF_TIMING_REPORT_BUILD,
    M61_PERF_TIMING_BT_SEND_CALL,
    M61_PERF_TIMING_BT_TOTAL,
    M61_PERF_TIMING_PAIR_AGE,
    M61_PERF_TIMING_REPORT_INTERVAL,
    M61_PERF_TIMING_COUNT,
} m61_perf_timing_stage_t;

typedef struct {
    uint32_t samples;
    uint32_t last_us;
    uint32_t average_us;
    uint32_t max_us;
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t p99_us;
} m61_perf_timing_snapshot_t;

typedef struct {
    uint32_t cycle;
    uint32_t instret;
    uint32_t icache_access;
    uint32_t icache_miss;
    uint32_t dcache_read;
    uint32_t dcache_read_miss;
} m61_perf_counter_sample_t;

typedef struct {
    bool enabled;
    uint32_t encode_samples;
    uint32_t encode_us_last;
    uint32_t encode_us_average;
    uint32_t encode_us_max;
    uint32_t encode_us_p50;
    uint32_t encode_us_p95;
    uint32_t encode_us_p99;
    uint32_t cycles_last;
    uint32_t cycles_average;
    uint32_t cycles_max;
    uint32_t instret_average;
    uint32_t icache_access_average;
    uint32_t icache_miss_average;
    uint32_t icache_miss_ppm;
    uint32_t dcache_read_average;
    uint32_t dcache_read_miss_average;
    uint32_t dcache_read_miss_ppm;
    uint32_t decode_samples;
    uint32_t decode_us_last;
    uint32_t decode_us_average;
    uint32_t decode_us_max;
    uint32_t decode_us_p50;
    uint32_t decode_us_p95;
    uint32_t decode_us_p99;
    uint32_t decode_cycles_last;
    uint32_t decode_cycles_average;
    uint32_t decode_cycles_max;
    uint32_t decode_instret_average;
    uint32_t decode_icache_access_average;
    uint32_t decode_icache_miss_average;
    uint32_t decode_icache_miss_ppm;
    uint32_t decode_dcache_read_average;
    uint32_t decode_dcache_read_miss_average;
    uint32_t decode_dcache_read_miss_ppm;
    uint32_t ingress_samples;
    uint32_t ingress_age_us_last;
    uint32_t ingress_age_us_max;
    uint32_t ingress_age_us_p95;
    uint32_t ingress_age_us_p99;
    uint32_t irq_mask_cycles_max;
    m61_perf_timing_snapshot_t timing[M61_PERF_TIMING_COUNT];
} m61_perf_profile_snapshot_t;

typedef struct {
    uint64_t encode_total_us;
    uint64_t encode_total_cycles;
    uint64_t encode_total_instret;
    uint64_t decode_total_us;
    uint64_t decode_total_cycles;
    uint64_t decode_total_instret;
    uint32_t encode_histogram_samples;
    uint32_t decode_histogram_samples;
    bool encode_consistent;
    bool decode_consistent;
} m61_perf_raw_snapshot_t;

void m61_perf_profile_init(void);
bool m61_perf_profile_is_enabled(void);
void m61_perf_profile_counter_begin(m61_perf_counter_sample_t *sample);
void m61_perf_profile_counter_end(const m61_perf_counter_sample_t *start,
                                  uint32_t elapsed_us);
void m61_perf_profile_counter_end_decode(const m61_perf_counter_sample_t *start,
                                         uint32_t elapsed_us);
void m61_perf_profile_record_encode(uint32_t elapsed_us,
                                    uint32_t cycles,
                                    uint32_t instret,
                                    uint32_t icache_access,
                                    uint32_t icache_miss,
                                    uint32_t dcache_read,
                                    uint32_t dcache_read_miss);
void m61_perf_profile_record_decode(uint32_t elapsed_us,
                                    uint32_t cycles,
                                    uint32_t instret,
                                    uint32_t icache_access,
                                    uint32_t icache_miss,
                                    uint32_t dcache_read,
                                    uint32_t dcache_read_miss);
void m61_perf_profile_record_ingress_age(uint32_t age_us);
void m61_perf_profile_record_timing(m61_perf_timing_stage_t stage,
                                    uint32_t elapsed_us);
void m61_perf_profile_record_irq_mask_cycles(uint32_t cycles);
void m61_perf_profile_get_snapshot(m61_perf_profile_snapshot_t *snapshot);
void m61_perf_profile_get_raw_snapshot(m61_perf_raw_snapshot_t *snapshot);
