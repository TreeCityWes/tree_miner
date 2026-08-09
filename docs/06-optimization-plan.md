# Deep-Dive Review & Full Optimization Plan (Fable first-hand analysis)

Direct source review of `repos/XenblocksMiner` (kernel + submission) and `repos/xenminer/gpage.py`
(server verify handler). This supplements the agent reports (docs 01–04) with first-hand findings
and is the working optimization plan for the new miner.

---

## Part A — Submission path: verified failure anatomy

### A.1 What the code actually does today (Woody, `src/main.cpp:398-462`)

- Empty response body → `retries_noResponse++` and `continue` with **no sleep**: a hot-spin of 10
  rapid-fire attempts (`main.cpp:402-412`), then the find is abandoned.
- **All exceptions silently swallowed** by an empty `catch` (`main.cpp:450-451`). Connection refused
  / DNS failure / timeout each burn one of `MAX_SUBMIT_RETRIES = 5` with a flat 2 s sleep.
  **A server outage destroys every find within ~10 seconds of discovery.**
- The payload IS logged before submission (`main.cpp:396`) — proving it is fully self-contained —
  but into a 1 MiB truncating rotation that nothing reads back.
- `"outside of time window"` in a response → immediate drop (`main.cpp:415-419`).

### A.2 Server verify handler, verified line-by-line (`repos/xenminer/gpage.py`)

| Server behavior | Location | Consequence for us |
|---|---|---|
| Rejects only if `submitted_m < current_difficulty` (strictly `<`) | `gpage.py:412-418` (401) | Headroom mining works; equal-or-higher `m=` always passes |
| `"outside of time window"` strings exist **only in XUNI paths** | `gpage.py:433-435`, `gpage.py:497` | **Discrepancy resolved: XEN11 can never trigger the time-window drop.** Woody's hard-drop on that string is effectively XUNI-only |
| XEN11 explicitly has no time gate | `gpage.py:481` (comment: "no time restrictions for XEN11") | Queued XEN11 resubmission is safe indefinitely (subject to difficulty) |
| XUNI checked against **server clock** at receive, twice | `gpage.py:433`, `gpage.py:469/497` | XUNI = submit-now-or-never |
| Duplicate key → `IntegrityError` → 400 `"Block already exists, continue"` | `gpage.py:507-510` | Idempotent resubmission; 400+that string = ACK |
| Server re-verifies Argon2 (`argon2.verify(key, hash)`) | `gpage.py:450-454` | Payload correctness is all that matters; no session state |
| Insert stores only `(hash_to_verify, key, account)` | `gpage.py:490` | No timestamp in payload or insert → server cannot age-discriminate |

### A.3 New finding: the 401 difficulty rejection is NOT permanent death

The check is `submitted_difficulty < int(difficulty)` against **current** difficulty at submit time
(`gpage.py:404,412`). Difficulty moves every 300 s (±, floor 100, typically **falling** during
outages at −2000/tick). Therefore a hash rejected with 401-difficulty today becomes **valid again
automatically** whenever network difficulty drops back to ≤ its baked-in `m=`.

→ The queue state machine gains a **`parked`** state: on 401-difficulty, don't mark dead — park the
record and re-attempt whenever the difficulty poller observes `current ≤ m`. Storage is free;
this recovers finds that every other miner design would discard.

### A.4 Submission architecture (final design)

```
GPU workers ──find──▶ SQLite WAL journal (write BEFORE first attempt, fsync)
                            │
                            ▼
                   Submitter thread (single, decoupled)
                     ├─ hard connect+read timeouts on every request
                     ├─ drain oldest-first, throttled after outage recovery
                     └─ response classification (verified against gpage.py):
                          200                        → acked
                          400 + "already exists"     → acked   (idempotent replay)
                          401 + difficulty message   → parked  (retry when diff ≤ m)
                          401 + XUNI window message   → dead    (unsalvageable)
                          5xx / timeout / conn error → pending (exp. backoff + jitter, cap 60 s, forever)
```

Schema: `finds(id, key UNIQUE, hash_to_verify, account, kind, m, found_at, status, attempt_count,
last_attempt_at, last_response)` — `status ∈ {pending, acked, parked, dead}`.
Startup: re-enqueue all `pending`; move `parked → pending` when difficulty allows.
XUNI: enqueue flagged `expires_at = next :05 boundary`; tight retry inside window; auto-dead after;
never allowed to starve XEN11 backlog (priority: XEN11 first, XUNI only when XEN11 queue idle or
window closing).
Config knob: `difficulty_margin` (mine at `current + margin`; ~5 min of rising-difficulty tolerance
per +1000 KiB; near-free insurance since outage difficulty usually falls).

This layer alone converts today's "outage = crash (reference miner) or 10-second data loss (Woody)"
into "outage = zero loss, zero hashrate impact."

---

## Part B — Hashing path: first-hand kernel review

### B.1 What Woody's kernel actually is (`src/kernelrunner.cu`)

- Launch: `argon2_kernel_oneshot<<<batchSize, THREADS_PER_LANE(=32), 1 KiB shared>>>` —
  **one hash per CUDA block, one warp per block** (`kernelrunner.cu:1163`, `:811-874`).
- State entirely in registers (4×u64 per thread), mixing via `__shfl_sync`, Blake2b G in PTX;
  1 KiB shared staging buffer per warp; **no local-memory spills** (ledger: 32–53 regs by arch).
- Slice loop already split: slices 0–1 use indexed (data-independent) addressing, slices 2–3
  data-dependent (`:838-872`) — recent optimization, +5–7% at high difficulty.
- Device-side first-blocks kernel (Blake2b prehash on GPU) — their single biggest historical win
  (+108–131%).
- Per-step traffic: `load_block_xor` (1 KiB read of ref block) + `store_block` (1 KiB write)
  (`:752-768`) = 2 KiB DRAM traffic per memory block.

### B.2 Roofline check (my math)

Per hash: `2 KiB × m` DRAM traffic. Ledger baseline d4096 ≈ 10.74 k H/s ⇒ ≈ 90 GB/s sustained.
On an RTX-3050-class card (~224 GB/s) that is **~40% of peak bandwidth**, and their own timing shows
compute = 92–98% of wall. Conclusion: **on consumer cards this kernel is integer-ALU-bound
(Blake2b is 64-bit adds/rotates, emulated as 32-bit pairs), not yet bandwidth-bound.** The
bandwidth wall is the *ceiling* (`H/s ≤ BW / (2 KiB × m)`), not the current limiter. There IS
headroom — but it's in ALU throughput and latency hiding, not "cleaning up unoptimized C."

### B.3 Optimization candidates, ranked by expected value

1. **Occupancy via multi-warp blocks** (High, est. +20–50% on ALU-bound cards).
   One warp per block caps resident warps at the per-SM *block* limit (typically 16), i.e. ~33%
   occupancy on sm86 (16/48). Pack N hashes per CUDA block (N warps, N KiB shared, no cross-warp
   sync needed — hashes are independent). Their failed `__launch_bounds__` experiment changed regs,
   not blocking; this is a different lever. Must A/B per arch — more resident warps also thrash L2.

2. **Precomputed ref-index table for the indexed half** (Medium alone, enabler for #3).
   Verified: the address-generation input (`thread_input`, `kernelrunner.cu:828`) contains **no
   job-specific field** — at fixed difficulty the indexed-half ref sequence is identical for every
   hash in the batch and across batches. Compute once per difficulty change into device memory;
   delete `next_addresses1` from the hot loop (~1.5% direct) and free the `addr` registers.
   (A vestigial `refs` buffer already exists in `KernelRunner` — `kernelrunner.cu:918,1016` —
   likely inherited from argon2-gpu's precompute mode; the plumbing half-exists.)

3. **`cp.async` prefetch pipeline for the indexed half** (High on sm80+, est. +10–25% on that half).
   With ref indices known in advance (#2), slices 0–1 (half of all work) can run a deep
   asynchronous prefetch pipeline: `cp.async` the ref block k+2 into shared while mixing block k —
   hiding DRAM latency entirely instead of stalling per step. The data-dependent half can't
   prefetch (index depends on the just-computed block), but can still overlap its 1 KiB store
   with the next load.

4. **Finalization fully on device** (Small but clean, ~4%).
   Ledger: `finalize_ms` ≈ 4.4%. Do final Blake2b + base64 + `XEN11`/`XUNI[0-9]` scan in a kernel;
   copy back only hits (bytes per hour instead of `batch × 1 KiB` per batch). Also kills the
   `cudaMemcpy2DAsync` strided copy and CPU finalize thread entirely.

5. **Double-buffered streams per GPU** (Small, ~2–6%).
   Two batches in flight: batch B's first-blocks + finalize overlap batch A's oneshot kernel.
   Near-free once #4 shrinks the tails.

6. **Per-arch tuned builds** (Small-medium, cumulative).
   Fat binary with per-SM tuning (batch size table, N-warps-per-block from #1, cp.async on/off),
   plus a startup micro-autotune (30 s sweep, cached per card+difficulty band). Woody's own data
   shows tuning results don't transfer across difficulty values — the autotune must key on both.

7. **What NOT to chase** (documented dead ends — theirs and mine):
   `__launch_bounds__` forcing (severe regression, ledger), source-lane-only address selection
   (regression), CPU-side work (already <8% combined), L1 carveout games (streaming 1 KiB blocks,
   L1 reuse ≈ 0), and any "rewrite the C" claims — state is in registers/PTX already; there is no
   naive C hot loop to fix.

### B.4 Measurement discipline (adopted from their ledger, improved)

- Keep an `IHashBackend`-style seam + per-stage timings + golden-hash tests (bit-exact vs CPU
  Argon2id) — every experiment validates correctness before benchmarking.
- Roofline dashboard per card: sustained GB/s vs theoretical, achieved occupancy, ALU utilization
  (Nsight Compute where counters permitted) — so we always know which wall we're against.
- Benchmark at realistic difficulty (thousands–tens of thousands), never extrapolate from tiny m.
- Record rejected experiments with numbers (their ledger practice — it saved us from re-running
  two dead ends already).

---

## Part C — Combined plan of record

**Phase 1 — Resilience (days, immediate value):** journal-first SQLite WAL queue + decoupled
submitter (A.4) wrapped around an existing engine. Every find survives outage, crash, reboot.
This is the differentiator no other miner has; ship it first.

**Phase 2 — Own engine (weeks):** C++/CUDA core adopting Woody's proven design (warp-per-hash,
PTX G, oneshot kernel, GPU first-blocks, VRAM-derived batching) — license check pending in doc 05 —
with the finalize-on-device (#4) and stream double-buffering (#5) built in from day one.

**Phase 3 — The new headroom (ongoing, measured):** multi-warp occupancy (#1), precomputed refs
(#2) + cp.async pipeline (#3), per-arch autotune (#6), guided by the roofline dashboard and the
experiment ledger.

Realistic expectation: Phase 1 recovers revenue no kernel work can (every outage-lost find +
zero stall time). Phase 2 reaches parity with the best public miner. Phase 3 is plausibly
+25–60% over parity on ALU-bound consumer cards, less on bandwidth-bound datacenter cards —
claims of "easy 2×" are not supported by the evidence; the ledger shows the cheap wins are taken.
