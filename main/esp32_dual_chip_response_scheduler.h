#pragma once

#include "dual_chip_scheduler_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t storage_class;
    uint8_t slot;
    uint16_t reserved;
    uint32_t version;
} esp32_dual_response_token_t;

typedef void (*esp32_dual_response_irq_cb_t)(bool asserted, void *ctx);

void esp32_dual_response_scheduler_init(esp32_dual_response_irq_cb_t irq_cb,
                                        void *irq_ctx);
bool esp32_dual_response_publish(uint8_t type,
                                 uint16_t flags,
                                 uint8_t channel,
                                 uint8_t priority,
                                 uint16_t wire_sequence,
                                 uint32_t timestamp_us,
                                 const uint8_t *payload,
                                 size_t payload_len);
bool esp32_dual_response_stage(uint8_t *frame,
                               size_t frame_capacity,
                               size_t *frame_len,
                               esp32_dual_response_token_t *token);
void esp32_dual_response_finish(const esp32_dual_response_token_t *token,
                                bool success);
void esp32_dual_response_reset_generation(uint32_t generation);
void esp32_dual_response_reset_metrics(void);
uint8_t esp32_dual_response_pending_count(void);
void esp32_dual_response_get_metrics(ds5_sched_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
