#!/usr/bin/env python3
"""Unit tests for the post-link scheduler memory gate."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import check_scheduler_memory as gate
import scheduler_ram_budget as budget


class SchedulerMemoryGateTests(unittest.TestCase):
    def test_python_forecast_matches_compiled_c_contract(self) -> None:
        source = (
            Path(__file__).resolve().parents[1]
            / "main"
            / "dual_chip_scheduler_types.c"
        ).read_text(encoding="utf-8")
        replaced = budget.total(budget.M61_REPLACED_STATIC)
        planned = budget.total(budget.M61_PLANNED_STATIC)
        projected_static = (
            budget.M61_BASELINE_STATIC_USED
            - replaced
            + planned
            + budget.M61_STATIC_CONTINGENCY
        )
        projected_heap = (
            budget.M61_BASELINE_MIN_HEAP
            - budget.M61_RUNTIME_HEAP_DELTA
            - (planned - replaced)
            - budget.M61_STATIC_CONTINGENCY
            - budget.M61_HEAP_CONTINGENCY
        )
        self.assertIn(
            f"DS5_SCHED_M61_REPLACED_STATIC_BYTES {replaced}U", source
        )
        self.assertIn(
            f"DS5_SCHED_M61_PROJECTED_STATIC_BYTES == {projected_static}U",
            source,
        )
        self.assertIn(
            f"DS5_SCHED_M61_PROJECTED_MIN_HEAP_BYTES == {projected_heap}U",
            source,
        )

    def test_current_m61_layout_example(self) -> None:
        symbols = {
            "__ram_start__": 0x62FC0400,
            "__HeapBase": 0x63003EA0,
            "__HeapLimit": 0x63028000,
        }
        with mock.patch.object(gate, "read_elf_symbols", return_value=symbols):
            _, measured = gate.measure_m61(Path("current.elf"))
        self.assertEqual(measured["static_used"], 277_152)
        self.assertEqual(measured["static_with_reserve"], 285_344)
        self.assertEqual(measured["heap_capacity"], 147_808)
        self.assertEqual(measured["heap_after_reserves"], 131_424)

    def test_m61_rejects_invalid_symbol_order(self) -> None:
        symbols = {
            "__ram_start__": 100,
            "__HeapBase": 90,
            "__HeapLimit": 200,
        }
        with mock.patch.object(gate, "read_elf_symbols", return_value=symbols):
            with self.assertRaises(gate.GateError):
                gate.measure_m61(Path("bad.elf"))

    def test_elf_reader_falls_back_to_target_nm(self) -> None:
        expected = {"__ram_start__": 1}
        with mock.patch.object(gate, "_symbols_with_pyelftools", side_effect=ImportError), mock.patch.object(
            gate, "_symbols_with_nm", return_value=expected
        ) as fallback:
            self.assertEqual(gate.read_elf_symbols(Path("firmware.elf"), "target-nm"), expected)
        fallback.assert_called_once_with(Path("firmware.elf"), "target-nm")

    def test_current_esp_json2_example(self) -> None:
        measured = gate.measure_esp(
            {"layout": [{"name": "DRAM", "used": 68_156, "total": 124_580}]}
        )
        self.assertEqual(measured["used_with_reserve"], 76_348)
        self.assertEqual(measured["limit"], 87_206)

    def test_phase_rules_are_opt_in(self) -> None:
        rules = gate.load_phase_rules("current", None, [], [])
        self.assertEqual(gate.check_phase_symbols(rules, set(), ""), [])

    def test_configurable_final_phase_symbols(self) -> None:
        config = {
            "final": {
                "require": {"m61": ["m61_spi_scheduler_submit"], "esp": ["bt_ds5_tx_scheduler_submit"]},
                "forbid": {"esp": ["legacy_send_fifo"]},
            }
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phases.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            rules = gate.load_phase_rules("final", path, [], [])
        failures = gate.check_phase_symbols(
            rules,
            {"m61_spi_scheduler_submit"},
            " .text.bt_ds5_tx_scheduler_submit\n",
        )
        self.assertEqual(failures, [])
        self.assertEqual(
            gate.check_phase_symbols(rules, set(), "legacy_send_fifo"),
            [
                "phase requires missing m61 symbol: m61_spi_scheduler_submit",
                "phase requires missing esp symbol: bt_ds5_tx_scheduler_submit",
                "phase forbids present esp symbol: legacy_send_fifo",
            ],
        )

    def test_bundled_final_phase_contract(self) -> None:
        path = Path(__file__).with_name("scheduler_symbol_phases.json")
        rules = gate.load_phase_rules("final", path, [], [])
        failures = gate.check_phase_symbols(
            rules,
            {
                "g_ds5_owner_m61_spi_scheduler",
                "g_ds5_owner_m61_audio_epochs",
                "g_m61_mic_opus_storage",
                "g_m61_usb_control_storage",
            },
            "\n".join(
                (
                    ".bss.g_ds5_owner_esp_tx_scheduler 0x1000 0x1",
                    ".bss.g_ds5_owner_esp_response_scheduler 0x1001 0x1",
                    ".bss.g_ds5_response_scheduler_storage 0x1600 0x1c00",
                    ".text.esp_mesh_push_to_ps_tx_queue",
                )
            ),
        )
        self.assertEqual(failures, [])

    def test_phase_config_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "phases.json"
            path.write_text(json.dumps({"final": {}}), encoding="utf-8")
            with self.assertRaises(gate.GateError):
                gate.load_phase_rules("finla", path, [], [])

    def test_symbol_matching_does_not_use_substrings(self) -> None:
        rules = gate.load_phase_rules("final", None, [], ["esp:s_tx_queue"])
        self.assertEqual(
            gate.check_phase_symbols(
                rules, set(), ".text.esp_mesh_push_to_ps_tx_queue"
            ),
            [],
        )

    def test_cli_symbol_rule_defaults_to_any_target(self) -> None:
        rules = gate.load_phase_rules("final", None, ["new_owner"], ["esp:old_queue"])
        self.assertEqual(rules["require"]["any"], ["new_owner"])
        self.assertEqual(rules["forbid"]["esp"], ["old_queue"])


if __name__ == "__main__":
    unittest.main()
