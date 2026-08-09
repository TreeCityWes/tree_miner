# Consolidated Protocol Spec & New-Miner Design Synthesis

Synthesis of docs 01–04 (agent research) and doc 06 (first-hand deep dive + optimization plan).
Doc 06 holds the detailed optimization plan and submission architecture; this doc is the
authoritative protocol reference, discrepancy resolutions, licensing, and roadmap.

---

## 1. Consolidated protocol spec

**Hash:** Argon2id v19, `t=1`, `p=1`, `hash_len=64`, `m = current network difficulty` (KiB).
- Password ("key"): 64-hex string (SHA-256 of a random 1–128 char string in the reference miner;
  Woody generates random 64-hex directly — server only cares that `argon2.verify(key, hash)` passes
  and `key` is unique).
- Salt: the raw 20 bytes of the miner's ETH address (`bytes.fromhex(account[2:])`) → the reward
  destination is baked into the hash; stolen payloads are worthless to a thief.

**Valid finds** (checked in `hash_to_verify[-87:]`, i.e. `$` + 86-char encoded digest):
- **XEN11 block**: contains `XEN11`. No time constraints (`gpage.py:481`).
- **Superblock**: a XEN11 block whose digest has ≥50 uppercase chars — classified offline by the
  server (`make_superblocks.py`); the miner needs no special handling beyond local stats.
- **XUNI**: matches `XUNI[0-9]`. Accepted only when the **server's clock** is within 5 min of the
  hour (`gpage.py:429-435, 469`). Submit-now-or-never.

**Difficulty:** `difficulty == Argon2 memory_cost` (KiB). Server targets ~70 blocks/min network-wide,
adjusts every 300 s (+1000 / −2000, floor 100; `manage_difficulty2.py`). Published via
`GET /difficulty`; Woody polls every 10 s. Validation at submit time is **strictly
`submitted_m < current_difficulty` → 401** (`gpage.py:404,412`) — equal-or-higher always passes.

**Live endpoints:**

| Method | Endpoint | Purpose | Notes |
|---|---|---|---|
| GET | `{rpc}/difficulty` | current difficulty | Woody timeout 5 s |
| POST | `{rpc}/verify` | submit find | Woody timeout 10 s; payload below |
| POST | `woodyminer.com/api/stat/upload` | Woody telemetry | optional, 3 s |

Payload: `{hash_to_verify, key, account, attempts, hashes_per_second, worker}` — **no timestamp
field exists**; server stores only `(hash_to_verify, key, account)` with DB-default timestamps.
`attempts`/`hashes_per_second`/`worker` are effectively decorative server-side.

Dead endpoints (referenced in code, not functional or never called): `xenminer.mooo.com:4445/getblocks/lastblock`,
`xenblocks.io:4446/send_pow` (Woody includes the header, never calls it; `PowSubmitter` dead code).

**Response semantics (the submission contract):**

| Response | Meaning | Correct client action |
|---|---|---|
| 200 | accepted | ack |
| 400 `"Block already exists, continue"` | duplicate `key` (UNIQUE constraint) | **ack** — idempotent replay is safe |
| 401 difficulty message | `m=` below current difficulty | **park** — auto-valid again when difficulty falls to ≤ m |
| 401 XUNI window message | XUNI outside :55–:05 (server clock) | dead |
| 5xx / timeout / empty / conn refused | server sick or down | keep pending; retry forever with backoff |

## 2. Discrepancy resolutions

**"Outside of time window" (doc 01 vs doc 02) — RESOLVED, twice over:**
1. All time-window rejection strings live exclusively in XUNI code paths (`gpage.py:433-435, 497`).
2. Sharper (synthesis agent, verified): Woody matches the literal substring `"outside of time window"`
   (`main.cpp:415`), which the server emits **only** at `gpage.py:497` — an `else` branch reachable
   only when the hash contains no `XEN11`. The actual XUNI rejection at `gpage.py:434` says
   `"XUNI Submitted outside of proper time frame."`, which doesn't even match Woody's substring.
   **A XEN11 submission can never receive that response.** Queued XEN11 resubmission is
   unconstrained except by the difficulty floor.

**Endpoint domains:** `xenblocks.io` vs `xenminer.mooo.com` vs `{rpcLink}` are the same logical
central service reached via different hostnames/eras; miners take an `rpcLink` config. Treat the
submission URL as configurable, never hardcoded.

## 3. Licensing (verified in-repo)

- **XenblocksMiner (Woody)**: README declares **MIT** (`README.md:211-213`); no LICENSE file in the
  repo. MIT permits our fork/refactor/commercial use with attribution. Low risk; consider asking
  upstream to add the LICENSE file, and carry the MIT notice + attribution in our fork.
- **xenminer (jacklevin74)**: **no license anywhere** → default all-rights-reserved. We may read it
  to understand the protocol (facts aren't copyrightable) but must **not copy its code**.
- **xgpu (Jozef)**: no license; same rule. (Moot — it's deployment scripts around a vanished repo.)

**Consequence:** the fork-Woody path (Phase 1) is license-clean. Any code derived from xenminer is not.

## 4. Outage-resilience architecture

See **doc 06 Part A** (authoritative): journal-first WAL SQLite (`synchronous=FULL`), closure-queue →
durable-record refactor of `BlockSubmitter`, response-driven state machine
(`pending/acked/parked/dead`), circuit breaker + `/difficulty` probing during outages, throttled
oldest-first drain on recovery, startup recovery pass, XEN11-before-XUNI priority with XUNI expiry,
`difficulty_margin` config knob (~5 min tolerance per +1000 KiB against rising difficulty; outages
usually push difficulty *down*, which helps).

Contrast with the field today: reference miner **crashes** on outage (unguarded POST, no timeout);
Woody loses every find within ~10 s (empty catch, 5×2 s retries, truncating write-only log);
XENGPUMiner deletes find-files unconditionally after any submit attempt; xenvast's queue isn't
crash-safe. Nobody has a durable journal — this is the differentiator.

## 5. Hashrate architecture

See **doc 06 Parts B–C** (authoritative): adopt Woody's proven core (warp-per-hash, PTX G,
oneshot kernel, GPU first-blocks, strided last-block D2H, VRAM-derived batching); pursue the
measured headroom — multi-warp-per-block occupancy, precomputed indexed-half refs + `cp.async`
prefetch, device-side finalize, stream double-buffering, per-arch autotune. The kernel is
ALU-bound on consumer cards at ~40% of the bandwidth ceiling; realistic upside +25–60% over
parity, not the rumored 2×.

## 6. Open questions & risks

1. **X1 migration risk (highest strategic):** X1 mainnet reportedly live since Oct 2025. If
   centralized `/verify` submission is replaced by on-chain/validator submission, the submitter
   module changes — the journal design survives (it's transport-agnostic), the HTTP client may not.
   Monitor docs.x1.xyz; keep the submitter behind an interface.
2. **Server drift:** our conclusions come from the cloned `xenminer` snapshot; the deployed server
   may differ. Mitigation: the state machine treats every response empirically (unknown 4xx →
   log + park, never silent-drop) and we should probe tolerance with real delayed submissions early.
3. **Token/branding inconsistencies** (XNM/XN/XNT, multiple "X1" projects) — flagged unconfirmed in
   doc 04; irrelevant to miner mechanics.
4. **No license file in Woody's repo** — MIT is declared in README only; low but nonzero ambiguity.

## 7. Roadmap

- **Phase 1 — Resilient fork (days):** fork XenblocksMiner; implement doc 06 Part A / §4 above.
  Zero kernel changes. Deliverable: a drop-in Woody replacement that never loses a find.
- **Phase 2 — Own engine (weeks):** MIT-clean rebuild around Woody's kernel design with
  device-side finalize + double-buffering built in; golden-hash test suite from day one.
- **Phase 3 — Beyond parity (ongoing):** occupancy/prefetch/autotune program from doc 06 B.3,
  run under ledger discipline (record every experiment, keep the rejected ones).
