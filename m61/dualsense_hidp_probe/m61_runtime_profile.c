#include "m61_runtime_profile.h"

#include "FreeRTOS.h"
#include "task.h"

static volatile uint64_t s_wall_cycles;
static volatile uint64_t s_idle_cycles;
static volatile uint64_t s_task_cycles;
static volatile uint32_t s_context_switches;
static volatile void *s_active_task;
static volatile uint64_t s_active_start;
static volatile void *s_idle_task;

static uint64_t read_mcycle(void)
{
#if __riscv_xlen == 32
    uint32_t high;
    uint32_t low;
    uint32_t high_again;

    do {
        __asm volatile("csrr %0, mcycleh" : "=r"(high) : : "memory");
        __asm volatile("csrr %0, mcycle" : "=r"(low) : : "memory");
        __asm volatile("csrr %0, mcycleh" : "=r"(high_again) : : "memory");
    } while (high != high_again);
    return ((uint64_t)high << 32) | low;
#else
    uint64_t value;

    __asm volatile("csrr %0, mcycle" : "=r"(value) : : "memory");
    return value;
#endif
}

static void account_active_until(uint64_t now)
{
    void *task = (void *)s_active_task;
    uint64_t start = s_active_start;
    uint64_t delta;

    if (!task || now < start) {
        return;
    }

    delta = now - start;
    s_wall_cycles += delta;
    if (task == (void *)s_idle_task) {
        s_idle_cycles += delta;
    } else {
        s_task_cycles += delta;
    }
    s_active_start = now;
}

void m61_runtime_profile_task_switched_out(void *task)
{
    uint64_t now = read_mcycle();

    (void)task;
    account_active_until(now);
    s_active_task = NULL;
    s_context_switches++;
}

void m61_runtime_profile_task_switched_in(void *task)
{
    uint64_t now = read_mcycle();

    if (!s_idle_task) {
        s_idle_task = (void *)xTaskGetIdleTaskHandle();
    }
    s_active_task = task;
    s_active_start = now;
}

void m61_runtime_profile_reset(void)
{
    taskENTER_CRITICAL();
    s_wall_cycles = 0U;
    s_idle_cycles = 0U;
    s_task_cycles = 0U;
    s_context_switches = 0U;
    s_active_start = read_mcycle();
    taskEXIT_CRITICAL();
}

void m61_runtime_profile_get_snapshot(m61_runtime_profile_snapshot_t *snapshot)
{
    uint64_t now;
    uint64_t wall;
    uint64_t idle;
    uint64_t task;

    if (!snapshot) {
        return;
    }

    taskENTER_CRITICAL();
    now = read_mcycle();
    account_active_until(now);
    wall = s_wall_cycles;
    idle = s_idle_cycles;
    task = s_task_cycles;
    snapshot->context_switches = s_context_switches;
    taskEXIT_CRITICAL();

    snapshot->wall_cycles = wall;
    snapshot->idle_cycles = idle;
    snapshot->task_cycles = task;
    snapshot->idle_percent_ppm = wall
                                     ? (uint32_t)((idle * 1000000ULL) / wall)
                                     : 0U;
    snapshot->task_percent_ppm = wall
                                     ? (uint32_t)((task * 1000000ULL) / wall)
                                     : 0U;
}
