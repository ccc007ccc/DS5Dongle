# M61 Realtime Optimization Specification

## Objective

Deliver the highest practical single-chip DualSense experience on the
Ai-M61-32S-Kit while preserving this strict priority order:

1. Buttons, sticks, gyro, touch, lights, adaptive triggers, and basic HID
2. Voice-coil haptics
3. Speaker
4. Microphone

Overload must degrade lower-priority media instead of delaying controller
input or haptics.

## Hardware Constraints

- E907 CPU: 320 MHz maximum; current firmware already runs at 320 MHz.
- I-cache: 32 KB, D-cache: 16 KB.
- Application RAM region: 415 KiB after wireless/system reservations.
- ITCM boot-safe ceiling: 40 KiB on the tested board.
- USB Device transfers already use the controller's dedicated VDMA.
- Four general DMA channels are available, but they do not accelerate Opus.
- 4 MB PSRAM is present but has higher and less deterministic latency than
  internal SRAM.
- External flash currently runs at 80 MHz.
- System bus maximum: 80 MHz; no documented bus-clock headroom remains.
- OCRAM/WRAM have cached AXI aliases and non-cached AHB aliases. The SDK
  `itcm` section uses cached OCRAM rather than an independent hardware TCM.
- E907 cache line: 32 bytes.
- USB device storage provides four shared 512-byte FIFOs. The current four
  non-control endpoints occupy all four in the default one-FIFO mapping.

## Global Gates

- ITCM must be at most 40 KiB at link time and post-link validation.
- Static physical RAM plus 8 KiB contingency must remain below 75% of the
  415 KiB application region.
- Twenty consecutive cold boots must complete PHY RF initialization.
- Basic control report age must remain below 25 ms under full media load.
- No Bluetooth TX scheduler drop, stale, or replacement is allowed for basic
  controls.
- Haptics must never wait indefinitely for speaker encoding.
- Changes are committed only after hardware validation.
- The validated Flash-XIP image is the protected A/B baseline. One risky
  optimization is tested at a time.

## Phase 0: Transport Correctness

Status: complete and hardware validated.

Work:

- Normalize positive `bt_l2cap_chan_send()` byte-count returns to success.

Acceptance:

- Realtime: 2,863/2,863 transmitted with zero drop/retry/stale/replacement.
- State 0x31: 1,917 transmitted with zero failures.

Commit: `db3dd2e`

## Phase 1: Encoder ITCM Placement

Status: failed hardware validation and rolled back.

Work:

- Place measured Opus/CELT encoder hot sections in copied ITCM.
- Enforce the 40 KiB ITCM ceiling in the linker and memory checker.

Failed build:

- ITCM: 39,576 bytes.
- Static RAM plus contingency: about 220 KB.

Measured result:

- Speaker encode average regressed from 7,807 us to 9,534 us.
- Speaker encode maximum regressed from 10,803 us to 12,283 us.
- Audio queue drops increased from 46 to 3,822.
- Realtime TX accumulated 19 replacements and 123 stale pairs.

Conclusion:

- Keep the Opus encoder in Flash XIP. The SDK `itcm` section is RAM-code
  placement, but the BL618 documentation does not identify a separate
  zero-wait hardware ITCM. The larger RAM-code working set competes with
  USB/Bluetooth/data traffic and is slower for this workload.

Rollback validation:

- ITCM returned to 10,632 bytes.
- Static physical RAM plus contingency is 190,956 bytes, with 127,764 bytes
  of margin below the 75% gate.
- The user reported no clearly audible stutter in the rollback haptics and
  speaker test.

## Phase 1B: Baseline Counters

Status: complete and hardware validated.

Work:

- Add compile-time-disabled-by-default E907 HPM collection around Opus encode.
- Record cycles, retired instructions, I-cache misses, D-cache read misses,
  and a fixed-size p50/p95/p99 timing histogram.
- Measure maximum global interrupt-mask duration and USB ingress age.
- Expose only accumulated values through `ds5 status`; do not print per frame.

Acceptance:

- Disabled instrumentation produces the same binary behavior as the rollback
  baseline.
- Enabled instrumentation adds less than 1% to average encode time.
- Counters remain monotonic and do not overflow during a ten-minute run.

Measured result:

- 57,143 speaker encodes: average 6,976 us, p95 8,750 us, p99 9,000 us,
  maximum 9,816 us.
- Average 2,232,670 cycles and 383,174 retired instructions per encode.
- I-cache miss rate 1.0572%; D-cache read miss rate 1.5947%.
- USB ingress age p95 1,000 us, p99 1,250 us, maximum 1,794 us.
- Maximum global interrupt mask 67,699 cycles, approximately 212 us.
- Realtime Bluetooth 28,605/28,605 with zero drop, stale, retry, or
  replacement; audio epoch drops remained zero.

Conclusion:

- The sustained encoder fits inside one 10.667 ms epoch in this run.
- Phase 3 USB ingress ownership is the next behavioral change because the
  interrupt-masked 416-byte record copy is the clearest measured latency tail.

## Phase 2: Haptics Hard Deadline

Status: first implementation failed hardware validation; redesign pending.

Work:

- Own two adjacent audio epochs as one transmission pair.
- Send full haptics plus speaker when both Opus frames are ready.
- At a 32 ms pair deadline, emit haptics-only report 0x39.
- Cancel speaker jobs that have not started by the haptics deadline.
- Discard a late completed encode without resending the committed pair.

Acceptance:

- Haptics continue when speaker encoding is intentionally overloaded.
- Haptics-only fallback is counted and bounded.
- No duplicate or partial speaker pair is transmitted.
- Basic controller operations remain unaffected.

Failed implementation:

- Added a 32 ms fallback and raised the USB/HID bridge task above the Opus
  codec task so the bridge could enforce the deadline.
- Haptics remained continuous, but the user reported that the speaker was
  almost silent.
- Hardware counters recorded 4,586 deadline fallbacks, 5,388 cancelled
  encodes, and 4,397 late encode completions.
- Encode p99 regressed from about 9,000 us to 15,500 us; maximum reached
  17,570 us. The priority change caused frequent Opus preemption and made the
  overload substantially worse.

Redesign constraint:

- Keep the validated codec and bridge priorities unchanged.
- Perform admission in the codec task before starting the next encode. When
  the remaining pair slack cannot cover an encode, convert only the pending
  speaker epoch to haptics-only and yield one tick so the bridge can transmit.
- Do not attempt to preempt an Opus call already in progress.

## Phase 3: USB Audio Ingress Ownership

Status: failed hardware validation and rolled back.

Work:

- Replace full-record copies under the global IRQ lock with fixed slot
  ownership states: FREE, WRITING, READY, READING, RETIRED.
- Copy the 392-byte USB payload outside the IRQ-disabled region.
- Retire WRITING and READING slots safely across audio generation resets.

Acceptance:

- No roughly 416-byte queue-record copy occurs while global interrupts are
  disabled.
- No WRITING-slot reuse race during reset.
- Zero ingress corruption and bounded whole-packet drops under overload.

Measured result:

- The ownership implementation passed host ownership and reset-race tests.
- Hardware status showed 5,166 haptics queue drops and 5,166 speaker queue
  drops; the protected baseline had zero audio epoch drops.
- Encode p99 regressed from about 9,000 us to 11,500 us.
- The user reported frequent, clearly audible speaker and haptics stutter.

Conclusion:

- The five-state slot design is rejected in its tested form. Moving the
  payload copy outside the IRQ-disabled region changed ingress production and
  cache/scheduling behavior enough to overflow the downstream realtime queue.
- Restore the validated bounded ring-copy implementation. Do not revisit
  ingress ownership until haptics have an independent hard deadline and a
  narrower zero-copy design can preserve the baseline queue cadence.

## Phase 4: Hardware Performance Counters

Status: folded into Phase 1B so measurements precede behavioral optimization.

Work:

- Use the E907 HPM support already present in the SDK.
- Measure cycles, retired instructions, I-cache misses, and D-cache read
  misses around Opus encode and decode.
- Add a fixed-size timing histogram for p50/p95/p99 instead of relying only on
  average and maximum.

Acceptance:

- Diagnostics add negligible work to the realtime path when disabled.
- ITCM decisions are backed by measured cache-miss reduction.

## Phase 5: Fixed-Priority Admission

Status: pending.

Work:

- Basic control state has absolute service priority.
- Realtime haptics/speaker uses a bounded burst.
- Microphone control receives bounded fairness but cannot block haptics.
- Codec execution admits microphone decode only when higher-priority work has
  sufficient deadline slack.

Acceptance:

- Total measured p99 realtime CPU demand remains at or below 75%.
- Control age stays below 25 ms during haptics, speaker, and microphone use.
- Haptics continuity is unchanged when the microphone endpoint is open.

## Phase 6: Microphone Full Duplex

Status: blocked on Phase 4 and Phase 5 measurements.

Work:

- Restore correct 16-bit mono decode to 16-bit stereo USB packing.
- Use whole-frame admission and bounded microphone FIFO depth.
- Do not remove speaker/haptics protection until CPU slack is demonstrated.

Acceptance:

- Recorded microphone audio is continuous and correctly formatted.
- Haptics remain smooth.
- Speaker quality and latency remain within the Phase 1 baseline.

## Phase 7: PSRAM and Cold Storage

Status: optional.

Use PSRAM only for:

- Long diagnostic histories
- Cold feature caches
- Non-realtime capture buffers

Do not place codec state, Bluetooth buffers, realtime queues, or realtime task
stacks in PSRAM without latency and cache-miss measurements.

## Deferred Hardware Experiments

The following are not part of the next firmware change:

- Align Opus state and epoch storage from 16 to 32 bytes, then retain only if
  HPM and p99 timing improve.
- Set explicit Bluetooth/USB CLIC priorities after interrupt-mask duration is
  bounded and measured.
- Develop a custom USB FIFO allocation only if ingress-age measurements show
  FIFO service, rather than CPU scheduling, is the bottleneck.

Explicitly rejected without new evidence:

- Large Opus/CELT RAM-code placement
- CPU or system-bus overclocking
- Flash clock above 80 MHz
- USB High-Speed conversion
- Global USB ping-pong/triple-buffer switches
- General DMA for Opus or short queue copies

## Phase 8: System Validation

Run in this order:

1. Twenty cold boots and ten first-press controller connections
2. Buttons, sticks, gyro, touch, lights, and adaptive triggers
3. Ten-minute haptics test
4. Ten-minute haptics plus speaker test
5. Distance/RF test
6. Microphone test only after all higher-priority gates pass

Capture `ds5 status` before and after every test. Record encode timing,
ingress drops, scheduler metrics, heap minimum, and task stack high-water
marks.

## UART Download Mode Note

The ROM samples GPIO2 during a full boot decision:

- GPIO2 high selects UART/USB/SDIO download mode.
- GPIO2 low selects the Flash application.
- The bootstrap level must be held for at least 2 ms around reset.

The SDK eflash loader configuration sets `cpu_reset_after_load=false`, and the
project flash command uses `--no-reset`. A CPU-only reset is also insufficient
to reproduce a full CHIP_EN/PWRON boot selection. To leave download mode
reliably, release BOOT/GPIO2 and hold CHIP_EN low for at least 1 ms before
restarting. This is why the application can request entry into download mode,
but the ROM download session cannot reliably return to the application using
an ordinary application UART command.
