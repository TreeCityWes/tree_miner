# Universal Miner Setup Design

## Purpose

Make the miner usable on a broad range of machines without sacrificing reliable
headless operation. A first-run setup wizard will validate the mining address,
inspect available hardware, recommend a safe GPU stream count, optionally enable
CPU sidecar mining, and persist the selected configuration. Later starts should
load the saved configuration without requiring interaction.

This document is the design breadcrumb for the implementation. It records the
constraints, measured evidence, and rollout order so future changes preserve
protocol correctness and server compatibility.

## Implementation Status

As of 2026-08-11, the miner has an explicit `--cudaStreams 1|2` control with a
fixed shared VRAM budget, joined GPU worker shutdown, and an opt-in
`--cpuWorkers N` sidecar. CPU work uses the server-selected difficulty,
Argon2id `t=1,p=1`, thread-safe XUNI-window checks, the existing journal-first
submission callback, and source-labeled discovery logs. CPU workers default to
disabled. Hardware-plan reporting, persistent versioned configuration, and the
interactive setup wizard remain future phases.

## Terminology

Use **CUDA streams** for independent GPU work pipelines. Do not describe this
setting as "lanes."

Argon2 lanes are protocol parameters. The Xenblocks work format currently
requires Argon2 parallelism `p=1`; changing it changes the hash and can make
submissions invalid. CUDA streams may pipeline independent valid `p=1` hash
batches, but they must not alter Argon2 inputs or parameters.

## First-Run Setup Wizard

The wizard runs only when no valid local configuration exists, or when the user
explicitly invokes `xenblocksMiner setup`.

Proposed flow:

```text
TreeMiner Setup

Mining address:
0xe4bB...BC9bC
Is this correct? [Y/n]

Detected GPU:
NVIDIA RTX 5060 Ti, 16 GiB, Blackwell sm_120

Recommended CUDA streams: 2
  1: lower memory pressure and power draw
  2: recommended from measured throughput
Select [2]:

Detected CPU:
AMD Ryzen 9 7900X
12 physical cores / 24 logical threads
62 GiB system RAM

Enable CPU sidecar mining? [y/N]
CPU workers [4]:

Configuration summary:
  GPU 0: enabled, 2 CUDA streams
  CPU sidecar: enabled, 4 workers
  Address: 0xe4bB...BC9bC
Save and start? [Y/n]
```

The implementation must validate the EVM address before writing configuration.
It must clearly state whether the GPU, CPU sidecar, or both are enabled in the
final summary.

## Non-Interactive Compatibility

The wizard is optional. Existing server, HiveOS, container, and automated restart
workflows must remain non-interactive.

The following command surface is proposed:

```text
xenblocksMiner setup
xenblocksMiner --minerAddr <address> --cudaStreams auto|1|2
xenblocksMiner --cpuWorkers auto|0..N
xenblocksMiner --device <list> --show-hardware --show-plan
xenblocksMiner --non-interactive
```

Rules:

- `xenblocksMiner` loads an existing valid configuration and starts immediately.
- Explicit CLI flags override saved configuration for that process only.
- `--non-interactive` fails with an actionable error when required configuration
  is absent; it must never wait for stdin.
- Environment-variable equivalents may be added for container platforms, but
  command-line options remain the canonical documented interface.
- The current command line remains supported during the migration.

## Hardware Detection And Recommendations

At setup time, detect:

- CUDA devices, compute capability, device name, total and available VRAM.
- CPU model, physical-core count, logical-thread count, and available system RAM.
- OS and CUDA runtime information needed to make support decisions.

Recommendations are conservative and overrideable:

- A known GPU may use a stored, measured stream recommendation.
- An unknown GPU starts at one CUDA stream unless a short autotune is requested.
- GPUs with insufficient free VRAM are disabled with an actionable explanation.
- CPU sidecar defaults off on laptops or thermally constrained devices.
- CPU workers reserve at least two physical cores for the OS, GPU preparation,
  submissions, and journal work.

`--show-hardware` prints detected capabilities without starting mining.
`--show-plan` prints the resolved effective configuration, including CLI
overrides, without starting mining.

## GPU Autotune

Autotune compares one and two CUDA streams while preserving a fixed total VRAM
budget. It must not allocate a full single-stream batch per stream. Each stream
receives independent buffers and a proportional share of the same total budget.

Autotune requirements:

- Run only on explicit user request or when setup offers it for unknown hardware.
- Validate a representative batch against the existing CPU/reference hash path
  before timing a candidate.
- Use a warm-up period and multiple timed samples at the current difficulty.
- Cache a result by GPU identity, driver/runtime version, and difficulty band.
- Select two streams only when the median gain exceeds a conservative threshold
  and run-to-run spread is acceptable.
- Default to at most two streams initially. More streams require separate design,
  measurements, and an explicit user request.

## CPU Sidecar Mining

CPU sidecar mining runs independent valid Argon2 work on CPU workers while the
GPU continues hashing. It uses the same journal-first submission path as GPU
finds, so retries, confirmation, and queue metrics remain unified.

The CPU sidecar must:

- Be independently enabled or disabled with `--cpuWorkers`.
- Use worker limits that do not starve the GPU feeder, submitter, journal, or OS.
- Report CPU hashrate separately from GPU and combined hashrate.
- Preserve the same difficulty and protocol parameters as GPU work.
- Stop cleanly before dependent journal and logging objects are destroyed.

System RAM is appropriate for CPU Argon2 workers, not as a substitute for GPU
VRAM. Moving a GPU hash working set over PCIe would introduce more overhead than
it removes.

## Configuration

Store a local, versioned configuration outside source control. The file must be
ignored by Git and must not contain private keys. A wallet address is operational
data and should be treated as local configuration.

Proposed schema:

```json
{
  "version": 1,
  "minerAddress": "0x...",
  "devices": [
    { "device": 0, "enabled": true, "cudaStreams": 2 }
  ],
  "cpuSidecar": { "enabled": false, "workers": 0 },
  "autotune": {
    "enabled": false,
    "cachedRecommendations": []
  }
}
```

Configuration loading must validate the schema and all values. Unknown newer
versions fail safely with a clear upgrade message. Version migrations must be
explicit and tested. Malformed configuration must not silently select risky
defaults; the user should rerun setup or pass explicit non-interactive flags.

## Safety And Fallbacks

- Reserve both a percentage and absolute VRAM headroom before allocating batches.
- On CUDA allocation failure, retry once with one stream and reduced batch size.
- On persistent CUDA errors, disable the affected device, preserve the journal,
  and continue CPU-only only when explicitly enabled.
- Do not begin autotune if active mining is already using the device unless the
  user explicitly accepts the interruption.
- Maintain durable journal-first writes before any submission attempt.
- Preserve accepted/retry/rejected/confirmed logs and separate `Q_XNM` and
  `Q_XUNI` queue counts.
- Shutdown must stop and join all worker threads before destroying their buffers,
  journal, or logging dependencies. The joined GPU worker lifecycle implemented
  during the CUDA stream experiment exited cleanly across repeated one-, two-,
  and four-stream live switches; the same ownership rule applies to CPU workers.

## Current Evidence

Measurements were taken live at difficulty 1100 on an NVIDIA RTX 5060 Ti:

| CUDA streams | Measured hashrate | Result |
| --- | ---: | --- |
| 1 | 159.81 kH/s | Baseline |
| 2 | 189.80 kH/s | +18.8%; GPU reached 99% utilization |
| 4 | 189.82 kH/s | No material gain over two streams |

The two-stream live run produced accepted and confirmed submissions, including
submissions 94, 95, 96, and 97 during the experiment. This confirms that the
two-stream implementation remains compatible with real submission handling, not
only offline hashing.

Power rose from roughly 147 W with one stream to roughly 170 W with two streams,
while performance per watt improved slightly. The recommended initial cap is
therefore two CUDA streams. Four streams are not justified by the measured data.

The local CPU is a Ryzen 9 7900X with 12 physical cores, 24 logical threads, and
about 58 GiB of available system RAM. At difficulty 1100, one persistent CPU
benchmark worker reached about 2.86 kH/s. Four concurrent workers reached
10.39 kH/s aggregate, about 5.5% of the measured two-stream GPU rate. This is
enough to justify an explicit CPU sidecar canary, but not enough evidence to
enable CPU mining by default on unknown machines.

The first combined live canary used two CUDA streams and four CPU workers. It
held GPU throughput near 189.8 kH/s, added about 10.3 kH/s from CPU workers, and
reported about 200.1 kH/s combined without additional GPU VRAM pressure.

## Phased Implementation

1. Fix and test orderly Ctrl-C shutdown for all existing GPU worker threads.
2. Add the CPU sidecar behind an explicit `--cpuWorkers` flag, using the normal
   journal and submission pipeline.
3. Measure 1, 2, 4, and 8 CPU workers at representative difficulties, including
   GPU-on and CPU-only runs.
4. Extract hardware detection and effective-plan reporting through
   `--show-hardware` and `--show-plan`.
5. Add persistent versioned configuration and non-interactive CLI precedence.
6. Add `setup` wizard for address confirmation, stream selection, and optional
   CPU sidecar configuration.
7. Add controlled one-versus-two-stream GPU autotune and recommendation caching.
8. Document deployment presets for HiveOS, containers, and headless systems.

Each phase must be independently usable. The wizard must not be required before
the existing command-line miner can start.

## Testing And Acceptance Criteria

- Unit tests cover configuration parsing, validation, migrations, precedence,
  hardware recommendation rules, and worker-count bounds.
- CUDA and CPU results remain bit-exact with the reference hash implementation.
- Integration tests verify GPU-only, CPU-only, and combined journal/submission
  flows, including retries and restart recovery.
- Shutdown tests demonstrate no use-after-free, crash, orphan worker, or lost
  journal record after Ctrl-C.
- Non-interactive invocations never read stdin and exit clearly when invalid.
- Autotune uses a fixed total VRAM budget and never selects a candidate that
  regresses median hashrate or has unacceptable variance.
- Live canaries produce accepted and confirmed submissions before a new default
  recommendation is published.
- Performance experiments use a 30-second warm-up and five 60-second samples at
  difficulties 100, 1100, and 2100. A candidate needs a median gain above 5%,
  less than 3% sample spread, valid reference parity, and zero CUDA errors.

## Open Questions

- Which local configuration path best fits current platform conventions while
  remaining easy to mount in containers?
- Should a known-GPU recommendation key include the full GPU UUID, or a less
  machine-specific model and compute-capability key for portability?
- How should CPU sidecar power and thermal policies be exposed for laptops?
- Is an automatic background CPU worker tuner appropriate, or should CPU tuning
  remain explicit because it competes with user workloads?
- What VRAM headroom policy is robust across desktop, display-attached, and
  containerized GPUs?
- Should the wizard offer a brief live submission canary, or rely on normal
  mining and journal outcome logs after setup completes?
- Which environment-variable names and packaging constraints are required for
  HiveOS before declaring its preset supported?
