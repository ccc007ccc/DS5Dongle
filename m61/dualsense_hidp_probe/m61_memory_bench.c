#include "m61_memory_bench.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bflb_l1c.h"
#include "m61_perf_profile.h"

#include "FreeRTOS.h"
#include "task.h"

#define M61_MEMBENCH_4K_BYTES 4096U
#define M61_MEMBENCH_40K_BYTES 40960U
#define M61_MEMBENCH_TRIALS 7U
#define M61_MEMBENCH_SEED 0x6D363142U

typedef uint32_t (*m61_membench_fn_t)(uint32_t seed);

typedef struct {
    uint32_t cycles;
    uint32_t instret;
    uint32_t icache_access;
    uint32_t icache_miss;
    uint32_t checksum;
} m61_membench_result_t;

typedef struct {
    const char *name;
    const uint8_t *xip_start;
    const uint8_t *xip_end;
    uint8_t *ocram;
    uint32_t expected_size;
} m61_membench_case_t;

extern const uint8_t m61_membench_4k_start[];
extern const uint8_t m61_membench_4k_end[];
extern const uint8_t m61_membench_40k_start[];
extern const uint8_t m61_membench_40k_end[];

static uint8_t s_ocram_4k[M61_MEMBENCH_4K_BYTES]
    __attribute__((aligned(32)));
static uint8_t s_ocram_40k[M61_MEMBENCH_40K_BYTES]
    __attribute__((aligned(32)));
static volatile uint32_t s_membench_sink;

static void instruction_sync(void)
{
    __asm volatile("fence.i" : : : "memory");
}

static m61_membench_result_t measure_once(m61_membench_fn_t fn,
                                          uint32_t seed)
{
    m61_perf_counter_sample_t start;
    m61_perf_counter_sample_t end;
    m61_membench_result_t result;

    m61_perf_profile_counter_begin(&start);
    result.checksum = fn(seed);
    m61_perf_profile_counter_begin(&end);
    result.cycles = end.cycle - start.cycle;
    result.instret = end.instret - start.instret;
    result.icache_access = end.icache_access - start.icache_access;
    result.icache_miss = end.icache_miss - start.icache_miss;
    s_membench_sink = result.checksum;
    return result;
}

static uint32_t miss_rate_ppm(const m61_membench_result_t *result)
{
    if (result->icache_access == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)result->icache_miss * 1000000ULL) /
                      result->icache_access);
}

static void print_result(const char *location,
                         const char *state,
                         const m61_membench_result_t *result)
{
    printf("  %-5s %-7s cycles=%lu instret=%lu ic_access=%lu "
           "ic_miss=%lu miss_ppm=%lu checksum=%08lx\r\n",
           location,
           state,
           (unsigned long)result->cycles,
           (unsigned long)result->instret,
           (unsigned long)result->icache_access,
           (unsigned long)result->icache_miss,
           (unsigned long)miss_rate_ppm(result),
           (unsigned long)result->checksum);
}

static int take_best(m61_membench_fn_t fn,
                     uint32_t size,
                     bool cold,
                     uint32_t expected_checksum,
                     m61_membench_result_t *best)
{
    bool have_best = false;

    for (uint32_t trial = 0; trial < M61_MEMBENCH_TRIALS; trial++) {
        m61_membench_result_t current;

        /* A slow XIP traversal otherwise spans Bluetooth/USB interrupts and
         * minstret then measures their handlers rather than identical code.
         * Keep only cache preparation and one timed traversal atomic. */
        taskENTER_CRITICAL();
        if (cold) {
            bflb_l1c_icache_invalid_range((void *)(uintptr_t)fn, size);
            instruction_sync();
        } else {
            s_membench_sink = fn(M61_MEMBENCH_SEED);
        }

        current = measure_once(fn, M61_MEMBENCH_SEED);
        taskEXIT_CRITICAL();
        if (current.checksum != expected_checksum) {
            printf("m61 membench checksum mismatch got=%08lx expected=%08lx\r\n",
                   (unsigned long)current.checksum,
                   (unsigned long)expected_checksum);
            return -EIO;
        }
        if (!have_best || current.cycles < best->cycles) {
            *best = current;
            have_best = true;
        }
    }
    return have_best ? 0 : -EIO;
}

static int run_case(const m61_membench_case_t *test)
{
    uint32_t actual_size = (uint32_t)(test->xip_end - test->xip_start);
    m61_membench_fn_t xip_fn;
    m61_membench_fn_t ocram_fn;
    m61_membench_result_t xip_cold;
    m61_membench_result_t xip_warm;
    m61_membench_result_t ocram_cold;
    m61_membench_result_t ocram_warm;
    uint32_t xip_checksum;
    uint32_t ocram_checksum;
    int err;

    if (actual_size != test->expected_size ||
        ((uintptr_t)test->xip_start & 31U) != 0U ||
        ((uintptr_t)test->ocram & 31U) != 0U) {
        printf("m61 membench invalid layout name=%s size=%lu xip=%p ocram=%p\r\n",
               test->name,
               (unsigned long)actual_size,
               test->xip_start,
               test->ocram);
        return -EINVAL;
    }

    memcpy(test->ocram, test->xip_start, actual_size);
    if (memcmp(test->ocram, test->xip_start, actual_size) != 0) {
        printf("m61 membench byte copy verification failed: %s\r\n",
               test->name);
        return -EIO;
    }
    bflb_l1c_dcache_clean_range(test->ocram, actual_size);
    bflb_l1c_icache_invalid_range(test->ocram, actual_size);
    instruction_sync();

    xip_fn = (m61_membench_fn_t)(uintptr_t)test->xip_start;
    ocram_fn = (m61_membench_fn_t)(uintptr_t)test->ocram;
    xip_checksum = xip_fn(M61_MEMBENCH_SEED);
    ocram_checksum = ocram_fn(M61_MEMBENCH_SEED);
    if (xip_checksum != ocram_checksum) {
        printf("m61 membench copied code differs xip=%08lx ocram=%08lx\r\n",
               (unsigned long)xip_checksum,
               (unsigned long)ocram_checksum);
        return -EIO;
    }

    err = take_best(xip_fn, actual_size, true, xip_checksum, &xip_cold);
    if (!err) {
        err = take_best(ocram_fn, actual_size, true, xip_checksum, &ocram_cold);
    }
    if (!err) {
        err = take_best(xip_fn, actual_size, false, xip_checksum, &xip_warm);
    }
    if (!err) {
        err = take_best(ocram_fn, actual_size, false, xip_checksum, &ocram_warm);
    }
    if (err) {
        return err;
    }

    printf("m61 membench %s bytes=%lu trials=%u xip=%p ocram=%p\r\n",
           test->name,
           (unsigned long)actual_size,
           (unsigned int)M61_MEMBENCH_TRIALS,
           test->xip_start,
           test->ocram);
    print_result("xip", "cold", &xip_cold);
    print_result("ocram", "cold", &ocram_cold);
    print_result("xip", "warm", &xip_warm);
    print_result("ocram", "warm", &ocram_warm);
    printf("  ratio ocram/xip cold=%lu ppm warm=%lu ppm "
           "(lower is faster)\r\n",
           (unsigned long)(((uint64_t)ocram_cold.cycles * 1000000ULL) /
                           xip_cold.cycles),
           (unsigned long)(((uint64_t)ocram_warm.cycles * 1000000ULL) /
                           xip_warm.cycles));
    return 0;
}

int m61_memory_bench_run(const char *selection)
{
    const m61_membench_case_t tests[] = {
        {
            .name = "4k",
            .xip_start = m61_membench_4k_start,
            .xip_end = m61_membench_4k_end,
            .ocram = s_ocram_4k,
            .expected_size = M61_MEMBENCH_4K_BYTES,
        },
        {
            .name = "40k",
            .xip_start = m61_membench_40k_start,
            .xip_end = m61_membench_40k_end,
            .ocram = s_ocram_40k,
            .expected_size = M61_MEMBENCH_40K_BYTES,
        },
    };
    bool matched = false;

    if (!selection || selection[0] == '\0') {
        selection = "all";
    }
    if (!m61_perf_profile_is_enabled()) {
        /* USB/audio normally initializes HPM after the first controller full
         * report.  Permit this standalone test before a controller connects
         * without resetting live codec statistics when it is already active. */
        m61_perf_profile_init();
    }
    if (!m61_perf_profile_is_enabled()) {
        printf("m61 membench requires CONFIG_M61_HPM_PROFILE\r\n");
        return -ENOTSUP;
    }
    printf("m61 membench: identical bytes, best-of-%u; cold invalidates "
           "target range, warm pre-runs once\r\n",
           (unsigned int)M61_MEMBENCH_TRIALS);

    for (uint32_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int err;

        if (strcmp(selection, "all") != 0 &&
            strcmp(selection, tests[i].name) != 0) {
            continue;
        }
        matched = true;
        err = run_case(&tests[i]);
        if (err) {
            return err;
        }
    }

    if (!matched) {
        printf("usage: m61 membench [all|4k|40k]\r\n");
        return -EINVAL;
    }
    return 0;
}
