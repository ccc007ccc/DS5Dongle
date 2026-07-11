#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dual_chip_scheduler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M61_SPI_SCHED_RT_REPORT_MAX 547U
#define M61_SPI_SCHED_STATE31_MAX 78U
#define M61_SPI_SCHED_STATE32_MAX 142U
#define M61_SPI_SCHED_CONTROL_PAYLOAD_MAX 160U

typedef struct {
    uint8_t type;
    uint16_t flags;
    uint8_t channel;
    uint8_t priority;
    const uint8_t *payload;
    uint16_t payload_len;
    uint32_t generation;
    uint64_t created_us;
} m61_spi_control_request_t;

typedef struct {
    uint64_t transactions;
    uint64_t tx_bytes;
    uint64_t rx_frames;
    uint64_t rx_frame_errors;
    uint64_t rx_crc_errors;
    uint64_t rt_accepted;
    uint64_t rt_transmitted;
    uint64_t rt_replaced;
    uint64_t rt_stale;
    uint64_t rt_failed;
    uint64_t state31_accepted;
    uint64_t state31_transmitted;
    uint64_t state31_coalesced;
    uint64_t state32_accepted;
    uint64_t state32_transmitted;
    uint64_t state32_coalesced;
    uint64_t control_accepted;
    uint64_t control_transmitted;
    uint64_t control_rejected;
    uint64_t control_retried;
    uint64_t control_failed;
    uint64_t ack_polls;
    uint64_t irq_notifications;
    uint64_t fallback_wakes;
    uint64_t ready_timeouts;
    uint64_t transfer_errors;
    uint64_t generation_resets;
    uint32_t generation;
    uint16_t last_tx_seq;
    uint8_t last_tx_type;
    int last_error;
} m61_spi_scheduler_stats_t;

typedef void (*m61_spi_scheduler_rx_cb_t)(const uint8_t *frame,
                                          size_t frame_len,
                                          void *ctx);

int m61_spi_scheduler_init(m61_spi_scheduler_rx_cb_t rx_cb, void *rx_cb_ctx);
bool m61_spi_scheduler_ready(void);

int m61_spi_submit_rt_report(const uint8_t *report,
                             size_t len,
                             uint32_t generation,
                             uint64_t created_us);
int m61_spi_publish_state31(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us);
int m61_spi_publish_state32(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us);
int m61_spi_submit_control(const m61_spi_control_request_t *request);

void m61_spi_scheduler_set_generation(uint32_t generation);
void m61_spi_scheduler_notify_irq(void);
void m61_spi_scheduler_get_stats(m61_spi_scheduler_stats_t *stats);
void m61_spi_scheduler_reset_stats(void);

#ifdef __cplusplus
}
#endif
