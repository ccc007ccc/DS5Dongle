#include "m61_perf_profile.h"

#include <limits.h>
#include <string.h>

#if CONFIG_M61_HPM_PROFILE
#include <arch/risc-v/t-head/rv_hpm.h>
#endif

typedef struct {
    volatile uint32_t sequence;
    uint32_t samples;
    uint32_t last_us;
    uint32_t max_us;
    uint64_t total_us;
    uint64_t total_cycles;
    uint32_t last_cycles;
    uint32_t max_cycles;
    uint64_t total_instret;
    uint64_t total_icache_access;
    uint64_t total_icache_miss;
    uint64_t total_dcache_read;
    uint64_t total_dcache_read_miss;
    uint32_t histogram[M61_PERF_HISTOGRAM_BUCKETS + 1U];
} encode_profile_t;

typedef struct {
    volatile uint32_t sequence;
    uint32_t samples;
    uint32_t last_us;
    uint32_t max_us;
    uint32_t histogram[M61_PERF_HISTOGRAM_BUCKETS + 1U];
} ingress_profile_t;

static encode_profile_t s_encode;
static encode_profile_t s_decode;
static ingress_profile_t s_ingress;
static volatile uint32_t s_irq_mask_cycles_max;
static bool s_enabled;

static void profile_barrier(void)
{
    __asm volatile("" : : : "memory");
}

static uint32_t saturate_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t histogram_bucket(uint32_t elapsed_us)
{
    uint32_t bucket = elapsed_us / M61_PERF_HISTOGRAM_BUCKET_US;

    return bucket < M61_PERF_HISTOGRAM_BUCKETS
               ? bucket
               : M61_PERF_HISTOGRAM_BUCKETS;
}

static uint32_t histogram_percentile(
    const uint32_t histogram[M61_PERF_HISTOGRAM_BUCKETS + 1U],
    uint32_t samples,
    uint32_t percentile)
{
    uint64_t cumulative = 0;
    uint64_t target;

    if (samples == 0U) {
        return 0U;
    }
    target = ((uint64_t)samples * percentile + 99U) / 100U;
    for (uint32_t i = 0; i <= M61_PERF_HISTOGRAM_BUCKETS; i++) {
        cumulative += histogram[i];
        if (cumulative >= target) {
            if (i == M61_PERF_HISTOGRAM_BUCKETS) {
                return M61_PERF_HISTOGRAM_BUCKETS *
                       M61_PERF_HISTOGRAM_BUCKET_US;
            }
            return (i + 1U) * M61_PERF_HISTOGRAM_BUCKET_US;
        }
    }
    return M61_PERF_HISTOGRAM_BUCKETS * M61_PERF_HISTOGRAM_BUCKET_US;
}

static uint32_t rate_ppm(uint64_t events, uint64_t opportunities)
{
    if (opportunities == 0U) {
        return 0U;
    }
    return saturate_u32((events * 1000000ULL) / opportunities);
}

#if CONFIG_M61_HPM_PROFILE
#define READ_COUNTER_LOW(name, value) \
    __asm volatile("csrr %0, " #name : "=r"(value) : : "memory")

static uint32_t read_mcountinhibit(void)
{
    uint32_t value;

    __asm volatile("csrr %0, mcountinhibit" : "=r"(value) : : "memory");
    return value;
}
#endif

void m61_perf_profile_init(void)
{
    memset(&s_encode, 0, sizeof(s_encode));
    memset(&s_decode, 0, sizeof(s_decode));
    memset(&s_ingress, 0, sizeof(s_ingress));
    s_irq_mask_cycles_max = 0U;
    s_enabled = false;

#if CONFIG_M61_HPM_PROFILE
    const uint32_t hpm_mask = (1UL << 3) | (1UL << 4) |
                              (1UL << 14) | (1UL << 15);
    const uint32_t required_mask = hpm_mask | (1UL << 0) | (1UL << 2);
    uint32_t inhibit = read_mcountinhibit();

    RV_HPM_Set_mcountinhibit(inhibit | hpm_mask);
    RV_HPM_L1_ICache_Miss_Init_M();
    RV_HPM_L1_DCache_RdMiss_Init_M();
    RV_HPM_Set_mcountinhibit(inhibit & ~required_mask);
    s_enabled = true;
#endif
}

bool m61_perf_profile_is_enabled(void)
{
    return s_enabled;
}

void m61_perf_profile_counter_begin(m61_perf_counter_sample_t *sample)
{
    if (!sample) {
        return;
    }
    memset(sample, 0, sizeof(*sample));

#if CONFIG_M61_HPM_PROFILE
    if (!s_enabled) {
        return;
    }
    READ_COUNTER_LOW(mcycle, sample->cycle);
    READ_COUNTER_LOW(minstret, sample->instret);
    READ_COUNTER_LOW(mhpmcounter3, sample->icache_access);
    READ_COUNTER_LOW(mhpmcounter4, sample->icache_miss);
    READ_COUNTER_LOW(mhpmcounter14, sample->dcache_read);
    READ_COUNTER_LOW(mhpmcounter15, sample->dcache_read_miss);
#endif
}

void m61_perf_profile_counter_end(const m61_perf_counter_sample_t *start,
                                  uint32_t elapsed_us)
{
#if CONFIG_M61_HPM_PROFILE
    m61_perf_counter_sample_t end;

    if (!start || !s_enabled) {
        return;
    }
    READ_COUNTER_LOW(mcycle, end.cycle);
    READ_COUNTER_LOW(minstret, end.instret);
    READ_COUNTER_LOW(mhpmcounter3, end.icache_access);
    READ_COUNTER_LOW(mhpmcounter4, end.icache_miss);
    READ_COUNTER_LOW(mhpmcounter14, end.dcache_read);
    READ_COUNTER_LOW(mhpmcounter15, end.dcache_read_miss);
    m61_perf_profile_record_encode(elapsed_us,
                                   end.cycle - start->cycle,
                                   end.instret - start->instret,
                                   end.icache_access - start->icache_access,
                                   end.icache_miss - start->icache_miss,
                                   end.dcache_read - start->dcache_read,
                                   end.dcache_read_miss -
                                       start->dcache_read_miss);
#else
    (void)start;
    (void)elapsed_us;
#endif
}

void m61_perf_profile_counter_end_decode(const m61_perf_counter_sample_t *start,
                                         uint32_t elapsed_us)
{
#if CONFIG_M61_HPM_PROFILE
    m61_perf_counter_sample_t end;

    if (!start || !s_enabled) {
        return;
    }
    READ_COUNTER_LOW(mcycle, end.cycle);
    READ_COUNTER_LOW(minstret, end.instret);
    READ_COUNTER_LOW(mhpmcounter3, end.icache_access);
    READ_COUNTER_LOW(mhpmcounter4, end.icache_miss);
    READ_COUNTER_LOW(mhpmcounter14, end.dcache_read);
    READ_COUNTER_LOW(mhpmcounter15, end.dcache_read_miss);
    m61_perf_profile_record_decode(elapsed_us,
                                   end.cycle - start->cycle,
                                   end.instret - start->instret,
                                   end.icache_access - start->icache_access,
                                   end.icache_miss - start->icache_miss,
                                   end.dcache_read - start->dcache_read,
                                   end.dcache_read_miss -
                                       start->dcache_read_miss);
#else
    (void)start;
    (void)elapsed_us;
#endif
}

static void record_codec_profile(encode_profile_t *profile,
                                 uint32_t elapsed_us,
                                 uint32_t cycles,
                                 uint32_t instret,
                                 uint32_t icache_access,
                                 uint32_t icache_miss,
                                 uint32_t dcache_read,
                                 uint32_t dcache_read_miss)
{
    profile->sequence++;
    profile_barrier();
    profile->samples++;
    profile->last_us = elapsed_us;
    profile->total_us += elapsed_us;
    if (elapsed_us > profile->max_us) {
        profile->max_us = elapsed_us;
    }
    profile->histogram[histogram_bucket(elapsed_us)]++;
    profile->last_cycles = cycles;
    profile->total_cycles += cycles;
    if (cycles > profile->max_cycles) {
        profile->max_cycles = cycles;
    }
    profile->total_instret += instret;
    profile->total_icache_access += icache_access;
    profile->total_icache_miss += icache_miss;
    profile->total_dcache_read += dcache_read;
    profile->total_dcache_read_miss += dcache_read_miss;
    profile_barrier();
    profile->sequence++;
}

void m61_perf_profile_record_encode(uint32_t elapsed_us,
                                    uint32_t cycles,
                                    uint32_t instret,
                                    uint32_t icache_access,
                                    uint32_t icache_miss,
                                    uint32_t dcache_read,
                                    uint32_t dcache_read_miss)
{
    record_codec_profile(&s_encode, elapsed_us, cycles, instret,
                         icache_access, icache_miss,
                         dcache_read, dcache_read_miss);
}

void m61_perf_profile_record_decode(uint32_t elapsed_us,
                                    uint32_t cycles,
                                    uint32_t instret,
                                    uint32_t icache_access,
                                    uint32_t icache_miss,
                                    uint32_t dcache_read,
                                    uint32_t dcache_read_miss)
{
    record_codec_profile(&s_decode, elapsed_us, cycles, instret,
                         icache_access, icache_miss,
                         dcache_read, dcache_read_miss);
}

void m61_perf_profile_record_ingress_age(uint32_t age_us)
{
    s_ingress.sequence++;
    profile_barrier();
    s_ingress.samples++;
    s_ingress.last_us = age_us;
    if (age_us > s_ingress.max_us) {
        s_ingress.max_us = age_us;
    }
    s_ingress.histogram[histogram_bucket(age_us)]++;
    profile_barrier();
    s_ingress.sequence++;
}

void m61_perf_profile_record_irq_mask_cycles(uint32_t cycles)
{
    if (cycles > s_irq_mask_cycles_max) {
        s_irq_mask_cycles_max = cycles;
    }
}

void m61_perf_profile_get_snapshot(m61_perf_profile_snapshot_t *snapshot)
{
    encode_profile_t encode;
    encode_profile_t decode;
    ingress_profile_t ingress;
    uint32_t sequence;

    if (!snapshot) {
        return;
    }
    do {
        sequence = s_encode.sequence;
        if (sequence & 1U) {
            continue;
        }
        profile_barrier();
        encode = s_encode;
        profile_barrier();
    } while (sequence != s_encode.sequence || (sequence & 1U));
    do {
        sequence = s_decode.sequence;
        if (sequence & 1U) {
            continue;
        }
        profile_barrier();
        decode = s_decode;
        profile_barrier();
    } while (sequence != s_decode.sequence || (sequence & 1U));
    do {
        sequence = s_ingress.sequence;
        if (sequence & 1U) {
            continue;
        }
        profile_barrier();
        ingress = s_ingress;
        profile_barrier();
    } while (sequence != s_ingress.sequence || (sequence & 1U));

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->enabled = s_enabled;
    snapshot->encode_samples = encode.samples;
    snapshot->encode_us_last = encode.last_us;
    snapshot->encode_us_max = encode.max_us;
    snapshot->cycles_last = encode.last_cycles;
    snapshot->cycles_max = encode.max_cycles;
    if (encode.samples != 0U) {
        snapshot->encode_us_average =
            saturate_u32(encode.total_us / encode.samples);
        snapshot->cycles_average =
            saturate_u32(encode.total_cycles / encode.samples);
        snapshot->instret_average =
            saturate_u32(encode.total_instret / encode.samples);
        snapshot->icache_access_average =
            saturate_u32(encode.total_icache_access / encode.samples);
        snapshot->icache_miss_average =
            saturate_u32(encode.total_icache_miss / encode.samples);
        snapshot->dcache_read_average =
            saturate_u32(encode.total_dcache_read / encode.samples);
        snapshot->dcache_read_miss_average =
            saturate_u32(encode.total_dcache_read_miss / encode.samples);
    }
    snapshot->icache_miss_ppm = rate_ppm(encode.total_icache_miss,
                                         encode.total_icache_access);
    snapshot->dcache_read_miss_ppm = rate_ppm(
        encode.total_dcache_read_miss, encode.total_dcache_read);
    snapshot->decode_samples = decode.samples;
    snapshot->decode_us_last = decode.last_us;
    snapshot->decode_us_max = decode.max_us;
    snapshot->decode_cycles_last = decode.last_cycles;
    snapshot->decode_cycles_max = decode.max_cycles;
    if (decode.samples != 0U) {
        snapshot->decode_us_average =
            saturate_u32(decode.total_us / decode.samples);
        snapshot->decode_cycles_average =
            saturate_u32(decode.total_cycles / decode.samples);
        snapshot->decode_instret_average =
            saturate_u32(decode.total_instret / decode.samples);
        snapshot->decode_icache_access_average =
            saturate_u32(decode.total_icache_access / decode.samples);
        snapshot->decode_icache_miss_average =
            saturate_u32(decode.total_icache_miss / decode.samples);
        snapshot->decode_dcache_read_average =
            saturate_u32(decode.total_dcache_read / decode.samples);
        snapshot->decode_dcache_read_miss_average =
            saturate_u32(decode.total_dcache_read_miss / decode.samples);
    }
    snapshot->decode_icache_miss_ppm = rate_ppm(
        decode.total_icache_miss, decode.total_icache_access);
    snapshot->decode_dcache_read_miss_ppm = rate_ppm(
        decode.total_dcache_read_miss, decode.total_dcache_read);
    snapshot->encode_us_p50 = histogram_percentile(
        encode.histogram, encode.samples, 50U);
    snapshot->encode_us_p95 = histogram_percentile(
        encode.histogram, encode.samples, 95U);
    snapshot->encode_us_p99 = histogram_percentile(
        encode.histogram, encode.samples, 99U);
    snapshot->decode_us_p50 = histogram_percentile(
        decode.histogram, decode.samples, 50U);
    snapshot->decode_us_p95 = histogram_percentile(
        decode.histogram, decode.samples, 95U);
    snapshot->decode_us_p99 = histogram_percentile(
        decode.histogram, decode.samples, 99U);
    snapshot->ingress_samples = ingress.samples;
    snapshot->ingress_age_us_last = ingress.last_us;
    snapshot->ingress_age_us_max = ingress.max_us;
    snapshot->ingress_age_us_p95 = histogram_percentile(
        ingress.histogram, ingress.samples, 95U);
    snapshot->ingress_age_us_p99 = histogram_percentile(
        ingress.histogram, ingress.samples, 99U);
    snapshot->irq_mask_cycles_max = s_irq_mask_cycles_max;
}
