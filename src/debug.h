//
// Created by Codex on 2026/7/7.
//

#ifndef DS5_BRIDGE_DEBUG_H
#define DS5_BRIDGE_DEBUG_H

#include <cstdint>

#ifndef ENABLE_DEBUG
#define ENABLE_DEBUG 0
#endif

#if ENABLE_DEBUG

void debug_fill_core1_stack_watermark(uint32_t *stack, uint32_t word_count);
void debug_log_core1_stack_usage();

#endif

#endif // DS5_BRIDGE_DEBUG_H
