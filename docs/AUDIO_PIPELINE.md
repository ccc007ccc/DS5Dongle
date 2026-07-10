# Audio Pipeline

## Speaker PCM Frame Size

The M61 USB audio interface runs at 48 kHz. The Bluetooth speaker payload is
Opus encoded with a 10 ms frame, so the encoder input is exactly 480 samples
per channel:

```text
48000 samples/s * 10 ms = 480 samples
```

Opus does not accept a 512-sample frame at 48 kHz. Valid frame sizes are tied
to 2.5/5/10/20/40/60 ms durations; passing 512 to `opus_encode()` returns
`OPUS_BAD_ARG`.

The pinned upstream implementation accumulates 512 samples and uses a WDL
resampler configured as 51.2 kHz -> 48 kHz before calling Opus with 480
samples. That compensates for its USB/buffering design; it is not an Opus
performance option.

The M61 path does not need that conversion. It now accumulates 480 native
48 kHz samples and sends the same buffer directly to the Opus task. There is
no 512 -> 480 speaker resampling stage.

## Haptics

The separate haptics path still reduces the 48 kHz actuator PCM to the
DualSense 3 kHz haptics block rate (factor 16). That conversion is required by
the controller report format and is unrelated to the removed speaker-frame
resampler.
