# TreeMiner — Grok Recommendations

**Status:** Review of `treeminer/PLAN.md` (planning stage; no code yet).  
**Scope:** Additional options for submission resilience and hashrate. Does **not** replace PLAN.md — read alongside it and `docs/01`–`docs/06`.

---

## 1. Overall verdict

The plan is solid and correctly prioritized:

1. **Phase 1** — journal-first, outage-proof submission (differentiator).
2. **Phase 2–3** — measured kernel work only after resilience ships.

Research already answers the critical protocol question: delayed **XEN11** resubmission is viable. The server does not validate client timestamps; acceptance depends on baked-in `m=` vs current difficulty; duplicate keys return `"already exists"` (idempotent ACK). XUNI remains server-clock gated.

**Ship Phase 1 as written** (with the amendments below as 1b or PLAN deltas). Do not expand kernel scope in Phase 1.

---

## 2. Submission — recommended amendments

### 2.1 Drain order (conflict with research)

| Source | Policy |
|---|---|
| PLAN.md | Oldest-first always |
| docs/01 §5.3 | When difficulty is **rising**, drain **lowest `m=` first** so low-margin finds are not starved |

**Recommendation:** make drain order depend on observed difficulty trend:

| Condition | Order |
|---|---|
| After long outage (difficulty usually falling) | Oldest-first is fine |
| Difficulty rising, or trend unknown | Ascending by `m=`, then oldest |
| Mixed kinds | XEN11 before XUNI; XUNI only if its window is open |

### 2.2 XUNI — multi-window park, not instant `dead`

| PLAN | Recommendation |
|---|---|
| 401 XUNI-window → `dead` | Park and retry for N subsequent `:55–:05` windows |

- New status or reuse `parked` + `kind=XUNI` with `expires_at` spanning **next** window(s).
- Config: `xuni_max_windows` (e.g. 2–3 hours of hourly windows).
- Mark `dead` only after budget exhausted.
- Never let XUNI starve XEN11 drain (unchanged from PLAN).

Rationale: first miss is often short outage or clock skew, not permanent loss.

### 2.3 Expand ResponseClassifier

Add explicit rows missing from PLAN §3.2:

| Server response | New status | Rationale |
|---|---|---|
| 401 `"Hash verification failed."` | `dead` | Corrupt/bad encode — do not retry forever |
| 400 invalid key / salt / missing fields | `dead` | Malformed; log loudly |
| Unknown 4xx | `parked` + log | Already in PLAN — keep |
| 401 difficulty body containing `m={N}` | update local difficulty cache | Free probe without waiting for poller |

### 2.4 Reconciliation: `GET /get_block?key=`

After ambiguous outcomes (timeout after POST, crash mid-submit), **probe by key** before re-POSTing.

- Automatic: if `attempt_count > 0` and last failure was transport/timeout → GET first; 200 ⇒ `acked`.
- Operator: `--verify-queue` CLI for audit (local Argon2 sample + get_block cross-check).

Idempotent POST already works; reconcile reduces noise and server load.

### 2.5 Local integrity before flush

On startup recovery (and optionally before high-attempt retries), re-run `argon2.verify(key, hash_to_verify)` for suspect records. Catches bit-rot without burning server CPU. Gate behind config if verify is expensive at high `m`.

### 2.6 Persistence (`synchronous=FULL` vs `NORMAL`)

| Mode | When |
|---|---|
| **`FULL` (keep as default)** | Finds are rare; durability is the product |
| `NORMAL` + checkpoint after insert | Only if profiling shows journal latency on the critical path |

Also:

- Journal-first remains: INSERT + commit **before** first HTTP attempt.
- Optional later escape hatch: append-only JSONL + compact (not Phase 1).
- Consider storing a content hash of the payload for migration safety.

**Measure:** artificial high find-rate; watch p99 `journal_insert_ms`. Optimize only if insert ever sits on the GPU path.

### 2.7 Parked record lifetime

| Policy | Tradeoff |
|---|---|
| Live forever | Correct for “never lose”; growth is tiny |
| Hard-delete after N days | Risk discarding value if difficulty later falls |

**Recommendation:** no hard delete. Optional `archived` after 30–90 days still-parked + CLI export.

### 2.8 Journal scope (per process vs per GPU)

| Model | Use |
|---|---|
| **One journal per process** | Woody-style multi-GPU process — default |
| Per-process DB under multi-process isolation | xgpu-style one process per GPU |
| Shared multi-writer SQLite | Avoid |

### 2.9 Adaptive drain rate

Fixed `drain_rate = 4` is a good default. Prefer adaptive:

1. Start low (1–2/s) when circuit breaker closes after outage.
2. Ramp while responses are 200 / `"already exists"`.
3. Back off hard on 5xx / lock-style slowness.
4. Hard cap (config) so one rig cannot push network blockrate into +1000 difficulty ticks.

Self-protective: bulk flush must not raise the floor above remaining queued `m=`.

### 2.10 Difficulty strategy while server is down

While breaker is OPEN:

- Continue mining at last-known difficulty **+ margin**.
- Optionally **step margin up** during long outages (e.g. +1000 every 5 min offline, capped) — mirrors worst-case network rise without a live poll.
- On reconnect: immediate difficulty poll → unpark eligible → throttled drain.

Cost hashrate only when finds are most at risk.

### 2.11 Dual / failover RPC

Config: `rpc_links = [primary, backup, ...]`. On primary OPEN, probe next endpoint. Same journal; transport-only change. Aligns with PLAN’s transport-agnostic journal for X1 migration.

### 2.12 Marketplace, MQTT, dev-fee (PLAN Q4 + risk 4)

**Phase 1 default binary:** self-mining + journal + local stats (`:42069`). Strip or hard-disable MQTT/marketplace/dev-fee paths unless every find path is routed through FindJournal.

Prefer **excise until journal is proven**. Any unjournaled fee path reintroduces silent loss.

### 2.13 Do not journal `/send_pow`

Best-effort only after a live `/verify` 200. Never durable-queue. Stale merkle pages are useless; hung POW submit historically killed the reference miner after successful verify.

### 2.14 Validation on journal insert

- Reject / dead-path records with `hash_to_verify` length issues (server rejects oversize; docs cite ≤150).
- Persist `account` with the hash; never “fix” account on resubmit (server attributes credit by `account` field, not salt).
- Startup: prefer no drain until first successful difficulty poll (or park conservatively on last-known).

### 2.15 Metrics to add beyond PLAN §3.5

- `expired_by_difficulty`
- `xuni_window_miss` / `xuni_recovered_next_window`
- `reconciled_via_get_block`
- `journal_insert_ms_p99`
- `drain_rate_current` / `breaker_state`

### 2.16 Live canary gate

Phase 1 is not “done” on mock chaos alone. Require a **live delayed-submit canary** against production (docs/05 risk 2: deployed server may ≠ cloned snapshot) before announcing outage-proof behavior.

---

## 3. Answers to PLAN §9 questions

| # | Question | Recommendation |
|---|---|---|
| 1 | `synchronous=FULL` vs `NORMAL`? | **FULL** for Phase 1; revisit only with measurements. |
| 2 | Should `parked` expire? | **No hard delete**; optional archive after 30–90 days. |
| 3 | Single vs per-GPU journals? | **One per process**; per-GPU only if multi-process isolation. |
| 4 | Keep telemetry/MQTT/marketplace? | **Strip/disable** for Phase 1 default; keep HiveOS local stats. |
| 5 | Is 4/s drain right? | **Fine default**; implement adaptive ramp + hard cap. |

---

## 4. Hashrate — options and discipline

### 4.1 Mental model (from docs/06)

- Woody’s kernel is **integer-ALU-bound** on consumer cards (~40% of bandwidth ceiling used).
- Realistic Phase 3 upside: **+25–60% over parity**, not “easy 2×”.
- Cheap historical wins (GPU first-blocks, oneshot, PTX G) are already taken.

### 4.2 Keep PLAN Phase 2 (high confidence)

| Item | Est. | Notes |
|---|---|---|
| Device-side finalize | ~4% | Blake2b + base64 + pattern scan on GPU; D2H only hits |
| Double-buffered streams | ~2–6% | Overlap first-blocks / oneshot / finalize tails |

Do these before occupancy/prefetch: smaller risk, golden-hash friendly, cleaner Phase 3 surface.

### 4.3 Keep PLAN Phase 3 ranking

| Rank | Idea | Est. | Caveat |
|---|---|---|---|
| 1 | Multi-warp blocks (N hashes / block) | +20–50% ALU-bound | A/B per arch; L2 thrash |
| 2 | Precomputed indexed-half ref table | ~1.5% + enabler | Vestigial `refs` in Woody |
| 3 | `cp.async` prefetch (slices 0–1) | +10–25% that half | sm80+; needs #2 |
| 4 | Per-arch autotune + fatbin | Cumulative | Key on **(GPU, difficulty band)** |

### 4.4 Additional hashrate options not fully spelled in PLAN

**A. Difficulty-margin cost model**

- At high `m`, each +1000 margin ≈ small % hashrate tax (~5 min worst-case rise tolerance).
- UI / config presets:
  - `aggressive` — margin 0  
  - `balanced` — +1000–2000  
  - `resilient` — +3000–5000  
  - `auto` — raise margin only when breaker OPEN or `pending > 0`
- **Do not default high margin when the server is healthy** — pure H/s tax.

**B. Batch size under margin**

Higher `m` shrinks VRAM-safe batch size. When margin activates, re-run batch selection; avoid OOM or thrashing with a stale batch size.

**C. Measure occupancy before multi-warp rewrite**

Nsight / occupancy counters first; optional intermediate stream/batch experiments before packing N warps per block.

**D. Explicit HashBackend seam**

Keep journal + SubmissionManager fixed; swap compute backends for experiments (Woody hashapi / `IHashBackend` pattern). Prevents Phase 3 from entangling submission code.

**E. AMD / HIP / OpenCL**

Market expansion, not NVIDIA hashrate. Phase 4+ only.

### 4.5 Dead ends (do not chase)

Documented in docs/06 — reaffirm:

- `__launch_bounds__` forcing  
- CPU hot-path rewrites  
- L1 carveout games on streaming 1 KiB blocks  
- Source-lane-only address selection  
- Claims of easy 2× without roofline evidence  

---

## 5. Suggested phase split (optional)

| Phase | Focus | Kernel diffs |
|---|---|---|
| **1a** | Journal + classifier + breaker + chaos (PLAN as written) | Zero |
| **1b** | Adaptive drain, m-order, get_block reconcile, XUNI multi-window, margin presets, metrics | Zero |
| **2** | Device finalize + double buffer | Allowed |
| **3** | Occupancy / prefetch / autotune under experiment ledger | Allowed |

1b is still “days” of work and hardens queue economics before kernel risk.

---

## 6. Architectural checklist (PLAN nits)

- [ ] Classifier: verify-failed + malformed → `dead`
- [ ] Drain: oldest-first **or** m-ascending by difficulty trend
- [ ] XUNI: multi-window park, not instant dead
- [ ] Reconcile via `GET /get_block` + `--verify-queue`
- [ ] Insert validation (length, account pairing)
- [ ] Startup: difficulty poll before aggressive drain
- [ ] Extended stats counters (§2.15)
- [ ] Live canary before “outage-proof” claims
- [ ] Fee/marketplace paths journaled or excised
- [ ] `/send_pow` never journaled
- [ ] Margin presets / auto-on-outage only
- [ ] HashBackend seam for Phase 2–3

---

## 7. Priority summary

| Priority | Action | Impact |
|---|---|---|
| P0 | Ship PLAN Phase 1 journal + submitter | Zero lost finds on outage/crash |
| P0 | Live canary of delayed XEN11 submit | Validates server ≠ snapshot risk |
| P1 | Adaptive drain + m-aware order | Protect own backlog + network |
| P1 | Classifier completeness + get_block | Correctness under ambiguity |
| P1 | XUNI multi-window | Recover otherwise-dead XUNI |
| P2 | Margin auto/presets | Resilience without permanent H/s tax |
| P2 | Phase 2 finalize + streams | ~6–10% combined, low risk |
| P3 | Multi-warp + refs + cp.async + autotune | +25–60% aspirational, measured |

---

## 8. Bottom line

- **Submission design in PLAN.md is correct and shippable.** Main upgrades: smarter drain, richer classifier, XUNI multi-window, get_block reconcile, adaptive drain, margin only when needed.
- **Hashrate roadmap is realistic.** Stick to measured Phase 2–3 levers; couple margin to outage state so resilience does not permanently tax H/s.
- **Largest revenue win is still Phase 1:** Woody’s ~10 s abandon path and the reference miner’s crash-on-outage dominate any known kernel gap for real finds.

This file is advisory. Implementation authority remains `treeminer/PLAN.md` until the team adopts specific items above.
