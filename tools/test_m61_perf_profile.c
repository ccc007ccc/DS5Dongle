#include <assert.h>
#include <stdio.h>

#include "m61_perf_profile.h"

int main(void)
{
    m61_perf_profile_snapshot_t snapshot;

    m61_perf_profile_init();
    for (uint32_t i = 0; i < 100U; i++) {
        uint32_t elapsed_us = i < 50U ? 7000U :
                              i < 95U ? 9000U : 12000U;
        m61_perf_profile_record_encode(elapsed_us,
                                       elapsed_us * 320U,
                                       1000U,
                                       2000U,
                                       20U,
                                       3000U,
                                       60U);
        m61_perf_profile_record_ingress_age(i < 99U ? 1000U : 5000U);
    }
    m61_perf_profile_record_irq_mask_cycles(3200U);
    m61_perf_profile_record_irq_mask_cycles(1600U);
    m61_perf_profile_get_snapshot(&snapshot);

    assert(!snapshot.enabled);
    assert(snapshot.encode_samples == 100U);
    assert(snapshot.encode_us_average == 8150U);
    assert(snapshot.encode_us_p50 == 7250U);
    assert(snapshot.encode_us_p95 == 9250U);
    assert(snapshot.encode_us_p99 == 12250U);
    assert(snapshot.cycles_average == 2608000U);
    assert(snapshot.icache_miss_ppm == 10000U);
    assert(snapshot.dcache_read_miss_ppm == 20000U);
    assert(snapshot.ingress_age_us_p95 == 1250U);
    assert(snapshot.ingress_age_us_p99 == 1250U);
    assert(snapshot.ingress_age_us_max == 5000U);
    assert(snapshot.irq_mask_cycles_max == 3200U);
    puts("M61 performance profile tests passed.");
    return 0;
}
