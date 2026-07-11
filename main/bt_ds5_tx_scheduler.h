#pragma once

#include "dual_chip_scheduler_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bt_ds5_tx_finalize_report_cb_t)(uint8_t *report, size_t len);
typedef void (*bt_ds5_tx_note_result_cb_t)(const uint8_t *packet,
                                           size_t len,
                                           int status);

void bt_ds5_tx_scheduler_init(bt_ds5_tx_finalize_report_cb_t finalize_report,
                              bt_ds5_tx_note_result_cb_t note_result);
void bt_ds5_tx_scheduler_set_interrupt_channel(uint16_t cid);
int bt_ds5_tx_scheduler_submit(const uint8_t *report,
                               size_t report_len,
                               bool realtime,
                               uint64_t created_us,
                               uint32_t generation);
void bt_ds5_tx_scheduler_handle_can_send_now(void);
void bt_ds5_tx_scheduler_reset_generation(uint32_t generation);
void bt_ds5_tx_scheduler_reset_metrics(void);
uint32_t bt_ds5_tx_scheduler_generation(void);
void bt_ds5_tx_scheduler_get_metrics(ds5_sched_metrics_t *metrics);
uint8_t bt_ds5_tx_scheduler_pending_count(void);
uint8_t bt_ds5_tx_scheduler_free_count(void);

#ifdef __cplusplus
}
#endif
