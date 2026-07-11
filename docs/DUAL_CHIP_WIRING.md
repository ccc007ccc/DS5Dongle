# M61 + ESP32 Dual-Chip Wiring

This file records the first wiring profile based on the provided ESP32-WROOM-32
and Ai-M61-32S pinout images. It is intentionally not enabled by default.

## Recommended Profile: M61 Left Side + ESP32 Left Side

Use a common ground. If both boards are connected to the PC by USB, do not tie
the two 5V/VIN rails together.

| Signal | M61 pin | ESP32 pin | Direction |
| --- | --- | --- | --- |
| SPI SCLK | IO13 | GPIO27 | M61 -> ESP32 |
| SPI MOSI | IO11 | GPIO26 | M61 -> ESP32 |
| SPI MISO | IO10 | GPIO25 | ESP32 -> M61 |
| SPI CS | IO20 | GPIO33 | M61 -> ESP32 |
| ESP_READY | IO16 | GPIO32 | ESP32 -> M61 |
| ESP_IRQ | IO17 | GPIO13 | ESP32 -> M61 |
| RESET | not connected | not connected | optional |
| GND | GND | GND | common ground |

The current ESP32 profile sets the ESP-IDF host value to `2`, which is
`SPI3/VSPI` on ESP32-IDF 5.3.2. `GPIO27/26/25/33` are routed through the GPIO
Matrix; they are not the VSPI native IO_MUX pins `SCLK=18`, `MOSI=23`,
`MISO=19`, `CS=5`. This matters for timing margin:

- 4 MHz is the fallback and protocol-debug profile;
- the ESP32 slave timing limit for Matrix-routed MISO is below 7.2 MHz, so
  8 MHz on the current wiring is an explicit overclock candidate allowed by
  the user only after a minimum 30-minute CRC/sequence stress test;
- 10 MHz is not a valid target for the current Matrix-routed profile;
- 12 MHz and higher are outside this project's ESP32-WROOM SPI target.

The existing `devkit-vspi` profile already selects the native VSPI pins
`18/23/19/5` and is the required wiring path for an in-spec 8 MHz production
profile. Moving to HSPI instead would require host value `1`, would conflict
with the current IRQ on GPIO13, and would require a boot-strapping review for
GPIO12/GPIO15.

The M61 side uses only left-side pins and avoids the current status LED defaults
on IO12/IO14/IO15. The ESP32 side also stays on the left header in the supplied
pinout image. This profile intentionally avoids ESP32 GPIO12 and GPIO15 because
they are boot strapping pins on common ESP32-WROOM-32 dev boards.

## ESP32 Build Profile

Default dual-chip builds keep SPI pins at `-1`, so no SPI hardware is touched.
To build the ESP32 firmware for the wiring above:

```powershell
python tools\build_esp32_stage1.py --backend dual-chip --pin-profile devkit-left
```

This uses `sdkconfig.dual_chip.devkit_left.defaults`.
The dual-chip ESP32 firmware enables auto-connect at boot. It remains
connectable/discoverable for controller-initiated page reconnects (press PS)
while also repeating inquiry rounds. The configured value `30` is the HCI
inquiry duration in 1.28-second units (about 38.4 seconds), not 30 wall-clock
seconds. Continuous inquiry allows PS+Create re-pairing even when a saved bond
has become stale.

The `devkit-vspi` profile uses the right-side native VSPI pins
`GPIO18/GPIO23/GPIO19/GPIO5` plus `GPIO22/GPIO21`. Use it when the boards are no
longer constrained to left-side wiring and an in-spec 8 MHz profile is needed.

## M61 Config Fragment

The matching M61 pin fragment is:

```text
m61/dualsense_hidp_probe/defconfig.dual_chip_left_spi.example
```

The matching M61 build command is:

```bash
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh all --profile dual-chip-left-spi
```

Do not merge it into `m61/dualsense_hidp_probe/defconfig` until the wires are
actually connected. The normal M61 defconfig keeps the dual-chip transport and
SPI hardware disabled.

## Bring-Up Checklist

Build both firmware images before flashing:

```powershell
python tools\build_esp32_stage1.py --backend dual-chip --pin-profile devkit-left
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/dualsense_hidp_probe/build.sh --profile dual-chip-left-spi
```

After flashing and rebooting both boards, run `ds5 status` on the M61 console.
The first useful gate is that the status output contains:

```text
M61 dual-chip mode: local Classic BT host is not started; ESP32 owns HIDP transport
M61 ESP32 dual-chip SPI transport ready ...
esp32_spi ready=1 ... hello_rx=1 ... tsync_rx=1 ... sync=1 ...
esp32_peer_stats role=2 ver=1 ...
```

M61 keeps a small in-RAM ESP32 transport event ring. Use it immediately after a
failed bring-up or wire test:

```text
ds5 esp32-log
ds5 esp32-log clear
```

The log is RAM-backed, so it survives missed serial capture but not a full M61
reset. It records HELLO/TIME_SYNC/BT_STATE/FLOW_CREDIT changes, negative ACKs,
transport errors, BT-not-ready drops, recovery attempts, and wire-test start/end.
In dual-chip mode, M61 treats ESP32 SPI readiness and ESP32 Bluetooth readiness
as separate gates; output/audio reports are held until ESP32 reports raw HIDP
ready through `BT_STATE` or `FLOW_CREDIT`.

To capture the M61 console and check the link in one command:

```powershell
python tools\validate_dual_chip_hardware.py -p COM5 --duration 30
```

Use the actual M61 serial port instead of `COM5`. This command only sends
`ds5 log quiet`, `ds5 esp32-wire-test`, and periodic `ds5 status` commands
through the M61 shell; it does not flash or reset either board.

The LED result is:

| Board | Test phase | Pass | Fail |
| --- | --- | --- | --- |
| M61 RGB LED | blue blinking | green solid | red solid |
| ESP32 LED | blue blinking when `WIRE_TEST_START` is received | green+blue if RGB is wired, usually blue solid on GPIO2-only boards | red if wired, otherwise blue blinking |

M61 green is the authoritative result because it requires READY, SCLK, MOSI,
MISO, CS, ACK, `TIME_SYNC`, `STATS`, and peer role checks to pass. ESP32 LED
activity only proves that at least the M61-to-ESP32 direction reached the
coprocessor. IRQ is reported as a warning instead of a hard failure because
the polling task can consume the response before the test samples the IRQ pin.
When the M61 LED turns red, the M61 console also prints
`esp32_wire_test HINT ...` lines that point to the most likely wire or firmware
profile to check first.

If `ESP_IRQ` is not wired yet, `CONFIG_M61_ESP32_RX_POLL_ENABLE=y` still lets
the M61 poll for pending ESP32 responses at a fixed interval. That is useful
for early bring-up, but wiring `ESP_IRQ` is still preferred so the M61 only
clocks empty SPI transactions when the coprocessor actually has queued data.

If the controller is not connected yet and you only want to prove the SPI link:

```powershell
python tools\validate_dual_chip_hardware.py -p COM5 --duration 15 --spi-only
```

After the controller is connected, require the full-report USB gate as well:

```powershell
python tools\validate_dual_chip_hardware.py -p COM5 --duration 30 --require-full-report --require-usb-after-ds
```

During a 60 s speaker/haptics and microphone run, require realtime traffic and
zero realtime transport errors:

```powershell
python tools\validate_dual_chip_hardware.py -p COM5 --duration 60 --require-full-report --require-usb-after-ds --require-input-reports --require-audio-rt --require-mic-opus --require-no-rt-errors
```

Saved M61 console output can also be checked offline:

```powershell
python tools\check_dual_chip_log.py path\to\m61_dual_chip.log --require-m61-mode --require-transport-ready --require-credit --require-stats --require-ack --require-bt-state
```

If the checker fails before the controller is connected, first relax the live
Bluetooth requirements and confirm the SPI link itself:

```powershell
python tools\check_dual_chip_log.py path\to\m61_dual_chip.log --require-m61-mode --require-transport-ready --require-stats --require-ack
```

Common early failures:

| Symptom | Likely cause |
| --- | --- |
| `ready=0` or no `M61 ESP32 dual-chip SPI transport ready` | ESP_READY not wired, ESP32 not flashed with the left-side profile, or SPI pins mismatch |
| `hello_rx=0` | SCLK/MOSI/MISO/CS wiring or SPI mode is wrong |
| `tsync_rx=0` or `sync=0` | SPI transactions work intermittently or ESP32 task is not responding |
| ESP32 logs occasional `bits=0` | A CS pulse completed without clocks. Current firmware treats it as a zero-length transaction and retains any queued response; old firmware incorrectly reported 692 received bytes and could discard the response. |
| `stats_rx=0` | M61 can transmit but cannot clock back queued ESP32 responses |
| `credit=0` | ESP32 has not emitted FLOW_CREDIT yet, or IRQ/readback path is not active |
| `crc` or `spi_crc` grows | wire length, ground, clock rate, or pin mapping problem |
