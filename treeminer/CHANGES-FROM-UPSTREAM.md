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
- **Difficulty margin — mining with headroom** (`src/treeminer/MarginPolicy.h`,
  `src/submit/MarginPolicy.cpp`, `SubmissionManager.*`, `MiningCommon.*`, `MineUnit.cpp`,
  `main.cpp`, `StatReporter.cpp`): the server rejects only when the baked-in m is strictly
  below the difficulty *current at submit time*, so a find mined with headroom survives a
  difficulty rise that occurs while it sits in the journal. New `effectiveMiningDifficulty()`
  = `globalDifficulty + globalDifficultyMargin` is the single value the batch sizer, the
  kernel request, and the `m=` in the PHC string all read; a margin change breaks the mine
  loop so the unit rebuilds at the new cost. Three modes (`off` default / `fixed` / `auto`)
  via `difficulty_margin_mode`, `difficulty_margin`, `difficulty_margin_max` in `config.txt`
  or `--difficultyMargin*` flags. **Auto** costs nothing while healthy (breaker closed and
  journal drained) and ramps one step per 300 s of outage, capped — one step per server
  adjustment period, because `manage_difficulty2.py` raises difficulty by at most +1000 KiB
  per 300 s tick, making the ramp track worst-case rise exactly rather than by guesswork.
  Headroom is paid for in hashrate (m IS the Argon2 memory cost), which is why it is off by
  default and zero while healthy.
- **First-observation un-park** (`SubmissionManager::observeDifficulty`): un-parking was
  gated on a strict difficulty *decrease*, but a freshly started process has no previous
  observation to compare against — so records parked before a restart stayed parked even when
  already valid at the current floor, waiting for a later decrease that may never come while
  difficulty trends upward. The first observation of a process now un-parks too. Found by
  reading the code, then reproduced in `tests/chaos/`.
- **Stats: margin and accepted-yield** (`MiningCommon.h` `TreeminerStats`, `StatReporter.cpp`):
  `/stats` now reports `marginInEffect`, `effectiveDifficulty`, `marginMode`, `serverState`,
  `outageMs`, `drainRatePerSecond` and the journal state counts, plus `acceptedYieldRatio` /
  `acceptedYieldHashrate` beside raw hashrate — raw H/s flatters a miner that is parking or
  losing finds. Yield is computed over *resolved* finds only, so an in-progress drain does not
  read as loss, and is reported as null (not 0%) until something resolves. The provider is a
  `std::function` set by `main()`, so the stats path keeps no journal/submitter headers and
  never shares fate with the submitter thread.
- **Chaos gate: difficulty rise during outage** (`tests/chaos/`): PLAN §6 case 3 as an
  automated CTest target driving the real `FindJournal` (SQLite on disk) and the real
  `SubmissionManager` against a scripted `gpage.py`-faithful server, with injected clocks so a
  multi-hour outage runs in milliseconds. Covers: outage → finds durable and breaker open;
  +3000 rise → 401 → `ParkedDifficulty` (never dead, never quarantined); fall → un-park →
  drain → `/get_block`-confirmed `Acked`; the same run under auto margin, where the headroom
  find clears the risen floor outright; and parked finds surviving a process restart.
- **XUNI head-of-line blocking fix** (`IFindJournal::fetchEligibleOfKind`, `FindJournal.*`,
  `SubmissionManager::submitStep_`, `FakeJournal.h`): the drain used to take ONE mixed
  oldest-first `LIMIT fetch_limit` slice of Pending rows. XUNI is journaled Pending whenever
  found — including outside the :55–:05 window, when it is not submittable — so 16+ queued
  XUNI ahead of a XEN11 backlog produced a slice with nothing selectable: the drain reported
  Idle and submitted NO XEN11 until the next window (up to ~50 min) against a healthy server.
  Symmetrically, a XEN11 backlog deeper than the slice hid any window-closing XUNI from the
  preemption rule. The step now fetches per kind (XUNI only while the window is open) and
  lets DrainScheduler prioritize over what actually exists. Regression tests cover both
  starvation directions (`test_submission_manager.cpp`) plus the SQL filter semantics
  (`FindJournalTests.cpp: fetch_eligible_of_kind`).
- **Fallback sink — journal write failures no longer drop finds** (`src/journal/FallbackSink.*`,
  `main.cpp`, `tests/unit/journal/FallbackSinkTests.cpp`): the one remaining hole in the
  journal-first invariant was `FindJournal::append()` throwing (SQLITE_BUSY past the 5 s
  timeout, stale lock, WAL corruption, quota) — the find was logged and dropped. Now it falls
  into an append-only fsync'd JSONL sink (`<journal_path>.fallback.jsonl`, mode 0600, pure
  std, no new deps in the journal lib) whose failure domain is deliberately disjoint from
  SQLite's; the next boot drains the sink back into the journal before recovery (idempotent
  by UNIQUE key, malformed lines skipped and counted, file renamed to `.imported` after a
  clean pass, left in place for retry otherwise). Honest limit, stated in the header: the
  sink shares the disk with SQLite, so total-disk death still loses the find — both failures
  together now log at the loudest severity. 5-case unit suite drives the real FindJournal.
- **Configurable journal path** (`main.cpp`): PLAN §5's `journal_path` config key implemented —
  `journal_path` in `config.txt` or `--journalPath` on the command line; default unchanged
  (`treeminer-journal.db`, CWD-relative) for drop-in compatibility. Startup now logs the
  RESOLVED absolute journal path: a miner launched from an unexpected working directory
  (systemd `WorkingDirectory`, HiveOS wrappers) silently opened a fresh empty journal and
  stranded every previously queued find in the orphaned file — the log line turns that
  from a mystery into an instant diagnosis, and the config key removes the hazard.
- **LAN-reachable local console** (`LocalServer.*`, `main.cpp`): the read-only miner
  dashboard listens on `0.0.0.0:42069` by default so operators can open it from any
  device on their network (all routes are read-only stats; no keys or secrets served).
  `--dashboard-bind 127.0.0.1` / the `dashboard_bind` config key restore a private
  console. IP literals are validated at startup, IPv6 browser URLs are bracketed, and
  the startup message prints the actual reachable LAN URL, not the bind address.
- **Crash hardening (live segfaults Aug 12–13):** TUI and ConsoleLog no longer write
  `std::cout` unlocked (libc near-null deref under `--display terminal`); SIGINT/SIGTERM
  only flip `running` (Crow/`cv` from the handler was async-signal-unsafe); dashboard
  thread is joined on shutdown instead of detached; file logger uses `localtime_r`.
- **CUDA 13 toolchain + GPU first-blocks re-enabled** (`HashApiTypes.h`, `MineUnit.cpp`,
  build lane `build-sm86-cuda13`): the invalid-digest bug that forced GPU first-blocks OFF
  (commit `12e241c`) was an **nvcc 11.5 miscompilation**, not a kernel logic error — a
  line-by-line RFC trace found no UB, and the identical kernel built with CUDA 13.3
  matches the CPU reference on real sm_86 hardware and passes live server verification
  (XUNI accepted HTTP 200). `kGpuFirstBlocksEnabled = true` again; the startup CPU/CUDA
  self-test exercises this exact flag and refuses to mine on mismatch, so a regressive
  toolchain fails closed at launch instead of mining unsubmittable finds. Side effect:
  host CPU load roughly halves (first-block hashing returns to the GPU), which matters on
  this rig — sustained CPU load correlates with the Aug 13–14 host reset storm. The
  CUDA 13 lane is single-compiler (system g++-11 for host and device host-code), killing
  the nvcc-11.5 g++-10 split and the `XENBLOCKS_STATIC_LIBSTDCXX=OFF` escape hatch.
- **Operator deploy kit** (`deploy/`): systemd unit (journal-first flags, `Restart=always`,
  `--display logs` enforced, core dumps enabled), boot fallback supervisor, GPU
  persistence/power-limit unit, crash diagnosis + hardware-guard scripts, and an
  unattended-upgrades blacklist (`51-treeminer-no-auto-driver`) so NVIDIA driver/kernel
  updates are deliberate operator actions — an automatic 2 AM driver swap mid-mining
  (2026-08-11) preceded the reset storm and the NVML mismatch that broke `nvidia-smi`.
- (planned) Strip/disable MQTT, marketplace, and telemetry paths in the Phase 1 default binary.
- **Rust host protocol crate** (`treeminer/rust/crates/treeminer-protocol`): 1:1 port of
  `ResponseClassifier`, `PhcAssembler` (unpadded PHC base64), `MarginPolicy`, and
  `xuniWindowAt`. Zero GPU, `unsafe_code` forbidden. C++ miner is unchanged; later crates
  (Hash API FFI, orchestrator) replace remaining host code behind the current process.
- **Rust journal crate** (`treeminer/rust/crates/treeminer-journal`): 1:1 port of
  `FindJournal` + `FallbackSink`. Same schema, WAL + `synchronous=FULL`, journal-first
  COMMIT, JSONL fallback with fsync. 17 unit tests port the C++ journal suites. Not wired
  into `xenblocksMiner` yet.
- **Rust submit crate** (`treeminer/rust/crates/treeminer-submit`): 1:1 port of
  `CircuitBreaker`, `DrainScheduler`, and `SubmissionManager`. Transport is a trait (no
  cpr/Crow). Tests drive `run_once` with injectable clocks; a Tokio current-thread worker
  hosts the drain loop. Same pending/acked/parked/dead state machine, lying-200
  confirmation, and fatal journal-error boundary. Not wired into `xenblocksMiner` yet.
