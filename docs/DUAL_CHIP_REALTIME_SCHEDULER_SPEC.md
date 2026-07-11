# Dual-Chip Realtime Scheduler Specification

Status: Software implementation complete; hardware qualification pending

Date: 2026-07-10

Depends on: `docs/DUAL_CHIP_REALTIME_SCHEDULER_DESIGN.md`

Implementation checkpoint (2026-07-11):

- shared ABI/types, host behavior model, and post-link RAM gates pass;
- M61 USB callbacks are deferred and HID IN completion retries latest pending;
- M61 has one SPI owner with typed RT/state/control storage;
- speaker and haptics use eight keyed audio epochs and adjacent-pair assembly;
- ESP32 uses canonical interrupt slots, a separate control CAN_SEND FIFO, and
  typed response stores with IRQ state updated under the store lock;
- both firmware targets build successfully at the selected 8 MHz M61 profile;
- no firmware has been flashed from this checkpoint; hardware latency, audio,
  haptics, reconnect, and long-run signal-integrity gates remain pending.

## 1. Normative Language

`MUST`, `MUST NOT`, `SHOULD`, and `MAY` are normative requirements.

This spec defines the target scheduler. Temporary compatibility adapters may
exist during migration, but a phase is not complete while its target path still
depends on the old queue semantics.

## 2. Required Modules

### 2.1 Shared definitions

Add a shared scheduler header usable by M61 and ESP32:

```text
main/dual_chip_scheduler_types.h
```

It MUST define:

```c
typedef enum {
    DS5_SCHED_RT_ACTUATION = 1,
    DS5_SCHED_LATEST_STATE_31,
    DS5_SCHED_LATEST_STATE_32,
    DS5_SCHED_RELIABLE_CONTROL,
    DS5_SCHED_INPUT_REALTIME,
    DS5_SCHED_MIC_STREAM,
    DS5_SCHED_LINK_STATE,
    DS5_SCHED_TELEMETRY,
} ds5_sched_class_t;

typedef struct {
    uint64_t created_us;
    uint32_t generation;
    uint32_t sequence;
    uint32_t reserved; /* must be zero; fixes the cross-ABI layout at 24 B */
    uint16_t length;
    uint8_t sched_class;
    uint8_t flags;
} ds5_sched_meta_t;
```

Internal counters MUST use at least 64 bits. Wire snapshots MAY use 32 bits only
when wrap is explicit and tested.

The `reserved` field MUST be initialized to zero. Both targets MUST compile
`_Static_assert(sizeof(ds5_sched_meta_t) == 24, ...)` and field-offset asserts;
the layout must not depend on ABI tail padding.

### 2.2 M61 modules

The target M61 implementation MUST separate:

```text
m61_audio_epoch.c/.h       USB audio epoch production and Opus completion
m61_ds5_rt_assembler.c/.h  two-epoch 0x39 assembly
m61_spi_scheduler.c/.h     sole SPI peripheral owner
m61_usb_input_pump.c/.h    latest controller input to HID IN scheduling
```

Names may vary only if the same ownership boundaries remain clear.

`m61_esp32_transport.c` becomes a compatibility/API facade over
`m61_spi_scheduler`. It MUST NOT perform producer-context SPI exchanges after
the scheduler migration phase.

### 2.3 ESP32 modules

The target ESP32 implementation MUST separate:

```text
esp32_dual_chip_spi.c/.h   SPI slave, validation, typed ingress/egress
bt_ds5_tx_scheduler.c/.h   canonical HIDP interrupt pending data
bt_ds5_btstack.c/.h        connection, HIDP channels, feature/control transport
```

`bt_ds5_tx_scheduler` MUST be the only application component that calls
`l2cap_request_can_send_now_event()` for the HID interrupt channel and handles
its `L2CAP_EVENT_CAN_SEND_NOW` payload selection.

## 3. Fixed Capacities

Initial capacities are normative. A later change requires measurement and a
spec update; it cannot be an ad-hoc fix.

### 3.1 M61

| Storage | Capacity | Full behavior |
| --- | ---: | --- |
| audio epoch slots | 8 epochs | discard oldest incomplete/unsent epoch as one unit |
| complete `0x39` ring | 2 reports | replace oldest READY report |
| `0x31` state mailbox | 1 | overwrite |
| `0x32` state mailbox | 1 | overwrite |
| reliable control FIFO | 8 | reject new slow-path request with `EAGAIN` |
| input realtime ring | 4 | mode-dependent newest preservation |
| mic Opus FIFO | 4 | drop oldest, count decoder loss |
| feature transaction slots | 8 | reject duplicate/inflight according to report ID |
| link-state mailbox | 1 | overwrite |
| telemetry snapshot | 1 | overwrite |

### 3.2 ESP32

| Storage | Capacity | Full behavior |
| --- | ---: | --- |
| canonical `0x39` READY slots | 2 | evict oldest READY, never SENDING |
| canonical `0x31` mailbox | 1 | overwrite |
| canonical `0x32` mailbox | 1 | overwrite |
| reliable control command FIFO | 8 | return `ENOBUFS` to slow-path caller |
| input response ring | 4 | preserve newest according to negotiated input mode |
| mic response FIFO | 4 | drop oldest |
| feature response FIFO | 8 | never displaced by input/telemetry |
| link-state mailbox | 1 | overwrite |
| scheduler-status mailbox | 1 | overwrite |
| telemetry snapshot | 1 | overwrite |

The same report MUST NOT occupy another payload queue between the canonical
ESP32 slot and `l2cap_send()`.

### 3.3 RAM budget gate

`python tools/scheduler_ram_budget.py` is a mandatory pre-build gate.
`python tools/check_scheduler_memory.py` is the corresponding mandatory
post-link gate. A phase is not build-complete until both linked application
artifacts pass it.

- M61 planned typed static storage MUST remain <= 32 KiB.
- M61 projected static RAM plus 8 KiB contingency MUST remain below 75% of
  415 KiB.
- M61 projected minimum heap after task-stack delta and 8 KiB contingency MUST
  remain >= 100 KiB.
- ESP32 planned typed static storage MUST remain <= 16 KiB.
- ESP32 projected static DRAM plus 8 KiB contingency MUST remain below 70%.

The initial measured/modelled values are:

```text
M61 planned typed static:          20,040 B
M61 projected static + reserve:   282,490 B
M61 projected min heap + reserves:126,262 B
ESP planned typed static:          10,952 B
ESP projected static + reserve:    78,188 B
ESP runtime queue payload freed:   13,888 B minimum
```

The post-refactor linked images currently measure:

```text
M61 linked static:                 276,784 B
M61 initial OCRAM heap:            148,176 B
M61 heap after both 8 KiB reserves:131,792 B
ESP32 linked DRAM:                  68,548 B
ESP32 DRAM plus 8 KiB reserve:      76,740 B
```

Changing a slot count, payload pool, task stack, or metadata layout without
updating this budget is a spec violation.

The post-link gate measures the linked artifacts rather than trusting the
forecast:

- M61 reads `__ram_start__`, `__HeapBase`, and `__HeapLimit` from the ELF.
  Static OCRAM plus 8 KiB MUST be <= 318,720 B. Initial OCRAM heap capacity
  minus both the 8 KiB static-growth reserve and 8 KiB runtime-heap reserve
  MUST be >= 102,400 B.
- ESP32 invokes the official `$IDF_PATH/tools/idf_size.py --format json2` on
  the application map. Reported DRAM used plus 8 KiB MUST be <= floor(70% of
  the reported DRAM total).
- `--phase` symbol contracts are opt-in through `--phase-config`,
  `--require-symbol`, and `--forbid-symbol`. The current migration phase has
  no implicit new-symbol requirement. This prevents an early gate from
  requiring code that belongs to a later phase; the final phase rules MUST be
  configured when the final ownership modules and retired legacy paths are
  fixed.

Current linked-artifact example (2026-07-11):

```text
M61 static:                     276,784 B
M61 static + reserve:           284,976 B / 318,720 B
M61 initial heap:               148,176 B
M61 heap after both reserves:   131,792 B / 102,400 B minimum
ESP32 DRAM:                      68,548 B / 124,580 B
ESP32 DRAM + reserve:            76,740 B / 87,206 B
```

Reference invocation:

```text
python tools/check_dual_chip_release.py
```

The release helper rejects stale ELF/bin/map/config artifacts, selects the
ESP-IDF Python environment, and invokes the final RAM/symbol gate. The
checked-in contract requires unique ownership markers for the M61 SPI/audio
and ESP32 TX/response schedulers plus the concrete typed USB and response
stores. It forbids the retired generic queues on both targets. A configured
phase name that is absent from the JSON is a hard gate error, so a typo cannot
silently disable the symbol contract.

## 4. Object State Machines

### 4.1 Realtime slot

Each realtime slot has this state machine:

```text
FREE -> WRITING -> READY -> SENDING -> FREE
                     |          |
                     +-> EVICTED+
```

Requirements:

- Producer reserves `FREE`; when no `FREE` exists it may change the oldest
  `READY` to `EVICTED` and reuse it.
- Producer MUST NOT modify `SENDING`.
- Payload copy occurs outside the critical section after a slot is reserved.
- Publishing `READY` is the release point.
- Consumer marks `SENDING` before exposing the slot outside the critical
  section.
- Generation change clears `WRITING`, `READY`, and stale state mailboxes.

### 4.2 State mailbox

A state mailbox stores payload, generation, update sequence, dirty bit, and
created timestamp. An update overwrites the previous unsent value and increments
`coalesced` when dirty was already set.

Consumer clears dirty only after reserving a stable payload snapshot. If a
producer updates concurrently, the mailbox remains dirty for the newer value.

### 4.3 Reliable transaction

Reliable control uses:

```text
FREE -> QUEUED -> SENT_WAIT_ACK -> COMPLETE
                         |-> RETRY_QUEUED
                         |-> FAILED
```

Maximum attempts are two unless a command-specific spec says otherwise. Retry
work is lower priority than ready realtime media and input receive service.

## 5. Audio Epoch Contract

### 5.1 Epoch creation

One epoch represents 512 USB stereo speaker frames and the corresponding
64-byte haptics block.

```c
typedef struct {
    uint32_t generation;
    uint32_t epoch;
    uint64_t captured_us;
    uint8_t state;
    uint8_t haptics[64];
    int16_t speaker_pcm[512 * 2];
    uint8_t speaker_opus[200];
    bool speaker_enabled;
} m61_audio_epoch_t;
```

The implementation MAY store PCM outside this exact struct, but ownership MUST
remain keyed by `generation + epoch`.

### 5.2 Epoch completion

- Haptics and speaker data from different epoch IDs MUST NOT be paired.
- Speaker inactive is represented by `speaker_enabled=false`, not by waiting for
  an absent Opus queue item.
- While the speaker USB interface is active, silence MUST still complete the
  speaker part of the epoch.
- If an epoch must be dropped, all of its speaker and haptics data are dropped
  together and `epoch_dropped` increments once.

### 5.3 Report assembly

Two adjacent complete epochs produce one `0x39` report. The assembler MUST NOT
pair non-adjacent epochs without incrementing a discontinuity counter.

The output format is fixed:

- 547-byte Bluetooth report;
- two 64-byte haptics blocks;
- two 200-byte speaker/headset Opus blocks when enabled;
- stereo 48 kHz, 160 kbps CBR, fullband, 10 ms Opus;
- valid sequence, packet counter, buffer length, and CRC.

## 6. M61 Submission APIs

All realtime/state submission APIs are non-blocking.

```c
int m61_spi_submit_rt_report(const uint8_t *report,
                             size_t len,
                             uint32_t generation,
                             uint64_t created_us);

int m61_spi_publish_state31(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us);

int m61_spi_publish_state32(const uint8_t *report,
                            size_t len,
                            uint32_t generation,
                            uint64_t created_us);

int m61_spi_submit_control(const m61_spi_control_request_t *request);
```

Return contract:

- realtime/state returns success after copying/publishing locally;
- replacement is success plus a counter, not a producer retry condition;
- control may return `EAGAIN` when its FIFO is full;
- no API waits for ESP READY, SPI completion, ACK, flow credit, or a mutex owned
  by the scheduler task.

The old API argument `deadline_tick` MUST be removed from the target path.

## 7. M61 SPI Scheduler

### 7.1 Ownership

`ds5_spi_scheduler` is the sole caller of the BL616 SPI transfer API after
initialization. No other task may hold a transport mutex or call a synchronous
exchange helper.

### 7.2 Wake sources

Use task notifications/event bits for:

```text
RT_READY
STATE31_DIRTY
STATE32_DIRTY
CONTROL_READY
ESP_IRQ
MAINTENANCE
STOP_OR_GENERATION_CHANGE
```

Notifications may coalesce because payloads live in typed storage.

### 7.3 Arbitration

The scheduler runs this logical selection:

```text
if rt report ready:
    send oldest rt
else if ESP IRQ asserted:
    receive peer item
else if reliable control age >= 2 ms:
    send oldest control
else if state31 dirty:
    send newest state31
else if state32 dirty:
    send newest state32
else if reliable control ready:
    send oldest control
else if maintenance due and system idle:
    send maintenance
else:
    block on notification
```

After at most four consecutive transactions, the task SHOULD yield when equal
or higher priority tasks are ready. It MUST immediately continue when an RT
report or ESP IRQ remains pending.

### 7.4 Transaction completion

After every exchange:

1. validate the received header and CRC;
2. route by typed class before formatting logs;
3. update admission/conservation counters;
4. publish peer pending descriptor/status;
5. schedule the next exchange when work remains.

Errors are class-specific. A telemetry CRC error cannot increment a realtime
drop counter or trigger an immediate coprocessor reset.

## 8. ESP32 SPI Ingress And Response Scheduler

### 8.1 Ingress routing

The SPI task validates a frame and routes directly to canonical typed storage:

- `BT_TX_AUDIO_RT` -> BT scheduler realtime slots;
- `BT_TX_REPORT` with report ID `0x31` -> state31 mailbox;
- `BT_TX_REPORT` with report ID `0x32` -> state32 mailbox;
- feature/connect/disconnect/forget -> reliable control FIFO;
- stats/wire-test -> maintenance path.

It MUST NOT enqueue all types into one generic FreeRTOS TX queue.

### 8.2 Response selection

Response priority:

1. reliable ACK/feature response;
2. input realtime;
3. mic stream;
4. link-state transition;
5. scheduler-status transition;
6. telemetry.

Input and feature responses MUST have separate storage. Telemetry replacement
MUST NOT count as a response queue failure.

## 9. ESP32 BT Interrupt Scheduler

### 9.1 CAN_SEND request latch

The application maintains `can_send_requested`. It calls
`l2cap_request_can_send_now_event()` only when:

- HID interrupt channel is open;
- at least one canonical interrupt item is pending;
- no application request is already outstanding.

On `L2CAP_EVENT_CAN_SEND_NOW`, clear the application latch before selection.
BTstack also coalesces its per-channel request bit; the application latch makes
the ownership explicit and measurable.

### 9.2 Selection

```text
drop rt READY items whose ESP-local age > 64 ms

if rt pending and (rt_burst < 3 or no state dirty):
    select oldest rt
else if state31 dirty:
    select newest state31
else if state32 dirty:
    select newest state32
else if rt pending:
    select oldest rt
else:
    no interrupt item
```

Feature/control L2CAP traffic is not inserted into this arbitration and uses
the HID control channel.

### 9.3 Send result

- `l2cap_send()==0`: increment class transmitted, free realtime slot or commit
  the state mailbox snapshot.
- failure for realtime: increment `rt_l2cap_failed`, free the item, do not retry.
- failure for state: increment state failure and leave the newest mailbox dirty.
- after result, request another CAN_SEND event if work remains.

The implementation MUST record time from ESP arrival to `l2cap_send()` and the
gap between consecutive realtime sends.

## 10. Input Scheduling

### 10.1 ESP receive

Every valid controller input report receives an ESP timestamp and sequence.
Behavior by negotiated USB polling mode:

- 250 Hz: preserve newest sample for each 4 ms service interval;
- 500 Hz: preserve newest sample for each 2 ms service interval;
- realtime: ring depth 4, deliver in order while age <= 8 ms, otherwise skip to
  newest and count skipped samples.

Button/wake edge detection runs before any sampling/coalescing decision.

### 10.2 M61 USB input pump

BT input dispatch MUST only update the input store and notify the pump. It MUST
NOT directly call `usbd_ep_start_write()` from the SPI RX callback.

The USB HID IN endpoint completion callback clears busy and notifies the pump.
The pump submits the newest eligible pending report immediately. If USB was
busy, the final controller state is therefore retried even when no later BT
report arrives.

## 11. USB ISR And Deferred Work

The active CherryUSB configuration executes EP0 and endpoint callbacks from
`USBD_IRQ`. Therefore USB callbacks MUST be treated as ISR context unless the
configuration is explicitly changed and verified.

USB callbacks MAY only:

- copy a bounded packet into a preallocated slot;
- update indices/flags under an ISR-safe critical section;
- rearm an endpoint;
- notify a worker using an ISR-safe primitive.

USB callbacks MUST NOT:

- call EasyFlash or any flash/NVS API;
- call `printf` or format diagnostic strings;
- create/delete tasks;
- wait on a mutex/semaphore;
- run Opus, resampling, haptics conversion, or a multi-kilobyte copy while
  interrupts remain disabled;
- synchronously submit SPI/BT work.

Required deferred workers:

- `m61_usb_control_worker`: bridge config updates, persistence, USB reconnect,
  descriptor/identity changes, and host feature work;
- `m61_audio_ingress`: consumes USB Audio OUT slots and produces audio epochs;
- `m61_usb_input_pump`: owns HID IN submission/retry.

The audio ISR ingress ring MUST contain at least two 1 ms USB packets and use
fixed storage. Overflow drops the oldest unprocessed packet, increments a
specific counter, and never performs partial PCM manipulation in ISR context.

## 12. State Report Scheduling

### 12.1 State 0x31

- Windows output writes update a canonical M61 state model.
- Rendering and sequence/CRC assignment occur when the scheduler snapshots a
  dirty state for transfer, not for every overwritten host write.
- Maximum send rate is initially 100 Hz.
- New state bypasses the rate limit when it changes a latency-sensitive field:
  trigger effect, rumble/haptics enable, mute, or audio routing, provided the
  previous `0x31` was sent at least 2 ms earlier.
- Identical rendered state does not become dirty.

### 12.2 State 0x32

- Send immediately on mic-active/audio-routing transition.
- Coalesce repeated status.
- Optional keepalive is at most 2 Hz and runs only when no realtime work is
  waiting.

## 13. Backpressure And Scheduler Status

Protocol v2 replaces generic flow credit with a coalesced class snapshot:

```c
typedef struct {
    uint8_t rt_pending;
    uint8_t rt_capacity;
    uint8_t control_pending;
    uint8_t control_capacity;
    uint8_t input_pending;
    uint8_t input_capacity;
    uint8_t mic_pending;
    uint8_t mic_capacity;
    uint8_t flags; /* bt_ready, state31_dirty, state32_dirty */
    uint32_t rt_replaced;
    uint32_t rt_stale;
    uint32_t transport_errors;
} ds5_dual_scheduler_status_t;
```

Send this snapshot only:

- when `bt_ready` changes;
- when a class crosses empty/full/high-water transition;
- on explicit request;
- at an idle heartbeat no faster than 2 Hz.

M61 MUST NOT reject a latest/realtime item solely because a cached peer free
count is zero. The peer canonical store performs its specified replacement.

## 14. SPI Protocol Versions

### 14.1 Phase A: scheduler over protocol v1

The first refactor keeps the 20-byte v1 header and 692-byte transaction.
Traffic class is inferred from message type and report ID. The 500 Hz hardware
gate targets 8 MHz. On the current Matrix-routed breadboard this is an explicit
overclock qualification; the in-spec 8 MHz production path uses native VSPI
IO_MUX wiring.

All example defconfigs, generated-profile inputs, build-script overrides, and
runtime status output MUST report the same selected SPI clock. Configuration
drift is a build/test failure.

### 14.2 Phase B: protocol v2 windows

`HELLO` negotiates:

```text
DS5_DUAL_CAP_TYPED_SCHEDULER
DS5_DUAL_CAP_VARIABLE_WINDOWS
DS5_DUAL_CAP_SCHED_STATUS
```

Window sizes:

```text
SMALL  = 128 bytes total
MEDIUM = 256 bytes total
LARGE  = 692 bytes total
```

When a response does not fit, ESP returns a pending descriptor:

```c
typedef struct {
    uint8_t type;
    uint8_t sched_class;
    uint16_t sequence;
    uint16_t total_length;
    uint8_t required_window;
} ds5_dual_pending_desc_t;
```

Realtime reports MUST NOT be fragmented. Reliable slow control MAY use explicit
fragments with transaction ID, offset, and final flag.

## 15. SPI Clock Profiles

Profiles:

| Profile | Clock | Maximum input mode |
| --- | ---: | --- |
| compatibility | 4 MHz | 250 Hz |
| matrix-candidate | 8 MHz | user-approved overclock; zero-error qualification required |
| production | 8 MHz | native VSPI IO_MUX; 500 Hz and variable-window candidate |
| margin-only | 10 MHz | native IO_MUX laboratory characterization only |

Boot starts at compatibility speed. The candidate firmware may switch to 8 MHz
after a non-destructive wire test. On current Matrix wiring this remains an
explicit overclock even after qualification. Eight MHz becomes an in-spec
saved/default production profile only on native VSPI IO_MUX wiring after at
least 30 minutes continuous traffic with zero CRC, sequence, and frame errors.
Any error burst falls back to 4 MHz outside an active media critical section
and records the transition.

Ten MHz MAY be used only for margin characterization. It MUST NOT be saved as a
production profile, and no test may require operation at the device limit.

The current host value `2` selects `SPI3/VSPI` and uses GPIO Matrix pins
`27/26/25/33`, not native VSPI IO_MUX `18/23/19/5`. The build MUST expose this
fact in status output and MUST NOT claim IO_MUX or in-spec 8 MHz timing margin
for the current wiring. HSPI is host value `1`, not `2`.

At 4 MHz a fixed 692-byte transaction takes about 1.384 ms before software
overhead. The current 1 ms delay after each RX poll limits practical service to
about 419 transactions/s and MUST NOT be used for 500 Hz mode.

## 16. Priorities And Blocking Rules

### 16.1 M61

Relative order, highest first:

1. USB ISR/driver requirements;
2. SPI scheduler and USB input pump;
3. audio ingress, codec, and realtime assembler;
4. state/control bridge;
5. connection/recovery;
6. LED, stats, shell, logging.

No task below level 3 may hold a resource needed by levels 1-3. Task priority
constants MUST be named in one header and validated at compile time; modules
MUST NOT mix absolute priorities (`6`, `5`, `4`) with
`configMAX_PRIORITIES-N` without an explicit mapping.

### 16.2 ESP32

- ESP-IDF BT controller retains its required core 0 priority.
- BTstack task remains on core 0 and MUST be above application maintenance
  tasks.
- SPI slave/scheduler remains on core 1.
- No core 1 critical section may wait for core 0 CAN_SEND completion.

BTstack Basic L2CAP has no hidden payload queue. Its can-send state is one
per-channel latch, HCI uses one shared outgoing packet buffer, and VHCI/ACL
credits gate subsequent sends. Metrics and scheduler tests MUST model these
actual wait points rather than assuming `NR_BUFFERED_ACL_PACKETS` is a data
queue.

### 16.3 Forbidden operations in realtime paths

- `printf`/formatting;
- NVS/flash access;
- heap allocation/free;
- mutex waits with a timeout;
- connection discovery/security state changes;
- synchronous stats or feature polling;
- retry loops;
- holding a critical section across multi-kilobyte audio copies.

## 17. Audio Scheduling Rules

- `0x39` readiness is driven by two adjacent complete epochs, not by a nominal
  20 ms timer.
- The steady period is about 21.333 ms and the long-term cadence is 46.875 Hz.
- The scheduler MUST NOT catch up a delayed timer by sending reports at 1 ms
  intervals or by repeatedly adding an old 20 ms deadline.
- If production is discontinuous, the next schedule anchors to the current
  complete epoch. Old media is replaced/dropped according to age; it is not
  burst-sent.
- Separate speaker/haptics source queues deeper than the epoch store are
  forbidden because they can accumulate 40-85 ms independently and lose
  alignment.

## 18. Metrics

Each class MUST expose:

```text
accepted
transmitted
pending
highwater
coalesced
replaced
stale
admission_rejected
transport_failed
age_last_us
age_max_us
age_sum_us
age_samples
```

Realtime additionally exposes:

```text
gap_last_us
gap_max_us
gap_over_25ms
gap_over_40ms
epoch_discontinuity
```

SPI exposes transaction counts and latency by window/class, CRC errors, READY
wait, IRQ-to-service latency, and clock-profile fallback.

BTstack exposes VHCI send-unavailable time, HCI ACL credit-zero time, outgoing
buffer busy time, CAN_SEND request-to-callback latency, L2CAP send failure, and
Controller-to-Host ring drops when available.

USB exposes ISR ingress overflow, deferred-control depth, endpoint busy time,
input retry, audio ingress latency, and maximum critical-section duration.

## 19. Required Invariants

Unit tests MUST prove:

```text
rt_accepted = rt_transmitted + rt_replaced + rt_stale
              + rt_transport_failed + rt_pending

input_received = input_usb_sent + input_coalesced + input_stale
                 + input_dropped + input_pending

control_accepted = control_completed + control_failed + control_pending
```

Additional invariants:

- at most one `0x31` and one `0x32` mailbox are dirty;
- at most two realtime reports are READY/SENDING per side;
- a `SENDING` slot is never overwritten;
- no item from an old generation is transmitted;
- telemetry cannot cause control/media admission failure;
- speaker and haptics epoch IDs match within every `0x39`;
- state sequence and CRC correspond to the transmitted snapshot;
- queue age never increases without bound;
- no USB ISR executes flash, task creation, logging, Opus, or SPI exchange;
- every SPI transfer originates from the single M61 scheduler task;
- every HID interrupt L2CAP payload originates from the ESP canonical scheduler.

## 20. Host-Side Scheduler Tests

Add deterministic host tests, independent of hardware:

```text
tools/test_dual_chip_scheduler.py
```

Required scenarios:

1. 10 minutes of 46.875 Hz realtime production with periodic 30 ms CAN_SEND
   stalls: no replacement when capacity is sufficient and hard age respected.
2. State flood at 1 kHz plus realtime: state coalesces, realtime is not rejected.
3. Reliable-control flood plus realtime/input: control makes bounded progress
   without delaying realtime beyond its limit.
4. USB input endpoint busy: newest pending report is sent on completion without
   requiring another BT report.
5. Generation change with every storage class nonempty: nothing old transmits.
6. 32-bit and 64-bit timer wrap vectors.
7. Randomized producer/consumer traces with conservation checks.
8. SPI capacity model for 4/8 MHz and small/medium/large windows; 10 MHz is an
   optional margin-only case.
9. Audio epoch encode delay and missing epoch: no cross-epoch speaker/haptics
   pairing.
10. Stats/credit flood: telemetry never occupies more than one snapshot.
11. USB control request: flash/config/reconnect work is deferred out of ISR.
12. L2CAP model: one can-send latch and one HCI outgoing buffer cannot lose a
   selected item without a class-specific failure counter.

Source contract tests MAY verify module/API presence, but behavioral scheduler
tests are the authoritative gate.

## 21. Build And Hardware Gates

### Gate 1: design skeleton

- both firmware images build;
- scheduler types/modules compile;
- host scheduler model tests pass;
- no hardware behavior change.

### Gate 2: M61 scheduler

- all M61 SPI calls originate from one task;
- old RX poll/time-sync tasks are absent from the realtime profile;
- USB input completion retry and deferred USB-control tests pass;
- protocol v1 compatibility build passes.

### Gate 3: ESP scheduler

- generic report command/L2CAP FIFOs no longer own report payloads;
- canonical slots and CAN_SEND arbitration tests pass;
- state flood cannot produce `ENOBUFS` for realtime;
- feature/control remains functional under synthetic load.

### Gate 4: hardware high-rate transport

- 8 MHz wire test: zero CRC/frame/sequence errors for at least 10 million
  transactions or the longest practical overnight run; current Matrix wiring
  is recorded as an overclock qualification, not an in-spec production result;
- 500 Hz input test: P99 < 4 ms, max < 8 ms;
- 30-minute stereo speaker + nonzero haptics test: no audible periodic gaps,
  zero normal realtime replace/stale/fail;
- state updates remain P99 < 8 ms.

### Gate 5: protocol v2 and performance profile

- variable-window interoperability and v1 fallback pass;
- variable-window operation at 8 MHz passes signal-integrity and recovery
  tests;
- realtime/1 kHz input remains experimental and is enabled only if measured
  latency and zero-error gates pass without exceeding 8 MHz.

### Gate 6: console-quality completion

- band-limited 51.2-to-48 kHz resampler validated;
- stereo 160 kbps fullband encode P99 < 10 ms;
- 60-minute mixed-load soak with speaker, haptics, input, state changes, feature
  reads, mic traffic, and stats collection;
- no USB disappearance while connected, no stuck USB device after controller
  disconnect, no BT reconnect, watchdog, or heap trend.

## 22. Refactor Order

Implementation MUST follow this order:

1. shared types, metrics, scheduler model tests;
2. deferred USB control/audio ingress skeleton;
3. M61 async SPI scheduler facade while keeping wire v1;
4. M61 USB input pump and state mailboxes;
5. ESP canonical BT TX scheduler;
6. typed ESP response stores;
7. audio epoch assembler;
8. 8 MHz hardware qualification;
9. protocol v2 variable windows;
10. final resampler and console-quality soak.

The boards currently in download mode are not flashed until at least Gates 1-3
pass and both final candidate binaries have been rebuilt from the same source
state.
