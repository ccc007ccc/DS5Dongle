# Performance and benchmark policy

[简体中文](PERFORMANCE.zh-CN.md)

## Objective

The optimization target is smooth simultaneous DualSense input, output,
speaker/headset audio, HD haptics, and microphone transport. Tail latency is
more important than a small average improvement:

1. no queue, deadline, stale, Bluetooth, codec, or packet-shortfall errors;
2. lower P95/P99/maximum encode and decode latency;
3. lower Bluetooth build/send maximum and audio pair age;
4. only then lower average cycles.

Quality is immutable: 48 kHz, 16-bit host audio, channel layout, 160 kbit/s
speaker Opus, medium-band, 10 ms codec frames, and the controller packet
format are not reduced to win a benchmark.

## Release performance configuration

The build lock makes the measured options the default: local `-O2` for the
USB/codec translation unit, Opus fixed-point `-O2 -flto`, E907 bit-exact
intrinsics, exact 512:480 resampling, D4 decode parser fast path, decode-MDCT
SRAM placement, a 16-entry Flash CRC table, and a 1 ms paired codec window.
All profiling is off.

The release memory result is:

| Region | Used | Capacity |
| --- | ---: | ---: |
| Application RAM | 215,584 B | 319 KiB |
| Non-cache alias usage | 10,888 B | 319 KiB alias |
| WRAM application section | 16 B | 128 KiB after controller reserve |
| ROM/XIP | 873,432 B | 4 MiB |

## Fixed hardware loads

`full-duplex-v1` runs for 90 seconds with four-channel 48 kHz/16-bit USB OUT,
speaker audio, HD haptics, 20 ms HID output activity, and an open USB
microphone endpoint receiving live controller Opus. `speaker-only-v1` keeps
the same OUT/HID load but disables mic decode and is not ranked against full
duplex.

For each run report:

- encode/decode average, P50, P95, P99, maximum;
- average cycles (and instret/cache counters in diagnostic builds);
- mic queue age and USB IN underflow delta;
- epoch/drop/deadline/stale counters;
- Bluetooth allocation/send/retry/error counters;
- host packet shortfall and subjective audio/haptics behavior.

## Current promoted results

The authoritative table is
[`benchmarks/PERFORMANCE_BEST.csv`](../benchmarks/PERFORMANCE_BEST.csv).
The current Bluetooth/tail default is commit `6bf8714` on top of
`4f8dfea` and `992111b`. Its three 400 MHz full-duplex runs had:

| Metric | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Encode avg/P95/P99/max ms | 3.673/4.500/4.750/5.138 | 3.483/4.250/4.750/5.138 | 3.530/4.250/4.750/5.138 |
| Decode avg/P95/P99/max ms | 2.735/4.000/4.000/4.530 | 2.762/3.750/4.000/4.687 | 2.762/3.750/4.000/4.795 |
| Codec average cycles | 2,566,094 | 2,500,302 | 2,518,108 |
| Mic underflow delta | 11 startup | 0 | 0 |
| All hard errors | 0 | 0 | 0 |

The same CRC change reduced profiled Bluetooth total average from 5.450 to
4.113 ms and maximum from 36.297 to 18.962 ms.

The 320 MHz stereo/mic-off baseline is valid only for speaker capacity:
encode avg/P95/P99/max was 6.026/7.000/7.500/7.655 ms with zero hard errors.

## Promotion rule

An optimization is promoted only after bit-exact host tests, offline gates,
locked release builds, repeated 90-second hardware runs, zero hard errors,
and user-visible function checks. A faster diagnostic-stage number alone is
insufficient; multiple SRAM-placement and unrolling experiments were rejected
because release P95/P99 regressed.

Effective changes are committed immediately and added to the CSV. Rejected
ideas are reverted without publishing a replacement firmware solely for the
rollback.
