# TreeMiner improvement plan

**Status:** review + plan (not an implementation authority — `treeminer/PLAN.md` still owns the journal/submit contract).
**Date:** 2026-08-12
**Tree:** `4034ea1` (`main`, 2 commits ahead of `origin/main`)
**This box:** 2× RTX 3060 (sm_86), host `nvcc` 11.5, driver NVIDIA 595.84 / CUDA 13.2. Live process is `treeminer.service` (not `xenblocks-pub-miner`). Journal at `runtime-live/`. Host reset storm 2026-08-10–13 — see `docs/09-ops-stability.md` before changing the live miner.

This document is Grok’s deep dive after Fabel’s Codex review. Fabel already landed:

- `fd46dd2` — startup CPU/CUDA self-test + ConsoleLog `line()` / `interactiveTerminal()` + first humanized log pass
- `4034ea1` — restored outage/margin/reason/pool-DOWN, latched outage span, LAN dashboard bind

Those two commits are good. What follows is what is still wrong, what is still slow, and how to ship 20/30/40/50-series binaries without pretending AMD is a week of work.

---

## 1. What we are optimizing for

Three product constraints, in this order:

1. **A find that exists on disk is never silently lost, and the operator can see that.** Pretty logs that hide outage state are a regression.
2. **Valid hashes, as fast as the card allows.** The server re-verifies Argon2id (`t=1,p=1,hash_len=64,m=difficulty`). Faster invalid hashes are worthless.
3. **One Linux release binary that actually boots on Turing through Blackwell.** AMD is a later market, not a Phase-2 kernel rewrite.

---

## 2. Logging — pretty and true

### 2.1 What is good now

`ConsoleLog` (`treeminer/src/ConsoleLog.h`) is the right TTY primitive: one mutex, `\033[2K\r` only on an interactive terminal, `NO_COLOR` no longer leaves event text spliced into the progress row.

After `4034ea1`:

- SUBMIT shows `m=` vs server difficulty and a signed margin, plus `c.reason` on every non-Acked outcome (`SubmissionManager.cpp:750-780`).
- Breaker OPEN names the cause and queue depth; RECOVERED prints a **latched** outage span (`last_outage_span_ms_`). That latch is a real bugfix: `updateMargin_` zeroes the live clock before the breaker fully closes, so `outageDurationMs()` at RECOVERED used to print `0s`.
- Difficulty poller logs first-failure `e.what()` and the 3-strike DOWN escalation.
- Live status line again prints `pool DOWN` and unconfirmed count.
- File log still has the structured `found id=` / `UPLINK …` lines.

`docs/MINER_EXPERIENCE.md` is the product brief we have not finished implementing.

### 2.2 Remaining accuracy bugs

| Severity | Bug | Evidence |
|---|---|---|
| **HIGH** | Status-line `pool DOWN` uses `outageDurationMs() > 0`. That is **0 in HalfOpen**. Unless the *poller* also tripped `globalDifficultyEndpointDown`, the red token vanishes while the breaker is still probing. | `main.cpp:764-765`, `SubmissionManager.cpp:407-420`, `884-885` |
| **HIGH** | `confirmed` / `unconfirmed` on the live line are **increment-only session counters**. `confirmStep_` can Ack a row and never increments `metrics_.acked` (only `reconciled_via_get_block`). Later Ack does not decrement `accepted_unconfirmed`. The line can show `3 confirmed • 5 unconfirmed` after everything is Acked. | `SubmissionManager.cpp:848` is the only `++metrics_.acked`; `confirmStep_` at `:919-924` |
| **MED** | Half-open is silent on the default scrolling console (`return;` on HalfOpen). Dashboard/TUI know `probing`; `--display logs` does not. | `SubmissionManager.cpp:884-885` vs `MiningCommon.cpp:158-159` |
| **MED** | Live line prints raw `diff` (server cache), never `margin_in_effect` / `effective_difficulty`. SUBMIT has margin; the glance line does not. | `main.cpp:1047` vs `735-746` |
| **MED** | Three vocabularies for one state: SUBMIT says `confirmed`, dashboard last-uplink says `accepted`, file says `UPLINK CONFIRMED`. `AcceptedUnconfirmed` is dumped as the enum name. | `logStatus`, `emitOutcome_`, `MiningCommon.cpp:146` |
| **MED** | `--display terminal` never sees `ConsoleLog::event`. Find/SUBMIT/NETWORK print onto the alternate screen and race the TUI. The EVENTS pane only gets `Logger::logToConsole` crumbs and looks for `CONFIRMED`/`PARKED` words that never arrive. | `main.cpp:1054-1060`, `TerminalUi.cpp:185-188, 298-301` |
| **LOW** | File `logN.txt` does not copy ConsoleLog events. Post-mortem of a TTY session is a different dialect. | `Logger::log` vs `ConsoleLog::event` |
| **LOW** | Dashboard default bind is `0.0.0.0` (`main.cpp`). Intentional for Vast.ai / Docker: map 42069 and open the printed URL. No token. `--dashboard-bind 127.0.0.1` hides it. |

### 2.3 Aesthetic direction

Keep the current visual grammar — dim timestamp, 6-char level, 12-char component, ` • ` bullets — but stop mixing implementation nouns into the operator sentence.

Two products, one process (this is `MINER_EXPERIENCE.md`, not a new invention):

| Surface | Job | Contents |
|---|---|---|
| Pretty console / TUI / dashboard | Decide | One sentence per *state change*. No raw keys, no enum names. |
| File + journald | Diagnose | One structured line (JSON or stable `k=v`) with id, http, status, reason, `server_m`, margin, `outage_ms`, next probe. |
| Status `\r` line | Glance | **Gauges**, not increment-only counters. |

Recommended glance line:

```text
372.1 kH/s  •  2 GPUs  •  02:32  •  q 12  •  3 confirming  •  net DOWN 4m12s  •  probe 8s  •  m 1100 (+100)
```

Rules:

- `net` comes from breaker state (`Closed` / `HalfOpen` / `Open`), never from `outageDurationMs() > 0`.
- Duration stays visible until Closed.
- Drop the word `confirmed` from the glance line, or drive it from a **journal gauge**, not `metrics.acked`.
- Retry storms stay in the file. Console: first failure + periodic “still down, N held, next probe”.

Operator copy (adopt the existing table, do not invent a third dialect):

| Internal | Console |
|---|---|
| Journal append | `Find secured locally` |
| Breaker open | `Network offline — mining continues` |
| Half-open | `Network probing` |
| 200, lookup pending | `Accepted — confirming` |
| `/get_block` match | `Accepted and confirmed` |
| Parked difficulty | `Held until difficulty ≤ m` |
| Permanent invalid | `Rejected — review required` |

One stdout owner: in `--display terminal`, `ConsoleLog::event` enqueues into the TUI. Today both write `std::cout`.

### 2.4 Logging PRs

1. **Accuracy hotfix** (no copy rewrite): HalfOpen log + `pool_down` from breaker state; status-line gauges; `metrics.acked` on delayed confirm *or* stop displaying increment-only counters. Dashboard stays `0.0.0.0` with no token for cloud port-maps.
2. **Single event, two sinks**: structured file line + pretty console from the same `OperatorEvent`. Delete the dual `UPLINK` + `SUBMIT` pair.
3. **TUI / dashboard consume the same event**. Outage duration + margin on the delivery panel. Stop the hash-rain from racing event stdout.
4. **Contract tests** pin fields and invariants (`outage_ms` latched, DOWN at 3, half-open visible, no journal I/O on the hashrate callback), not exact ` • ` marketing strings.

---

## 3. Hashing — fast, only if valid

### 3.1 What the engine actually is

`argon2_kernel_oneshot` (`kernelrunner.cu:811-874`, launch `:1158-1164`): **one hash per CUDA block, one warp (32 threads), 1 KiB shared**. State in registers, Blake2b `G` in generic PTX (`add.u64`, `mul.hi.u32`, `prmt.b32`). Indexed slices 0–1 and data-dependent slices 2–3 are already split.

`argon2_first_blocks_kernel` (`:876-911`): one *thread* per hash, device Blake2b prehash into blocks 0 and 1. Historically the largest win (+108–131% in the Woody ledger). **It is illegal to enable today.**

Mining path (`MineUnit.cpp:117-123`):

```cpp
request.gpu_first_blocks = hashapi::kGpuFirstBlocksEnabled; // true on CUDA 13+
```

Reproduced on this box: CPU reference and CUDA-with-CPU-first-blocks agree; CUDA-with-GPU-first-blocks diverges; the server correctly 401s `Hash verification failed`. The oneshot kernel is therefore trustworthy. The device prehash is not.

CPU `Argon2Params::initialHash` hex-decodes the salt (`argon2params.cpp:120-124`). The GPU kernel hashes whatever bytes `prepareInputBlocksOnDevice` uploaded (`kernelrunner.cu:1094-1122`). Password plumbing looks parallel (64-hex key uploaded as bytes). The mismatch is almost certainly **device Blake2b / `device_initial_hash` packing**, not the memory-hard loop. Next step is a known-vector dump of host vs device *first-block bytes* before oneshot, not another end-to-end guess.

Roofline (`docs/06` §B.2): at d4096 this kernel is **integer-ALU bound** (~40% of a 3050-class card’s DRAM). Headroom is occupancy and latency hiding, not “rewrite the C.” Honest Phase-3 target after first-blocks is fixed: **+25–60%** on consumer cards. Claims of easy 2× are not supported.

Current high-d baseline (GPU-first, now **off** in production): ~10.7 kH/s at d4096, ~5.6 kH/s at d8192 on the Hash API ledger. With first-blocks off we pay the CPU prehash + HtoD of 2 KiB × batch every batch. At realistic `m` that is not 90% of wall time (`compute_ms` was ~92–93% even *with* GPU first-blocks), but it is the first legal win.

### 3.2 Correctness gates we have / lack

Have:

- Startup `runCpuCudaSelfTest` per device, fail-closed, before journal/network/mining (`main.cpp:540-566`). Compares CPU PHC digest vs CUDA bare digest.
- Fake-backend unit test (`tests/unit/hashapi/HashApiSelfTestTests.cpp`).
- `kGpuFirstBlocksEnabled` as the single switch.

Lack:

- Self-test is **one key, `batch_size=1`, `m=8`**. A batched-only bug can still slip through. Committed CPU first-block 2 KiB goldens now exist so a GPU box can diff device blocks 0/1 without re-deriving the host reference. Mining uses GPU first-blocks (`kGpuFirstBlocksEnabled = true`); the startup self-test exercises that flag.
- No committed golden vectors of host vs device first-block memory.
- `docs/HASH_OPTIMIZATION_GOAL.md` still says mining uses “the validated GPU first-block path.” That sentence is **stale and dangerous**. Update it the same PR that documents the disable.

CPU sidecar (`--cpuWorkers`, default ceiling 100) is a correctness-friendly spare lane at low difficulty, not a substitute for the kernel.

### 3.3 Ranked hash work

Do **not** enable occupancy / `cp.async` / `__launch_bounds__` on the live unit without Nsight + golden hashes. `__launch_bounds__` already burned a 10.7 → 5.9 kH/s regression in the ledger. GPU first-blocks is **on** (CUDA 13).

| Order | Work | Why |
|---|---|---|
| **H1** | Done: GPU first-blocks isolated; CUDA 13 matches CPU. Remaining: GPU box diffs device blocks 0/1 against committed host goldens in `tests/unit/hashapi/goldens/`. | Host vectors are GPU-free. |
| **H2** | Done: `kGpuFirstBlocksEnabled = true` with startup self-test fail-closed. Keep the one-line kill switch. | Production speed. |
| **H3** | Device-side finalize + hit-only DtoH (`docs/06` B.3 #4). Host `HostHitBuffer` ownership model is in tree (CPU goldens). Kernel not wired. ~4% and it deletes the CPU finalize thread. PLAN §10.10: needs the persistent hit-buffer model; upstream failed on output-buffer lifetime. | Phase 2, golden-gated. |
| **H4** | Double-buffered streams per GPU (already have `--cudaStreams 1\|2`). Overlap batch B first-blocks/finalize with batch A oneshot. | Small, safer after H3. |
| **H5** | Multi-warp flag is in tree (`--warpsPerBlock`, default 1). Indexed-half ref table is in tree (`--precomputedRefs`, default off) with CPU goldens. Next: Nsight occupancy dump + both goldens on a GPU box (`scripts/occupancy_canary.sh`), then `cp.async` on sm80+. Per-arch autotune keyed on `(cc, m-band)`. | Phase 3. Honest +25–60%. Do not enable N>1 or precomputed refs on the live unit until the canary is green. |

Dead ends (do not reopen): `__launch_bounds__`, source-lane-only address selection, CPU hot-path rewrites, L1 carveout games, “1000%” as a near-term claim (`HASH_OPTIMIZATION_GOAL.md` aspirational 11× is a direction, not this quarter).

---

## 4. Compatibility — 20 / 30 / 40 / 50, and AMD later

### 4.1 What the build system already does

`CMakeLists.txt:18-92` precedence:

1. Explicit `-DCMAKE_CUDA_ARCHITECTURES`
2. `nvidia-smi --query-gpu=compute_cap` (this box → `86`)
3. Toolkit-tier fat list on GPU-less hosts (`75…90`, plus `120` only if nvcc ≥ 12.8)

The kernel has **no** `__CUDA_ARCH__` specials, no tensor cores, no `cp.async` yet. Turing through Blackwell can run this SASS. What fails is **missing cubin**, not an illegal instruction.

`ComputeBackend` is still a CUDA-shaped interface (`enumerateBackends()` → `CudaBackend::enumerate()`). An opt-in HIP/ROCm backend now compiles the same sources (`-DTREEMINER_GPU_BACKEND=HIP`); NVIDIA CUDA remains the default binary. HiveOS still points AMD at `levykrak/xengpuminer` until a TreeMiner ROCm artifact is the advertised download.

### 4.2 Honest matrix

| GPU | Kernel source | Current **Linux CI** (`nvidia/cuda:11.8` in `.github/workflows/linux-build.yml`) | **This machine’s** `build-sm86-gcc10` binary |
|---|---|---|---|
| 20-series sm_75 | Fine | Native `75-real` in the GPU-less fat default | **No** — cannot JIT newer PTX |
| 30-series sm_86 | Fine | Native | **Native** (what we run) |
| 40-series sm_89 | Fine | Native | JIT from `compute_86` (works, first-launch tax) |
| 50-series sm_120 | Fine (no 12.0 ISA used) | **Not emitted** (CI toolkit 11.8 < 12.8) | JIT hope, unproven as a product |
| Datacenter Blackwell sm_100 | Not listed | No | Unknown |
| AMD GPU | HIP opt-in (`TREEMINER_GPU_BACKEND=HIP`) | ROCm CI lane | No (this box is NVIDIA) |
| CPU-only process | Sidecar / `hashapi-cli` | `xenblocksMiner` still requires CUDA init | Sidecar after GPU init |

Docs oversell 50-series. `BUILD_INSTRUCTIONS.md` and the Windows `cuda-release-vcpkg-modern` preset talk about a 12.8 fat binary with `120`. Linux CI cannot produce that artifact. Configuring **on** a 50-series box with toolkit < 12.8 **fatal-errors** instead of falling back to highest virtual PTX.

Detect-on-this-3060 is why a TreeMiner zip from this host will not boot a 2080. That is the real 20-series gap, not the kernel.

### 4.3 Compatibility roadmap

**Ship one Linux fat package. Keep per-arch presets for local/benchmark builds.**

1. Move Linux release CI to **CUDA 12.8+**. Fat list: native `75,80,86,87,89,90` + `120-real` + last-arch PTX. Two artifacts only if you also need a CUDA 11.8 build for ancient drivers.
2. Add a **Linux** `cuda-release-vcpkg-modern` preset (today’s preset is `nvcc.exe` / `CUDA_PATH_V12_8`).
3. Detect fallback: if local CC > toolkit max, use highest virtual PTX instead of `FATAL_ERROR`.
4. Runtime: if `cudaErrorNoKernelImageForDevice`, **skip that card**, mine the others, log CC. Today the process dies at self-test for everyone.
5. Point `HIVEOS.md` at TreeMiner artifacts once CI actually contains 75+89+120. It still installs Woody 1.3.1.
6. HIP is opt-in (`-DTREEMINER_GPU_BACKEND=HIP`). Do not change the default CUDA kernel, occupancy experiment, or live `treeminer.service`. The PTX `g()` stays NVIDIA-only; ROCm uses the C++ `g1()` form behind `TREEMINER_GPU_HIP`. Occupancy N>1 and precomputed refs stay default-off on both vendors.
7. CPU remains a sidecar and a no-GPU `hashapi-cli`. Do not market the CPU path as an AMD miner.

50-series is a **builder + CI** problem, not a kernel problem. 20-series is a **stop shipping detect-on-3060 zips** problem.

---

## 5. What not to do

- Re-enable `gpu_first_blocks` because a benchmark looks faster.
- Commit another logging rewrite that drops `outage_ms` / margin / `pool DOWN`.
- Multi-warp or `__launch_bounds__` without Nsight + golden hashes.
- Promise AMD or “1000% faster” in a release note.
- Treat `HASH_OPTIMIZATION_GOAL.md` Current Control Summary as mining policy; the ledger below it is historical.
- Push these two local commits (`fd46dd2`, `4034ea1`) until this plan’s H1/logging-hotfix priority is agreed — they are already the right base, just unpushed.

---

## 6. PR plan

Each PR independently reviewable. Kernel diffs stay zero until H1.

| PR | Title | Files | Depends | Notes |
|---|---|---|---|---|
| **P0** | Logging accuracy hotfix | `SubmissionManager.cpp/.h`, `main.cpp`, `test_hash_api_contract.py` | — | HalfOpen visible; glance line uses breaker state + gauges; delayed-ack counts; decide dashboard bind default. |
| **P1** | Document first-blocks ban + stale goal file | `HASH_OPTIMIZATION_GOAL.md`, `CHANGES-FROM-UPSTREAM.md`, `PLAN.md` kernel note | — | Stop telling future agents that GPU first-blocks is the mining path. |
| **P2** | First-block known vectors + device Blake2b fix | `kernelrunner.cu`, `blake2b*`, new `tests/` golden, extend `HashApiSelfTest` to `gpu_first_blocks=true` | P1 | Mining switch stays **false** until vectors pass. |
| **P3** | Re-enable GPU first-blocks | `HashApiTypes.h`, self-test, canary notes | P2 | One constant flip after a live 401-free run. |
| **P4** | Linux fat-binary CI (75…89 + 120) | `.github/workflows/*`, `CMakeLists.txt`, `CMakePresets.json`, `HIVEOS.md` | — | Parallel with P0–P2. Skip-bad-GPU at runtime. |
| **P5** | Structured log + pretty sink | new event type, `Logger`, `ConsoleLog`, TUI, contract tests | P0 | MINER_EXPERIENCE language. |
| **P6** | Device finalize + double-buffer | `kernelrunner.cu`, `CudaHashBackend.cpp` | P3 | PLAN Phase 2. Golden-gated. |
| **P7** | Occupancy / prefetch / per-arch autotune | kernel + `HashApiTuning` | P6 | PLAN Phase 3. Ledger every reject. |
| **P8** | AMD/HIP | `GpuRuntime.h` + HIP CMake | landed, opt-in | Default binary stays CUDA. Do not enable HIP on the live NVIDIA box. |

---

## 7. Key decisions

1. **Pretty console is allowed to be brief; it is not allowed to be wrong.** Half-open and outage duration are product features, not decorations.
2. **GPU first-blocks stays off until byte-level goldens pass.** Speed work that reintroduces 401s is a revenue bug, not an optimization.
3. **Distribute a fat Linux binary; detect-on-build is a developer convenience.** 20-series and 50-series fail today for packaging reasons, not kernel reasons.
4. **AMD is opt-in HIP, not the default download.** NVIDIA fat binary stays the product; `-DTREEMINER_GPU_BACKEND=HIP` is the AMD farm path. CPU sidecar is still the no-GPU answer.
5. **Dashboard is public operator telemetry on `0.0.0.0:42069`.** Hashrate, queues, and a truncated address are not treated as secrets. Vast.ai / Docker: map the port. Use `--dashboard-bind 127.0.0.1` only when you want it hidden.

---

## 8. Open questions

1. **Dashboard bind (resolved):** keep `0.0.0.0` for Vast.ai / Docker. The page is public operator telemetry (hashrate, queues, truncated address). No token, SSH, or VPN. Map port 42069 and open the printed URL. `--dashboard-bind 127.0.0.1` hides it.
2. Do we want a CUDA 11.8 *and* a 12.8 artifact, or only 12.8+ (drops pre-Turing fat defaults on CUDA 13)?
3. Is a live 50-series canary available before we claim sm_120 support?
4. After P0, should TreeMiner replace `xenblocks-pub-miner` on this box so the 35 pending journal rows drain?
