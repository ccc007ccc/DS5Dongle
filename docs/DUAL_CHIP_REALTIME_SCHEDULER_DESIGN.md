# Dual-Chip Realtime Scheduler Design

Implementation status (2026-07-11): the scheduler/queue refactor described
here is implemented and builds on both targets. Hardware qualification is
intentionally deferred until both boards can be flashed together. The current
candidate is built at 8 MHz on the existing Matrix-routed breadboard wiring;
that remains an explicit overclock qualification, while native VSPI
`18/23/19/5` is the in-spec 8 MHz production wiring.

Status: Proposed architecture

Date: 2026-07-10

## 1. Purpose

This document defines the long-term scheduling and queue architecture for the
M61 + ESP32 DualSense bridge. It replaces the incremental approach of adding
depth to generic queues whenever a drop is observed.

The target is a console-class experience:

- stable speaker audio and HD haptics with no periodic gaps;
- bounded and normally near-zero transport queueing delay;
- high USB input report rate without losing the latest controller state;
- fast LED, trigger, rumble, mute, and audio-control updates;
- no low-priority diagnostic or maintenance work on a realtime critical path;
- fixed memory use, measurable queue conservation, and maintainable ownership.

This is an architecture document. Exact interfaces, constants, state
transitions, and acceptance tests are defined in the companion scheduler spec.

## 2. Evidence From The Current System

The design is based on observed hardware behavior, not only source review.

### 2.1 Audio deadline drops were artificial

The M61 successfully produced and transferred `7936` realtime reports while
the ESP32 sent `7646`; ESP32 `deadline36=289` accounted for all but one report
in flight. Opus, haptics, and M61 output queues reported no drops.

The deadline was exactly one report period. Normal task jitter therefore
discarded a complete 20 ms audio/haptics report even when no newer report was
ready to send. The 32-bit microsecond deadline also wrapped after about 71.6
minutes while ESP32 compared it with a non-wrapping 64-bit clock, after which
nearly every realtime report was rejected.

Conclusion: cross-chip absolute deadlines are not the audio clock and must not
be used as the primary congestion mechanism.

### 2.2 Generic FIFO accumulation caused time-dependent failures

After deadline removal, ESP32 reported `err=-105` (`ENOBUFS`). Repeated normal
`0x31`/`0x32` state reports accumulated in the same L2CAP send FIFO as `0x39`.
The FIFO eventually filled, so audio and haptics failed more often the longer
the stream ran.

Conclusion: replaceable state and periodic media cannot share ordinary FIFO
semantics.

### 2.3 Multiple SPI callers caused priority inversion

Over one measured interval:

```text
audio send errors +58
time sync failures +57
```

Periodic time sync, the 1 ms RX poll task, feature/status traffic, and audio
submission all used the same SPI mutex and could wait for ESP READY while
holding that mutex. A low-priority maintenance exchange could make the audio
path time out.

Conclusion: exactly one task must own the SPI peripheral. Producers submit
work asynchronously and never wait on the bus mutex.

### 2.4 Flow-credit traffic became load

ESP32 emitted flow credit when a TX item was accepted and again when it was
consumed. Credit responses were pushed to the front of the response queue and
were not initially coalesced. Under steady load this kept IRQ asserted,
increased RX polling, displaced useful responses, and generated stale capacity
snapshots.

Conclusion: telemetry and backpressure are per-class state snapshots, not an
event for every packet.

### 2.5 The current SPI transaction size limits report rate

The v1 transport clocks a fixed 692-byte transaction. Raw transfer time is:

```text
692 bytes * 8 / 4 MHz = 1.384 ms
```

This excludes chip-select, task wakeup, driver, and turnaround overhead. A
fixed large transaction at 4 MHz cannot provide a reliable 1 kHz service rate.
It also wastes most clocks when carrying a 63-78 byte input or state report.

Conclusion: 4 MHz fixed-MTU mode can be a compatibility mode, but it is not the
high-report-rate target.

## 3. Design Principles

1. **One owner per hardware scheduler.** One M61 task owns SPI. The BTstack run
   loop owns L2CAP interrupt transmission.
2. **One canonical congestion point per traffic class.** Do not duplicate the
   same audio report in SPI, command, and L2CAP FIFOs with independent drop
   decisions.
3. **Queue semantics follow data semantics.** Media uses a bounded chronological
   ring, state uses keyed mailboxes, reliable control uses a FIFO, and telemetry
   uses coalesced snapshots.
4. **Freshness is measured by age.** Depth is a memory limit; maximum age is the
   latency limit.
5. **Realtime paths never block.** USB callbacks, audio production, controller
   RX callbacks, and report submission perform bounded copies/state updates.
6. **Low priority work is pull-based or idle-only.** Logs, stats, time sync, NVS,
   and connection maintenance cannot consume realtime service slots.
7. **All storage is fixed.** No heap allocation, flash write, formatting, or
   unbounded retry is allowed in a realtime path.
8. **Every loss is attributable.** Per-class conservation counters must explain
   every accepted, replaced, stale, rejected, transmitted, and failed item.

## 4. Traffic Classes

The scheduler does not operate on a generic "report" priority. Each message is
classified when it enters the system.

| Class | Examples | Semantics | Congestion policy |
| --- | --- | --- | --- |
| `RT_ACTUATION` | BT `0x39` speaker + haptics | ordered periodic media | two-report chronological ring; replace oldest only when full; hard age cap |
| `LATEST_STATE_31` | BT `0x31` LED, trigger, rumble, audio control | keyed current state | one mailbox; overwrite unsent value |
| `LATEST_STATE_32` | BT `0x32` mic/audio status | keyed current state | one mailbox; overwrite unsent value |
| `RELIABLE_CONTROL` | connect, disconnect, forget, feature get/set | ordered command/response | bounded FIFO with ACK, timeout, and limited retry |
| `INPUT_REALTIME` | controller BT input `0x31` | newest sampled controller state | timestamped small ring/mailbox selected by USB polling mode |
| `MIC_STREAM` | controller microphone Opus | ordered lossy media | bounded FIFO; drop oldest and expose loss for PLC |
| `LINK_STATE` | connected, MTU, RSSI, generation | keyed state snapshot | mailbox; publish on transition |
| `TELEMETRY` | stats, credits, diagnostics | observation only | coalesced snapshot; requested or idle-only |

Feature responses are reliable control data. They must never share a FIFO with
input reports or telemetry.

## 5. Clock Model

### 5.1 Audio and haptics clock

The M61 USB audio path is the authority for `0x39` production.

- USB Audio OUT is 48 kHz, four channels, 16 bit.
- Speaker input accumulates 512 stereo frames and resamples to a valid 480-frame
  10 ms Opus block.
- Haptics reduces the corresponding PCM to one 64-byte block.
- Two consecutive completed blocks form one `0x39` report.
- The resulting report cadence is 46.875 Hz, or one report every about 21.333 ms.

The implementation must represent the speaker and haptics data from the same
production epoch in one logical object. Independent speaker and haptics queues
may not silently drift and later be paired by arrival order.

Proposed logical object:

```c
typedef struct {
    uint32_t epoch;
    uint64_t captured_us;
    uint8_t haptics[64];
    uint8_t speaker_opus[200];
    bool speaker_valid;
} ds5_audio_block_t;
```

Two adjacent epochs are assembled into a `ds5_rt_report_t`. Silent speaker or
haptics content is explicit data, not absence of a queue item.

### 5.2 Transport clocks

M61 timestamps an assembled report using its monotonic clock. ESP32 records its
local arrival time. These timestamps are for diagnostics and local age limits.

No periodic cross-chip time synchronization is required for normal audio
scheduling. A one-time time exchange may remain for correlated traces, but a
clock offset must not decide whether an otherwise sendable `0x39` is dropped.

### 5.3 Freshness limits

Initial limits, subject to hardware validation:

| Class | Soft target | Hard limit |
| --- | ---: | ---: |
| `RT_ACTUATION` M61 assembly to SPI acceptance | < 1 ms | 10 ms |
| `RT_ACTUATION` ESP arrival to L2CAP submit | < 5 ms | 64 ms |
| `LATEST_STATE_31` host update to L2CAP submit | < 8 ms | 50 ms, then newest only |
| `LATEST_STATE_32` transition to L2CAP submit | < 10 ms | 100 ms, then newest only |
| `INPUT_REALTIME` BT receive to USB submit | < 4 ms | 8 ms |
| `MIC_STREAM` BT receive to M61 decode queue | < 10 ms | 40 ms |

The 64 ms realtime hard cap is an emergency bound, not a target buffer depth.
Normal operation should keep the realtime queue empty or at one in-flight item.

## 6. M61 Architecture

### 6.1 Task ownership

The long-term M61 tasks are:

| Task | Responsibility | Blocking allowed |
| --- | --- | --- |
| USB device/ISR callbacks | bounded endpoint copies and mailbox updates | no |
| `ds5_audio_codec` | speaker resample/Opus encode and mic decode | only on task notification/own queues |
| `ds5_rt_assembler` | epoch pairing and `0x39` assembly | no peripheral waits |
| `ds5_spi_scheduler` | sole SPI owner, arbitration, RX dispatch | SPI driver only |
| `ds5_usb_input_pump` | submit newest eligible controller input when HID IN is free | USB endpoint only |
| control/connection tasks | slow state machines | yes, outside realtime paths |

The current catch-all 1 ms USB bridge loop is split. Feature handling, host
state forwarding, audio assembly, and transport arbitration no longer share a
single loop iteration budget.

Module separation does not require one FreeRTOS task per module. Lightweight
input pumping and realtime assembly SHOULD share a realtime dispatcher when
that avoids unnecessary stacks. Adding or enlarging a task is forbidden unless
the RAM budget tool is updated and still passes.

### 6.2 M61 outbound storage

- `RT_ACTUATION`: ring depth 2 of complete `0x39` reports, with original epoch
  and capture timestamp.
- `LATEST_STATE_31`: one mailbox.
- `LATEST_STATE_32`: one mailbox.
- `RELIABLE_CONTROL`: FIFO depth 8, small payload objects from a fixed pool.
- `TELEMETRY`: one request bit and one response snapshot, not a FIFO.

Submitting state or media is non-blocking. Reliable-control submission may
return `EAGAIN` to a slow-path caller; it must not spin in a USB callback.

### 6.3 M61 inbound storage

- `INPUT_REALTIME`: depth 4 timestamped reports in realtime mode; in 250/500 Hz
  modes the scheduler may keep only the newest report before the next USB slot.
- `MIC_STREAM`: FIFO depth 4 Opus packets.
- feature responses: FIFO depth 8 or transaction table keyed by report ID.
- link state and telemetry: mailboxes.

USB HID IN completion triggers the input pump. If fresher controller data
arrived while the endpoint was busy, the pump submits that newest data without
waiting for another Bluetooth report.

### 6.4 M61 SPI scheduler

All existing synchronous `send_payload()` callers become asynchronous
submissions to the scheduler. The RX poll task, periodic time-sync task, and
producer-side SPI mutex are removed.

The scheduler wakes on:

- a new outbound class item;
- ESP IRQ assertion;
- completion of the prior SPI transaction;
- a slow maintenance timer when the system is otherwise idle.

Selection order is deadline/freshness aware:

1. service a ready `RT_ACTUATION` item;
2. service ESP IRQ / pending `INPUT_REALTIME` receive;
3. service aged `RELIABLE_CONTROL`;
4. service `LATEST_STATE_31` then `LATEST_STATE_32`;
5. service `MIC_STREAM` receive;
6. service telemetry only if no realtime work is ready.

SPI is full duplex. Whenever possible, the selected outbound item is combined
with a useful inbound response window.

## 7. ESP32 Architecture

### 7.1 Core ownership

- Core 0: BT controller/BTstack run loop and L2CAP/HIDP state machines.
- Core 1: SPI slave transactions, frame validation, and response staging.

Core 1 never calls L2CAP directly. Core 0 never blocks on SPI.

### 7.2 Canonical BT interrupt scheduler

There is one canonical set of pending interrupt reports owned by the BTstack
module:

- `RT_ACTUATION` ring depth 2;
- one `0x31` mailbox;
- one `0x32` mailbox.

The current pattern of an SPI TX queue, a realtime command queue, and a mixed
L2CAP FIFO all independently holding the same report is removed. Cross-core
submission copies directly into the canonical slots under a short critical
section and schedules a BTstack run-loop callback.

Connection commands and feature control remain in a separate bounded control
queue because they have different ordering and reliability semantics.

### 7.3 CAN_SEND arbitration

On `L2CAP_EVENT_CAN_SEND_NOW`:

1. discard only realtime items older than the hard age cap;
2. select the oldest realtime report when one is pending;
3. after at most three consecutive realtime sends, send one pending state
   mailbox if present;
4. request another CAN_SEND event while any canonical slot remains pending.

State mailboxes remain keyed and replaceable. They cannot fill the realtime
capacity. A full normal/control FIFO cannot prevent accepting a new `0x39`.

L2CAP send failure is counted by class and error code. Realtime data is not
retried after an actual L2CAP failure; state remains dirty so its newest value
can be sent later; reliable control follows its own limited retry policy.

### 7.4 ESP-to-M61 response classes

The response side is also typed:

- input: high-priority timestamped ring/mailbox;
- mic: bounded FIFO;
- feature response: reliable FIFO;
- link state: mailbox;
- flow/telemetry: coalesced mailbox.

An input storm must not displace feature responses. Telemetry must not remain
at the head of the response path or hold IRQ continuously.

## 8. SPI Transport Capacity And Protocol Evolution

### 8.1 Required service rate

A high-report-rate configuration can require approximately:

- up to 500 controller input reports/s as a practical initial target;
- 46.875 realtime `0x39` reports/s;
- bursty state reports, bounded to at most 100/s;
- mic, feature, and link traffic.

Full-duplex pairing reduces the number of physical transactions, but fixed
692-byte transactions at 4 MHz leave insufficient deterministic margin.

### 8.2 Physical target

- 4 MHz is the protocol-development and fallback profile;
- 8 MHz is the intended production profile on native IO_MUX wiring; on the
  current Matrix-routed breadboard it is a user-approved overclock candidate
  requiring a minimum 30-minute continuous CRC/sequence stress test;
- 10 MHz is margin characterization only on native IO_MUX wiring and is not a
  valid target for the current GPIO Matrix profile;
- no design or acceptance gate may require 12 MHz;
- signal-integrity qualification is required separately on the breadboard and
  final PCB.

The current ESP32 profile uses ESP-IDF host value `2`, which is `SPI3/VSPI`,
with SCLK/MOSI/MISO/CS on `GPIO27/26/25/33`. Those are GPIO Matrix routes, not
the VSPI native IO_MUX pins `GPIO18/23/19/5`. ESP-IDF documents the classic
ESP32 slave MISO limit as below 7.2 MHz through the GPIO Matrix, so 8 MHz on the
current breadboard is explicitly outside the guaranteed timing envelope. The
native `devkit-vspi` profile is the production path for in-spec 8 MHz operation.

### 8.3 Protocol v2 transaction windows

Protocol v2 should support at least three master-selected windows:

| Window | Intended data |
| --- | --- |
| small, about 128 B | input, `0x31`, ACK, link state |
| medium, about 256 B | mic Opus, feature fragments |
| large, current 692 B | complete `0x39`, large feature data |

If ESP has a pending response larger than the current master window, it returns
a small pending descriptor containing class, total length, and sequence. M61
then clocks an appropriate window. Fragmentation is reserved for reliable slow
control; realtime `0x39` remains a single large transaction.

Protocol v2 is negotiated through `HELLO`. A v1 compatibility path remains
available during migration.

## 9. Backpressure

Generic `free=N` flow credit is insufficient because one free count cannot
describe media rings, keyed mailboxes, reliable control, and response traffic.

The scheduler uses per-class status:

```text
rt_pending / rt_capacity
state31_dirty
state32_dirty
control_pending / control_capacity
input_pending / input_capacity
mic_pending / mic_capacity
bt_ready
```

This status is coalesced and sent only on meaningful transitions, on explicit
request, or at a low idle heartbeat. It is not emitted twice per report.

Backpressure policies are local and deterministic:

- realtime full: replace oldest realtime and increment `replaced_oldest`;
- state dirty: overwrite mailbox and increment `coalesced`;
- reliable control full: reject submission and retry from the slow path;
- input full: preserve newest according to polling mode and count skipped
  intermediate samples;
- mic full: drop oldest and notify decoder loss/PLC accounting.

## 10. CPU, Memory, And Audio Quality

Current M61 static RAM use after restoring stereo 160 kbps fullband Opus is
about 63.25% of 415 KiB, leaving sufficient space for typed scheduler objects.
All queue objects must remain static and the target total RAM use is below 75%.

The budget is executable in `tools/scheduler_ram_budget.py`. The current
forecast is:

| Target | Current | Planned typed storage | Replaced storage | Forecast with contingency |
| --- | ---: | ---: | ---: | ---: |
| M61 static RAM | 268,800 B | 20,040 B | 14,542 B | 282,490 B including 8 KiB reserve |
| M61 minimum heap | 154,544 B | 6,400 B estimated net task/TCB cost plus 5,498 B linked-static growth | n/a | 126,262 B after charging both 8 KiB static and heap contingencies |
| ESP32 static DRAM | 71,604 B | 10,952 B | 12,560 B | 78,188 B including 8 KiB reserve |
| ESP32 runtime queue payload | allocated dynamically | canonical storage is static | at least 13,888 B reclaimed | net reduction |

M61 remains 36,302 B below its 75% static limit after contingency, and its
forecast minimum heap remains above the 100 KiB gate. The budget script MUST be
updated before any capacity, payload pool, task stack, or slot layout change.
Compiler `_Static_assert` checks later bind the model to actual `sizeof` values.

Forecasting is not the release gate. `tools/check_scheduler_memory.py` closes
the budget against each linked M61 ELF and ESP32 map. It derives M61 static
OCRAM and initial heap capacity from `__ram_start__`, `__HeapBase`, and
`__HeapLimit`, and derives ESP32 DRAM used/total by invoking the official IDF
`idf_size.py --format json2` tool. The current linked example measures
276,784 B M61 static, 148,176 B initial M61 heap, and 68,548 / 124,580 B ESP32
DRAM. After reserves, the checked values are 284,976 B M61 static, 131,792 B
M61 heap, and 76,740 B ESP32 DRAM.

Migration symbol ownership is also a post-link concern. The checker accepts
phase-specific required and forbidden symbols from configuration or CLI. No
future scheduler symbol is hard-coded into the default phase; once final
module names and retired paths are stable, CI supplies the `final` phase
contract and fails on either missing new ownership or surviving legacy paths.

Speaker quality target:

- 48 kHz stereo;
- 160 kbps CBR;
- Opus fullband, `OPUS_APPLICATION_AUDIO`, complexity 0;
- 10 ms, exactly 200 bytes after valid Opus padding;
- no mono downmix.

Codec targets:

- encode average < 8 ms;
- encode P99 < 10 ms;
- no sustained frame queue growth;
- no encode or output queue drops in a 30-minute test.

The existing linear 512-to-480 resampler is acceptable for transport bring-up
but is not the final console-quality resampler. A band-limited 51.2-to-48 kHz
implementation, validated against the pinned upstream WDL behavior, is a later
quality phase and must preserve the same epoch/cadence model.

## 11. Observability And Invariants

Each class exposes:

- accepted;
- transmitted;
- currently pending;
- high-water mark;
- coalesced/replaced;
- stale drops;
- admission rejects;
- transport errors;
- age last/average/max and age histogram buckets;
- inter-send gap last/average/max and threshold counts.

Required conservation examples:

```text
rt_accepted = rt_transmitted + rt_replaced + rt_stale + rt_failed + rt_pending
control_accepted = control_completed + control_failed + control_pending
input_received = input_usb_sent + input_coalesced + input_dropped + input_pending
```

Counters are 64-bit in long-running diagnostics or have explicit wrap-aware
handling. Stats collection takes a snapshot and never formats logs in a
realtime callback.

Optional trace GPIOs should mark:

- USB audio epoch complete;
- `0x39` SPI transaction start;
- ESP `0x39` accepted;
- L2CAP send;
- BT input receive;
- USB HID IN submit.

These signals allow a logic analyzer to measure real latency rather than infer
it only from counters.

## 12. Failure And Recovery

- Controller disconnect atomically clears media/state pending data for that
  controller generation.
- M61 or ESP reset increments a generation and invalidates old pending data.
- Reconnect control is reliable but never runs in realtime callbacks.
- NVS writes occur only after link validation and outside hot paths.
- A stalled telemetry request cannot trigger transport reset.
- A sustained realtime hard-age violation is reported as overload; the system
  does not hide it by increasing queue depth.

## 13. Quantitative Acceptance Targets

### 13.1 Input

- 500 Hz mode sustained without SPI queue growth.
- BT receive to USB submit P99 < 4 ms, maximum < 8 ms.
- USB busy handling always sends the newest pending state on endpoint
  completion.

### 13.2 Audio and haptics

- exact 46.875 Hz long-term `0x39` cadence for the 512-to-480 pipeline;
- 30 minutes simultaneous stereo speaker and nonzero haptics;
- no audible gaps and no periodic vibration interruption;
- zero normal-operation realtime replacement/stale/error counts;
- L2CAP inter-send gap P99 < 25 ms and no gap > 40 ms while streaming, except
  an explicitly correlated radio/link event;
- queue age normally below one report period and always below the hard cap.

### 13.3 State and control

- `0x31` host update to L2CAP submit P99 < 8 ms;
- repeated identical/obsolete host state does not increase queue depth;
- feature and connection control complete under continuous audio/input load;
- telemetry causes no measurable realtime loss.

### 13.4 Resources

- M61 RAM < 75%, minimum free heap > 100 KiB;
- ESP32 minimum free heap target > 64 KiB after link establishment;
- no dynamic allocation in realtime submit/send/receive paths;
- no watchdog reset, USB detach, or BT reconnect during a 60-minute mixed-load
  soak test.

## 14. Migration Plan

1. Add scheduler simulation tests, per-class counters, age fields, and trace
   points without changing wire behavior.
2. Introduce typed message/class definitions shared by M61 and ESP32.
3. Replace M61 synchronous SPI calls and RX poll mutex with one asynchronous
   `ds5_spi_scheduler` owner task.
4. Replace ESP32 report command queues plus mixed L2CAP FIFO with canonical
   realtime/state slots owned by the BTstack scheduler.
5. Introduce audio epochs so speaker and haptics are paired by production time.
6. Add protocol v2 variable transaction windows and validate the 8 MHz profile;
   10 MHz is margin characterization only.
7. Restore/validate full stereo audio quality and replace the linear resampler.
8. Run staged hardware latency, load, recovery, and soak gates before deleting
   v1 compatibility code.

Each phase must keep both firmware images buildable. Wire-protocol changes are
capability negotiated and are not activated until both sides advertise them.

## 15. Non-Goals

- retransmitting stale audio or input reports;
- unbounded jitter buffers;
- hiding overload by increasing every queue depth;
- using logs or periodic time sync as realtime transport work;
- moving USB protocol ownership or Opus encoding to ESP32;
- allowing ESP32 to rewrite DualSense payload semantics.

## 16. Open Validation Questions

The spec will assign provisional values, but hardware tests must answer:

- highest error-free SPI clock on the current breadboard and final PCB;
- actual DualSense BT input cadence and ESP32 HCI ACL buffer service rate;
- P99 stereo/fullband Opus encode time after the final resampler;
- whether `audio_buffer_length` changes controller-side latency materially;
- best small/medium SPI transaction sizes for ESP-IDF slave DMA;
- whether 500 Hz is sustainable with full mic traffic, and what is required for
  a true 1 kHz realtime mode.
