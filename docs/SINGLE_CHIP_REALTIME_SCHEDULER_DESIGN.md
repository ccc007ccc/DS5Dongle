# Single-Chip Realtime Scheduler Design

## Scope

This repository targets RP2350/Pico 2 W with CYW43, not BL616/M61. The
dual-chip scheduler's semantic rules still apply, but SPI ownership is replaced
by a single BTstack/L2CAP transmit owner on core 0.

## Failure Model

The legacy design amplified short stalls:

- one depth-10 FIFO mixed `0x39`, `0x31`, and `0x32`, preserving up to about
  213 ms of old audio during CAN_SEND stalls;
- speaker and haptics used independent depth-2 queues and could pair data from
  different production times;
- BOOTSEL polling parked core 1 every 100 ms;
- config saves and USB reconnects blocked realtime work.

RF loss can still create a real gap. The scheduler must not turn that gap into
a long tail of stale audio.

## Core Ownership

- Core 0 owns CYW43 polling, BTstack/L2CAP, TinyUSB, audio ingress, report
  assembly, and maintenance state machines.
- Core 1 owns speaker resampling/Opus encoding and microphone Opus decoding.
- Core 1 never calls BTstack.

## Audio Epochs

One epoch represents 512 USB frames (10.667 ms):

- stereo speaker PCM;
- 64 bytes of haptics produced by 16-frame box-filtered decimation from the
  same USB frames;
- monotonically increasing epoch number;
- speaker-enabled state.

Core 0 publishes one latest raw epoch slot to core 1. Core 1 returns completed
epochs through a four-slot queue. Core 0 only assembles adjacent epochs with
matching speaker state into one `0x39`. A missing epoch retires the older orphan
instead of cross-pairing it.

## BT Transmit Stores

- `0x39`: two chronological realtime slots, replace oldest when full, hard age
  limit 64 ms;
- `0x31`: one latest mailbox;
- `0x32`: one latest mailbox;
- realtime burst limit: three frames before a pending state mailbox progresses;
- one CAN_SEND request latch prevents duplicate requests;
- failed realtime frames are dropped, while latest state remains pending for
  retry.

The four-bit report sequence and output CRC are stamped only after CAN_SEND
selects the next frame. Scheduler priority may reorder report classes, so
publish-time sequence assignment would otherwise make the wire sequence move
backward.

Reliable control/feature traffic remains a follow-up phase and should move to a
separate fixed FIFO owned by the same CAN_SEND dispatcher.

## Maintenance Rules

- BOOTSEL/flash operations are deferred while audio arrived in the last 100 ms;
- config saves prefer realtime idle but explicit save/reconnect requests have a
  two-second maximum deferral;
- USB reconnect uses a nonblocking 150 ms state machine.

## RAM Budget

The release map is checked by `tools/check_realtime_memory.py`:

- static RAM plus 8 KiB contingency must stay below 80% of 512 KiB;
- one raw epoch slot, four completed epochs, and mic queues are charged to heap;
- heap after typed queues and 2 KiB runtime contingency must stay at or above
  100 KiB.

The raw epoch store intentionally has one slot. If encoding cannot keep up with
the 10.667 ms production period, deeper buffering only increases latency and
delays the inevitable drop.

## Validation

1. Host tests cover realtime replacement, 64 ms stale retirement, bounded state
   progress, mailbox retry, and adjacent epoch pairing.
2. Release and debug firmware must compile with the CI Pico SDK/toolchain.
3. Thirty-minute speaker+haptics validation should show no stale/replaced
   realtime frames at close range.
4. Artificial CAN_SEND stalls must recover by sending current audio, never a
   backlog of old frames.
