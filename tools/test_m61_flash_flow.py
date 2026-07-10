#!/usr/bin/env python3
"""Offline tests for the M61 software-ISP flash and loader-exit flow."""

from __future__ import annotations

from contextlib import contextmanager, redirect_stderr
import io
from pathlib import Path
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import flash_m61_firmware as flash  # noqa: E402


class FakeSerial:
    def __init__(self, responses: list[bytes]) -> None:
        self.responses = list(responses)
        self.pending = bytearray()
        self.writes: list[bytes] = []
        self.dtr: bool | None = None
        self.rts: bool | None = None
        self.input_reset = False

    def __enter__(self) -> "FakeSerial":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        return None

    def setDTR(self, value: bool) -> None:
        self.dtr = value

    def setRTS(self, value: bool) -> None:
        self.rts = value

    def reset_input_buffer(self) -> None:
        self.input_reset = True
        self.pending.clear()

    def write(self, data: bytes) -> int:
        self.writes.append(bytes(data))
        if self.responses:
            self.pending.extend(self.responses.pop(0))
        return len(data)

    def flush(self) -> None:
        return None

    def read(self, size: int) -> bytes:
        data = bytes(self.pending[:size])
        del self.pending[:size]
        return data


class ResetDisconnectSerial(FakeSerial):
    def read(self, size: int) -> bytes:
        if len(self.writes) >= 2:
            raise flash.serial.SerialException("device reset")
        return super().read(size)


def serial_factory(device: FakeSerial):
    def open_serial(*args, **kwargs) -> FakeSerial:
        assert args[0] == "COM5"
        assert kwargs["baudrate"] == 460800
        return device

    return open_serial


def test_loader_exit_protocol() -> None:
    success = FakeSerial([b"OK", b"OK"])
    assert flash.exit_bl616_uart_loader(
        "COM5",
        460800,
        timeout_ms=5,
        serial_factory=serial_factory(success),
    )
    assert success.writes == [
        flash.BL616_CLEAR_HBN_RSV2_COMMAND,
        flash.BL616_RESET_COMMAND,
    ]
    assert success.dtr is False
    assert success.rts is False
    assert success.input_reset

    reset_disconnect = ResetDisconnectSerial([b"OK"])
    assert flash.exit_bl616_uart_loader(
        "COM5",
        460800,
        timeout_ms=5,
        serial_factory=serial_factory(reset_disconnect),
    )
    assert reset_disconnect.writes == [
        flash.BL616_CLEAR_HBN_RSV2_COMMAND,
        flash.BL616_RESET_COMMAND,
    ]

    clear_timeout = FakeSerial([b""])
    with redirect_stderr(io.StringIO()):
        assert not flash.exit_bl616_uart_loader(
            "COM5",
            460800,
            timeout_ms=1,
            serial_factory=serial_factory(clear_timeout),
        )
    assert clear_timeout.writes == [flash.BL616_CLEAR_HBN_RSV2_COMMAND]

    clear_nack = FakeSerial([b"FL"])
    with redirect_stderr(io.StringIO()):
        assert not flash.exit_bl616_uart_loader(
            "COM5",
            460800,
            timeout_ms=1,
            serial_factory=serial_factory(clear_nack),
        )
    assert clear_nack.writes == [flash.BL616_CLEAR_HBN_RSV2_COMMAND]

    reset_timeout = FakeSerial([b"OK", b""])
    with redirect_stderr(io.StringIO()):
        assert not flash.exit_bl616_uart_loader(
            "COM5",
            460800,
            timeout_ms=1,
            serial_factory=serial_factory(reset_timeout),
        )
    assert reset_timeout.writes == [
        flash.BL616_CLEAR_HBN_RSV2_COMMAND,
        flash.BL616_RESET_COMMAND,
    ]


@contextmanager
def patched_flash_environment():
    original_tool = flash.FLASH_TOOL
    original_app = flash.FIRMWARE_APPS["bridge"]
    original_call = flash.subprocess.call
    original_reboot = flash.try_reboot_isp
    original_exit = flash.exit_bl616_uart_loader
    with tempfile.TemporaryDirectory() as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        app_dir = temp_dir / "app"
        build_out = app_dir / "build" / "build_out"
        build_out.mkdir(parents=True)
        (build_out / "fw_bl616.bin").write_bytes(b"firmware")
        (app_dir / "flash_prog_cfg.ini").write_text(
            "[cfg]\n"
            "erase = 1\n"
            "skip_mode = 0x0, 0x0\n"
            "boot2_isp_mode = 0\n\n"
            "[FW]\n"
            "filedir = ./build/build_out/fw_$(CHIPNAME).bin\n"
            "address = 0x000000\n",
            encoding="utf-8",
        )
        tool = temp_dir / "BLFlashCommand.exe"
        tool.write_bytes(b"tool")
        flash.FLASH_TOOL = tool
        flash.FIRMWARE_APPS["bridge"] = flash.FirmwareApp(
            directory=app_dir,
            bl616_bin="fw_bl616.bin",
            build_command="build",
        )
        try:
            yield app_dir
        finally:
            flash.FLASH_TOOL = original_tool
            flash.FIRMWARE_APPS["bridge"] = original_app
            flash.subprocess.call = original_call
            flash.try_reboot_isp = original_reboot
            flash.exit_bl616_uart_loader = original_exit


def test_main_manages_software_isp_exit() -> None:
    with patched_flash_environment() as app_dir:
        calls: list[tuple[list[str], Path, str]] = []
        exits: list[tuple[str, int]] = []

        def fake_call(cmd: list[str], cwd: Path) -> int:
            config_arg = next(arg for arg in cmd if arg.startswith("--config="))
            config_path = cwd / config_arg.split("=", 1)[1]
            calls.append((list(cmd), cwd, config_path.read_text(encoding="utf-8")))
            return 0

        flash.subprocess.call = fake_call
        flash.try_reboot_isp = lambda *args: True
        flash.exit_bl616_uart_loader = lambda port, baud: exits.append((port, baud)) is None

        assert flash.main(["--app", "bridge", "-p", "COM5", "--reboot-isp"]) == 0
        assert len(calls) == 1
        cmd, cwd, config_text = calls[0]
        assert cwd == app_dir
        assert "--reset" not in cmd
        assert "boot2_isp_mode = 1" in config_text
        assert exits == [("COM5", 460800)]

        exits.clear()
        assert flash.main(
            ["--app", "bridge", "-p", "COM5", "--reboot-isp", "--no-reset"]
        ) == 0
        assert "--reset" not in calls[-1][0]
        assert "boot2_isp_mode = 1" in calls[-1][2]
        assert not exits


def test_main_preserves_manual_flash_behavior() -> None:
    with patched_flash_environment() as app_dir:
        calls: list[tuple[list[str], Path, str]] = []
        exits: list[tuple[str, int]] = []

        def fake_call(cmd: list[str], cwd: Path) -> int:
            config_arg = next(arg for arg in cmd if arg.startswith("--config="))
            config_path = cwd / config_arg.split("=", 1)[1]
            calls.append((list(cmd), cwd, config_path.read_text(encoding="utf-8")))
            return 0

        flash.subprocess.call = fake_call
        flash.exit_bl616_uart_loader = lambda port, baud: exits.append((port, baud)) is None

        assert flash.main(["--app", "bridge", "-p", "COM5"]) == 0
        assert "--reset" in calls[-1][0]
        assert "--config=flash_prog_cfg.ini" in calls[-1][0]
        assert "boot2_isp_mode = 0" in calls[-1][2]
        assert not exits

        custom_build = app_dir / "build_custom"
        custom_build_out = custom_build / "build_out"
        custom_build_out.mkdir(parents=True)
        (custom_build_out / "fw_bl616.bin").write_bytes(b"custom firmware")
        assert flash.main(
            ["--app", "bridge", "-p", "COM5", "--build-dir", str(custom_build)]
        ) == 0
        assert "--reset" in calls[-1][0]
        assert "filedir = ./build_custom/build_out/fw_$(CHIPNAME).bin" in calls[-1][2]
        assert not exits


def test_main_propagates_flash_and_exit_failures() -> None:
    with patched_flash_environment():
        exits: list[tuple[str, int]] = []
        flash.try_reboot_isp = lambda *args: True
        flash.subprocess.call = lambda *args, **kwargs: 7
        flash.exit_bl616_uart_loader = lambda port, baud: exits.append((port, baud)) is None
        assert flash.main(["--app", "bridge", "-p", "COM5", "--reboot-isp"]) == 7
        assert not exits

        flash.subprocess.call = lambda *args, **kwargs: 0
        flash.exit_bl616_uart_loader = lambda *args: False
        assert flash.main(["--app", "bridge", "-p", "COM5", "--reboot-isp"]) == 1


def main() -> int:
    test_loader_exit_protocol()
    test_main_manages_software_isp_exit()
    test_main_preserves_manual_flash_behavior()
    test_main_propagates_flash_and_exit_failures()
    print("M61 flash flow tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
