# Changes from upstream woodysoil/XenblocksMiner

TreeMiner is a fork of woodysoil/XenblocksMiner (MIT, declared in upstream README §License).
Protocol knowledge additionally derives from reading jacklevin74/xenminer (unlicensed — **no code
copied from it, ever**) and ops lessons from JozefJarosciak/xgpu (no code copied).

Kernel policy (Phase 1): `src/kernelrunner.cu` and all hashing-path files carry **zero diffs**.

## Change log

- Added `sqlite3` to vcpkg.json (journal dependency).
- Added `src/treeminer/Types.h` — shared journal/submitter contract types.
- (in progress) `src/journal/` — durable WAL-mode SQLite FindJournal (journal-first invariant).
- (in progress) `src/submit/` — ResponseClassifier, CircuitBreaker, SubmissionManager replacing
  the upstream in-RAM closure queue (`BlockSubmitter`).
- (planned) Immutable `FoundPayload` capture at discovery — fixes upstream stale-difficulty
  silent drop (`main.cpp:371-381`) and removes the double CPU re-verify (`main.cpp:364-378`).
- (planned) Persistent collision-safe key generation — replaces 32-bit-seeded `mt19937`
  (`RandomHexKeyGenerator.h:15-17`).
- (planned) Strip/disable MQTT, marketplace, and telemetry paths in the Phase 1 default binary.
