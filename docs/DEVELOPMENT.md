# Development and validation

[简体中文](DEVELOPMENT.zh-CN.md)

## Repository layout

```text
m61/dualsense_hidp_probe/   production firmware and build system
m61/usb_*_probe/            native-USB hardware bring-up probes
tools/                      host tests, flashing, capture and analysis
benchmarks/                 promoted machine-readable performance results
docs/                       maintained bilingual documentation
```

Build output, Opus source/build caches, logs, captures, binaries, vendor PDFs,
and `artifacts/` are intentionally ignored.

## Offline gate

Run before every source commit:

```powershell
python tools\run_offline_checks.py
python tools\test_dualsense_crc32_nibble.py
python tools\test_m61_feature_bridge.py
python tools\verify_m61_build_environment.py --sdk C:\work\bl_mcu_sdk --toolchain-bin C:\work\toolchain_gcc_t-head_windows\bin
git diff --check
```

Run a locked release build for changes touching C/CMake, patches, memory,
toolchain flags, or build scripts. Check the generated manifest has
`\"profile\": \"release\"` and run the RAM/TCM gate against the ELF.

## Hardware gate

Functional changes require:

1. native USB composite enumeration;
2. Bluetooth control and interrupt channels connected;
3. full `0x31` input activity;
4. output LED/rumble/adaptive-trigger behavior;
5. speaker, HD haptics, headset route, and microphone as applicable;
6. Feature GET/SET pages for changes near the HID bridge;
7. zero relevant queue/codec/BT errors.

Performance changes additionally require repeated fixed 90-second runs and an
entry in `benchmarks/PERFORMANCE_BEST.csv` only when promoted.

## Realtime coding rules

- Keep USB and Bluetooth callbacks bounded and non-blocking.
- Express ownership with a queue/state transition, not a shared informal flag.
- Keep one policy point for Bluetooth TX and one writer for codec statistics.
- Preserve generation/deadline metadata across queues.
- Do not allocate, log, read HPM counters, or switch clocks in release hot
  paths unless the cost is measured and required.
- Place code/data in SRAM selectively; every placement change needs release,
  not only stage-profile, measurements.
- Preserve bit-exact Opus/PCM results and all quality invariants.

## Build profiles

Release settings live only in the JSON lock and build-script defaults. New
diagnostics must have a compile-time switch, default off, and appear in the
manifest. Do not reuse a diagnostic binary as the release baseline.

## Source and documentation policy

- Keep public maintained documents in English/`.zh-CN.md` pairs.
- Prefer one current feature/architecture/performance document over dated
  planning diaries; Git history preserves old experiments.
- Never commit local absolute paths, COM-port captures, pairing data, SDK
  trees, compiler binaries, or vendor PDFs.
- Use focused commits. Effective optimizations and correctness fixes get
  their own commit; documentation and performance-table updates accompany
  the change they describe.
- `master` is the development mainline. Use short-lived topic branches for
  risky experiments and merge only validated outcomes.

## Management protocol work

Do not expose shell text directly over WebHID. A management command must have
a versioned binary schema, length/range validation, capability discovery,
bounded asynchronous execution, explicit persistence semantics, and host
tests. Unknown fields/versions must fail safely.
