#!/usr/bin/env python3
"""Focused contracts for M61 peer recovery and connection generations."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = ROOT / "m61" / "dualsense_hidp_probe" / "m61_esp32_transport.c"
MAIN = ROOT / "m61" / "dualsense_hidp_probe" / "main.c"
USB = ROOT / "m61" / "dualsense_hidp_probe" / "m61_usb_gamepad.c"


def main() -> int:
    transport = TRANSPORT.read_text(encoding="utf-8")
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
    ):
        assert snippet in transport, f"missing transport generation contract: {snippet}"

    recovery = transport.index("static void perform_recovery(uint8_t reason)")
    not_ready = transport.index("s_transport.ready = false;", recovery)
    invalidate = transport.index("invalidate_peer_generation();", not_ready)
    reset = transport.index("bflb_gpio_reset(s_transport.gpio", invalidate)
    assert not_ready < invalidate < reset

    send_hello = transport.index("static int send_hello(void)", recovery)
    hello_expected = transport.index(
        "s_transport.hello_response_expected = true;", send_hello
    )
    hello_send = transport.index(
        "send_payload(DS5_DUAL_MSG_HELLO", hello_expected
    )
    assert hello_expected < hello_send

    for snippet in (
        "generation != dual_chip_bt_generation",
        "m61_ds5_dse_reset();",
        "m61_usb_gamepad_reset_feature_cache();",
        "m61_usb_gamepad_reset_transport_queues();",
        "hidp_usb_output_pending = false;",
        "observed_bt_generation != dual_chip_bt_generation",
        "pending_speaker_count = 0;",
        "pending_haptics_count = 0;",
    ):
        assert snippet in bridge, f"missing bridge generation contract: {snippet}"

    for snippet in (
        "pending_feature_request_valid = false;",
        "pending_host_report_valid = false;",
        "flush_haptics_queue();",
        "flush_speaker_queues();",
        "flush_mic_queues();",
    ):
        assert snippet in usb, f"missing USB queue reset contract: {snippet}"

    print("M61 transport generation tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
