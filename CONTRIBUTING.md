# Contributing

[简体中文](CONTRIBUTING.zh-CN.md)

Thank you for improving the M61 DualSense bridge. Read
[Development and validation](docs/DEVELOPMENT.md) and
[Building and flashing](docs/BUILDING.md) first.

Keep changes focused, preserve the locked release profile, add deterministic
tests for protocol/algorithm changes, and provide hardware evidence for
hardware-dependent behavior. Performance pull requests must preserve audio
quality and report average, P95, P99, maximum, cycles, underflows, and all
hard-error counters under the fixed load.

Do not commit generated artifacts, SDK/toolchain trees, controller secrets,
vendor PDFs, or captures containing personal device data. User-facing
documentation changes require matching English and Simplified Chinese files.
