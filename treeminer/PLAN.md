# TreeMiner — Comprehensive Build Plan (v2)

**Status: IMPLEMENTATION AUTHORITY — build phase active.** v2 incorporates the adopted amendments
from the three reviews (KIMMY-PLAN.md, GROK-RECOMMENDATIONS.md, SOL-PLAN.md), all verified against
code. **§10 below supersedes conflicting v1 text.**

TreeMiner is a new XenBlocks miner derived from the three studied projects, whose defining
feature is outage-proof submission: no found hash is ever lost to a server disconnect, crash,
or reboot. Research basis: `docs/01`–`docs/06` in this repository (all claims below carry
file:line evidence there).

---

## 1. What TreeMiner inherits from each parent

| Parent | What we take | What we must NOT take |
|---|---|---|
| **woodysoil/XenblocksMiner** (MIT, README:211) | The codebase itself — Phase 1 is a direct fork: CUDA Argon2id engine (warp-per-hash, PTX G, oneshot kernel, GPU first-blocks, VRAM batching), difficulty poller, HiveOS/stats plumbing, build system | Its submission layer (in-RAM closure queue, empty-catch retry loop) — replaced wholesale |
| **jacklevin74/xenminer** (NO license) | Protocol knowledge only: verify semantics, difficulty rules, response strings, XUNI window, duplicate-key behavior | **Any code.** Unlicensed = all rights reserved. Facts aren't copyrightable; lines are. |
| **JozefJarosciak/xgpu** (no license) | Ops lessons only: per-GPU process isolation, GPU auto-detection, the failure catalog of what not to do | Any code (moot — deployment scripts around a vanished upstream) |

Attribution: retain Woody's MIT notice and add ours; state derivation in README.

## 2. Languages & toolchain

- **Core miner: C++17 + CUDA** — inherited from the fork; the kernel is proven and MIT-clean.
  No rewrite in another language in Phase 1; churn there risks the one component that already works.
- **Persistence: SQLite ≥3.35** via the `sqlite3` vcpkg port (public domain), C API wrapped in a
  thin RAII class. WAL mode, `synchronous=FULL`.
- **Build: CMake + vcpkg** (inherited; add one dependency line).
- **Test harness: Python 3.11+** — mock XenBlocks server (Flask or stdlib http.server) replicating
  the exact response strings/codes documented in docs/05 §1; chaos scripts (kill server, kill miner,
  clock/difficulty manipulation). Python is test-only, never shipped.
- **Target platforms:** Linux x86_64 (primary, incl. HiveOS), Windows x86_64 (secondary). Both
  already supported by the fork's CMake presets.

## 3. Architecture

```
                 ┌────────────────────────────────────────────────┐
                 │                  TreeMiner process              │
                 │                                                │
  GPU 0..N ────▶│  MiningEngine (Woody kernel, UNTOUCHED Phase 1) │
                 │        │ find (hash_to_verify, key, m, kind)   │
                 │        ▼                                       │
                 │  FindJournal ──── finds.db (SQLite WAL, FULL)  │◀── crash-safe truth
                 │        │ notify                                │
                 │        ▼                                       │
                 │  SubmissionManager (single thread)             │
                 │    ├─ state machine: pending/acked/parked/dead │
                 │    ├─ CircuitBreaker (server up/down + probe)  │
                 │    └─ DrainScheduler (oldest-first, throttled) │
                 │        │ HTTP POST /verify (hard timeouts)     │
                 │        ▼                                       │
                 │  DifficultyService (10s poll, cached, feeds    │
                 │    engine + un-parks eligible finds)           │
                 │                                                │
                 │  StatsServer :42069 (+ journal counters)       │
                 └────────────────────────────────────────────────┘
```

### 3.1 FindJournal (new, ~150 LOC)

```sql
CREATE TABLE finds (
  id            INTEGER PRIMARY KEY,
  key           TEXT NOT NULL UNIQUE,      -- Argon2 password; server-side dedupe key
  hash_to_verify TEXT NOT NULL,            -- full encoded hash incl. m=
  account       TEXT NOT NULL,
  kind          TEXT NOT NULL CHECK(kind IN ('XEN11','XUNI')),
  m             INTEGER NOT NULL,          -- baked-in memory_cost
  found_at      TEXT NOT NULL,             -- ISO-8601 UTC, local bookkeeping only
  expires_at    TEXT,                      -- XUNI only: end of :55–:05 window
  status        TEXT NOT NULL DEFAULT 'pending'
                CHECK(status IN ('pending','acked','parked','dead')),
  attempt_count INTEGER NOT NULL DEFAULT 0,
  last_attempt_at TEXT,
  last_response TEXT
);
CREATE INDEX idx_finds_status ON finds(status, id);
```

Invariant: **INSERT + fsync completes before the first submission attempt** ("journal-first").
Superblock detection (≥50 uppercase) is local-stats-only; the server classifies offline.

### 3.2 SubmissionManager (replaces BlockSubmitter, ~250 LOC)

Response classification — the contract, verified against server code (docs/05 §1):

| Server response | New status | Rationale |
|---|---|---|
| 200 | `acked` | accepted |
| 400 + `"already exists"` | `acked` | UNIQUE-key duplicate ⇒ prior attempt landed; replay is idempotent |
| 401 + difficulty message | `parked` | strictly-`<` check vs *current* difficulty ⇒ auto-valid again when difficulty ≤ m |
| 401 + XUNI-window message | `dead` | server-clock gated; unsalvageable |
| 5xx / timeout / empty body / connect error | `pending` | retry forever, backoff below |

- Hard timeouts on every request (connect 5 s, total 10 s). All exceptions classified, none swallowed.
- **CircuitBreaker:** 3 consecutive transport failures ⇒ OPEN (server down). While OPEN: no /verify
  attempts; probe `GET /difficulty` at 5 s → ×2 backoff + jitter, cap 60 s. First success ⇒ CLOSED.
- **DrainScheduler:** on CLOSED after outage, submit oldest-first at ≤4/s (configurable) — never
  stampede a recovering server. XEN11 strictly before XUNI; XUNI skipped/dead past `expires_at`.
- Per-record backoff independent of breaker for flapping (single-request) failures.

### 3.3 DifficultyService (modified, ~30 LOC delta)

Woody's 10 s poller, plus two hooks: (a) publish to engine with optional
`difficulty_margin` added (config; default 0; each +1000 KiB ≈ 5 min tolerance vs rising
difficulty); (b) on observed decrease, `UPDATE finds SET status='pending' WHERE status='parked'
AND m >= :current` and wake the submitter.

### 3.4 Startup RecoveryService (~40 LOC)

Open journal → log `recovered: P pending, K parked, A acked, D dead` → expire stale XUNI →
re-evaluate parked vs current difficulty → submitter drains. A rig rebooted mid-outage
self-heals with zero operator action.

### 3.5 Stats (small delta)

Extend `:42069` JSON + console line with `pending`, `parked`, `acked_total`, `dead_total`,
`server_state (up/down, downtime)`. HiveOS surfaces it for free.

## 4. Repository & folder outline

Phase 1 lives in `treeminer/` as the fork:

```
treeminer/
  PLAN.md                    ← this document
  README.md                  ← derivation + MIT attribution (Woody)
  LICENSE                    ← MIT
  CMakeLists.txt, CMakePresets.json, vcpkg.json (+sqlite3)
  src/
    ... (forked Woody sources, kernel untouched)
    journal/FindJournal.{h,cpp}        ← NEW
    submit/SubmissionManager.{h,cpp}   ← NEW (replaces BlockSubmitter)
    submit/CircuitBreaker.{h,cpp}      ← NEW
    submit/ResponseClassifier.{h,cpp}  ← NEW (pure function; unit-tested hardest)
  tests/
    unit/          (classifier, journal, breaker — no GPU, no network)
    mockserver/    (Python mock of /verify + /difficulty, exact strings)
    chaos/         (scripted: server kill, miner kill -9, difficulty swings)
  docs/CHANGES-FROM-UPSTREAM.md
```

Diff discipline: kernel and hashing files carry **zero diffs** in Phase 1 (preserves upstream
merge-ability and isolates risk to the submission layer).

## 5. Configuration additions

```
difficulty_margin = 0          # KiB headroom baked into m=
journal_path = finds.db
drain_rate = 4                 # submissions/sec after recovery
breaker_threshold = 3          # consecutive failures to open
probe_max_interval = 60        # seconds
```

All optional; defaults reproduce safe behavior. Existing Woody config keys unchanged.

## 6. Test plan (acceptance gates for Phase 1)

1. **Unit:** ResponseClassifier against every documented server string/code; journal CRUD +
  UNIQUE conflict; breaker transitions.
2. **Golden hashes:** forked build reproduces upstream hashes bit-exact (kernel untouched — this
  guards accidental drift).
3. **Chaos (the point of the project):** against the mock server —
   - kill server 30 s / 10 min / 12 h mid-mining ⇒ zero finds lost, hashrate unaffected;
   - `kill -9` the miner with queued finds, restart ⇒ all finds ack exactly-once-effective;
   - difficulty +3000 during outage ⇒ affected finds park, then auto-submit when lowered;
   - XUNI found at :57, server down until :10 ⇒ marked dead, never starves XEN11 drain;
   - duplicate replay ⇒ "already exists" counted as acked, no operator-visible error.
4. **Live canary:** small real-wallet run against the production endpoint to empirically confirm
  delayed-acceptance behavior before announcing (docs/05 §6 risk 2).

## 7. Phases

- **Phase 1 (this plan, ~days):** fork + journal/submitter refactor. Deliverable: drop-in
  Woody replacement that never loses a find. Kernel diffs: zero.
- **Phase 2 (~weeks):** engine work begins — device-side finalize, double-buffered streams
  (docs/06 B.3 #4–5); still MIT-clean fork lineage.
- **Phase 3 (ongoing):** occupancy (multi-warp blocks), precomputed indexed-half refs +
  `cp.async` prefetch, per-arch autotune (docs/06 B.3 #1–3, #6) under experiment-ledger
  discipline with golden-hash gates. Honest target: +25–60% over parity on consumer GPUs.

## 8. Risks (carried from docs/05 §6)

1. **X1 migration** may replace HTTP `/verify` — journal is transport-agnostic; submitter sits
  behind an interface so only the HTTP adapter would change.
2. **Deployed server ≠ cloned snapshot** — classifier treats unknown 4xx as `parked`+logged
  (never silent-drop); live canary validates early.
3. **Woody license is README-declared MIT without a LICENSE file** — low risk; ask upstream,
  document in CHANGES-FROM-UPSTREAM.
4. **Dev-fee/marketplace code paths** in the fork touch submission — must be routed through the
  journal too or cleanly excised; decide at review.

## 9. Questions for reviewing agents

1. Is `synchronous=FULL` vs `NORMAL` the right WAL trade? (Writes are rare; we chose max durability.)
2. Should `parked` records ever expire (e.g., after 30 days) or live forever?
3. Single journal per process vs per-GPU journals for multi-process rigs (xgpu pattern)?
4. Keep or strip telemetry/MQTT/marketplace subsystems in the Phase 1 fork?
5. Is the 4/s drain default right, given unknown server capacity after recovery?

---

## 10. v2 amendments (ADOPTED — supersede conflicting text above)

All verified in code before adoption. Shared types live in `src/treeminer/Types.h` (authoritative
for the journal/submitter contract).

1. **Immutable `FoundPayload` at discovery (Phase 1 step zero).** Upstream re-hashes with
   current difficulty at submit time and silently drops on mismatch (`main.cpp:371-381` — verified
   race). TreeMiner constructs the full PHC string once from the batch's actual parameters; the
   submission path never recomputes Argon2. Also delete the duplicate CPU re-verify
   (`main.cpp:364-378`).
2. **Confirmation-aware acks.** Server returns 200 even when its DB insert failed
   (`gpage.py:492-494,515` — verified). `200 → AcceptedUnconfirmed`, then confirm via
   `GET /get_block?key=` (`gpage.py:331` — verified) → `Acked`. Duplicate ("already exists")
   likewise confirmed by lookup. If lookup unavailable, remain AcceptedUnconfirmed with metrics.
3. **Richer state machine** (see Types.h): `Quarantined` (unknown 4xx — never auto-unparks)
   split from `ParkedDifficulty`; `PermanentlyInvalid` for malformed/verify-failed
   (server string "Hash verification failed." → PermanentlyInvalid, `gpage.py:519`);
   `ParkedXuniWindow` replaces instant-dead XUNI (see #5).
4. **Classifier additions:** 401 difficulty message embeds current difficulty in
   `m={N}` (`gpage.py:416` — verified) → parse into `server_difficulty_hint`. 429 honors
   Retry-After. Both real server rejection strings ("XUNI Submitted outside of proper time
   frame.", "XUNI found outside of time window") are explicit test cases; substring matching is
   fallback after structured parse.
5. **XUNI scheduling corrected:** server gates XUNI on its own clock only — nothing binds a XUNI
   to the hour it was found (verified), so missed XUNIs park for up to `xuni_max_windows`
   (default 3) subsequent windows. Near an open window's end, eligible XUNI **preempts** XEN11;
   backlog order otherwise: oldest XEN11 first (ascending-`m` first when difficulty trend is
   rising). Track server-clock offset from HTTP `Date` headers; while eligible XUNI exists, cap
   breaker probe intervals at ~5 s.
6. **Adaptive drain** replaces fixed rate: start 1/s on breaker close, double per healthy
   round-trip, halve on 5xx/429; `drain_rate` config is the ceiling. Separate breaker health for
   `/verify` vs `/difficulty` (half-open probes use a real queued submission).
7. **Difficulty policy during outage:** keep mining at last-known difficulty + margin; margin
   presets `aggressive/balanced/resilient/auto` where `auto` raises margin only while the breaker
   is open or backlog exists (no healthy-state hashrate tax). Persist difficulty observations
   (`difficulty_seen` table) so startup recovery can drain immediately on last-known state.
8. **Keygen hardening:** replace 32-bit-seeded `mt19937` (`RandomHexKeyGenerator.h:15-17` —
   verified) with per-device 128-bit boot nonce + monotonic counter → collision-safe keys at
   fleet scale.
9. **Rulings on §9:** WAL + `synchronous=FULL` (+ finite `busy_timeout`); parked records never
   auto-expire (prune only terminal states by policy; default acked >30 d); one journal per
   process, never shared/NFS; strip MQTT/marketplace/dev-fee/telemetry from the Phase 1 binary
   (HiveOS local stats stay); `/send_pow` is never journaled (best-effort only, currently dead
   code upstream).
10. **Kernel-plan notes:** device-side finalize previously failed upstream on output-buffer
    lifetime (ledger 246-251, 834) — Phase 2 retry requires the fixed-capacity persistent
    hit-buffer ownership model. Multi-warp-per-block remains a candidate (the rejected upstream
    experiment was `__launch_bounds__`, a different lever) but requires Nsight occupancy evidence
    before implementation. Extra Phase 3 candidates: 128-bit vectorized block load/store,
    fused/persistent kernel, L2 persistence + cross-hash step alignment (bounded experiments,
    ledger-recorded).
11. **Metrics additions:** `expired_by_difficulty`, `xuni_window_miss/recovered`,
    `reconciled_via_get_block`, `journal_insert_ms_p99`, `drain_rate_current`, `breaker_state`,
    and accepted-yield (eligible/accepted H/s) alongside raw H/s.
12. **Positioning:** competitors already advertise SQLite queues; TreeMiner's claims are the
    verified extras — immutable payloads, confirmed acks, crash-safe ambiguity handling,
    server-clock-aware XUNI recovery, transparent accepted-yield.

---

## 11. Stats delivery (miner-hosted JSON API + dashboard) — ADOPTED

Build on the existing Crow server (`src/LocalServer.cpp`, currently hardcoded `:42069` with
`/stats` and `/platform/status`). The upstream Vite/TS `web/` app is marketplace UI — excluded
from Phase 1; the dashboard is a single self-contained static page instead.

### 11.1 Configuration

```
stats_enabled = true
stats_bind    = 127.0.0.1   # default local-only; set 0.0.0.0 to expose on LAN
stats_port    = 42069       # configurable (replaces hardcoded value)
stats_token   =             # optional; when set, non-localhost requests require
                            # Authorization: Bearer <token>
```

Security stance: no auth by default but bound to localhost by default. Exposing beyond LAN is
the operator's job (reverse proxy); README says so explicitly. The endpoint is read-only —
wallet address appears in stats (it's public on-chain anyway), but no keys/config are served.

### 11.2 JSON API

- `GET /api/summary` — hashrate (raw + accepted-yield), current difficulty + margin in effect,
  blocks found by kind (normal/super/XUNI), uptime, version.
- `GET /api/gpus` — per-GPU: model, hashrate, batch size, memory in use, last batch latency
  (extends the existing `/stats` payload; `/stats` kept as alias for HiveOS compatibility).
- `GET /api/journal` — pending / parked_difficulty / parked_xuni / quarantined / acked / dead
  counts, oldest-pending age, last-ack time, `journal_insert_ms_p99`.
- `GET /api/server` — breaker state (up/down/half-open), consecutive-failure count, current
  probe interval, cumulative downtime, per-endpoint latency, server-clock offset estimate,
  last difficulty observation (+age), drain_rate_current.
- `GET /api/finds?limit=50` — recent finds: kind, m, found_at, status, attempts, last_response
  (truncated). The "did my block make it" question, answerable at a glance.
- `GET /api/history` — in-memory ring buffer (~24 h at 30 s samples) of hashrate, difficulty,
  journal depth, breaker state — powers dashboard charts; no DB reads on the hot path.
- `GET /metrics` — Prometheus text format mirroring the above (fleet Grafana for free).
- Existing `/platform/status` removed with the marketplace strip.

All handlers read from an in-memory `StatsRegistry` (atomic counters + ring buffer) fed by the
engine, journal, and submitter; journal COUNT queries run on a 5 s cache, never per-request.

### 11.3 Dashboard (`GET /`)

One static, self-contained HTML file (inline CSS/JS, **zero external CDN/fonts** — the dashboard
must render on a rig whose upstream network is down, which is exactly when the operator looks).
Embedded in the binary at build time (string resource) so there is nothing to install. Polls the
JSON API every 2–5 s. Content, in order of prominence:

1. **Outage banner** — the product's showcase moment: green "connected" strip normally; when the
   breaker is open, a bold amber banner: "Server unreachable for 12m — mining continues,
   **37 finds safely queued**, next probe in 40 s." Zero-loss made visible.
2. Hashrate hero number + 24 h sparkline; accepted-yield beside raw.
3. Per-GPU tiles (hashrate, batch, memory).
4. Journal panel: state counts + recent-finds table with status chips.
5. Difficulty chart with margin overlay; block-found event markers.

### 11.4 Delivery & ownership

Phase 1 deliverable (integration lead), after journal/submitter wiring: port/bind/token config
plumbed through AppConfig, StatsRegistry, API routes, dashboard page, Prometheus mirror.
Chaos-test addition: dashboard/API stay live and truthful while the mock server is down (the
stats server must never share fate with the submitter thread).
