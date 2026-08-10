# Kimmy Plan — Addendum to TreeMiner PLAN.md

**Status: PLANNING — companion to `treeminer/PLAN.md`. Where the two conflict, this document wins
until PLAN.md is revised.**

This plan captures the review additions to the TreeMiner build plan: new submission-layer
findings, extra hashrate candidates, and rulings on the open questions in PLAN.md §9. Evidence
citations refer to `repos/XenblocksMiner` (first-hand) and `docs/05`–`docs/06`.

---

## 1. New first-hand findings (justify existing design choices)

These came from re-reading Woody's submit path (`repos/XenblocksMiner/src/main.cpp`). PLAN.md
already fixes both by design; record them as justification.

1. **Stale-difficulty silent drop** (`main.cpp:371-381`): Woody's submit closure re-hashes the key
   with `globalDifficulty` read at *task-execution* time and silently `return`s if the result
   doesn't match the find. If difficulty ticks between find and submit, the find is dropped with
   no error, no log, no retry. TreeMiner's journal stores `hash_to_verify` computed at find time —
   this whole failure class is eliminated. State this explicitly in the README/changes doc.
2. **Double CPU re-verify per find** (`main.cpp:360-368`, then again inside the task closure):
   every find triggers two full CPU Argon2id runs at current `m` before submission. Rare enough to
   not cost hashrate, but pure dead work — the GPU already produced a verified hash. Delete both
   in the fork.

## 2. Submission layer — changes and additions

### 2.1 XUNI priority correction (overrides PLAN.md §3.2)

PLAN.md says "XEN11 strictly before XUNI" in the DrainScheduler. That rule applies to **backlog
only**. Corrected rule:

- **Live XUNI preempts everything.** A fresh XUNI found while a backlog is draining jumps the
  queue — it is submit-now-or-never (`gpage.py:433`, server-clock gated), XEN11 has no deadline
  (`gpage.py:481`).
- Backlog drains **XEN11-first**; a XUNI past `expires_at` never enters the drain (marked `dead`
  at enqueue/expire time, not at drain time).

### 2.2 Engine behavior while the server is down (fills a PLAN.md gap)

PLAN.md never specifies what difficulty the engine mines at when `/difficulty` is unreachable.
Rule: **keep mining at last-known difficulty + `difficulty_margin`.** During real outages network
difficulty typically *falls* (−2000 per 300 s tick), so outage-time finds at last-known difficulty
are very likely valid on reconnect — this converts downtime into a modest revenue advantage, not
just zero-loss.

### 2.3 Server-clock offset tracking for XUNI

The XUNI window is gated on the **server's** clock, not ours. Read the `Date` header from every
`/difficulty` response, maintain a running offset estimate, and schedule XUNI submission and
`expires_at` against **server time**. A rig with a skewed local clock otherwise wastes XUNIs.

### 2.4 HTTP keep-alive on the submitter

Fresh TCP connect per attempt adds latency exactly where it hurts (XUNI window, post-outage
drain). Use one persistent connection per endpoint, wrapped by the CircuitBreaker; reconnect on
transport failure.

### 2.5 Multi-endpoint failover (optional, config)

`rpcLink` becomes a **list**. If the primary is circuit-OPEN but a secondary responds, submit
there. Sits behind the transport interface already specified for X1-migration safety
(PLAN.md §8 risk 1); no journal changes.

### 2.6 Difficulty history in the journal

Persist observed difficulty (small table: `difficulty_seen(at, value)`). On startup recovery,
un-park eligible finds using last-known difficulty + decay model *before* the first successful
probe, so the drain starts the instant the server is reachable rather than one probe later.

### 2.7 Adaptive drain rate (replaces fixed 4/s default)

Start at 1/s after breaker CLOSE, double every successful round-trip up to `drain_rate` cap,
halve on any 5xx. A recovering server reports its own capacity; don't guess it in config.
`drain_rate` remains as the ceiling.

### 2.8 Classifier test case from the string discrepancy

Docs/05 §2: Woody matches `"outside of time window"`, but the server's actual XUNI rejection is
`"XUNI Submitted outside of proper time frame."` — a string Woody never matched. Both strings must
be explicit ResponseClassifier unit-test cases, since our classifier must correctly handle
responses the parent miner never saw.

## 3. Hashrate — candidates beyond docs/06 B.3

PLAN.md Phase 2/3 list (multi-warp occupancy, precomputed refs, `cp.async`, device finalize,
double-buffering, autotune) stands. Additions:

1. **Fused persistent kernel** (Phase 2 structural choice): fuse first-blocks → oneshot →
   finalize into a single kernel launch per batch — or one persistent kernel per GPU. Eliminates
   per-batch launch/sync overhead and the stage tails that B.3 #4/#5 work around.
2. **128-bit vectorized loads/stores** (`ulonglong2`/`int4`) for the 1 KiB ref-block load and
   store paths, if not already present — fewer memory instructions, better memory-level
   parallelism. Cheap experiment; golden-hash gated like everything else.
3. **L2 persistence + cross-hash step alignment** (Phase 3 stretch): B.3 #2 established that the
   indexed-half ref sequence is identical for every hash in a batch. Aligning warps to the same
   step makes their ref loads hit L2 simultaneously; an `cudaAccessPolicyWindow` over the memory
   region can then raise L2 hit rate for half of all traffic. Doc 06 correctly dismisses L1 games
   (streaming blocks, ~0 reuse) — this is a different lever. Constrains batch scheduling, so run
   it as a bounded experiment with a rejection budget, recorded in the ledger either way.
4. **CUDA Graphs** per batch shape: only meaningful if kernels stay unfused; low priority given
   92–98% compute.
5. **Clock policy (ops, not code):** the kernel is ALU-bound on consumer cards (docs/06 B.2), so
   locked *core* clocks beat memory overclocks. Ship a recommended `nvidia-smi -lgc` profile in
   the README — free few percent, and it stabilizes benchmark numbers for the experiment ledger.

Non-goals remain as documented in docs/06 B.3 #7 — no `__launch_bounds__` forcing, no
source-lane-only addressing, no L1 carveout tuning, no "rewrite the C."

## 4. Rulings on PLAN.md §9 open questions

1. **`synchronous=FULL` vs `NORMAL` → FULL, confirmed.** Finds per rig are rare (network target
   is 70 blocks/min across *all* miners), so fsync cost is noise; FULL is the only mode that
   survives power loss mid-write.
2. **Parked expiry → never auto-expire.** ~200 bytes/record and difficulty does come back down.
   Bound DB size by pruning `acked` records older than N days (config, default 30). Ship a manual
   `--reap-dead` for operators.
3. **Journal topology → one journal per process; recommend one process per GPU** (xgpu pattern).
   WAL tolerates multi-process writers, but per-process DBs remove lock contention and keep one
   GPU's crash away from other GPUs' queues. Do not share one DB across miner processes.
4. **Telemetry/MQTT/marketplace → strip in Phase 1.** Any code path that can submit must go
   through the journal; the cheapest guarantee is deleting the paths we don't need (this resolves
   PLAN.md §8 risk 4). Record the dev-fee excision explicitly in `CHANGES-FROM-UPSTREAM.md`.
5. **Drain default → adaptive** (see §2.7); the configured `drain_rate` becomes the ceiling, not
   the fixed rate.

## 5. Process changes

- **Live canary becomes a blocking gate for Phase 2**, not just for announcement (PLAN.md §6
  gate 4 upgraded). Every kernel decision in Phase 2/3 is wasted if the deployed server handles
  delayed submissions differently than the cloned snapshot — confirm empirically before engine
  work starts.
- **Journal-first invariant test:** chaos suite must include a `kill -9` *between* GPU find and
  first submit attempt, proving the INSERT+fsync-before-attempt ordering (PLAN.md §3.1) holds in
  the real binary, not just in unit tests.
