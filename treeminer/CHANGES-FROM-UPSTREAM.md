# Changes from upstream woodysoil/XenblocksMiner

TreeMiner is a fork of woodysoil/XenblocksMiner (MIT, declared in upstream README §License).
Protocol knowledge additionally derives from reading jacklevin74/xenminer (unlicensed — **no code
copied from it, ever**) and ops lessons from JozefJarosciak/xgpu (no code copied).

Kernel policy (Phase 1): `src/kernelrunner.cu` and all hashing-path files carry **zero diffs**.

## Change log

- Added `sqlite3` to vcpkg.json (journal dependency).
- Added `src/treeminer/Types.h` — shared journal/submitter contract types.
- `src/journal/` — durable WAL-mode SQLite FindJournal (journal-first invariant); 11-test
  unit suite.
- `src/submit/` — ResponseClassifier, CircuitBreaker, DrainScheduler, SubmissionManager,
  HttpTransport replacing the upstream in-RAM closure queue; 4 unit suites (200+ checks)
  plus a gpage.py-faithful mock server with fault injection (`tests/mockserver/`).
- **Journal-first integration** (`main.cpp`, `DifficultyManager.*`, `MineUnit.cpp`,
  root `CMakeLists.txt`): submit callback now journals the find durably and wakes the
  SubmissionManager; the upstream `BlockSubmitter` worker thread is no longer started
  (its retry loop dropped finds after 5 tries / 10 no-responses). Block counters count
  journaled finds. Difficulty poller feeds `SubmissionManager::observeDifficulty` via
  `globalDifficultyObserver`. `MineUnit` no longer drops XUNI matches when the local
  clock says the window closed mid-batch — the journal parks them instead. Journal db:
  `treeminer-journal.db` in the working directory.
- **Immutable payload capture** (`src/treeminer/PhcAssembler.h`, `MiningCommon.h`,
  `MineUnit.cpp`, `main.cpp`): the PHC string is assembled once at discovery from the batch's
  actual `memory_cost` (now passed through `SubmitCallback`); both CPU Argon2 re-hashes are
  deleted. Fixes upstream's stale-difficulty silent drop (`main.cpp:371-381`) and removes
  ~2 wasted CPU Argon2id runs per find. Salt encoding rule validated against the documented
  legacy salt value.
- **Collision-safe keygen** (`RandomHexKeyGenerator.h`): `mt19937_64` seeded from 256 bits of
  OS entropy + clock + per-instance counter, replacing the single-32-bit-seed `mt19937`.
- **VRAM-pool release on difficulty change** (`ComputeBackend.h`, `CudaBackend.*`,
  `MineUnit.cpp`, `main.cpp`): on a difficulty drop the retained kernel pool starved the
  free-memory estimate, producing a `batch=0` → "Not enough memory" hot spin (observed live:
  46M log lines, mining idle). The backend now releases its buffers and re-measures before
  giving up, and `runMineLoop`'s non-fatal exit backs off 5 s instead of spinning.
- **Persistent difficulty cache** (`DifficultyManager.*`, `main.cpp`): every successful
  difficulty sample is written to `difficulty.cache` (write-then-rename) and seeds
  `globalDifficulty` at startup. Upstream boots at the hardcoded 42069 fallback until the
  server answers — during a server outage that meant mining indefinitely at ~50x the real
  difficulty (observed live: 1.9 kH/s effective vs 92 kH/s at the true difficulty of 1100).
- (planned) Strip/disable MQTT, marketplace, and telemetry paths in the Phase 1 default binary.
