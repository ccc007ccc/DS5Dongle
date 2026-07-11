# M61 Hardware Performance Audit

This audit applies to the Ai-M61-32S-Kit used by the single-chip M61
DualSense bridge. Sources reviewed:

- Ai-M61-32S-Kit product specification V1.1.2
- BL616/BL618 datasheet V2.5
- Ai-M61-32S-Kit schematic V1.1
- BL616/BL618 reference manual V0.98

## Verified Hardware

- The module uses the BL618-class QFN56 configuration with 4 MB Winbond
  PSRAM and 8 MB external flash.
- The E907 CPU runs at the documented maximum 320 MHz in the SDK board
  initialization.
- The CPU has a 32 KB two-way I-cache and 16 KB two-way D-cache.
- The E907 cache line is 32 bytes in the BL616/BL618 SDK.
- The chip provides 320 KB OCRAM and 160 KB WRAM. The current Bluetooth
  firmware/linker leaves 415 KiB available to the application.
- Cached OCRAM/WRAM aliases use AXI and an AXI-to-AHB path. Their non-cached
  aliases access the SRAM directly over AHB. The SDK `itcm` output at
  `0x62FC...` is therefore cached OCRAM, not a separate zero-wait TCM bank.
- USB is USB 2.0 High-Speed/Full-Speed Device with dedicated VDMA and four
  shared 512-byte FIFOs. The current Bouffalo USB driver already enables and
  uses VDMA for endpoint transfers.
- The general DMA controller has four independent channels, LLI chaining,
  1/2/4-byte widths, and INCR1/4/8/16 bursts. It is not the limiting factor
  for the current native USB path because USB uses its own VDMA.
- Native USB is connected to GPIO32/33. UART0 uses GPIO21/22 through CH340C.
  GPIO2 is the boot strap. GPIO12/14/15 drive the onboard RGB LED.
- GPIO4-9 are shared with external flash on this module and must not be used.
- The board specification requires a 3.3 V or 5 V supply capable of at least
  500 mA.

## Current Firmware Utilization

- CPU clock: 320 MHz, already at the documented maximum.
- Flash clock: 80 MHz. Do not raise it without a dedicated long-duration XIP
  and RF stability test.
- USB VDMA: enabled and used by the SDK driver.
- PSRAM: physically present but disabled in the firmware configuration.
- Baseline ITCM: 10,632 bytes.
- Failed encoder RAM-code experiment: 39,576 bytes in the SDK `itcm` section.
- Validated ITCM boot ceiling: 40 KiB. Larger layouts have caused PHY RF
  initialization failure and a solid green status LED.
- Baseline static physical RAM: 182,764 bytes.
- Baseline static RAM plus 8 KiB contingency: 190,956 bytes, leaving 127,764
  bytes below the 75% gate.
- USB VDMA buffers and CherryUSB control storage are already in the
  non-cached `0x22FC...` alias. Codec state, PCM epochs, task stacks, and
  scheduler data remain in cached SRAM, which is the correct split.

## Measured Realtime Load

After fixing the BR/EDR L2CAP success return handling, a hardware run reported:

- Realtime Bluetooth reports: 2,863 transmitted, zero replacement, stale,
  retry, or drop.
- State 0x31 reports: 1,917 transmitted, zero failures.
- Speaker encode: 5,700 successful, zero encode errors.
- Speaker encode time: 7,807 us average, 10,803 us maximum.
- Audio epoch period: about 10,667 us.

The encoder peak already exceeds one epoch period. This explains occasional
speaker and voice-coil haptics jitter even when Bluetooth transmission itself
has no loss.

After rolling the failed encoder RAM-code experiment back to Flash XIP, the
user could not hear obvious stutter in the same haptics and speaker test. This
Flash-XIP image is now the protected performance baseline for later A/B tests.

## Remaining Hardware Headroom

### High-confidence work

1. Add E907 HPM instrumentation before changing placement or clocks. The SDK
   already exposes cycle, retired-instruction, L1 I-cache miss, L1 D-cache
   read-miss, and branch-miss counters.
2. Remove whole-packet copies from global interrupt-disabled sections. The
   current USB audio ingress path copies a 392-byte payload into a roughly
   416-byte queue record while all interrupts are masked, and copies the full
   record again when dequeuing it.
3. Bound haptics latency independently of Opus. Speaker encoding can exceed
   the 10.667 ms epoch period, so haptics need a deadline-based fallback even
   when the average encode time is acceptable.
4. Measure p50/p95/p99 encode time, cycles per frame, cache misses, ingress
   age, and maximum interrupt-mask duration. Average and maximum encode time
   alone cannot distinguish CPU load from memory or scheduling stalls.

### Measurement-gated experiments

1. Align the large Opus encoder/decoder states and epoch store to the 32-byte
   cache line. They are currently only 16-byte aligned; the encoder state
   starts at an address offset by 16 bytes from a cache-line boundary. This is
   low risk but must still demonstrate a p99 improvement.
2. Set explicit CLIC priorities for Bluetooth and USB only after long global
   interrupt masks are removed and IRQ latency is measured. Priority changes
   cannot preempt code while global interrupts are disabled and can violate
   RTOS ISR assumptions if applied blindly.
3. Evaluate a specialized USB FIFO allocation only if ingress age proves that
   VDMA/FIFO service is limiting. The controller has four shared 512-byte
   FIFOs, but the current four non-control endpoints already consume all four.
4. Use PSRAM for cold diagnostic history or optional capture storage after its
   latency is measured. It must not hold codec state, active audio epochs,
   Bluetooth buffers, or realtime task stacks.

### Rejected as first-line optimizations

- Do not move the Opus/CELT encoder back into the SDK `itcm` section.
- Do not exceed the documented 320 MHz CPU or 80 MHz system-bus limits.
- Do not raise the 80 MHz Flash clock without a separate signal-integrity,
  cold-boot, XIP, and RF stability campaign.
- Do not enable USB High-Speed merely for this workload. The current native
  wiring is intentionally Full-Speed tolerant, the bandwidth already fits,
  and changing speed alters isochronous interval semantics and enumeration.
- Do not enable the driver's global ping-pong or triple-buffer switch. Its
  FIFO mapping assumes spare FIFO IDs that this four-endpoint device does not
  have.
- Do not use general DMA for Opus or short queue copies. USB already has VDMA,
  Opus is compute-heavy, and general DMA would add setup and AHB contention.

## Optimization Rules

1. Basic HID input, gyro, touch, lights, triggers, and state 0x31 have the
   highest correctness priority.
2. Voice-coil haptics must have a bounded deadline and may bypass late speaker
   encoding.
3. Speaker may lose a frame under overload; it must not delay haptics.
4. Microphone decode is admitted only when it cannot violate control,
   haptics, or speaker budgets.
5. Keep IRQ-disabled sections bounded. Large USB audio copies belong outside
   global interrupt locks.
6. Treat the SDK `itcm` section as measured RAM-code placement, not inherently
   faster memory. Enforce the 40 KiB link-time and post-link gates.
7. PSRAM is suitable for cold diagnostics, infrequent feature caches, and
   non-realtime storage. Do not place codec state, realtime queues, task
   stacks, or Bluetooth buffers there without latency measurements.
8. Do not overclock the CPU. It already runs at 320 MHz.
9. Do not raise flash clock above 80 MHz as a first-line optimization. Keep
   measured XIP hotspots in Flash unless a small RAM-code A/B test proves a
   p99 improvement.

## Implementation Order

1. Keep Opus encoder code in Flash XIP. Moving its hot path into the SDK
   `itcm` RAM-code section increased average encode time from 7,807 us to
   9,534 us, maximum from 10,803 us to 12,283 us, and queue drops from 46 to
   3,822. The BL618 documentation describes cached/non-cached OCRAM and WRAM,
   not an independent zero-wait hardware ITCM, so the copied code competes
   with realtime data traffic on the internal bus.
2. Add E907 HPM measurements for cycles, retired instructions, I-cache misses,
   D-cache read misses, timing percentiles, ingress age, and interrupt-mask
   duration.
3. Add a haptics deadline that emits a haptics-only 0x39 packet when speaker
   encoding is late.
4. Convert USB audio ingress to explicit slot ownership so the 392-byte
   payload and roughly 416-byte queue record are not copied under a global
   IRQ lock.
5. Revisit microphone full duplex only after measured p99 CPU demand leaves a
   minimum 25% realtime reserve.
6. Evaluate PSRAM only after the realtime path is stable; use it to increase
   observability or cold capacity, not to hide an over-budget scheduler.

## Validation Gates

- ITCM must be at most 40 KiB.
- Static physical RAM plus contingency must remain below 75% of the 415 KiB
  application region.
- No PHY RF initialization failures across repeated cold boots.
- No Bluetooth scheduler replacement, stale, retry, or drop during the
  haptics/speaker test.
- Basic controls remain responsive under simultaneous haptics and speaker.
- Haptics continuity is validated before speaker quality and microphone.
