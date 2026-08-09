# Woody Miner (woodysoil/XenblocksMiner) — Deep Technical Analysis

**Analysed tree:** `C:\projects\treeminer_xnm\repos\XenblocksMiner`
**Remote:** `https://github.com/woodysoil/XenblocksMiner` (branch `main`, HEAD `fdf9f31`)
**Date:** 2026-08-09

> **Important framing.** The checked-out tree is *not* the plain upstream miner. It is the
> `main` branch of woodysoil/XenblocksMiner as it exists today, which has grown three extra
> layers on top of the original CUDA miner:
>
> 1. the original **C++/CUDA XenBlocks miner** (`src/`),
> 2. an extracted, benchmarkable **Hash API** (`src/hashapi/`, `scripts/`, `docs/HASH_*`),
> 3. an experimental **hashpower marketplace platform** (`server/` Python + `web/` React +
>    `proto/` MQTT schemas + MQTT client code in `src/`).
>
> For our project (a miner that stores found hashes locally and resubmits when the central
> server returns), layers 1 and 2 are the interesting ones. Layer 3 is a different product.

---

## 0. Executive summary

* **Algorithm:** Argon2id (`t=1`, `p=1` single lane, `m = difficulty` KiB, 64-byte tag),
  salt = miner's Ethereum address bytes, password = 64 hex chars ("key"). A block is any
  hash whose Argon2 encoded output contains the literal `XEN11`; `XUNI<digit>` is a
  secondary reward valid only in the first/last 5 minutes of each hour.
* **GPU design:** one **warp (32 threads) per Argon2 hash**, `batchSize` warps launched as
  `batchSize` blocks in a **single "oneshot" kernel** that walks all four Argon2 slices
  without a host round-trip. 1 KiB of shared memory per block used as a transpose scratch
  buffer. Optional **GPU-side first-block (pre-hash) kernel** removes the CPU Blake2b
  bottleneck — the single largest measured win in the repo's optimisation ledger.
* **Network layer:** three live HTTP endpoints (`GET {rpc}/difficulty`,
  `POST {rpc}/verify`, `POST https://woodyminer.com/api/stat/upload`), plus a local Crow
  HTTP server on `:42069` for HiveOS, plus an optional MQTT channel for the marketplace.
* **Persistence for failed submissions: essentially none.** The submit queue is an
  in-memory `std::queue<std::function<void()>>` (`src/BlockSubmitter.cpp:7`) drained by one
  worker thread. A submission gets at most 5 retries (`MAX_SUBMIT_RETRIES = 5`) with a fixed
  2-second sleep, then the payload is written to a **rotating, self-truncating 1 MiB text
  log** and abandoned (`src/main.cpp:454-461`, `src/Logger.cpp:51-56`). Nothing survives a
  process restart. **This is the gap our project exists to fill.**

---

## 1. Architecture

### 1.1 Component map

```
                       ┌──────────────────────────────────────────┐
 process launch ──►    │ main() (src/main.cpp)                    │
   (no --execute)      │  · ProcessMonitor watchdog (re-spawns     │
                       │    self with --execute, up to 42069×)     │
                       └────────────────┬─────────────────────────┘
                                        │ --execute
      ┌─────────────────────────────────┴──────────────────────────────────┐
      │                                                                    │
┌─────▼──────────────┐  ┌──────────────────┐  ┌──────────────┐  ┌──────────▼─────────┐
│ per-GPU mining     │  │ difficulty thread│  │ stat upload  │  │ Crow LocalServer   │
│ threads (N GPUs)   │  │ (10 s poll)      │  │ thread (60 s)│  │ :42069 /stats      │
│  runMiningOnDevice │  │ DifficultyManager│  │ StatReporter │  │ LocalServer.cpp    │
└─────┬──────────────┘  └──────────────────┘  └──────────────┘  └────────────────────┘
      │ MineUnit::runMineLoop()
      │   ├─ hashapi::selectCudaBatchSize()
      │   ├─ CudaHashBackend::runBatch()  ── CudaBackend ── KernelRunner ── *.cu kernels
      │   └─ submitMatches() ──► submitCallback (lambda in main.cpp)
      │                             │
      │                             ├─► taskQueue (in-memory) ──► workerThread ──► POST /verify
      │                             └─► PlatformManager::onBlockFound() (MQTT, optional)
      ▼
```

### 1.2 Threading model

| Thread | Created at | Lifetime | Job |
|---|---|---|---|
| main | — | blocks on `while(running) sleep(1s)` (`src/main.cpp:592-595`) | supervision |
| difficulty poller | `src/main.cpp:343` | detached | `updateDifficultyPeriodically()`, 10 s cadence |
| `uploadGpuInfos` | `src/main.cpp:349` | detached | **effectively dead** — builds the JSON then sleeps 5 min without POSTing (`src/StatReporter.cpp:73-74`) |
| submit worker (×1) | `src/main.cpp:352` | detached | drains `taskQueue`, does the blocking HTTP submit |
| mining thread per GPU | `src/main.cpp:550-553` | detached | `runMiningOnDevice()` → `MineUnit::runMineLoop()` |
| Crow server | `src/main.cpp:585` | detached | `:42069`, multithreaded |
| stat uploader | `src/main.cpp:588` | detached (unless `--donotupload`) | POSTs to woodyminer.com every 60 s |
| MQTT heartbeat + lease watchdog | `src/PlatformManager.cpp:67,70` | joined on stop | platform mode only |
| first-block CPU workers | `src/hashapi/CudaHashBackend.cpp:160-210` | per batch | parallel Argon2 pre-hash (only when `gpu_first_blocks` is off) |

Notable: **there is exactly one submit worker thread.** A slow/hanging `/verify` call blocks
every subsequent submission behind it. Each retry cycle sleeps 2 s synchronously
(`src/main.cpp:453`), so a single stubborn block can hold the queue for ~10 s minimum, and
a "no response" path can burn 10 × the 10 s HTTP timeout ≈ 100 s (`src/main.cpp:401-406`).

### 1.3 Work generation — salt / key / nonce construction

XenBlocks has no block header or nonce in the Bitcoin sense. Work is:

* **salt** = the *target account's* Ethereum address with `0x` stripped, decoded from hex to
  20 raw bytes (`src/MineUnit.cpp:62`, `src/argon2params.cpp:120-124`,
  `src/hashapi/CudaHashBackend.cpp:398`). Validation requires ≥16 hex chars and even length
  (`src/hashapi/HashApiValidation.cpp:57-70`).
* **key (the Argon2 "password")** = 64 lowercase hex characters, generated by
  `RandomHexKeyGenerator` (`src/RandomHexKeyGenerator.h:26-43`) from a `std::mt19937` seeded
  with `random_device ^ high_resolution_clock`. Each 32-bit draw is consumed 4 bits at a time
  (8 hex chars per draw) — an accepted micro-optimisation.
* **key prefix** — optional, forced onto the front of every generated key. Three sources:
  * dev-fee prefixes `FFFFFFFF` / `EEEEEEEE` + the miner's own address
    (`src/MiningCommon.h:15-16`, `src/MineUnit.cpp:66-76`),
  * marketplace lease prefix (16 hex chars, `PLATFORM_PREFIX_LENGTH`,
    `src/MineUnit.cpp:56-60`),
  * remote-set `globalSelfMiningPrefix` (`src/PlatformManager.cpp:307-312`).
* **difficulty** = Argon2 `m_cost` in KiB. `segmentBlocks = max(m_cost, 8)/4`
  (`src/argon2params.cpp:45-46`), lane blocks = `4 × segmentBlocks ≈ difficulty`, so **memory
  per attempt ≈ difficulty KiB**.
* **target** = substring `XEN11` in the Argon2 encoded output. Superblock =
  ≥50 uppercase characters in the hash (`src/hashapi/HashApiMatching.cpp:8-14`).
  XUNI = `XUNI` followed by an ASCII digit (`src/hashapi/HashApiMatching.cpp:16-31`),
  only accepted when `minute < 5 || minute >= 55` (`src/MineUnit.cpp:11-17,109,124`).

There is **no server-issued work**: the miner picks its own random keys and only asks the
server for the current `difficulty`. That is exactly why offline mining + deferred
submission is viable for XenBlocks — the only server dependency during hashing is the
difficulty value, and the only time-sensitive gate is the server-side "outside of time
window" rejection (`src/main.cpp:415-419`).

### 1.4 Difficulty fetching

```cpp
// src/DifficultyManager.cpp:17
HttpResponse response = httpClient.HttpGet(globalRpcLink+"/difficulty", 5000);
// :24  return json_response["difficulty"].get<std::string>();
```

`updateDifficulty()` swallows every exception silently (`src/DifficultyManager.cpp:50-52`),
so a server outage simply freezes `globalDifficulty` at its last value — mining continues.
`updateDifficultyPeriodically()` loops every 10 s (`src/DifficultyManager.cpp:55-62`).
Initial value before the first fetch is `42069` (`src/main.cpp:341`); the compiled-in default
is `1727` (`src/MiningCommon.cpp:4`).

When the difficulty changes, `MineUnit::runMineLoop()` breaks out
(`src/MineUnit.cpp:44-49`), and `runMiningOnDevice()` constructs a fresh `MineUnit`
(`src/main.cpp:120-128`) which re-derives batch size and re-allocates GPU memory.

### 1.5 GPU kernel design — Argon2id on CUDA

**CUDA only.** There is no HIP/OpenCL/ROCm path anywhere in the tree; `ComputeBackend` is an
abstract interface (`src/ComputeBackend.h:16-60`) but `CudaBackend` is the only
implementation and `enumerateBackends()` hard-returns `CudaBackend::enumerate()`
(`src/CudaBackend.cpp:113-116`). AMD users are pointed at a separate OpenCL miner
(`doc/HIVEOS.md:3`).

Two kernels in `src/kernelrunner.cu`:

**(a) `argon2_first_blocks_kernel` (`src/kernelrunner.cu:876-911`)** — optional, opt-in via
`gpu_first_blocks`. One *thread* per hash (128 threads/block,
`src/kernelrunner.cu:1138-1139`). Each thread runs a full device-side Blake2b
(`device_blake2b_*`, `src/kernelrunner.cu:286-383`), computes the Argon2 pre-hash
(`device_initial_hash`, `:427-463`) and expands it into blocks 0 and 1 via `device_digest_long`
(`:385-425`). This replaces the CPU-side `Argon2Params::fillFirstBlocks` **and** the
host→device memcpy entirely.

**(b) `argon2_kernel_oneshot` (`src/kernelrunner.cu:811-874`)** — the main memory-hard pass.
Launch geometry (`src/kernelrunner.cu:1162-1164`):

```cpp
argon2_kernel_oneshot
    <<<dim3(batchSize), dim3(THREADS_PER_LANE /*32*/), sizeof(struct block_l) /*1024*/, stream>>>(
        memory_blocks, segmentBlocks);
```

* **One warp = one Argon2 lane = one candidate hash.** `THREADS_PER_LANE = 32`,
  `QWORDS_PER_THREAD = 128/32 = 4` (`:31-32`). Each thread owns 4 of the 128 uint64 words of
  a 1 KiB Argon2 block, held in registers as `block_th {a,b,c,d}` (`:102-104`).
* All cross-thread data movement inside the block-mixing function is done with
  **warp shuffles**, never shared memory: `transpose()` (`:573-599`) uses three
  `__shfl_xor_sync` ops, `shift1/shift2_shuffle` (`:602-678`) use `__shfl_sync`. This is the
  classic `argon2-gpu` layout, kept intact.
* The Blake2-style G function is **hand-written PTX inline asm** (`g()`, `:494-557`) — it uses
  `mul.hi.u32`/`mul.lo.u32` for the Argon2 `f(x,y)` multiply, `prmt.b32` byte permutes for the
  24- and 16-bit rotations, and `shl+shr+add` for the 63-bit rotation ("emits less
  instructions than or", `:553`).
* **Single kernel, all four slices.** Because `lanes == 1`, no inter-lane synchronisation is
  needed, so the entire pass runs without returning to the host: slice 0 from offset 2
  (`:838-845`), slice 1 (`:854-861`), then slices 2-3 with data-dependent addressing
  (`:863-872`). Slices 0-1 use Argon2i-style indexed addressing
  (`argon2_step_indexed`, `:770-795`), slices 2-3 use Argon2d-style dependent addressing
  (`argon2_step_dependent`, `:797-809`) — i.e. correct Argon2**id** for `t=1`.
* **Shared memory** is exactly one `block_l` (1024 B: split lo/hi uint32 arrays,
  `:97-100`) used as the per-block XOR scratch in `argon2_core` (`:752-768`). Splitting into
  lo/hi 32-bit planes avoids shared-memory bank conflicts on 64-bit accesses.
* `block_th_get_uniform()` (`:129-141`) — a plain `switch` rather than the branch-free
  `cmpeq_mask` version — is used for indexed address-word selection because the index is
  warp-uniform. Per `goal.md:100-108`, this dropped sm75 register use from 56 → 53 while
  keeping sm80 at 32 and sm86/89 at 40, with 1024 B shared and zero local memory.

### 1.6 CPU-side orchestration

`MineUnit::runMineLoop()` (`src/MineUnit.cpp:19-96`) per iteration:

1. re-read `globalDifficulty`; bail if it changed,
2. read the `MiningCoordinator` context (self vs. platform mining),
3. pick salt + key prefix (dev-fee rotation happens here),
4. `batchCompute()` → `hashapi::CudaHashBackend::runBatch()`,
5. `submitMatches()` → fires `submitCallback` per match,
6. `stat()` → updates hashrate, pushes a `gpuInfo` through `statCallback`.

`CudaHashBackend::runBatch()` (`src/hashapi/CudaHashBackend.cpp:290-529`) is a fully
instrumented pipeline: validate → setup → keygen → first blocks → `run()`/`finish()` →
finalize (Argon2 tag) → base64 → pattern match, with ~30 timing counters
(`src/hashapi/HashApiTypes.h:42-72`).

Dev-fee rotation is a simple modulo counter: `batchComputeCount` counts batches 0..999, and
when `1000 - batchComputeCount <= globalDevfeePermillage` the salt/prefix are swapped to the
dev address for that batch (`src/MineUnit.cpp:66-76`).

---

## 2. Network layer — **the key section**

### 2.1 Complete endpoint inventory

| # | Method / URL | Where | Timeout | Purpose |
|---|---|---|---|---|
| 1 | `GET {globalRpcLink}/difficulty` | `src/DifficultyManager.cpp:17` | 5 000 ms | current Argon2 `m_cost` |
| 2 | `POST {globalRpcLink}/verify` | `src/main.cpp:409` | 10 000 ms | **block/XUNI submission** |
| 3 | `POST https://woodyminer.com/api/stat/upload` | `src/StatReporter.cpp:183,191` | 3 000 ms | telemetry to Woody's dashboard |
| 4 | `GET http://xenminer.mooo.com:4445/getblocks/lastblock` | `src/PowSubmitter.cpp:13` | 5 000 ms | **dead code** — header included in `main.cpp:23`, never called |
| 5 | `POST http://xenblocks.io:4446/send_pow` | `src/PowSubmitter.cpp:57` | 10 000 ms | **dead code** — Merkle-root PoW submission |
| 6 | `GET http://localhost:42069/stats` | server: `src/LocalServer.cpp:19,26-30`; client: `hiveos/h-stats.sh:3` | — | HiveOS stats scrape |
| 7 | `GET http://localhost:42069/platform/status` | `src/LocalServer.cpp:32-54` | — | lease/platform introspection |
| 8 | MQTT `xenminer/{worker_id}/{register,heartbeat,status,block}` (pub) and `/{task,control}` (sub), QoS 1 | `src/MqttClient.cpp:127-130`, `src/WorkerReporter.cpp` | connect 10 s, keepalive 60 s | marketplace mode only (`--platform-mode`) |

`globalRpcLink` defaults to `http://xenblocks.io` (`src/MiningCommon.cpp:25`) and is
overridable with `--rpcLink`. HTTP is done with **libcpr** (`src/HttpClient.cpp`), a thin
wrapper with **no retry, no connection reuse, no keep-alive, no backoff** — a fresh
`HttpClient` is constructed *inside the retry loop* on every attempt (`src/main.cpp:408`).

### 2.2 Submission payload format

```jsonc
// built at src/main.cpp:386-393
{
  "hash_to_verify":    "$argon2id$v=19$m=<diff>,t=1,p=1$<b64salt>$<b64tag>", // full encoded string
  "key":               "<64 hex chars>",
  "account":           "0x<40 hex>",      // == salt, re-prefixed at src/main.cpp:385
  "attempts":          "12345",            // string, not number
  "hashes_per_second": "1234.56",          // string, 2 decimals
  "worker":            "<16-hex machineId>"
}
```

Note the double-hash design: the GPU path produces only the **raw base64 of the 64-byte tag**
(`src/hashapi/HashApiEncoding.cpp:20-51`) and matches `XEN11` against that; the CPU then
**re-computes the full canonical hash with libargon2** before submitting
(`src/main.cpp:377-381`, `src/Argon2idHasher.cpp:13-27`) and drops the candidate if the
GPU-side substring is not present in the CPU result. In platform mode the same verification
happens a second time (`src/main.cpp:363-369`).

`machineId` = first 16 hex chars of `SHA256(MAC-addresses + device list)`
(`src/main.cpp:73-87`, `src/MachineIDGetter.cpp`), falling back to a random key.

### 2.3 Submit path, retry logic, and response handling

```cpp
// src/BlockSubmitter.cpp:5-7  — the entire "queue"
std::mutex mtx_submit;
std::condition_variable cv;
std::queue<std::function<void()>> taskQueue;
```

```cpp
// src/main.cpp:398-461 (abridged, line numbers exact)
398  int retries = 0;
399  int retries_noResponse = 0;
401  while (true) {
402      if (retries_noResponse >= 10) {
403          std::cout << RED << "No response from server after " ... ;
404          logger.log("No response from server: " + payload.dump(-1));
405          return;                                   // <-- payload dropped
406      }
407      try {
408          HttpClient httpClient;
409          HttpResponse response = httpClient.HttpPost(globalRpcLink+"/verify", payload, 10000);
410          if (response.GetBody() == "") { retries_noResponse++; continue; }
415          if (response.GetBody().find("outside of time window") != npos) { ... return; }  // hard drop
420          if (response.GetBody().find("already exists") != npos) errorButFound = true;
425          if (response.GetStatusCode() == 200 || errorButFound) { ...count block...; break; }
448      }
450      catch (const std::exception& e) { }                 // swallowed
452      retries++;
453      std::this_thread::sleep_for(std::chrono::seconds(2));
454      if (retries >= MAX_SUBMIT_RETRIES) {                // MAX_SUBMIT_RETRIES == 5
455          if (hashed_pure.find("XEN11") != npos) globalFailedBlockCount++;
458          std::cout << RED << "Failed to submit block after " << retries << " retries";
459          logger.log("Failed to submit block: " + payload.dump(-1));
460          return;                                   // <-- payload dropped
461      }
462  }
```

`MAX_SUBMIT_RETRIES = 5` is defined at `src/MiningCommon.h:12`.

Semantics worth copying:

* **`"already exists"` is treated as success** (`src/main.cpp:420-421`) — idempotent
  resubmission is safe against this server. Critical for a resubmit-on-reconnect design.
* **`"outside of time window"` is a permanent failure** (`src/main.cpp:415-419`) — the server
  time-boxes acceptance. This is the one real constraint on deferred submission and must be
  measured empirically before building on it.
* HTTP 500 is treated as retryable-but-quiet (`src/main.cpp:422,443-447`).
* Empty body → separate counter (`retries_noResponse`, up to 10) that **does not sleep**, so
  a fast-failing connection burns 10 immediate attempts.

### 2.4 Does it have ANY resubmission / queue / persistence for failed submissions?

**No. There is no durable queue, no on-disk spool, no pending-submission store, and no
resubmit-on-reconnect logic anywhere in the C++ miner, the Python server, or the scripts.**

Evidence:

1. **The queue is in-memory only.** `std::queue<std::function<void()>> taskQueue`
   (`src/BlockSubmitter.cpp:7`) holding `std::function` closures — not serialisable data.
   `workerThread()` (`src/BlockSubmitter.cpp:9-26`) pops and runs them. On `SIGINT`,
   `running = false` and the loop exits with `if (!running && taskQueue.empty()) break`
   (`:16-18`) — but a non-empty queue at shutdown is simply lost when the process dies,
   because the thread is **detached** (`src/main.cpp:353`) and nothing joins it.

2. **The only "persistence" is a self-truncating text log.**
   `logger.log("Failed to submit block: " + payload.dump(-1))` (`src/main.cpp:459`) writes the
   JSON to `log0.txt`/`log1.txt`. `Logger` rotates between exactly two 1 MiB files and
   **truncates on rotation**:
   ```cpp
   // src/Logger.cpp:51-56
   void Logger::switchFile() {
       outputFile_.close();
       fileIndex_ = (fileIndex_ + 1) % 2;                                  // only 2 files
       outputFile_.open(getCurrentFilename(), std::ios::out | std::ios::trunc);  // data destroyed
       currentFileSize_ = 0;
   }
   ```
   Nothing ever *reads* these logs back. `grep -rn "log0\|log1\|readLog\|replay" src/` returns
   only the writer.

3. **No timestamps are attached to a found hash.** The payload has no `found_at` field
   (`src/main.cpp:386-393`); only the log line carries a `%m-%d %H:%M` prefix
   (`src/Logger.cpp:30-38`). A resubmitting miner needs the discovery timestamp, and this
   design does not preserve it.

4. **No startup recovery.** `main()` (`src/main.cpp:131-604`) never opens a pending store,
   never scans the log directory, and never enqueues anything before starting the mining
   threads.

5. **The server side has the same gap.** `server/storage/_schema.py:62-75` defines the
   `blocks` table with `chain_verified`/`chain_block_id` but **no submission-status column**,
   and `BlockRepo` is insert/select only — never `UPDATE`. `server/watcher.py:82-86` logs an
   unverified block and stores it anyway (`:92-103`), then only counts it if verified
   (`:106`); it is never revisited. `server/simulator.py:464-471` swallows chain-submission
   failures and `return None`. `server/broker.py:11` states the broker deliberately has no
   persistence — no retained messages, no offline QoS-1 session queue.

6. **MQTT is not a substitute.** `MqttClient::publish()` returns `false` immediately when
   disconnected (`src/MqttClient.cpp:92`), and `clean_session(true)`
   (`src/MqttClient.cpp:27`) explicitly discards any broker-side session state. Paho's
   automatic reconnect (`:28-30`) reconnects the socket but does **not** replay the block
   report that failed while offline. `PlatformManager::onBlockFound()`
   (`src/PlatformManager.cpp:110-126`) ignores the boolean return value entirely.

**The closest things to prior art in this repo** are (a) the exponential backoff on the
*telemetry* uploader — `failureCount >= 10 → uploadPeriod *= 2`, capped at 600 s
(`src/StatReporter.cpp:196-203`) — and (b) the `"already exists"` idempotency handling. Both
are patterns worth reusing, but neither is persistence.

---

## 3. Performance techniques (what makes it fast)

Ranked roughly by measured impact per `docs/HASH_OPTIMIZATION_GOAL.md` and `goal.md:72-148`.

### 3.1 GPU-side first blocks (`gpu_first_blocks`) — the single biggest win

Reported ~**+108 % to +131 %** median throughput. Before this, every attempt required a
CPU Blake2b pre-hash producing 2 KiB, then a 2 KiB host→device copy per attempt. With it:

* only the 64-byte keys and the 20-byte salt are uploaded
  (`src/kernelrunner.cu:1094-1122`; keys flattened into one contiguous buffer, device buffers
  grown-and-reused via `deviceKeysCapacity`/`deviceSaltCapacity`),
* `argon2_first_blocks_kernel` writes blocks 0 and 1 directly into the working memory
  (`src/kernelrunner.cu:900-910`),
* `copyInputBlocks()` is skipped entirely (`src/kernelrunner.cu:1174-1179`).

The miner always opts in: `request.gpu_first_blocks = true` and
`first_block_dynamic_chunk_auto = true` (`src/MineUnit.cpp:110-111`).

### 3.2 Only the last block is copied back

`copyOutputBlocks()` (`src/kernelrunner.cu:1051-1063`) uses a **strided `cudaMemcpy2DAsync`**
to pull only the final 1 KiB block of each job out of a `jobSize`-strided allocation:

```cpp
cudaMemcpy2DAsync(blocksOut.get(), copySize,          // dst, dst pitch = 1 KiB
                  mem + (jobSize - copySize), jobSize, // src at end of each lane, src pitch = full lane
                  copySize, batchSize, cudaMemcpyDeviceToHost, stream);
```

At difficulty 4096 that is 1 KiB copied back per attempt out of 4 MiB computed — a ~4000×
reduction in D2H traffic, in a single async call for the whole batch.

### 3.3 Memory layout

* One flat `cudaMalloc` of `batchSize × segmentBlocks × 4 × 1024` bytes
  (`src/kernelrunner.cu:936-938`); no per-attempt allocation.
* Argon2 blocks are stored **transposed for coalescing**: thread `t` of a warp owns words
  `{0,1,2,3}×32 + t` (`load_block`/`store_block`, `src/kernelrunner.cu:164-189`), so each
  128-byte segment of a warp access is contiguous.
* Shared scratch is split into separate `lo[]`/`hi[]` uint32 planes (`block_l`,
  `src/kernelrunner.cu:97-100`) to avoid 64-bit bank conflicts.
* `__align__(16)` on `block_g`/`block_l`, `__align__(32)` on `block_th`.
* Blake2b IV and sigma tables live in `__constant__` memory (`src/kernelrunner.cu:233-253`).

### 3.4 Batch sizing and allocation reuse

`hashapi::selectCudaBatchSize()` (`src/hashapi/HashApiTuning.cpp:52-77`) computes
`batch = (free_mem − 100 MiB reserve) / (difficulty × 1024 × 1.001)`
(`:15-21`, reserve at `src/hashapi/HashApiTuning.h:9`), then clamps by an explicit
`--batchSize` or by a tuned default table (`recommendedCudaBatchSize()`, `:24-36` — only
covers d≤64; at real mining difficulties it returns 0 and the memory limit wins).

`KernelRunner::canReuse()` / `reconfigure()` (`src/kernelrunner.cu:952-978`, gated at
`src/CudaBackend.cpp:40-44`) keep the allocation and CUDA events alive when only
`segmentBlocks` shrinks — reported **+7.86 %** on variable-difficulty sequences.
`CudaHashBackend::ensureInitialized()` (`src/hashapi/CudaHashBackend.cpp:272-288`) adds a
second short-circuit so steady-state batches do zero allocation work.

### 3.5 Finalization / matching hot path

* `Argon2Params::finalize()` skips the lane-XOR copy entirely when `lanes == 1`
  (`src/argon2params.cpp:229-232`) — just `digestLong` straight from the mapped block.
* Finalization runs in fixed 64-attempt chunks with a **stack-allocated**
  `std::array<std::array<uint8_t,64>,64>` and **reused** `std::string` buffers pre-reserved to
  the base64 length (`src/hashapi/CudaHashBackend.cpp:472-509`) — reported +6.71 %.
* Hand-rolled base64 with no padding and `push_back` into a reserved string
  (`src/hashapi/HashApiEncoding.cpp:20-51`).
* XUNI detection is a `find` loop plus an `isdigit` check, **not** a regex
  (`src/hashapi/HashApiMatching.cpp:16-31`). (The legacy `std::regex R"(XUNI\d)"` still exists
  in the submit callback at `src/main.cpp:400`, but only runs once per *found* block.)
* Blake2b has a little-endian 64-bit load/store fast path and writes its full 64-byte output
  directly to the destination (`src/blake2b.cpp`).

### 3.6 Multi-GPU handling

Deliberately simple and shared-nothing: one detached `std::thread` per selected device
(`src/main.cpp:546-553`), each owning its own `CudaBackend` → `KernelRunner` → stream and its
own `MineUnit`. `cudaSetDevice` is called per thread via `backend.activate()`
(`src/main.cpp:118`, `src/CudaBackend.cpp:31-34`). Devices are chosen with
`--device=1,2,7` parsed by `parseDeviceList()` (`src/main.cpp:89-112`; invalid entries are
ignored, empty selection = all GPUs). Cross-GPU state is only the three globals guarded by
`globalGpuInfosMutex` and `mtx`. There is **no** per-GPU submission queue, no work
partitioning, and no NVLink/peer usage — none of which are needed when keys are random.

### 3.7 Instrumentation as a technique

Roughly 30 named timing fields (`src/hashapi/HashApiTypes.h:42-72`) plus CUDA-event-based
kernel/H2D/D2H timings (`src/kernelrunner.cu:1169-1231`) feed a machine-readable benchmark
harness (`scripts/hash_api_benchmark.py`, `hash_api_compare.py`,
`cuda_resource_summary.py`). Combined with the accept/reject ledger in
`docs/HASH_OPTIMIZATION_GOAL.md`, this is arguably the most valuable artefact in the repo —
it records *what did not work* (launch-bounds `(THREADS_PER_LANE,4)`, device-side final
hashes, blanket force-inline, indexed-range chunking, low-32 reference shuffle) with the
register-pressure evidence for each.

---

## 4. Config & ops

### 4.1 Config file — `config.txt`

Flat `key=value`, parsed by `ConfigManager` (`src/ConfigManager.cpp:7-19`, whitespace
trimmed, `#` comments **not** supported):

```
account_address=0xYourEip55Address
devfee_permillage=21
ecodev_address=0x...        # optional
```

`AppConfig::load()` (`src/AppConfig.cpp:5-46`) validates EIP-55 and interactively prompts on
first run or on invalid values; `tryLoad()` (`:47-62`) is the non-interactive variant used
when both `--minerAddr` and `--totalDevFee` are on the command line.

### 4.2 CLI flags (`src/main.cpp:159-176`)

| Flag | Effect |
|---|---|
| `--help,-h` | usage |
| `--execute` | run the miner; **without it the binary becomes a supervisor** that re-spawns itself with `--execute` up to 42069 times (`src/main.cpp:142-156`, `src/ProcessMonitor.cpp`) |
| `--minerAddr <0x…>` | mining address (EIP-55 validated) |
| `--totalDevFee <0-1000>` | dev fee permille |
| `--ecoDevAddr <0x…>` | ecosystem dev address (receives half the fee) |
| `--saveConfig` | persist CLI values into `config.txt` |
| `--donotupload` | disable the woodyminer.com telemetry thread |
| `--device=0,2,3` | GPU subset |
| `--batchSize <n>` | cap GPU batch (VRAM control) |
| `--rpcLink <url>` | override `http://xenblocks.io` |
| `--customName <s>` | label in telemetry |
| `--testFixedDiff <n>` | offline test mode: fixed difficulty, zero-address, dev fee 0, **RPC submission skipped** (`src/main.cpp:465-470`) |
| `--testBlockPattern <s>` | override `XEN11` |
| `--platform-mode`, `--mqtt-broker <uri>`, `--worker-id <s>` | marketplace mode |
| `hash-one` / `hash-batch` / `hash-benchmark` / `hash-help` | Hash API sub-CLI, intercepted before anything else (`src/main.cpp:133-135`) |

### 4.3 HiveOS integration (`hiveos/`)

* `h-manifest.conf` — `MINER_NAME=xenblocksMiner`, `MINER_VER=1.4.0`.
* `h-config.sh` — reads `CUSTOM_TEMPLATE` (wallet) and `CUSTOM_USER_CONFIG` JSON
  (`devfee_permillage`, `ecodev_address`, `device_id`) via `jq`, writes `config.txt`, and
  *generates* `h-run.sh` with the right `--device=` flag (`hiveos/h-config.sh:18-39`).
* `h-run.sh` — `./xenblocksMiner`.
* `h-stats.sh` — curls `http://localhost:42069/stats`, joins it with `gpu-stats` by PCI bus
  id, emits HiveOS's `{hs, hs_units:"hs", temp, fan, uptime, ar:[accepted,rejected],
  algo:"argon2id", bus_numbers}` envelope.

### 4.4 Dev-fee mechanism

* **Default 1/1000 = 0.1 %** in a bare build (`src/MiningCommon.cpp:11`); **21/1000 = 2.1 %**
  under HiveOS (`hiveos/h-config.sh:8`), overridable by the user.
* Dev address hard-coded: `0x24691E54aFafe2416a8252097C9Ca67557271475`
  (`src/MiningCommon.cpp:9`).
* Implementation (`src/MineUnit.cpp:66-76`): batches are counted 0..999; the last
  `devfeePermillage` batches of each 1000 mine for the dev instead of the user. Within that
  window, the last half goes to `globalEcoDevfeeAddress` when set. The user's own address is
  embedded as the **key prefix** (`FFFFFFFF` / `EEEEEEEE` + user address,
  `src/MiningCommon.h:15-16`) so the dev can attribute contributions.
* Range is validated 0..1000 in both the CLI and config paths — i.e. **the fee is fully
  user-disableable** (`--totalDevFee 0`).

### 4.5 Local HTTP surface

Crow app on port **42069** (`src/LocalServer.cpp:19`), `loglevel(Warning)`,
`signal_clear()` so Crow does not steal `SIGINT`. Routes: `/stats`
(`src/StatReporter.cpp:78-105`) and `/platform/status` (`src/LocalServer.cpp:32-54`).

---

## 5. Build system

* **CMake ≥ 3.18**, C++17 and CUDA 17, `CMAKE_BUILD_TYPE` forced to `Release`
  (`CMakeLists.txt:1-25`).
* **Default architectures:** `50;52;61;70;75;80;86;89;90` (`CMakeLists.txt:24`), i.e. Maxwell
  through Hopper.
* **Three build products** selected by options (`CMakeLists.txt:3-6`):
  * `XENBLOCKS_BUILD_MINER=ON` (default) → `xenblocksMiner`, needs CUDA + all deps,
  * `XENBLOCKS_BUILD_HASHAPI_CLI=ON` → `hashapi-cli`, **CPU-only, no CUDA/CPR/MQTT** — the
    isolation boundary that lets the hash path be optimised without the platform,
  * `XENBLOCKS_HASHAPI_STUB_BACKEND=ON` → deterministic stub for CLI parsing smoke tests,
  * `XENBLOCKS_BUILD_HASH_DIAGNOSTICS=ON` (default) → `blake2b-copy-selftest`,
    `argon2-finalize-benchmark` (`CMakeLists.txt:203-216`).
* **vcpkg manifest** (`vcpkg.json`): `boost-program-options`, `argon2`, `cryptopp`, `cpr`,
  `nlohmann-json`, `openssl`, `secp256k1`, `crow`, `paho-mqttpp3`. vcpkg is a git submodule
  (`.gitmodules`). Plus `CUDA::nvml` for power/utilisation (`CMakeLists.txt:96`).
* **Presets** (`CMakePresets.json`): `ninja-multi-vcpkg` (general), plus
  `cuda-release-vcpkg-modern` (`75;80;86;89;90`) and per-arch `…-sm75/86/89/90` for
  reproducible benchmarking, and two MinGW presets for the CLI.
* **Platforms:** Windows (MSVC, `x64-windows-static`, links `Iphlpapi`, ships `res/version.rc`
  + `res/xenminer.ico`) and Linux (`x64-linux-static` overlay triplet in
  `custom-triplets/`, static libgcc/libstdc++, `CMakeLists.txt:161`). **macOS/AMD are not
  supported** — `MachineIDGetter.cpp:23` `#error "OS not supported"` outside Win/Linux.
* **CI:** `.github/workflows/linux-build.yml` (container `nvidia/cuda:11.8.0-devel-ubuntu22.04`,
  CMake 3.29), `windows-build.yml` (CUDA 12.6.3 via `Jimver/cuda-toolkit`, MSVC 2022),
  `release.yml`.

---

## 6. Repo map

### 6.1 `src/` — C++/CUDA miner core

| Path | Description |
|---|---|
| `main.cpp` | Entry point: arg parsing, config, thread launch, **the submit callback with all retry logic**, stat callback |
| `MiningCommon.h/.cpp` | Global mutable state (address, difficulty, counters, RPC link), `gpuInfo`, callback typedefs, dev-fee prefixes, `MAX_SUBMIT_RETRIES` |
| `MineUnit.h/.cpp` | Per-GPU mining loop: batch sizing, salt/prefix selection, dev-fee rotation, match dispatch, hashrate |
| `ComputeBackend.h` | Abstract GPU backend interface (only CUDA implements it) |
| `CudaBackend.h/.cpp` | `ComputeBackend` over `KernelRunner`; device props, free memory, allocation reuse |
| `CudaDevice.h/.cpp` | Device enumeration, names, PCI bus ids |
| `CudaException.h` | `cudaError_t` → exception helper |
| `kernelrunner.h/.cu` | **All CUDA code**: Argon2id oneshot kernel, GPU first-block kernel, device Blake2b, PTX `g()`, buffers, streams, events |
| `argon2params.h/.cpp` | CPU-side Argon2 parameterisation, initial hash, `fillFirstBlocks`, `finalize`, `digestLong` |
| `argon2-common.h` | Argon2 constants and enums |
| `blake2b.h/.cpp` | Optimised host Blake2b |
| `Argon2idHasher.h/.cpp` | libargon2 wrapper producing the canonical encoded hash + `verifyHash` |
| `HttpClient.h/.cpp`, `HttpResponse.h/.cpp` | Thin libcpr GET/POST wrapper; no retry/keep-alive |
| `BlockSubmitter.h/.cpp` | **The in-memory submit queue** + its single worker thread |
| `DifficultyManager.h/.cpp` | `GET /difficulty`, 10 s poll loop |
| `StatReporter.h/.cpp` | `/stats` JSON, NVML power/utilisation, telemetry POST with exponential backoff |
| `LocalServer.h/.cpp` | Crow app on `:42069` (`/stats`, `/platform/status`) |
| `Logger.h/.cpp` | 2-file, 1 MiB rotating, **truncating** text logger |
| `AppConfig.h/.cpp`, `ConfigManager.h/.cpp` | `config.txt` load/save + interactive prompts |
| `EthereumAddressValidator.*` | EIP-55 checksum validation |
| `EthereumSignatureValidator.*` | secp256k1 signature checks — **unused by the miner** |
| `MerkleTree.*`, `SHA256Hasher.*` | Merkle root + SHA-256; used only by `PowSubmitter` |
| `PowSubmitter.h/.cpp` | Legacy Merkle-root PoW submission — **dead code** (header included, never called) |
| `MachineIDGetter.h/.cpp` | MAC/host-derived machine id (Windows + Linux only) |
| `RandomHexKeyGenerator.h` | mt19937 hex key generator with prefix support |
| `ProcessMonitor.h/.cpp` | Self-respawn supervisor (fork/execv on Linux, CreateProcess on Windows) |
| `MiningCoordinator.h/.cpp` | `shared_mutex` singleton holding the active `MiningContext` |
| `MqttClient.h/.cpp` | Paho MQTT client, QoS 1, LWT, auto-reconnect, `clean_session(true)` |
| `WorkerReporter.h/.cpp` | Builds/publishes register, heartbeat, status, block MQTT payloads |
| `LeaseManager.h/.cpp` | Marketplace lease lifecycle and expiry |
| `PlatformManager.h/.cpp` | 6-state marketplace machine, message dispatch, heartbeat + watchdog threads |
| `hashapi/HashApiTypes.h` | `HashApiRequest` / `HashApiMatch` / `HashApiTimings` / `HashApiResult` / `IHashBackend` |
| `hashapi/CudaHashBackend.h/.cpp` | The instrumented CUDA batch pipeline (the real hot path) |
| `hashapi/CpuHashBackend.h/.cpp` | libargon2 reference backend (correctness oracle, `difficulty ≥ 8`) |
| `hashapi/StubHashBackend.cpp` | Deterministic stub for CLI smoke tests |
| `hashapi/HashApiCli.h/.cpp` | `hash-one` / `hash-batch` / `hash-benchmark` argument parsing and driver |
| `hashapi/HashApiJson.h/.cpp` | JSON serialisation of requests/results/timings |
| `hashapi/HashApiValidation.h/.cpp` | Shared request validation + `normalizeHex` |
| `hashapi/HashApiMatching.h/.cpp` | `XEN11` / superblock / `XUNI<digit>` detection |
| `hashapi/HashApiEncoding.h/.cpp` | Padding-free base64 into a reused buffer |
| `hashapi/HashApiTuning.h/.cpp` | CUDA batch-size selection from free VRAM and difficulty |
| `hashapi/main.cpp` | `hashapi-cli` entry point |

### 6.2 Other top-level directories

| Path | Description |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `custom-triplets/` | Build system (see §5) |
| `.github/workflows/` | `linux-build.yml`, `windows-build.yml`, `release.yml` |
| `doc/` | User-facing: `BUILD_INSTRUCTIONS.md`, `HIVEOS.md`, `HOW_TO_RUN_ON_VASTAI_ASAP.md` |
| `docs/` | Platform + Hash API docs: `architecture.md`, `api.md` (zh), `api-reference.md`, `authentication.md`, `deployment.md`, `design-system.md`, `development-baseline.md`, `hash-api.md`, `websocket-protocol.md`, `testing.md` (zh), `HASH_API_EXTRACTION_GOAL.md`, `HASH_OPTIMIZATION_GOAL.md` (1691-line experiment ledger), `xnmhash-whitepaper.pdf` |
| `goal.md` | Stable `/goal` entry point for the autonomous hash-optimisation loop |
| `hiveos/` | `h-manifest.conf`, `h-config.sh`, `h-run.sh`, `h-stats.sh` |
| `proto/` | MQTT protocol: `README.md`, `worker_to_platform.json`, `platform_to_worker.json`, `examples/*.json` (11 payloads) |
| `res/` | `version.rc`, `xenminer.ico` (Windows resources) |
| `server/` | Python FastAPI marketplace platform: embedded MQTT broker (`broker.py`), SQLite storage (`storage/`, schema v11), routers (`routers/`), matching/settlement/pricing/reputation engines, JWT+EIP-191 auth, dashboard, chain simulator, and `hash_api/` — an HTTP wrapper that shells out to `hashapi-cli` (`GET /hash/v1/health`, `POST /hash/v1/{hash-one,batch,benchmark,validate}`) |
| `web/` | Vite + React 18 + TS 5.7 + Tailwind v4 SPA (`src/pages/{Overview,Monitoring,Marketplace,Provider,Renter,Account}.tsx`, `src/design/` token system) |
| `scripts/` | `hash_api_benchmark.py` (1914 L), `hash_api_compare.py`, `hash_benchmark_trends.py`, `cuda_resource_summary.py`, `mock_fleet.py`, `generate_test_data.py`, `run_mock_server.sh`, `demo.sh`, `test_cpp_integration.sh` |
| `tests/` | `cpp/` (blake2b self-test, argon2 finalize benchmark), `unit/` (Hash API contract/service/benchmark/compare, state machine, key prefix, security), `integration/` (block discovery, lease flow, settlement, websocket, multi-worker, error recovery, C++ worker) |
| `requirements-dev.txt`, `CONTRIBUTING.md`, `README.md` | Dev tooling and docs |

---

## 7. Implications for our miner

### 7.1 What to borrow

1. **`gpu_first_blocks`** — the device-side Argon2 pre-hash kernel
   (`src/kernelrunner.cu:876-911` + `prepareInputBlocksOnDevice`, `:1065-1134`). Doubles
   throughput and removes the CPU from the inner loop. Non-negotiable.
2. **Warp-per-hash + PTX `g()` + shuffle-only mixing + oneshot single-kernel pass**
   (`src/kernelrunner.cu:494-557, 811-874`). This is the mature `argon2-gpu` lineage; do not
   reinvent it.
3. **Strided last-block-only D2H copy** (`src/kernelrunner.cu:1051-1063`) — ~4000× less PCIe
   traffic at real difficulties, one call per batch.
4. **VRAM-derived batch sizing with a fixed reserve + allocation reuse across difficulty
   changes** (`src/hashapi/HashApiTuning.cpp:52-77`, `src/kernelrunner.cu:952-978`).
5. **The `IHashBackend` request/result seam with per-stage timings**
   (`src/hashapi/HashApiTypes.h`) plus the benchmark/compare/resource-gate scripts. This is
   what makes the kernel independently optimisable, and it is why this repo has a credible
   optimisation ledger at all.
6. **Idempotent-submission semantics:** treat `"already exists"` as success
   (`src/main.cpp:420-421`). A resubmitting miner will hit this constantly and must not count
   it as a failure.
7. **Exponential backoff with a cap** as used for telemetry
   (`src/StatReporter.cpp:196-203`) — apply it to `/verify` instead of the flat 2 s sleep.

### 7.2 What to replace (our differentiator)

| Woody Miner | Our design |
|---|---|
| `std::queue<std::function<void()>>` in RAM (`src/BlockSubmitter.cpp:7`) | durable store (SQLite/WAL or append-only journal + fsync) holding **serialised payloads**, not closures |
| No discovery timestamp in the payload (`src/main.cpp:386-393`) | record `found_at` (monotonic + wall clock) at match time, in `MineUnit::submitMatches` |
| 5 retries × 2 s then drop (`src/main.cpp:452-461`) | unbounded retry with capped exponential backoff + jitter; rows leave the store only on terminal success/rejection |
| Failure recorded to a truncating 1 MiB log (`src/Logger.cpp:51-56`) | the log stays a log; the queue is the source of truth |
| No startup recovery | replay pending rows on boot, before/alongside mining threads |
| Single submit thread that blocks on HTTP | separate the enqueue path (never blocks mining) from a submitter that can be paused, and add a reachability probe (reuse `GET /difficulty` as the health check) |
| `"outside of time window"` → silent drop (`src/main.cpp:415-419`) | **must be characterised first.** Measure the actual server acceptance window; it bounds how long deferred submission can be useful, and dictates whether stored hashes need a TTL and an "expired" terminal state |

### 7.3 Open question to resolve before building

`src/main.cpp:415` is the only evidence in the codebase that XenBlocks time-boxes block
acceptance. Neither the C++ miner nor `server/chain_simulator.py` documents the width of that
window. Determining it empirically against `http://xenblocks.io/verify` (or the X1 node) is
the highest-value next investigation, because it decides whether our feature is "survive a
30-minute outage" or "survive a 3-day outage".
