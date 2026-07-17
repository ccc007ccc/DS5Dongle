# Performance data

[简体中文](README.zh-CN.md)

`PERFORMANCE_BEST.csv` is the maintained, machine-readable table of promoted
hardware results. A row is added only when:

- the fixed load and audio format are unchanged;
- average, P95, P99, maximum, cycles, underflow delta, and all hard errors are
  recorded;
- protocol/PCM bit-exact tests pass;
- at least two stability runs pass for a new release default;
- the effective change has its own Git commit.

Blank decode fields mean the microphone workload was deliberately disabled;
such a row is never compared with full-duplex results. Historical rejected
experiments remain available in Git history instead of cluttering the current
best table.
