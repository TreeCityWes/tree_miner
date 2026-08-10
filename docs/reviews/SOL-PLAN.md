# TreeMiner — SOL Review Plan

**Status:** REVIEW PROPOSAL — separate from `PLAN.md`; no implementation is authorized by this file.

This document records additional submission-resilience and hashrate recommendations found while
reviewing the committed TreeMiner plan against the current Woody miner, the reference verification
server, and Woody's optimization experiment ledger. It is intentionally separate so the main plan
can be updated only after review.

## 1. Recommended priority order

1. Preserve the exact find produced by a GPU batch and eliminate the current difficulty race.
2. Make server acceptance verifiable rather than equating every HTTP 200 with durable storage.
3. Implement the durable journal and recovery state machine.
4. Correct XUNI scheduling and server-clock handling.
5. Isolate submission behind a transport interface and add endpoint diagnostics.
6. Establish correctness and accepted-yield benchmarks before new CUDA experiments.
7. Pursue only profiler-supported kernel changes that differ materially from rejected upstream
   experiments.

## 2. Immediate submission bug: preserve the original batch parameters

The current Woody submission callback reconstructs the full Argon2 hash later, using the latest
`globalDifficulty`. If difficulty changes after the GPU found a matching digest but before the
submission task executes, the reconstructed hash differs. The local comparison fails and the find
is silently discarded before any HTTP request.

### Required change

At match discovery, construct an immutable `FoundPayload` from the parameters actually used by
that batch:

```cpp
struct FoundPayload {
    std::string key;
    std::string hash_to_verify; // complete PHC string, constructed once
    std::string account;
    std::string kind;           // XEN11 or XUNI
    std::uint32_t memory_cost;
    std::string worker;
    std::uint64_t attempts;
    double hashes_per_second;
    std::string found_at_utc;
};
```

The complete PHC string must be assembled from the original batch's algorithm, version, `m`, `t`,
`p`, salt, and finalized Base64 digest. Do not recompute Argon2 in the submission thread. Journal
the immutable payload before exposing it to any submission, marketplace, telemetry, or dev-fee
consumer.

### Acceptance test

Pause the submitter after a match, change network difficulty, then resume it. The submitted
`hash_to_verify` must remain byte-identical to the discovery-time value and pass CPU verification.

## 3. Verifiable acceptance instead of `200 == acked`

The reviewed reference server can exhaust its database-lock retries without inserting a record,
then still fall through to a success response. Therefore HTTP 200 is evidence of a successful
request, but not conclusive evidence of durable server storage.

### Preferred server-side fix

- Return `503 Service Unavailable` when insertion retries are exhausted.
- Commit the insert before generating the success response.
- Return a structured receipt such as `{status, kind, key, block_id}`.
- Return a structured duplicate response, preferably `409 Conflict`, for the unique key.
- Retain or add an idempotent lookup endpoint keyed by `key`.

### Client-side compatibility path

When the server cannot be changed, use the existing lookup capability (`GET /get_block?key=...`)
to confirm a 200 response. Because actual finds are rare, one confirmation read per find is an
acceptable reliability trade.

Recommended state flow:

```text
pending -> submitting -> accepted_unconfirmed -> acked
                  |                |
                  |                +-> pending (lookup says absent/transient failure)
                  +-> parked_difficulty
                  +-> expired_xuni
                  +-> quarantined
                  +-> permanently_invalid
```

If lookup is unsupported on a deployed server, retain `accepted_unconfirmed` with explicit metrics
and configurable recheck policy. Do not silently present it as a confirmed acknowledgement.

## 4. Journal schema and durability

Use SQLite WAL with `synchronous=FULL` by default. Finds are sufficiently rare that maximum commit
durability is preferable to optimizing journal write throughput.

```sql
CREATE TABLE finds (
  id                    INTEGER PRIMARY KEY,
  key                   TEXT NOT NULL UNIQUE,
  hash_to_verify        TEXT NOT NULL,
  account               TEXT NOT NULL,
  kind                  TEXT NOT NULL CHECK(kind IN ('XEN11','XUNI')),
  m                     INTEGER NOT NULL,
  worker                TEXT,
  attempts              INTEGER,
  hashes_per_second     REAL,
  found_at              TEXT NOT NULL,
  local_window_end      TEXT,
  status                TEXT NOT NULL,
  status_reason         TEXT,
  attempt_count         INTEGER NOT NULL DEFAULT 0,
  next_attempt_at       TEXT,
  last_attempt_at       TEXT,
  last_http_status      INTEGER,
  last_response         TEXT,
  confirmed_at          TEXT
);

CREATE INDEX idx_finds_ready
  ON finds(status, next_attempt_at, kind, id);
```

Additional requirements:

- Set a finite SQLite `busy_timeout` and report lock failures visibly.
- Use UTC wall time for records and monotonic time for in-process retry delays.
- Keep pending and difficulty-parked XEN11 records indefinitely unless an operator explicitly
  archives them.
- Apply retention only to acknowledged, expired, or permanently invalid rows.
- Use one journal per miner process with a stable, unique path. Do not put a shared journal on NFS.
- Checkpoint WAL during quiet periods and on orderly shutdown, but do not require a checkpoint for
  a find to be considered locally durable.

## 5. Response classification

Do not use a single generic `parked` state for unrelated failures.

| Condition | State/action |
|---|---|
| Transport error, timeout, empty body, 408, 425, 5xx | `pending`, exponential backoff with jitter |
| 429 | `pending`, honor `Retry-After` |
| 200 | `accepted_unconfirmed`, then receipt/lookup confirmation |
| Exact known duplicate response | Confirm by lookup, then `acked` |
| Difficulty-too-low response | `parked_difficulty` until current difficulty is `<= m` |
| Explicit XUNI-window rejection | `expired_xuni` |
| Malformed payload or failed Argon2 verification | `permanently_invalid` plus high-severity log |
| Unknown 4xx or unknown response schema | `quarantined`; never auto-unpark as difficulty changes |

Parse structured JSON first. Substring matching should be a compatibility fallback covered by
golden response tests.

## 6. Circuit breaker and recovery drain

The difficulty endpoint and verification endpoint must not share a single notion of health. A
successful `/difficulty` response proves connectivity, not that `/verify` and its database work.

- Open the verification breaker after a configurable number of transport/5xx failures.
- In half-open state, allow one controlled `/verify` attempt from the durable queue.
- Close the breaker only after a verification-path success or conclusive duplicate receipt.
- Begin a recovery drain at 1–4 submissions/second.
- Increase gradually while latency and responses remain healthy; reduce on 429, 503, or rising
  latency.
- Keep per-record backoff so a poison/quarantined record cannot block the queue.
- Persist the next eligible attempt time so rapid restarts do not reset every backoff.

## 7. XUNI scheduling and clock authority

XEN11 is durable while XUNI is perishable. During an active XUNI window, scheduling should favor
XUNI records approaching their deadline rather than always draining XEN11 first.

Recommended ordering:

1. Eligible XUNI near the locally estimated window end.
2. Oldest eligible XEN11.
3. Other eligible XUNI.
4. Confirmation and quarantine probes.

The server clock is authoritative. Treat the local `local_window_end` as a scheduling hint, not a
reason for permanent deletion. Use the HTTP `Date` header or a future explicit server-time field to
estimate skew. Mark XUNI expired only after an explicit server-window rejection, with a bounded
grace policy if the service remains unreachable.

While eligible XUNI exists, cap breaker probes at roughly 2–5 seconds so a 60-second outage
backoff cannot consume the remainder of the submission window.

## 8. Endpoint and transport resilience

The legacy default endpoint currently lacks a reliable HTTPS `/difficulty` path in a simple probe,
and official ecosystem documentation describes eventual X1-ledger/decentralized verification.
Make the transport boundary part of Phase 1.

```cpp
class ISubmissionTransport {
public:
    virtual SubmitResult submit(const FoundPayload&) = 0;
    virtual ConfirmationResult confirm(std::string_view key) = 0;
    virtual DifficultyResult difficulty() = 0;
};
```

Add startup diagnostics that report DNS, TCP/TLS, HTTP status, response schema, server clock skew,
and per-endpoint latency without logging wallet-private or machine-private data. Support an ordered
list of operator-configured endpoints, but do not automatically submit a find to an untrusted
endpoint discovered through unsigned remote data.

## 9. Effective-hashrate correctness: globally unique keys

The current miner creates a new `std::mt19937` from a single 32-bit seed for each generated batch.
At fleet scale, colliding seeds can reproduce complete key sequences, wasting GPU work and risking
server-side duplicate-key rejection.

Replace this with a persistent per-device key sequence based on:

- an OS-generated 128-bit or 256-bit boot nonce;
- a stable device/process discriminator; and
- a monotonically increasing 64-bit or 128-bit counter.

Derive the final 64-hex key with a fast cryptographic hash or counter-based generator while
preserving required dev-fee/platform prefixes. Add cross-device, multi-process, and restart tests
that generate millions of keys without duplication. This improves effective accepted work even if
the displayed raw H/s is unchanged.

## 10. Hashrate optimization program

### 10.1 Re-rank the existing candidates

The exact four-warps-per-block experiment has already been rejected upstream: it raised register
use and did not beat trusted d4096/d8192 baselines. Do not retain a projected `+20–50%` benefit for
that shape. Two- or three-warp, architecture-specific variants are new experiments only if Nsight
data shows a credible occupancy benefit without the same register-pressure failure.

Device-side finalization has also produced output-buffer lifetime/synchronization failures in an
upstream experiment. A retry must use a materially different ownership model: a fixed-capacity
sparse-hit buffer that remains alive through stream completion, explicit overflow handling, and
repeated process-teardown stress tests.

### 10.2 Best remaining candidates

1. **Precomputed indexed-half reference table.** Generate and golden-test the deterministic
   reference sequence once per difficulty. Measure instruction savings and register changes before
   attempting prefetch.
2. **Indexed-half asynchronous prefetch on sm80+.** With known references, test a shared-memory
   ping-pong pipeline using `cp.async`. Keep an architecture fallback and measure shared-memory
   occupancy, memory stalls, and ALU utilization.
3. **Sparse device-side finalize and matching.** Finalize and search on-device, copying only hits,
   using the redesigned lifetime model above.
4. **Two-stream overlap.** Compare one VRAM-maximized batch with two smaller batches whose setup,
   kernel, finalize, and transfers overlap. Reserve memory explicitly; do not assume two streams
   improve throughput.
5. **Per-architecture and per-difficulty autotuning.** Cache results by GPU UUID, driver, build,
   compute capability, and difficulty band. Revalidate when any key changes.
6. **Latency-aware batch tuning.** Include difficulty reaction time and XUNI window timing in the
   objective, not only peak hashes/second.

### 10.3 Adaptive difficulty margin

`difficulty_margin` trades raw hashrate for reduced rejection risk; it is not free. Replace a
static recommendation with an optional adaptive policy based on:

- age and freshness of the last difficulty observation;
- recent direction and magnitude of difficulty changes;
- expected time until the next adjustment;
- measured H/s loss per additional 1000 KiB; and
- value of parked/recovered finds.

Report both raw H/s and eligible/accepted H/s so a larger margin cannot appear beneficial merely by
moving rejected work out of sight.

## 11. Measurement and acceptance gates

Every performance experiment must pass, in order:

1. CPU/GPU golden hashes across representative difficulties.
2. Exact found-payload reconstruction and CPU verification.
3. CUDA process startup/teardown stress with zero invalid exits.
4. Same-device, same-build, alternating A/B runs with warm-up and stability gates.
5. Realistic high-difficulty and variable-difficulty workloads.
6. Raw H/s, eligible H/s, accepted H/s, power, memory, register, occupancy, and latency reporting.
7. Multi-GPU confirmation before becoming a default.

Record rejected experiments with the implementation shape and measured reason so future work does
not repeat them without a new hypothesis.

## 12. Expanded test matrix

Add these cases to the main plan's tests:

- Difficulty changes after discovery but before journal insertion/submission.
- Server returns 200 after intentionally skipping its insert; client must not mark confirmed.
- Response is lost after a successful insert; replay resolves through duplicate plus lookup.
- Unknown 4xx remains quarantined through subsequent difficulty decreases.
- `/difficulty` is healthy while `/verify` returns 503 or its database is locked.
- Recovery occurs during the final seconds of an XUNI window; XUNI outranks XEN11.
- Local clock is ahead/behind server time across an XUNI boundary.
- Process dies between journal commit and HTTP, during HTTP, and between HTTP response and local
  acknowledgement.
- Two miner processes use separate journals and recover independently.
- Millions of generated keys across GPUs/processes contain no duplicates.
- WAL grows during a long outage and checkpoints safely after recovery.

## 13. Recommendations on the original review questions

1. **SQLite FULL vs NORMAL:** use WAL + `synchronous=FULL` by default; allow NORMAL only as an
   explicit operator durability tradeoff.
2. **Parked retention:** do not expire difficulty-parked XEN11 automatically. Archive rather than
   delete; prune final states by policy.
3. **Journal topology:** one journal per process with a stable unique path. Prefer a supervisor over
   multiple processes sharing one SQLite file, especially on rented or networked storage.
4. **Telemetry/marketplace:** compile off by default or defer from Phase 1. If retained, route every
   find through journal-first persistence before notifying those subsystems.
5. **Drain rate:** 4/s is a reasonable ceiling for an initial release, but begin lower and adapt to
   server feedback. Respect `Retry-After` and give time-sensitive XUNI separate urgency.

## 14. Product-positioning note

SQLite block storage, retry, and parking until difficulty becomes eligible are already advertised
by another active XenBlocks miner. TreeMiner should not claim those mechanisms alone as unique.
Stronger differentiators are:

- exact immutable payload capture at discovery;
- confirmed rather than assumed server acknowledgement;
- crash-safe state transitions around every network ambiguity;
- server-clock-aware XUNI recovery;
- transparent accepted-yield metrics; and
- transport readiness for a future X1 submission path.

## 15. Proposed incorporation into the main plan

Before implementation, amend the main plan only after review to:

- add immutable discovery-time payload construction as Phase 1 step zero;
- replace `200 -> acked` with confirmation-aware states;
- split difficulty parking from unknown-response quarantine;
- reverse XUNI/XEN11 priority near an active deadline;
- demote the rejected four-warp estimate;
- document the prior device-finalization failure mode;
- add persistent globally unique key generation; and
- make transport isolation a Phase 1 deliverable.

