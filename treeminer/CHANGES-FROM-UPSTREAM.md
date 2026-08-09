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
- **Immutable payload capture** (`src/treeminer/PhcAssembler.h`, `MiningCommon.h`,
  `MineUnit.cpp`, `main.cpp`): the PHC string is assembled once at discovery from the batch's
  actual `memory_cost` (now passed through `SubmitCallback`); both CPU Argon2 re-hashes are
  deleted. Fixes upstream's stale-difficulty silent drop (`main.cpp:371-381`) and removes
  ~2 wasted CPU Argon2id runs per find. Salt encoding rule validated against the documented
  legacy salt value.
- **Collision-safe keygen** (`RandomHexKeyGenerator.h`): `mt19937_64` seeded from 256 bits of
  OS entropy + clock + per-instance counter, replacing the single-32-bit-seed `mt19937`.
- (planned) Strip/disable MQTT, marketplace, and telemetry paths in the Phase 1 default binary.
