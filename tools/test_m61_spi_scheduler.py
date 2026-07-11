#!/usr/bin/env python3
"""Source contracts for the BL616 single-owner SPI scheduler."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
M61 = ROOT / "m61" / "dualsense_hidp_probe"
SCHEDULER = M61 / "m61_spi_scheduler.c"
SCHEDULER_HEADER = M61 / "m61_spi_scheduler.h"
TRANSPORT = M61 / "m61_esp32_transport.c"


def main() -> int:
    scheduler = SCHEDULER.read_text(encoding="utf-8")
    header = SCHEDULER_HEADER.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")

    owners = []
    for source in M61.glob("*.c"):
        if "bflb_spi_poll_exchange" in source.read_text(encoding="utf-8"):
            owners.append(source.name)
    assert owners == ["m61_spi_scheduler.c"], owners

    for forbidden in (
        "xSemaphoreTake",
        "xSemaphoreGive",
        "bflb_spi_poll_exchange",
        "static void rx_poll_task",
        "static void time_sync_task",
    ):
        assert forbidden not in transport, forbidden

    for snippet in (
        "DS5_SCHED_M61_RT_REPORT_CAPACITY",
        "DS5_SCHED_M61_RELIABLE_CONTROL_CAPACITY",
        "M61_SPI_CONTROL_MAX_ATTEMPTS 2U",
        "m61_spi_submit_rt_report",
        "m61_spi_publish_state31",
        "m61_spi_publish_state32",
        "m61_spi_submit_control",
        "m61_spi_scheduler_set_generation",
        "GPIO_INT_TRIG_MODE_SYNC_RISING_EDGE",
        "M61_SPI_SCHED_FALLBACK_MS 1U",
        "DS5_SCHED_SLOT_EVICTED",
        "DS5_SCHED_M61_PLANNED_TYPED_STORAGE_BYTES",
    ):
        assert snippet in scheduler or snippet in header, snippet

    # Every wire header built by the scheduler carries deadline_us == 0.
    header_calls = scheduler.count("ds5_dual_spi_header_init(")
    assert header_calls == 4, header_calls
    assert "seq, 0U, snapshot.meta.length" in scheduler
    assert "seq, 0U,\n                             slot->meta.length" in scheduler
    assert "seq,\n                             0U,\n                             slot->meta.length" in scheduler

    rt = scheduler.index("if (take_oldest_rt(&index))")
    irq = scheduler.index("if (take_poll_request() || irq_pin_active())")
    ack_timeout = scheduler.index("if (s_scheduler.ack_wait.active", irq)
    aged_control = scheduler.index("if (take_oldest_control(&index, true))")
    state31 = scheduler.index("state31.control.state", aged_control)
    state32 = scheduler.index("state32.control.state", state31)
    normal_control = scheduler.index("if (take_oldest_control(&index, false))")
    assert rt < irq < ack_timeout < aged_control < state31 < state32 < normal_control

    assert "(void)deadline_tick;" in transport
    assert "m61_spi_submit_rt_report" in transport
    assert "m61_spi_publish_state31" in transport
    assert "m61_spi_publish_state32" in transport
    assert "m61_spi_submit_control" in transport

    print("M61 single-owner SPI scheduler tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
