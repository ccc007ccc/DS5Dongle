#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    int (*request_feature)(void *ctx, uint8_t report_id, uint32_t requested_len);
    int (*set_feature_exact)(void *ctx, uint8_t report_id, const uint8_t *data, size_t len);
} m61_ds5_dse_ops_t;

void m61_ds5_dse_init(const m61_ds5_dse_ops_t *ops);
void m61_ds5_dse_reset(void);
void m61_ds5_dse_note_feature_report(uint8_t report_id, const uint8_t *data, size_t len);
void m61_ds5_dse_note_feature_set(uint8_t report_id);
bool m61_ds5_dse_is_profile_report(uint8_t report_id);
bool m61_ds5_dse_profiles_ready(void);
bool m61_ds5_dse_is_edge(void);
bool m61_ds5_dse_should_nak_profile(uint8_t report_id);
void m61_ds5_dse_task(TickType_t now);

#ifdef __cplusplus
}
#endif
