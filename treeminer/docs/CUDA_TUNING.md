# CUDA hashrate tuning

Batch size, stream count, and warps per block interact. Filling all available
VRAM does not necessarily produce the highest hashrate. Tune at the memory cost
you actually mine: rates at different difficulties are not comparable.

## Measured example

A Release CUDA 13 build on two RTX 3060 12 GiB GPUs was tested at difficulty
10000 (`m=10000`, `t=1`, `p=1`), with GPU first-block preparation enabled after
startup validation. Both GPUs ran together during stream/batch measurements.

| Configuration | Combined H/s | Evidence |
|---|---:|---|
| Two streams per GPU, automatic batch, one warp | 28,455 | Fastest original baseline median |
| Two streams per GPU, batch 384 per stream, one warp | 30,937 | 90-second production canary |
| Two streams per GPU, batch 448 per stream, one warp | 31,347 | 180-second production canary |

The final configuration improved throughput by approximately **10.2%** over
the original baseline. No hash algorithm, GPU clocks, or power limits changed.
These results do not establish an optimum for other GPUs or difficulties, or
measure network acceptance yield. The miner's defaults remain unchanged.

The final batch scan held two streams and one warp fixed. Each setting warmed
for 15 seconds, then measured three 30-second intervals. Batch 384 was repeated
before, within, and after the scan; before/after drift was -0.009%.

| Batch cap per stream | Median combined H/s | Change from fastest 384 baseline |
|---:|---:|---:|
| 320 | 31,008 | +0.20% |
| 352 | 30,744 | -0.65% |
| 384 | 30,946 | baseline |
| 416 | 31,194 | +0.80% |
| 448 | 31,346 | +1.29% |

Batch 448's sample spread was 0.050%; its slowest sample exceeded the fastest
sample from any batch-384 baseline by 1.20%. Earlier sweeps found no useful gain
from precomputed references. Eight warps helped some batch/stream combinations
and hurt others; it was not the final choice.

To try the measured profile, add these options to your existing miner command:

```text
--cudaStreams 2 --batchSize 448 --warpsPerBlock 1
```

`--batchSize` is a per-stream cap. Automatic memory limits can select a smaller
batch when necessary. Re-measure after a material change in difficulty or hardware.

## Reproduce a tuning experiment

1. Record the binary, build type, GPU class, difficulty, and current settings.
   Validate CPU/GPU digests before measuring an experimental kernel path.
2. Pause competing GPU work for the measurement. Keep clocks and power settings
   fixed, warm the miner, and compare completed attempts over repeated intervals.
3. First compare warps and references with batch size held fixed. Then compare
   stream count and batch caps using the actual mining loop.
4. Repeat the baseline before and after candidates. Reject unstable results or
   repeat the experiment if baseline drift is comparable to the claimed gain.
5. Confirm a winning configuration in sustained production mining, preserving
   the previous launch configuration for rollback.

The committed Hash API harness can isolate a single-stream configuration. From
the repository root, with the GPU available:

```bash
python3 treeminer/scripts/hash_api_benchmark.py \
  --binary build/bin/xenblocksMiner \
  --warmup 1 --repeat 3 \
  --preflight-report-quality --fail-on-report-quality \
  --scenario 'name=d10000-b448-w1,backend=cuda,device=0,difficulty=10000,batch_size=448,seconds=15,gpu_first_blocks=true,warps_per_block=1,allow_xuni=false' \
  --output treeminer/.benchmarks/d10000-b448-w1.json
```

The harness warm-up is a separate CLI invocation; it warms the device but does
not remove backend initialization from each measured invocation. A Hash API
single-stream result does not establish a gain in a multi-stream mining service.

For whole-miner measurements, run the miner in a separate working directory with
`--testFixedDiff 10000 --donotupload`, a separate `--journalPath`, and a loopback
dashboard on an unused `--dashboard-port`. Supply your normal required identity
arguments and the tuning flags being tested. Fixed-difficulty test mode disables
the submission manager; production journal files should not be used for tests.

Read the uncached `/api/rig` endpoint and calculate each stream's change in
`gpus[].hash_count` over monotonic elapsed time, summing rates for the rig. Use
`index` and `stream` to identify lanes. Abort on missing lanes, counter resets,
stalls, or fatal durability errors. The dashboard's displayed average since
startup is not a substitute for these fixed measurement intervals.

Machine-specific service wrappers, raw reports, and deployment backups are local
operator artifacts, not release dependencies. The reusable benchmark harness is
under `scripts/`; keep local reports in the ignored `.benchmarks/` directory.

## Measurement limits

Linux CPU-load sampling uses `/proc/stat` over a 100 ms interval. It avoids
double-counting guest CPU time and treats iowait as idle. Missing, malformed,
or invalid counter intervals report unavailable metadata. CPU-load trust does
not detect GPU contention or guarantee a representative workload. The original
Hash API sweep preceded Linux sampling, so its automatic environment trust was
unknown; later production canaries measured completed hashes directly.

The investigation compared 654 sampled GPU digests against CPU references over
48 combinations of two devices, difficulties 8/10000/10003, warps 1/2/4/8, and
references off/on. Generated batches of 17 exercised partially filled blocks.
This was sampled validation, not a comparison of every attempted digest.

GPU-first key/salt copies occur before the CUDA transfer timing events, so
`host_to_device_ms` excludes those copies; host input-preparation time includes
them. In the original single-stream baseline, the main kernel accounted for
roughly 90% of elapsed time and finalization about 5%. Use a GPU profiler to
identify the limiting resource before attempting further kernel changes.
