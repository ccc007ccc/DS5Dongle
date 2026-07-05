#!/usr/bin/env python3
"""Windows desktop tester for the M61 DualSense USB composite device.

The app intentionally uses only the Python standard library for the desktop UI
and Windows HID access. Serial status uses pyserial when it is installed.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import math
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk
from tkinter.scrolledtext import ScrolledText
from typing import Any

import check_m61_usb_windows


VID = 0x054C
PID = 0x0CE6
TARGET_VID_PID = "vid_054c&pid_0ce6"
TARGET_MI = "mi_03"
DEFAULT_PORT = "COM5"

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x00000080
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

DIGCF_PRESENT = 0x00000002
DIGCF_DEVICEINTERFACE = 0x00000010
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
HIDP_STATUS_SUCCESS = 0x00110000

DS5_OUTPUT_REPORT_ID = 0x02
DS5_USB_SET_STATE_LEN = 47
DS5_INPUT_REPORT_ID = 0x01
DS5_INPUT_REPORT_LEN = 64


DEFAULT_SET_STATE_63 = bytes([
    0xFD, 0xF7, 0x00, 0x00, 0x64, 0x64, 0xFF, 0x09,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00,
    0x00, 0x02, 0x01, 0x00, 0xFF, 0xD7, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
])
DEFAULT_SET_STATE_47 = DEFAULT_SET_STATE_63[:DS5_USB_SET_STATE_LEN]


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", wintypes.BYTE * 8),
    ]


ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wintypes.DWORD),
        ("Reserved", ULONG_PTR),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("DevicePath", wintypes.WCHAR * 1024),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wintypes.USHORT),
        ("UsagePage", wintypes.USHORT),
        ("InputReportByteLength", wintypes.USHORT),
        ("OutputReportByteLength", wintypes.USHORT),
        ("FeatureReportByteLength", wintypes.USHORT),
        ("Reserved", wintypes.USHORT * 17),
        ("NumberLinkCollectionNodes", wintypes.USHORT),
        ("NumberInputButtonCaps", wintypes.USHORT),
        ("NumberInputValueCaps", wintypes.USHORT),
        ("NumberInputDataIndices", wintypes.USHORT),
        ("NumberOutputButtonCaps", wintypes.USHORT),
        ("NumberOutputValueCaps", wintypes.USHORT),
        ("NumberOutputDataIndices", wintypes.USHORT),
        ("NumberFeatureButtonCaps", wintypes.USHORT),
        ("NumberFeatureValueCaps", wintypes.USHORT),
        ("NumberFeatureDataIndices", wintypes.USHORT),
    ]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ULONG_PTR),
        ("InternalHigh", ULONG_PTR),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


@dataclass(frozen=True)
class HidCaps:
    input_len: int
    output_len: int
    feature_len: int
    usage_page: int
    usage: int


@dataclass(frozen=True)
class HidInfo:
    path: str
    caps: HidCaps


@dataclass(frozen=True)
class DecodedInput:
    text: str
    fields: dict[str, str]


class WindowsHid:
    def __init__(self) -> None:
        if sys.platform != "win32":
            raise RuntimeError("Windows HID API is only available on Windows")

        self.hid = ctypes.WinDLL("hid", use_last_error=True)
        self.setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._setup_prototypes()

    def _setup_prototypes(self) -> None:
        self.hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
        self.hid.HidD_GetHidGuid.restype = None
        self.hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
        self.hid.HidD_GetPreparsedData.restype = wintypes.BOOL
        self.hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
        self.hid.HidD_FreePreparsedData.restype = wintypes.BOOL
        self.hid.HidD_SetOutputReport.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.ULONG]
        self.hid.HidD_SetOutputReport.restype = wintypes.BOOL
        self.hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
        self.hid.HidP_GetCaps.restype = ctypes.c_long

        self.setupapi.SetupDiGetClassDevsW.argtypes = [
            ctypes.POINTER(GUID),
            wintypes.LPCWSTR,
            wintypes.HWND,
            wintypes.DWORD,
        ]
        self.setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
        self.setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            ctypes.POINTER(GUID),
            wintypes.DWORD,
            ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
        ]
        self.setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
        self.setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
            ctypes.POINTER(SP_DEVICE_INTERFACE_DETAIL_DATA_W),
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.c_void_p,
        ]
        self.setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
        self.setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
        self.setupapi.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

        self.kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            ctypes.c_void_p,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        self.kernel32.CreateFileW.restype = wintypes.HANDLE
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.kernel32.ReadFile.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(OVERLAPPED),
        ]
        self.kernel32.ReadFile.restype = wintypes.BOOL
        self.kernel32.WriteFile.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.c_void_p,
        ]
        self.kernel32.WriteFile.restype = wintypes.BOOL
        self.kernel32.CreateEventW.argtypes = [
            ctypes.c_void_p,
            wintypes.BOOL,
            wintypes.BOOL,
            wintypes.LPCWSTR,
        ]
        self.kernel32.CreateEventW.restype = wintypes.HANDLE
        self.kernel32.ResetEvent.argtypes = [wintypes.HANDLE]
        self.kernel32.ResetEvent.restype = wintypes.BOOL
        self.kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.kernel32.WaitForSingleObject.restype = wintypes.DWORD
        self.kernel32.GetOverlappedResult.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(OVERLAPPED),
            ctypes.POINTER(wintypes.DWORD),
            wintypes.BOOL,
        ]
        self.kernel32.GetOverlappedResult.restype = wintypes.BOOL
        if hasattr(self.kernel32, "CancelIoEx"):
            self.kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(OVERLAPPED)]
            self.kernel32.CancelIoEx.restype = wintypes.BOOL

    def _last_error(self) -> int:
        return ctypes.get_last_error()

    def _last_error_text(self) -> str:
        return f"WinError {self._last_error()}"

    def _open_handle(self, path: str, access: int, overlapped: bool) -> wintypes.HANDLE:
        flags = FILE_ATTRIBUTE_NORMAL | (FILE_FLAG_OVERLAPPED if overlapped else 0)
        handle = self.kernel32.CreateFileW(
            path,
            access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            flags,
            None,
        )
        if handle == INVALID_HANDLE_VALUE:
            raise OSError(self._last_error(), f"CreateFile failed for {path}")
        return handle

    def close_handle(self, handle: wintypes.HANDLE | None) -> None:
        if handle and handle != INVALID_HANDLE_VALUE:
            self.kernel32.CloseHandle(handle)

    def enumerate_hid_paths(self) -> list[str]:
        guid = GUID()
        self.hid.HidD_GetHidGuid(ctypes.byref(guid))
        info_set = self.setupapi.SetupDiGetClassDevsW(
            ctypes.byref(guid),
            None,
            None,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
        )
        if info_set == INVALID_HANDLE_VALUE:
            raise OSError(self._last_error(), "SetupDiGetClassDevsW failed")

        paths: list[str] = []
        try:
            index = 0
            while True:
                interface_data = SP_DEVICE_INTERFACE_DATA()
                interface_data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
                ok = self.setupapi.SetupDiEnumDeviceInterfaces(
                    info_set,
                    None,
                    ctypes.byref(guid),
                    index,
                    ctypes.byref(interface_data),
                )
                if not ok:
                    break

                detail = SP_DEVICE_INTERFACE_DETAIL_DATA_W()
                detail.cbSize = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
                required_size = wintypes.DWORD()
                ok = self.setupapi.SetupDiGetDeviceInterfaceDetailW(
                    info_set,
                    ctypes.byref(interface_data),
                    ctypes.byref(detail),
                    ctypes.sizeof(detail),
                    ctypes.byref(required_size),
                    None,
                )
                if ok and detail.DevicePath:
                    paths.append(detail.DevicePath)
                index += 1
        finally:
            self.setupapi.SetupDiDestroyDeviceInfoList(info_set)

        return paths

    def read_caps(self, path: str) -> HidCaps:
        handle = self._open_handle(path, GENERIC_READ | GENERIC_WRITE, overlapped=False)
        preparsed = ctypes.c_void_p()
        try:
            if not self.hid.HidD_GetPreparsedData(handle, ctypes.byref(preparsed)):
                raise OSError(self._last_error(), "HidD_GetPreparsedData failed")
            caps = HIDP_CAPS()
            status = self.hid.HidP_GetCaps(preparsed, ctypes.byref(caps))
            if status != HIDP_STATUS_SUCCESS:
                raise OSError(status, "HidP_GetCaps failed")
            return HidCaps(
                input_len=int(caps.InputReportByteLength),
                output_len=int(caps.OutputReportByteLength),
                feature_len=int(caps.FeatureReportByteLength),
                usage_page=int(caps.UsagePage),
                usage=int(caps.Usage),
            )
        finally:
            if preparsed:
                self.hid.HidD_FreePreparsedData(preparsed)
            self.close_handle(handle)

    def find_dualsense_hid(self) -> list[HidInfo]:
        matches: list[HidInfo] = []
        for path in self.enumerate_hid_paths():
            lower = path.lower()
            if TARGET_VID_PID not in lower:
                continue
            if TARGET_MI not in lower:
                continue
            try:
                caps = self.read_caps(path)
            except OSError:
                caps = HidCaps(
                    input_len=DS5_INPUT_REPORT_LEN,
                    output_len=DS5_USB_SET_STATE_LEN + 1,
                    feature_len=64,
                    usage_page=0,
                    usage=0,
                )
            matches.append(HidInfo(path=path, caps=caps))
        return matches

    def open_reader(self, path: str) -> wintypes.HANDLE:
        return self._open_handle(path, GENERIC_READ, overlapped=True)

    def open_writer(self, path: str) -> wintypes.HANDLE:
        return self._open_handle(path, GENERIC_WRITE, overlapped=False)

    def write_output_report(self, handle: wintypes.HANDLE, report: bytes, output_len: int) -> int:
        length = max(output_len, len(report))
        data = report + bytes(length - len(report))
        buf = ctypes.create_string_buffer(data, length)
        written = wintypes.DWORD()
        ok = self.kernel32.WriteFile(handle, buf, length, ctypes.byref(written), None)
        if ok:
            return int(written.value)

        # Some HID stacks prefer control SET_REPORT for output reports.
        if self.hid.HidD_SetOutputReport(handle, buf, length):
            return length
        raise OSError(self._last_error(), f"WriteFile/HidD_SetOutputReport failed ({self._last_error_text()})")


def read_i16_le(data: bytes, offset: int) -> int:
    value = data[offset] | (data[offset + 1] << 8)
    if value & 0x8000:
        value -= 0x10000
    return value


def read_u32_le(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def dpad_name(value: int) -> str:
    return ["N", "NE", "E", "SE", "S", "SW", "W", "NW", "idle"][value] if 0 <= value <= 8 else "invalid"


BUTTONS = [
    (7, 0x10, "square"),
    (7, 0x20, "cross"),
    (7, 0x40, "circle"),
    (7, 0x80, "triangle"),
    (8, 0x01, "L1"),
    (8, 0x02, "R1"),
    (8, 0x04, "L2"),
    (8, 0x08, "R2"),
    (8, 0x10, "create"),
    (8, 0x20, "options"),
    (8, 0x40, "L3"),
    (8, 0x80, "R3"),
    (9, 0x01, "PS"),
    (9, 0x02, "touchpad"),
    (9, 0x04, "mute"),
    (9, 0x10, "FnL"),
    (9, 0x20, "FnR"),
    (9, 0x40, "PaddleL"),
    (9, 0x80, "PaddleR"),
]


def decode_touch(data: bytes, offset: int) -> str:
    if len(data) < offset + 4:
        return "n/a"
    b0, b1, b2, b3 = data[offset : offset + 4]
    active = 0 if (b0 & 0x80) else 1
    x = b1 | ((b2 & 0x0F) << 8)
    y = ((b2 >> 4) | (b3 << 4)) & 0x0FFF
    return f"{active}:{x},{y}"


def decode_input_report(report: bytes) -> DecodedInput:
    if len(report) >= 1 and report[0] == DS5_INPUT_REPORT_ID:
        payload = report[1:]
    else:
        payload = report

    if len(payload) < 55:
        return DecodedInput(
            text=f"short report len={len(report)} raw={report.hex(' ')}",
            fields={"report": "short"},
        )

    buttons = [name for offset, mask, name in BUTTONS if offset < len(payload) and payload[offset] & mask]
    button_text = "+".join(buttons) if buttons else "none"
    battery_raw = payload[52] & 0x0F
    fields = {
        "LX": str(payload[0]),
        "LY": str(payload[1]),
        "RX": str(payload[2]),
        "RY": str(payload[3]),
        "L2": str(payload[4]),
        "R2": str(payload[5]),
        "DPad": dpad_name(payload[7] & 0x0F),
        "Buttons": button_text,
        "Gyro": f"{read_i16_le(payload, 15)}, {read_i16_le(payload, 19)}, {read_i16_le(payload, 17)}",
        "Accel": f"{read_i16_le(payload, 21)}, {read_i16_le(payload, 23)}, {read_i16_le(payload, 25)}",
        "Battery": f"{battery_raw * 10}% raw={battery_raw}",
        "Power": f"0x{payload[52] >> 4:X}",
        "Headset": "yes" if payload[53] & 0x01 else "no",
        "Mic": "yes" if payload[53] & 0x02 else "no",
        "Muted": "yes" if payload[53] & 0x04 else "no",
        "Touch0": decode_touch(payload, 32),
        "Touch1": decode_touch(payload, 36),
        "SensorTS": str(read_u32_le(payload, 27)),
        "Temp": str(ctypes.c_int8(payload[31]).value),
        "TrigStatus": f"R=0x{payload[41]:02X} L=0x{payload[42]:02X} active=0x{payload[47]:02X}",
    }
    text = (
        f"LX={fields['LX']} LY={fields['LY']} RX={fields['RX']} RY={fields['RY']} "
        f"L2={fields['L2']} R2={fields['R2']} dpad={fields['DPad']} buttons={fields['Buttons']} "
        f"gyro=({fields['Gyro']}) accel=({fields['Accel']}) battery={fields['Battery']}"
    )
    return DecodedInput(text=text, fields=fields)


def parse_hex_bytes(text: str) -> bytes:
    cleaned = (
        text.replace("0x", " ")
        .replace(",", " ")
        .replace(";", " ")
        .replace("\n", " ")
        .replace("\r", " ")
        .replace("\t", " ")
    )
    values = [part for part in cleaned.split(" ") if part]
    return bytes(int(part, 16) for part in values)


class HidReadThread(threading.Thread):
    def __init__(
        self,
        hid: WindowsHid,
        path: str,
        input_len: int,
        out_queue: queue.Queue[tuple[str, Any]],
    ) -> None:
        super().__init__(daemon=True)
        self.hid = hid
        self.path = path
        self.input_len = input_len or DS5_INPUT_REPORT_LEN
        self.out_queue = out_queue
        self.stop_event = threading.Event()
        self.handle: wintypes.HANDLE | None = None

    def stop(self) -> None:
        self.stop_event.set()

    def run(self) -> None:
        event = None
        try:
            self.handle = self.hid.open_reader(self.path)
            event = self.hid.kernel32.CreateEventW(None, True, False, None)
            if not event:
                raise OSError(self.hid._last_error(), "CreateEventW failed")
            self.out_queue.put(("log", "HID input reader started"))
            while not self.stop_event.is_set():
                self.hid.kernel32.ResetEvent(event)
                overlapped = OVERLAPPED()
                overlapped.hEvent = event
                buf = ctypes.create_string_buffer(self.input_len)
                read_len = wintypes.DWORD()
                ok = self.hid.kernel32.ReadFile(
                    self.handle,
                    buf,
                    self.input_len,
                    ctypes.byref(read_len),
                    ctypes.byref(overlapped),
                )
                if not ok:
                    err = ctypes.get_last_error()
                    if err != ERROR_IO_PENDING:
                        self.out_queue.put(("error", f"ReadFile failed: WinError {err}"))
                        time.sleep(0.2)
                        continue
                    completed = False
                    while not self.stop_event.is_set():
                        wait = self.hid.kernel32.WaitForSingleObject(event, 100)
                        if wait == WAIT_OBJECT_0:
                            completed = True
                            break
                        if wait != WAIT_TIMEOUT:
                            self.out_queue.put(("error", f"WaitForSingleObject returned {wait}"))
                            break
                    if not completed:
                        if hasattr(self.hid.kernel32, "CancelIoEx"):
                            self.hid.kernel32.CancelIoEx(self.handle, ctypes.byref(overlapped))
                        continue
                    ok = self.hid.kernel32.GetOverlappedResult(
                        self.handle,
                        ctypes.byref(overlapped),
                        ctypes.byref(read_len),
                        False,
                    )
                    if not ok:
                        err = ctypes.get_last_error()
                        if not self.stop_event.is_set():
                            self.out_queue.put(("error", f"GetOverlappedResult failed: WinError {err}"))
                        continue

                if read_len.value:
                    data = bytes(buf.raw[: read_len.value])
                    self.out_queue.put(("input", data))
        except Exception as exc:
            self.out_queue.put(("error", f"HID reader error: {exc}"))
        finally:
            if event:
                self.hid.close_handle(event)
            if self.handle:
                self.hid.close_handle(self.handle)
            self.out_queue.put(("log", "HID input reader stopped"))


class Ds5WindowsTestApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("M61 DualSense Windows Tester")
        self.root.minsize(1040, 720)
        self.queue: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.hid: WindowsHid | None = None
        self.hid_infos: list[HidInfo] = []
        self.selected_hid: HidInfo | None = None
        self.write_handle: wintypes.HANDLE | None = None
        self.reader: HidReadThread | None = None
        self.report_count = 0
        self.output_state = bytearray(DEFAULT_SET_STATE_47)

        self.status_var = tk.StringVar(value="Not connected")
        self.hid_path_var = tk.StringVar(value="")
        self.caps_var = tk.StringVar(value="")
        self.port_var = tk.StringVar(value=DEFAULT_PORT)
        self.red_var = tk.IntVar(value=0xFF)
        self.green_var = tk.IntVar(value=0xD7)
        self.blue_var = tk.IntVar(value=0x00)
        self.rumble_right_var = tk.IntVar(value=0)
        self.rumble_left_var = tk.IntVar(value=0)
        self.trigger_force_var = tk.IntVar(value=128)
        self.input_vars: dict[str, tk.StringVar] = {}

        self._build_ui()
        self._init_hid()
        self.root.after(100, self._drain_queue)
        self.refresh_pnp()
        self.connect_hid()

    def _init_hid(self) -> None:
        if sys.platform != "win32":
            self.log("This tester requires Windows for HID read/write.")
            return
        try:
            self.hid = WindowsHid()
        except Exception as exc:
            self.log(f"HID API init failed: {exc}")

    def _build_ui(self) -> None:
        main = ttk.Frame(self.root, padding=8)
        main.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main.columnconfigure(0, weight=1)
        main.columnconfigure(1, weight=1)
        main.rowconfigure(2, weight=1)

        top = ttk.Frame(main)
        top.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        top.columnconfigure(9, weight=1)
        ttk.Button(top, text="Refresh PnP", command=self.refresh_pnp).grid(row=0, column=0, padx=(0, 4))
        ttk.Button(top, text="Connect HID", command=self.connect_hid).grid(row=0, column=1, padx=4)
        ttk.Button(top, text="Start Input", command=self.start_input).grid(row=0, column=2, padx=4)
        ttk.Button(top, text="Stop Input", command=self.stop_input).grid(row=0, column=3, padx=4)
        ttk.Label(top, text="Serial").grid(row=0, column=4, padx=(16, 4))
        ttk.Entry(top, textvariable=self.port_var, width=8).grid(row=0, column=5)
        ttk.Button(top, text="ds5 status", command=self.read_serial_status).grid(row=0, column=6, padx=4)
        ttk.Label(top, textvariable=self.status_var).grid(row=0, column=9, sticky="e")

        pnp = ttk.LabelFrame(main, text="Windows PnP")
        pnp.grid(row=1, column=0, sticky="nsew", padx=(0, 4), pady=(0, 8))
        pnp.columnconfigure(0, weight=1)
        self.device_tree = ttk.Treeview(
            pnp,
            columns=("status", "class", "name", "id"),
            show="headings",
            height=8,
        )
        for col, width in (("status", 70), ("class", 110), ("name", 250), ("id", 520)):
            self.device_tree.heading(col, text=col)
            self.device_tree.column(col, width=width, anchor="w")
        self.device_tree.grid(row=0, column=0, sticky="nsew")
        pnp.rowconfigure(0, weight=1)

        hid_box = ttk.LabelFrame(main, text="HID")
        hid_box.grid(row=1, column=1, sticky="nsew", padx=(4, 0), pady=(0, 8))
        hid_box.columnconfigure(1, weight=1)
        ttk.Label(hid_box, text="Path").grid(row=0, column=0, sticky="w")
        ttk.Entry(hid_box, textvariable=self.hid_path_var, state="readonly").grid(row=0, column=1, sticky="ew")
        ttk.Label(hid_box, text="Caps").grid(row=1, column=0, sticky="w")
        ttk.Label(hid_box, textvariable=self.caps_var).grid(row=1, column=1, sticky="w")
        ttk.Label(hid_box, text="Raw output 0x02 payload/report hex").grid(row=2, column=0, columnspan=2, sticky="w")
        self.raw_output = tk.Text(hid_box, height=3, width=70)
        self.raw_output.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(2, 4))
        ttk.Button(hid_box, text="Send Raw", command=self.send_raw_output).grid(row=4, column=0, sticky="w")
        ttk.Button(hid_box, text="Load Current 47B", command=self.load_current_output_hex).grid(row=4, column=1, sticky="w")

        input_box = ttk.LabelFrame(main, text="Input report 0x01")
        input_box.grid(row=2, column=0, sticky="nsew", padx=(0, 4), pady=(0, 8))
        for i in range(4):
            input_box.columnconfigure(i, weight=1)
        field_names = [
            "LX",
            "LY",
            "RX",
            "RY",
            "L2",
            "R2",
            "DPad",
            "Buttons",
            "Gyro",
            "Accel",
            "Battery",
            "Power",
            "Headset",
            "Mic",
            "Muted",
            "Touch0",
            "Touch1",
            "SensorTS",
            "Temp",
            "TrigStatus",
        ]
        for idx, name in enumerate(field_names):
            row = idx // 2
            col = (idx % 2) * 2
            ttk.Label(input_box, text=name).grid(row=row, column=col, sticky="w", padx=(0, 4))
            var = tk.StringVar(value="-")
            self.input_vars[name] = var
            ttk.Label(input_box, textvariable=var).grid(row=row, column=col + 1, sticky="w")

        output_box = ttk.LabelFrame(main, text="Output report 0x02")
        output_box.grid(row=2, column=1, sticky="nsew", padx=(4, 0), pady=(0, 8))
        for i in range(4):
            output_box.columnconfigure(i, weight=1)

        self._scale(output_box, "R", self.red_var, 0, 0)
        self._scale(output_box, "G", self.green_var, 1, 0)
        self._scale(output_box, "B", self.blue_var, 2, 0)
        ttk.Button(output_box, text="Send LED", command=self.send_led).grid(row=3, column=0, sticky="ew", padx=2, pady=4)
        ttk.Button(output_box, text="Default LED", command=self.send_default_led).grid(row=3, column=1, sticky="ew", padx=2, pady=4)

        self._scale(output_box, "Rumble R", self.rumble_right_var, 4, 0)
        self._scale(output_box, "Rumble L", self.rumble_left_var, 5, 0)
        ttk.Button(output_box, text="Send Rumble", command=self.send_rumble).grid(row=6, column=0, sticky="ew", padx=2, pady=4)
        ttk.Button(output_box, text="Stop Rumble", command=self.stop_rumble).grid(row=6, column=1, sticky="ew", padx=2, pady=4)

        self._scale(output_box, "Trigger force", self.trigger_force_var, 7, 0)
        ttk.Button(output_box, text="Trigger Feedback", command=self.send_trigger_feedback).grid(
            row=8,
            column=0,
            sticky="ew",
            padx=2,
            pady=4,
        )
        ttk.Button(output_box, text="Trigger Off", command=self.send_trigger_off).grid(
            row=8,
            column=1,
            sticky="ew",
            padx=2,
            pady=4,
        )
        ttk.Button(output_box, text="Send Current State", command=self.send_current_state).grid(
            row=9,
            column=0,
            sticky="ew",
            padx=2,
            pady=4,
        )
        ttk.Button(output_box, text="Reset Local State", command=self.reset_local_state).grid(
            row=9,
            column=1,
            sticky="ew",
            padx=2,
            pady=4,
        )
        ttk.Button(output_box, text="HD Haptics Tone", command=self.send_hd_haptics_test).grid(
            row=10,
            column=0,
            sticky="ew",
            padx=2,
            pady=4,
        )
        ttk.Button(output_box, text="Status After HD", command=self.read_serial_status).grid(
            row=10,
            column=1,
            sticky="ew",
            padx=2,
            pady=4,
        )

        log_box = ttk.LabelFrame(main, text="Log")
        log_box.grid(row=3, column=0, columnspan=2, sticky="nsew")
        log_box.columnconfigure(0, weight=1)
        log_box.rowconfigure(0, weight=1)
        main.rowconfigure(3, weight=1)
        self.log_text = ScrolledText(log_box, height=10, wrap="word")
        self.log_text.grid(row=0, column=0, sticky="nsew")

    def _scale(self, parent: ttk.Frame, label: str, var: tk.IntVar, row: int, col: int) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=col, sticky="w")
        ttk.Scale(parent, from_=0, to=255, orient="horizontal", variable=var).grid(
            row=row,
            column=col + 1,
            sticky="ew",
            padx=4,
        )
        ttk.Label(parent, textvariable=var, width=4).grid(row=row, column=col + 2, sticky="w")

    def log(self, text: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.insert("end", f"[{timestamp}] {text}\n")
        self.log_text.see("end")

    def _drain_queue(self) -> None:
        try:
            while True:
                kind, value = self.queue.get_nowait()
                if kind == "log":
                    self.log(str(value))
                elif kind == "error":
                    self.log(f"ERROR: {value}")
                    self.status_var.set(str(value))
                elif kind == "input":
                    self.handle_input_report(value)
                elif kind == "serial":
                    self.log("Serial ds5 status:\n" + str(value).strip())
                elif kind == "pnp":
                    self._update_pnp_tree(value)
        except queue.Empty:
            pass
        self.root.after(50, self._drain_queue)

    def refresh_pnp(self) -> None:
        def worker() -> None:
            try:
                raw = check_m61_usb_windows.pnp_query_json()
                devices = check_m61_usb_windows.parse_devices(raw)
                classification = check_m61_usb_windows.classify_devices(devices)
                self.queue.put(("log", "PnP refresh complete"))
                self.queue.put(("pnp", classification.matching_devices))
            except Exception as exc:
                self.queue.put(("error", f"PnP refresh failed: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    def _update_pnp_tree(self, devices: list[check_m61_usb_windows.UsbDevice]) -> None:
        for item in self.device_tree.get_children():
            self.device_tree.delete(item)
        for device in devices:
            self.device_tree.insert(
                "",
                "end",
                values=(device.status, device.device_class, device.friendly_name, device.instance_id),
            )

    def connect_hid(self) -> None:
        if not self.hid:
            self.status_var.set("HID API unavailable")
            return
        self.stop_input()
        if self.write_handle:
            self.hid.close_handle(self.write_handle)
            self.write_handle = None
        try:
            self.hid_infos = self.hid.find_dualsense_hid()
            if not self.hid_infos:
                self.status_var.set("No VID_054C&PID_0CE6 MI_03 HID path found")
                self.log("No target HID path found. Check USB composite enumeration.")
                return
            self.selected_hid = self.hid_infos[0]
            self.write_handle = self.hid.open_writer(self.selected_hid.path)
            self.hid_path_var.set(self.selected_hid.path)
            caps = self.selected_hid.caps
            self.caps_var.set(
                f"in={caps.input_len} out={caps.output_len} feature={caps.feature_len} "
                f"usage_page=0x{caps.usage_page:04X} usage=0x{caps.usage:04X}"
            )
            self.status_var.set("HID connected")
            self.log(f"Connected HID: {self.selected_hid.path}")
        except Exception as exc:
            self.status_var.set(f"HID connect failed: {exc}")
            self.log(f"HID connect failed: {exc}")

    def start_input(self) -> None:
        if not self.hid or not self.selected_hid:
            self.connect_hid()
        if not self.hid or not self.selected_hid:
            return
        if self.reader and self.reader.is_alive():
            self.log("Input reader already running")
            return
        self.reader = HidReadThread(
            self.hid,
            self.selected_hid.path,
            self.selected_hid.caps.input_len or DS5_INPUT_REPORT_LEN,
            self.queue,
        )
        self.reader.start()
        self.status_var.set("Reading HID input")

    def stop_input(self) -> None:
        if self.reader:
            self.reader.stop()
            self.reader = None

    def handle_input_report(self, data: bytes) -> None:
        self.report_count += 1
        decoded = decode_input_report(data)
        for key, var in self.input_vars.items():
            if key in decoded.fields:
                var.set(decoded.fields[key])
        if self.report_count <= 3 or self.report_count % 100 == 0:
            self.log(f"input#{self.report_count}: {decoded.text}")

    def output_len(self) -> int:
        if self.selected_hid:
            return self.selected_hid.caps.output_len or (DS5_USB_SET_STATE_LEN + 1)
        return DS5_USB_SET_STATE_LEN + 1

    def send_report(self, payload47: bytes, note: str) -> None:
        if len(payload47) != DS5_USB_SET_STATE_LEN:
            raise ValueError(f"payload must be {DS5_USB_SET_STATE_LEN} bytes, got {len(payload47)}")
        if not self.hid or not self.write_handle:
            self.connect_hid()
        if not self.hid or not self.write_handle:
            return
        report = bytes([DS5_OUTPUT_REPORT_ID]) + payload47
        try:
            written = self.hid.write_output_report(self.write_handle, report, self.output_len())
            self.log(f"sent {note}: {len(report)}B report, host write={written}B")
        except Exception as exc:
            self.log(f"send {note} failed: {exc}")
            self.status_var.set(f"send failed: {exc}")

    def send_current_state(self) -> None:
        self.send_report(bytes(self.output_state), "current SetState")

    def reset_local_state(self) -> None:
        self.output_state = bytearray(DEFAULT_SET_STATE_47)
        self.red_var.set(self.output_state[44])
        self.green_var.set(self.output_state[45])
        self.blue_var.set(self.output_state[46])
        self.rumble_right_var.set(0)
        self.rumble_left_var.set(0)
        self.log("Local SetState reset to upstream default")
        self.load_current_output_hex()

    def send_led(self) -> None:
        self.output_state[1] |= 0x0C  # AllowLedColor + ResetLights
        self.output_state[38] |= 0x03  # allow brightness/fade fields
        self.output_state[41] = 0x00
        self.output_state[42] = 0x00
        self.output_state[44] = self.red_var.get() & 0xFF
        self.output_state[45] = self.green_var.get() & 0xFF
        self.output_state[46] = self.blue_var.get() & 0xFF
        self.send_report(bytes(self.output_state), "LED")

    def send_default_led(self) -> None:
        self.red_var.set(0xFF)
        self.green_var.set(0xD7)
        self.blue_var.set(0x00)
        self.send_led()

    def send_rumble(self) -> None:
        self.output_state[0] |= 0x03  # EnableRumbleEmulation + UseRumbleNotHaptics
        self.output_state[2] = self.rumble_right_var.get() & 0xFF
        self.output_state[3] = self.rumble_left_var.get() & 0xFF
        self.send_report(bytes(self.output_state), "rumble")

    def stop_rumble(self) -> None:
        self.rumble_right_var.set(0)
        self.rumble_left_var.set(0)
        self.send_rumble()

    def send_trigger_feedback(self) -> None:
        force = self.trigger_force_var.get() & 0xFF
        # Experimental common feedback-shaped 11-byte payload. The raw field
        # remains editable for exact captured vectors from DSX/Steam.
        effect = bytes([0x21, 0x02, force, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.output_state[0] |= 0x0C  # Allow right + left trigger FFB
        self.output_state[10:21] = effect
        self.output_state[21:32] = effect
        self.send_report(bytes(self.output_state), "trigger feedback experimental")

    def send_trigger_off(self) -> None:
        self.output_state[0] |= 0x0C
        self.output_state[10:21] = bytes(11)
        self.output_state[21:32] = bytes(11)
        self.send_report(bytes(self.output_state), "trigger off")

    def send_hd_haptics_test(self) -> None:
        def worker() -> None:
            try:
                import sounddevice as sd  # type: ignore[import-not-found]
            except Exception as exc:
                self.queue.put(("error", f"sounddevice is required for HD haptics audio test: {exc}"))
                return

            try:
                devices = sd.query_devices()
                target_index = None
                target_name = ""
                for index, device in enumerate(devices):
                    name = str(device.get("name", ""))
                    max_outputs = int(device.get("max_output_channels", 0) or 0)
                    lower = name.lower()
                    if max_outputs >= 4 and ("dualsense" in lower or "wireless controller" in lower):
                        target_index = index
                        target_name = name
                        break
                if target_index is None:
                    self.queue.put(("error", "No 4-channel DualSense output device found for HD haptics"))
                    return

                sample_rate = 48000
                duration_s = 2.0
                chunk_frames = 480
                total_frames = int(sample_rate * duration_s)
                amplitude = 22000
                frequency = 160.0
                frame_index = 0

                self.queue.put(("log", f"HD haptics audio test using output device #{target_index}: {target_name}"))
                with sd.RawOutputStream(
                    samplerate=sample_rate,
                    channels=4,
                    dtype="int16",
                    device=target_index,
                    blocksize=chunk_frames,
                ) as stream:
                    while frame_index < total_frames:
                        frames = min(chunk_frames, total_frames - frame_index)
                        block = bytearray(frames * 4 * 2)
                        pos = 0
                        for _ in range(frames):
                            t = frame_index / sample_rate
                            sample = int(math.sin(2.0 * math.pi * frequency * t) * amplitude)
                            if frame_index > total_frames * 0.75:
                                sample = 0
                            for value in (0, 0, sample, sample):
                                block[pos] = value & 0xFF
                                block[pos + 1] = (value >> 8) & 0xFF
                                pos += 2
                            frame_index += 1
                        stream.write(bytes(block))
                self.queue.put(("log", "HD haptics audio test finished; run Status After HD to check usb_haptics/hidp_haptics"))
            except Exception as exc:
                self.queue.put(("error", f"HD haptics audio test failed: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    def load_current_output_hex(self) -> None:
        self.raw_output.delete("1.0", "end")
        self.raw_output.insert("1.0", self.output_state.hex(" "))

    def send_raw_output(self) -> None:
        try:
            data = parse_hex_bytes(self.raw_output.get("1.0", "end"))
            if len(data) == DS5_USB_SET_STATE_LEN + 1 and data[0] == DS5_OUTPUT_REPORT_ID:
                payload = data[1:]
            elif len(data) == DS5_USB_SET_STATE_LEN:
                payload = data
            else:
                raise ValueError(
                    f"enter 47 payload bytes, or 48 bytes beginning with report id 02; got {len(data)}"
                )
            self.output_state = bytearray(payload)
            self.send_report(bytes(self.output_state), "raw")
        except Exception as exc:
            self.log(f"raw output parse/send failed: {exc}")

    def read_serial_status(self) -> None:
        port = self.port_var.get().strip() or DEFAULT_PORT

        def worker() -> None:
            try:
                import serial  # type: ignore[import-not-found]
            except Exception as exc:
                self.queue.put(("error", f"pyserial is not available: {exc}"))
                return
            try:
                with serial.Serial(port, 115200, timeout=0.05, rtscts=False, dsrdtr=False) as ser:
                    ser.setDTR(False)
                    ser.setRTS(False)
                    time.sleep(0.1)
                    ser.reset_input_buffer()
                    for command in (b"ds5 log quiet\n", b"ds5 status\n"):
                        ser.write(command)
                        ser.flush()
                        time.sleep(0.1)
                    deadline = time.monotonic() + 3.0
                    chunks: list[bytes] = []
                    while time.monotonic() < deadline:
                        data = ser.read(4096)
                        if data:
                            chunks.append(data)
                        else:
                            time.sleep(0.02)
                text = b"".join(chunks).decode("utf-8", errors="replace")
                self.queue.put(("serial", text or "<NO_RESPONSE>"))
            except Exception as exc:
                self.queue.put(("error", f"serial status failed: {exc}"))

        threading.Thread(target=worker, daemon=True).start()

    def on_close(self) -> None:
        self.stop_input()
        if self.hid and self.write_handle:
            self.hid.close_handle(self.write_handle)
        self.root.destroy()


def smoke_test() -> int:
    report = bytes(range(DS5_USB_SET_STATE_LEN))
    decoded = decode_input_report(bytes([DS5_INPUT_REPORT_ID]) + DEFAULT_SET_STATE_63)
    if decoded.fields.get("LX") != str(DEFAULT_SET_STATE_63[0]):
        print("decode smoke failed", file=sys.stderr)
        return 1
    if parse_hex_bytes(report.hex(" ")) != report:
        print("hex parser smoke failed", file=sys.stderr)
        return 1
    print("DS5 Windows test app smoke test passed.")
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--smoke-test" in argv:
        return smoke_test()
    if "--pnp" in argv:
        return check_m61_usb_windows.main([])

    root = tk.Tk()
    app = Ds5WindowsTestApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
