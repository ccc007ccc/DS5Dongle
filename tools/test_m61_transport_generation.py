#!/usr/bin/env python3
"""Focused contracts for M61 peer recovery and connection generations."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = ROOT / "m61" / "dualsense_hidp_probe" / "m61_esp32_transport.c"
SCHEDULER = ROOT / "m61" / "dualsense_hidp_probe" / "m61_spi_scheduler.c"
MAIN = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"
USB = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"


def main() -> int:
    transport = TRANSPORT.read_text(encoding="utf-8")
    scheduler = SCHEDULER.read_text(encoding="utf-8")
    bridge = MAIN.read_text(encoding="utf-8")
    usb = USB.read_text(encoding="utf-8")

    for snippet in (
        "static void invalidate_peer_generation(void)",
        "uintptr_t irq_flags = bflb_irq_save();",
        "s_transport.stats.rx_hello = 0;",
        "s_transport.stats.peer_bt_flags = 0;",
        "s_transport.stats.last_credit_bt_ready = 0;",
        "memset(s_transport.stats.peer_bt_bda, 0,",
        "state_cb(0, empty_bda, 0, generation, state_cb_ctx);",
        "sequence_delta != 0U",
        "sequence_delta != 1U",
        "s_transport.hello_response_expected = true;",
        "s_transport.stats.rx_hello > 0U && !expected_response",
        "s_transport.hello_response_expected = false;",
        "if (!peer_link_ready())",
        "m61_spi_scheduler_set_generation(generation);",
        "m61_spi_scheduler_set_generation(s_transport.peer_generation);",
    ):
        assert snippet in transport, f"missing transport generation contract: {snippet}"

    send_hello = transport.index("static int send_hello(void)")
    hello_expected = transport.index(
        "s_transport.hello_response_expected = true;", send_hello
    )
    hello_send = transport.index(
        "send_payload(DS5_DUAL_MSG_HELLO", hello_expected
    )
    assert hello_expected < hello_send

    for snippet in (
        "void m61_spi_scheduler_set_generation(uint32_t generation)",
        "DS5_SCHED_SLOT_EVICTED",
        "s_scheduler.store.state31.control.state = DS5_SCHED_SLOT_FREE;",
        "s_scheduler.store.state32.control.state = DS5_SCHED_SLOT_FREE;",
        "memset(&s_scheduler.ack_wait, 0, sizeof(s_scheduler.ack_wait));",
        "s_scheduler.stats.generation_resets++;",
    ):
        assert snippet in scheduler, f"missing scheduler generation contract: {snippet}"

    for snippet in (
        "generation != dual_chip_bt_generation",
        "m61_ds5_dse_reset();",
        "m61_usb_gamepad_reset_feature_cache();",
        "m61_usb_gamepad_reset_transport_queues();",
        "hidp_usb_output_pending = false;",
        "observed_bt_generation != dual_chip_bt_generation",
    ):
        assert snippet in bridge, f"missing bridge generation contract: {snippet}"

    for snippet in (
        "pending_feature_request_valid = false;",
        "reset_host_report_queues_locked();",
        "pending_output_report_valid = false;",
        "g_m61_usb_control_storage",
        "m61_audio_epoch_reset(usb_generation);",
        "flush_mic_queues();",
    ):
        assert snippet in usb, f"missing USB queue reset contract: {snippet}"

    print("M61 transport generation tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
