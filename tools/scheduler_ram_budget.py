#!/usr/bin/env python3
"""Calculate the planned dual-chip scheduler RAM budget.

The inputs are deliberately explicit. Update this file and the scheduler spec
together when a capacity or object layout changes. The script models 8-byte
alignment for shared metadata/storage so it is conservative on both targets.
"""

from __future__ import annotations

from dataclasses import dataclass


def align_up(value: int, alignment: int = 8) -> int:
    return (value + alignment - 1) // alignment * alignment


@dataclass(frozen=True)
class Item:
    name: str
    bytes: int


META_BYTES = 24
SLOT_STATE_BYTES = 8


def slot(payload_bytes: int) -> int:
    return align_up(META_BYTES + SLOT_STATE_BYTES + payload_bytes)


M61_TOTAL_RAM = 415 * 1024
M61_BASELINE_STATIC_USED = 268_800
M61_STATIC_TARGET_LIMIT = M61_TOTAL_RAM * 75 // 100
M61_BASELINE_MIN_HEAP = 154_544
M61_MIN_HEAP_TARGET = 100 * 1024

# These are pre-refactor baseline symbols measured from the BL616 ELF via nm.
# Objects that remain after the refactor are intentionally excluded.
M61_REPLACED_STATIC = (
    Item("haptics_queue", 8 * 64),
    Item("speaker_frame_queue", 4 * 2048),
    Item("speaker_opus_queue", 8 * 200),
    Item("speaker_accum", 2048),
    Item("speaker_input", 2048),
    Item("mic_opus_queue", 2 * 71),
)

M61_PLANNED_STATIC = (
    Item("audio_epochs", 8 * slot(64 + 2048 + 200)),
    Item("complete_rt_reports", 2 * slot(547)),
    Item("state31_mailbox", slot(78)),
    Item("state32_mailbox", slot(142)),
    Item("control_descriptors", 8 * 40),
    Item("control_small_payload_pool", 8 * 160),
    Item("control_large_payload_pool", 2 * 672),
    Item("input_realtime_ring", 4 * slot(78)),
    Item("mic_opus_ring", 4 * slot(71)),
    Item("feature_transactions", 8 * 32),
    Item("link_state_mailbox", 56),
    Item("scheduler_status_mailbox", 64),
    Item("telemetry_snapshot", 512),
    Item("usb_audio_ingress_ring", 4 * align_up(8 + 2 + 2 + 392)),
    Item("usb_control_descriptors", 8 * 40),
    Item("usb_control_payload_pool", 8 * 64),
    Item("scheduler_metrics", 2048),
)

# Steady-state heap delta: the new SPI owner uses about 6 KiB instead of the
# current 4 KiB RX poll task, and a deferred USB control worker adds about 4 KiB.
# TCB/queue bookkeeping is included conservatively.
M61_RUNTIME_HEAP_DELTA = 6_400
M61_STATIC_CONTINGENCY = 8 * 1024
M61_HEAP_CONTINGENCY = 8 * 1024


ESP_DRAM_TOTAL = 124_580
ESP_BASELINE_STATIC_USED = 71_604

# Exact current sizes from the ESP ELF debug types:
# bt_cmd_t=696, send_element_t=696, dual_chip_tx_item_t=688,
# dual_chip_response_item_t=700. Static arrays and runtime FreeRTOS queue
# payload allocations are kept separate.
ESP_REPLACED_STATIC = (
    Item("mixed_l2cap_send_fifo", 10 * 696),
    Item("generic_spi_response_queue", 8 * 700),
)

ESP_REPLACED_RUNTIME_HEAP = (
    Item("normal_command_queue", 12 * 696),
    Item("realtime_command_queue", 2 * 696),
    Item("state31_state32_command_queues", 2 * 696),
    Item("generic_spi_tx_queue", 4 * 688),
)

ESP_PLANNED_STATIC = (
    Item("canonical_rt_slots", 2 * slot(547)),
    Item("state31_mailbox", slot(78)),
    Item("state32_mailbox", slot(142)),
    Item("control_descriptors", 8 * 40),
    Item("control_small_payload_pool", 8 * 160),
    Item("control_large_payload_pool", 2 * 672),
    Item("input_response_ring", 4 * slot(78)),
    Item("mic_response_ring", 4 * slot(71)),
    Item("feature_response_descriptors", 8 * 40),
    Item("feature_small_payload_pool", 8 * 160),
    Item("feature_large_payload_pool", 2 * 672),
    Item("link_state_mailbox", 56),
    Item("scheduler_status_mailbox", 64),
    Item("telemetry_snapshot", 512),
    Item("pending_descriptor", 64),
    Item("scheduler_metrics", 2048),
)

ESP_STATIC_TARGET_LIMIT = ESP_DRAM_TOTAL * 70 // 100
ESP_STATIC_CONTINGENCY = 8 * 1024


def total(items: tuple[Item, ...]) -> int:
    return sum(item.bytes for item in items)


def print_items(title: str, items: tuple[Item, ...]) -> None:
    print(title)
    for item in items:
        print(f"  {item.name:38s} {item.bytes:6d} B")
    print(f"  {'TOTAL':38s} {total(items):6d} B")


def main() -> int:
    print(f"shared metadata size budget: {META_BYTES} B")
    print()

    print_items("M61 replaced static storage:", M61_REPLACED_STATIC)
    print()
    print_items("M61 planned typed static storage:", M61_PLANNED_STATIC)
    m61_projected = (
        M61_BASELINE_STATIC_USED
        - total(M61_REPLACED_STATIC)
        + total(M61_PLANNED_STATIC)
    )
    m61_with_contingency = m61_projected + M61_STATIC_CONTINGENCY
    # BL616 static data and the FreeRTOS heap share OCRAM.  A net increase in
    # the linked static image moves __HeapBase upward by the same amount, so it
    # must be charged to the minimum-heap forecast as well as the static gate.
    m61_static_delta = (
        total(M61_PLANNED_STATIC) - total(M61_REPLACED_STATIC)
    )
    m61_heap_projected = (
        M61_BASELINE_MIN_HEAP
        - M61_RUNTIME_HEAP_DELTA
        - max(0, m61_static_delta)
    )
    m61_heap_with_contingency = (
        m61_heap_projected
        - M61_STATIC_CONTINGENCY
        - M61_HEAP_CONTINGENCY
    )
    print()
    print("M61 forecast:")
    print(f"  baseline static used                {M61_BASELINE_STATIC_USED:6d} B")
    print(f"  projected static used               {m61_projected:6d} B")
    print(f"  projected + 8 KiB contingency       {m61_with_contingency:6d} B")
    print(f"  75% static limit                    {M61_STATIC_TARGET_LIMIT:6d} B")
    print(f"  remaining to limit with contingency {M61_STATIC_TARGET_LIMIT - m61_with_contingency:6d} B")
    print(f"  baseline minimum heap               {M61_BASELINE_MIN_HEAP:6d} B")
    print(f"  linked static growth charged to heap {max(0, m61_static_delta):6d} B")
    print(f"  projected minimum heap              {m61_heap_projected:6d} B")
    print(f"  projected heap - both contingencies {m61_heap_with_contingency:6d} B")
    print(f"  minimum heap target                 {M61_MIN_HEAP_TARGET:6d} B")

    print()
    print_items("ESP32 replaced static storage:", ESP_REPLACED_STATIC)
    print()
    print_items("ESP32 replaced runtime queue payload:", ESP_REPLACED_RUNTIME_HEAP)
    print()
    print_items("ESP32 planned typed static storage:", ESP_PLANNED_STATIC)
    esp_projected = (
        ESP_BASELINE_STATIC_USED
        - total(ESP_REPLACED_STATIC)
        + total(ESP_PLANNED_STATIC)
    )
    esp_with_contingency = esp_projected + ESP_STATIC_CONTINGENCY
    print()
    print("ESP32 forecast:")
    print(f"  baseline static DRAM used            {ESP_BASELINE_STATIC_USED:6d} B")
    print(f"  projected static DRAM used           {esp_projected:6d} B")
    print(f"  projected + 8 KiB contingency        {esp_with_contingency:6d} B")
    print(f"  70% static DRAM limit                {ESP_STATIC_TARGET_LIMIT:6d} B")
    print(f"  runtime queue payload reclaimed      {total(ESP_REPLACED_RUNTIME_HEAP):6d} B")

    failures: list[str] = []
    if total(M61_PLANNED_STATIC) > 32 * 1024:
        failures.append("M61 planned typed storage exceeds 32 KiB")
    if m61_with_contingency > M61_STATIC_TARGET_LIMIT:
        failures.append("M61 static forecast exceeds 75% limit")
    if m61_heap_with_contingency < M61_MIN_HEAP_TARGET:
        failures.append("M61 heap forecast falls below 100 KiB")
    if total(ESP_PLANNED_STATIC) > 16 * 1024:
        failures.append("ESP32 planned typed storage exceeds 16 KiB")
    if esp_with_contingency > ESP_STATIC_TARGET_LIMIT:
        failures.append("ESP32 static forecast exceeds 70% limit")

    if failures:
        print()
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1

    print()
    print("Scheduler RAM budget passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
