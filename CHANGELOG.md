# Changelog

## 1.4.1 — 2026-09-04

- Add Linux CPU-utilization sampling to the Hash API benchmark harness using
  `/proc/stat`. Linux runs can now report CPU-load trust instead of always
  reporting an unavailable environment. Invalid or unavailable counters remain
  explicitly unavailable; Windows sampling is unchanged.
- Publish a CUDA tuning guide covering batch size, streams, correctness checks,
  and repeatable measurement. A two-RTX-3060 experiment at difficulty 10000
  sustained 31.35 kH/s with two streams per GPU, a batch cap of 448 per stream,
  and one warp per block: approximately 10.2% above its original automatic-batch
  baseline. This is a measured configuration result, not a universal speedup.
- Replace machine-specific operating notes with general troubleshooting guidance.
- Align CMake and Windows miner version metadata at 1.4.1.

Mining defaults, hash semantics, submission behavior, and GPU power settings are
unchanged. Users must explicitly select and validate tuning for their hardware
and difficulty. This release provides source archives; it does not publish a
new prebuilt GPU binary.

Validation: 161 benchmark, comparison, and trend tests (including 109 harness
tests), plus a clean Release build and CLI smoke check with the deterministic
stub backend. The tuning investigation also compared 654 sampled GPU digests with CPU
references across 48 configurations and confirmed the selected settings with a
three-minute production canary. See the [tuning guide](treeminer/docs/CUDA_TUNING.md).
