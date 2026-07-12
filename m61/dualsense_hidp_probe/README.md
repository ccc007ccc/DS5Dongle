# M61 DualSense HIDP Probe

This is the Ai-M61/BL616/BL618 M61-only DualSense path. It connects to a real
DualSense over Classic Bluetooth HIDP and now also exposes a Sony
`054C:0CE6` DualSense composite USB device through the BL618 native USB device
peripheral.

The probe validates these pieces:

- BR/EDR controller startup with `ble1m2s1bredr1`
- inquiry scan for `DualSense Wireless Controller`
- ACL connection by Bluetooth address
- saved last DualSense address through EasyFlash and `ds5 autoconnect`
- boot-time auto connect or scan, followed by automatic HIDP bring-up retries
- BR/EDR pairing/security callback flow
- SDP query for HID service `0x1124`
- L2CAP HID Control PSM `0x11`
- L2CAP HID Interrupt PSM `0x13`
- raw HIDP packet logging, shared DualSense input parsing, and optional raw packet send commands
- HIDP bring-up attempts for Set Protocol, feature report reads, and DualSense output init reports
- USB DualSense composite device with Audio Control, Audio OUT, Audio IN, and
  HID interface 3

The USB composite device appears on the BL618 native USB `USB_DP`/`USB_DM`
pins. A board USB connector wired only to CH340 remains a serial/debug port and
will not show up as a controller or audio device.

Build:

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh
```

Flash after putting M61 into UART download mode:

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 --manual-hint
```

The default M61 download baudrate is `460800` for the Ai-M61-32S-Kit CH340
path. Pass `-b` explicitly if needed.

After this probe or the M61 bridge is running once, later updates can request a
software reboot into UART download mode before flashing:

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 --reboot-isp -b 460800
```

The pre-flash reboot request tries `115200`, `2000000`, and `460800` by
default. Override with repeated `--reboot-baud` options if a board is known to
use a different console speed.

Check a captured runtime log:

```powershell
python tools\check_m61_hidp_log.py m61_hidp.log --require-auto --require-hidp --require-full-report --require-input-activity
```

Capture and check in one step:

```powershell
python tools\validate_m61_hidp_hardware.py -p COM5
```

Useful shell commands after flashing this probe to M61:

```text
ds5 status
ds5 auto [on|off|now]
ds5 scan
ds5 autoconnect
ds5 connect <aa:bb:cc:dd:ee:ff|last>
ds5 security
ds5 sdp
ds5 hidp
ds5 bringup
ds5 log [normal|quiet]
ds5 set-protocol
ds5 get-feature <id_hex>
ds5 output-init
ds5 send-ctrl <hex>
ds5 send-intr <hex>
ds5 forget
ds5 disconnect
ds5 reboot-isp
m61 reboot-isp
```

Run the deterministic speaker, HD-haptics, and HID-output performance load
from Windows (requires `numpy`, `sounddevice`, `pyserial`, and the four-channel
DualSense USB audio endpoint):

```powershell
python tools\run_m61_full_load.py --duration 90 `
  --status-log artifacts\m61_full_load_status.log
python tools\compare_m61_perf_status.py `
  artifacts\m61_full_load_status_before.log `
  artifacts\m61_full_load_status.log
```

The default amplitudes are intentionally quiet. The sample rate, four-channel
layout, 10 ms audio blocks, and 20 ms HID output cadence remain fixed so A/B
results are comparable without requiring loud playback.

On boot, the probe now starts this automatic sequence:

```text
load saved DualSense address
connect saved address, or scan if none is saved
request BR/EDR security
run HID SDP
open HID Control and Interrupt L2CAP channels
retry DualSense bring-up until report=0x31 appears or attempts are exhausted
```

`ds5 auto off` disables that loop for manual packet experiments. `ds5 auto now`
reenables it and starts immediately.

After one successful scan or connection, `ds5 autoconnect` uses the saved
DualSense address. `ds5 forget` clears the probe's saved address; Bluetooth
bond data is still owned by the SDK settings layer.

The first runtime goal is seeing parsed HID Interrupt input frames. `report=0x01
mode=basic` proves the raw HIDP input path is alive; `report=0x31 mode=full`
is the required proof that M61-only can reach full DualSense input quality.
After the PC enumerates native USB, `ds5 status` reports `usb_gamepad
configured=1` and the `sent` counter increases while input reports arrive.
If continuous HIDP logs hide the status response, send `ds5 log quiet` first;
`ds5 log normal` restores per-report logging for validation captures.
