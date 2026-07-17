# M61 Runtime DVFS Specification

## 1. Goals

The DVFS service provides a single clock-control path for the shell, future
Web/HID control, and firmware subsystems. It must:

- offer fixed `eco`, `balanced`, and `performance` profiles;
- accept a custom integer CPU frequency;
- support a resident manual lock and a SpeedStep-like realtime governor;
- keep workload requests allocation-free and O(1);
- never reconfigure a PLL in an audio, USB, Bluetooth, or interrupt critical
  path;
- avoid periodic profiling in release firmware;
- persist settings only after an explicit save operation.

This is frequency scaling, not automatic voltage scaling. Core voltage is left
under the existing SDK/silicon policy because changing it during active radio
and USB traffic has not been validated.

The factory/default policy is a resident manual lock at 320 MHz, microphone
disabled, and automatic speaker routing. The internal mono speaker receives an
L/R downmix; inserting a 3.5 mm headset selects native stereo. Realtime scaling
is opt-in only; audio activity never changes the clock while the manual
governor is selected.

## 2. Profiles and ranges

| Profile | CPU | PBCLK | Intended use |
| --- | ---: | ---: | --- |
| `eco` | 320 MHz | 80 MHz | SDK/default clock and idle floor |
| `balanced` | 384 MHz | 96 MHz | one-way realtime audio |
| `performance` | 400 MHz | 100 MHz | full-duplex realtime audio |

Custom frequencies from 320 through 400 MHz are accepted by the normal API.
401 through 480 MHz require an explicit `experimental` flag. Experimental
frequencies cannot be persisted, preventing an unstable setting from creating
an automatic boot loop.

The 40 MHz XTAL conversion follows the BL616 SDK definition exactly:

```text
AUPLL_SDMIN = round(target_MHz * 204.8)
             = (target_MHz * 1024 + 2) / 5
```

320 MHz uses WIFIPLL. Frequencies through 384 MHz use the SDK's 384 MHz AUPLL
analog band; higher frequencies use its 400 MHz band. Switching inside the
same analog band only retunes `SDMIN`; crossing a band reconfigures AUPLL.

## 3. Governors

### Manual/resident

Manual mode locks the requested profile or custom frequency. Audio floors and
temporary boosts are recorded but do not override the lock. This is the mode
for deterministic benchmarks and a Web UI's “fixed frequency” setting.

### Realtime

Realtime mode chooses the maximum of all active client floors and boosts:

- no active audio: 320 MHz;
- speaker or microphone only: 384 MHz;
- speaker and microphone together: 400 MHz.

“Active” means actual encode/decode work within a 250 ms lease, not merely an
open Windows USB endpoint. A steady codec stream only refreshes two timestamps;
it does not wake the worker on every frame. Upshifts are immediate. Downshifts
require 2 seconds of continuously lower demand and a minimum 500 ms residency.
The governor therefore does not switch on every 10 ms audio epoch and cannot
create frame-by-frame PLL jitter.

## 4. Execution model

Callers only update a fixed client slot and notify one statically allocated
worker task. There is no heap allocation, queue copy, sampling counter, timer
callback, or polling loop in a request path. Calls are valid in task or ISR
context.

Only the DVFS worker changes hardware clocks. Before touching AUPLL it:

1. masks task/interrupt preemption for the short transition;
2. moves the CPU root to RC32M;
3. configures or fine-tunes AUPLL;
4. selects WIFIPLL or AUPLL and restores HCLK `/1`, PBCLK `/4`;
5. restores the prior XCLK selection and exits the critical section.

The transition routine is linked into the SDK clock SRAM/ITCM section. This
fixes the unsafe old sequence that could power-cycle AUPLL while the CPU was
still executing from it.

## 5. Firmware API

The public interface is in `m61_dvfs.h`:

- `m61_dvfs_set_profile()` — lock a fixed profile;
- `m61_dvfs_set_custom_frequency()` — lock an integer frequency;
- `m61_dvfs_set_governor()` — select manual or realtime operation;
- `m61_dvfs_request_floor()` — publish/release a persistent client floor;
- `m61_dvfs_request_boost()` — request a bounded temporary boost;
- `m61_dvfs_set_audio_enabled()` — publish audio endpoint availability;
- `m61_dvfs_note_audio_activity()` — refresh actual codec activity leases;
- `m61_dvfs_get_status()` — O(1) state snapshot for shell/Web status;
- `m61_dvfs_measure_cpu_mhz()` — explicit 2 ms diagnostic measurement;
- `m61_dvfs_save_persistent_config()` and
  `m61_dvfs_clear_persistent_config()` — explicit Flash lifecycle.

The future Web driver should call these APIs through its transport adapter; it
must not write PLL registers or EasyFlash directly. Interactive slider changes
remain RAM-only. A separate “Save as startup setting” action performs the one
Flash write.

## 6. Shell contract

```text
m61 clock status
m61 clock profile eco|balanced|performance
m61 clock lock <profile|320..400>
m61 clock lock <401..480> experimental
m61 clock governor manual|realtime
m61 clock boost <320..400> <hold-ms>
m61 clock save
m61 clock clear-saved
```

Legacy forms such as `m61 clock 400` remain resident manual locks.

## 7. Validation gates

Before treating runtime DVFS as release-ready:

1. boot and report 320 MHz with automatic speaker routing and microphone
   disabled;
2. switch 320 -> 384 -> custom 350 -> 400 -> 320 without reconnecting USB or
   Bluetooth;
3. verify the reported actual clock and PBCLK at every step;
4. run the normal 90-second audio test at locked 320 MHz;
5. run the same test at locked 400 MHz;
6. enable realtime mode and toggle mic/speaker load, confirming hysteresis and
   zero codec/BT/USB errors;
7. save a validated setting, reboot once, verify restore, then clear it.

Speaker routing is intentionally independent of the clock governor. The
DualSense full input report supplies the 3.5 mm jack bit. `auto` maps an empty
jack to Opus mono and an inserted headset to Opus stereo; `mono` and `stereo`
are explicit diagnostic overrides. A route change advances the audio
generation so packets encoded for one physical endpoint cannot be relabelled
and sent to the other.
