# M61 ESP32 Programming Bridge Firmware

This is a development helper firmware for Ai-M61/BL616/BL618 boards. It exposes
a PC serial bridge through the Ai-M61-32S-Kit CH340/UART0 path, keeps native USB
CDC ACM available when it enumerates correctly, forwards bytes to ESP32 UART0,
and uses two M61 GPIO pins to drive ESP32 `BOOT/IO0` and `EN/RST`.

It is not the final BL618 USB HID gamepad firmware. The final USB HID firmware
must wait until ESP32 Bluetooth stage 1 produces stable full DualSense reports.

Default pins for the current Ai-M61-32S-Kit bridge build:

- host UART: `uart0`, fixed at `115200`
- M61 `GPIO21` TX -> board CH340 RX
- M61 `GPIO22` RX <- board CH340 TX
- target UART: `uart1`
- M61 `GPIO23` TX -> ESP32 `U0RXD/GPIO3`
- M61 `GPIO24` RX <- ESP32 `U0TXD/GPIO1`
- M61 `GPIO27` -> ESP32 `BOOT/IO0`
- M61 `GPIO28` -> ESP32 `EN/RST`

Ai-M61-32S module flashing uses M61 `GPIO2` as its own bootstrap pin: high at
reset enters firmware flashing, low at reset boots normally. That is separate
from ESP32 `BOOT/IO0`, which this bridge drives through `GPIO27` by default.

Build:

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh
```

For BL618DG SDK targets:

```powershell
wsl bash /mnt/c/code/MCU/DS5Dongle/m61/esp32_prog_bridge/build.sh --chip bl618dg --board bl618dgdk --cpu-id ap
```

The build script looks for the T-HEAD Xuantie toolchain in
`$HOME/riscv-toolchain/toolchain_gcc_t-head_linux/bin`,
`/home/ccc007/riscv-toolchain/toolchain_gcc_t-head_linux/bin`, then
`/opt/toolchain_gcc_t-head_linux/bin`. The verified BL616 output path is:

```text
m61/esp32_prog_bridge/build/build_out/m61_esp32_prog_bridge_bl616.bin
```

Flash from Windows after putting M61 into UART download mode manually:

```powershell
python tools\flash_m61_bridge.py -p COM5 --manual-hint
```

The default M61 download baudrate is `460800` because the Ai-M61-32S-Kit host
port is a CH340. Pass `-b` explicitly if your adapter is stable at another
rate.

After this bridge is running once, later updates can ask M61 itself to reboot
into UART download mode before flashing:

```powershell
python tools\flash_m61_bridge.py -p COM5 --reboot-isp
python tools\flash_m61_firmware.py --app bridge -p COM5 --reboot-isp -b 460800
```

The pre-flash reboot request tries `115200`, `2000000`, and `460800` by
default. Override with repeated `--reboot-baud` options if a board is known to
use a different console speed.

The current CH340-host bridge image builds successfully but must be flashed
manually once before `COM5 @ 115200` can be used for `~m61` commands.

Host commands are ASCII lines sent to the CH340 COM port or native USB CDC port:

```text
~m61 boot
~m61 boot-hold
~m61 reset
~m61 run
~m61 reboot-isp
~m61 status
~m61 help
```

All non-command bytes are forwarded to the target UART, so esptool can use the
same host port after `~m61 boot`.

`~m61 reboot-isp` controls the M61 chip itself. `~m61 boot` and
`~m61 boot-hold` control the external ESP32 download pins.
