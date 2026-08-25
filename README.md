# TreeMiner

An outage-proof GPU miner for [XenBlocks](https://xenblocks.io) (X1 Network), forked from
[woodysoil/XenblocksMiner](https://github.com/woodysoil/XenblocksMiner) (MIT).

**The problem it solves:** the XenBlocks central server goes down several times a day.
Existing miners keep found blocks in RAM and drop them after a few failed retries — every
outage permanently loses real finds. TreeMiner journals every find to durable storage
*before* the first network attempt and drains the journal with outage-aware retry.

## How it works

```
GPU find -> immutable PHC capture -> SQLite WAL journal (fsync'd)  ->  SubmissionManager
                                          |                              |  circuit breaker
                                     survives crashes,              adaptive drain, backoff,
                                     restarts, outages              /get_block confirmation
```

- **Journal-first invariant** — `append()` returns only after the find is crash-safe on
  disk (`src/journal/FindJournal.cpp`, WAL + `synchronous=FULL`).
- **9-state find lifecycle** — including `ParkedDifficulty` (401-difficulty finds
  auto-resubmit when difficulty allows) and `ParkedXuniWindow` (XUNI retry across
  future :55–:05 windows). Nothing is ever silently dropped.
- **Lying-200 detection** — the reference server can return 200 without storing the block;
  every accept is re-verified via `GET /get_block` before being counted.
- **Circuit breaker + adaptive drain** — no hammering a dead server; queued finds drain at
  a controlled, escalating rate on recovery.
- **Fixes inherited upstream bugs** — stale-difficulty silent find drops, weak 32-bit
  keygen seeding, VRAM-pool starvation spin on difficulty drops.

Validated end-to-end: unit suites (15 CTest targets), chaos harness against a
`gpage.py`-faithful mock server with fault injection (60 s hard outage → 13/13 finds
recovered and acked), and a live canary against the real server (first real XEN11 +
XUNI blocks accepted and `/get_block`-confirmed on day one).

## Layout

| Path | Contents |
|---|---|
| `treeminer/` | The miner (fork of XenblocksMiner + TreeMiner components). See `treeminer/PLAN.md` (design authority) and `treeminer/CHANGES-FROM-UPSTREAM.md` (divergence log) |
| `treeminer/rust/` | Incremental Rust host crates (protocol, journal, submit). CUDA stays C++. See `treeminer/rust/README.md` |
| `treeminer/src/journal/` | Durable SQLite find journal |
| `treeminer/src/submit/` | Classifier, circuit breaker, drain scheduler, submission manager |
| `treeminer/tests/` | Unit suites + mock XenBlocks server with fault injection |
| `docs/` | Research docs `01`–`08` plus `09-ops-stability.md` (this-box crash/reset record) |
| `docs/reviews/` | Verbatim review docs from other models (Kimmy, Grok, Sol) |
| `research/` | Experiment notes and validation records |

## Building (Linux / WSL2)

Requires CUDA toolkit (12.x), CMake ≥ 3.18, Ninja, and [vcpkg](https://github.com/microsoft/vcpkg)
(dependencies are declared in `treeminer/vcpkg.json`; a repo-local overlay port enables the
secp256k1 recovery module).

```sh
cmake -S treeminer -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
ctest --test-dir build          # 15 suites
./build/bin/xenblocksMiner --execute --minerAddr 0xYourAddress --totalDevFee 0
```

## License

MIT (`treeminer/LICENSE`), preserving woodysoil/XenblocksMiner attribution. The reference
server repo (jacklevin74/xenminer) is unlicensed: it was read for protocol semantics only
and **no code from it is included**.
