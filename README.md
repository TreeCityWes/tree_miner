# TreeMiner

**A durability-first GPU miner for [XenBlocks](https://xenblocks.io) (X1 Network).**
Forked from [woodysoil/XenblocksMiner](https://github.com/woodysoil/XenblocksMiner) (MIT).

![HashHead Console](docs/img/hashhead-console.png)

Mining networks live in the real world: connections drop, endpoints restart, outages
happen. Most miners hold found blocks in RAM and give up after a few failed retries —
so every network interruption silently costs real, already-mined work. TreeMiner treats
every find as money in hand: it is journaled to durable storage *before* the first
network attempt, and an outage-aware submission pipeline delivers it when the network
allows. Nothing is ever silently dropped.

## Highlights

- **Journal-first durability** — a find is crash-safe on disk (SQLite WAL,
  `synchronous=FULL`) before any network I/O is attempted. Power loss, crashes, and
  restarts cannot lose work.
- **Outage-aware delivery** — a circuit breaker stops submissions the moment the server
  is unreachable, probes gently for recovery, then drains the queued backlog at a
  controlled, adaptive rate.
- **Verified acceptance** — every accepted submission is independently confirmed via
  `GET /get_block` before it is counted. An acceptance isn't trusted until the record
  is provably stored.
- **Full find lifecycle** — nine explicit journal states cover parked difficulty,
  XUNI submission windows, quarantine, and confirmation, with automatic re-eligibility
  when conditions change.
- **Live operations console** — the built-in HashHead web console (screenshot above)
  serves real-time telemetry per CUDA stream, delivery-channel state, and an exportable
  runtime profile on `http://<rig>:42069`.
- **Hardened compute path** — per-stream VRAM budgeting, difficulty-change-safe batch
  rebuilds, and CUDA 13 support across dual-GPU rigs.

## How it works

```
GPU find -> immutable PHC capture -> SQLite WAL journal (fsync'd)  ->  SubmissionManager
                                          |                              |  circuit breaker
                                     survives crashes,              adaptive drain, backoff,
                                     restarts, outages              /get_block confirmation
```

Validated end-to-end: 27 CTest suites, a chaos harness against a protocol-faithful mock
server with fault injection (60 s hard outage → 13/13 finds recovered and acknowledged),
and live production mining with server-confirmed XEN11 and XUNI blocks.

## Layout

| Path | Contents |
|---|---|
| `treeminer/` | The miner (fork of XenblocksMiner + TreeMiner components). See `treeminer/PLAN.md` (design authority) and `treeminer/CHANGES-FROM-UPSTREAM.md` (divergence log) |
| `treeminer/rust/` | Incremental Rust host crates (protocol, journal, submit, hash FFI, orchestrator). CUDA stays C++. See `treeminer/rust/README.md` |
| `treeminer/src/journal/` | Durable SQLite find journal |
| `treeminer/src/submit/` | Response classifier, circuit breaker, drain scheduler, submission manager |
| `treeminer/web/` | HashHead operations console (React) |
| `treeminer/tests/` | Unit suites + mock server with fault injection |
| `docs/` | Research and design documentation |

## Building (Linux / WSL2)

Requires CMake ≥ 3.18, Ninja, [vcpkg](https://github.com/microsoft/vcpkg) (dependencies are
declared in `treeminer/vcpkg.json`; a repo-local overlay port enables the secp256k1 recovery
module), and a GPU toolchain: **CUDA toolkit 12.x for NVIDIA** (default) or **ROCm with HIP
for AMD** (CMake ≥ 3.21).

```sh
cmake -S treeminer -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
ctest --test-dir build
./build/bin/xenblocksMiner --execute --minerAddr 0xYourAddress --totalDevFee 0
```

For AMD cards, add `-DTREEMINER_GPU_BACKEND=HIP` to the configure line. Both vendors share
one kernel source; see `treeminer/doc/BUILD_INSTRUCTIONS.md` for the gfx targets, the
ROCm SMI telemetry dependency, and what differs from the NVIDIA build. `nix develop` (see
`flake.nix`) gives a ready ROCm toolchain and all dependencies without vcpkg.

The AMD path is validated on an RX 7900 XTX (gfx1100, ROCm 7.2): digests match the CPU
Argon2 reference, all 27 CTest suites pass, and the miner sustains ~5.2 kH/s at
difficulty 42069.

The console is served on port `42069` by default (`--dashboard-port` to change,
`--dashboard-bind 127.0.0.1` to keep it private to the machine).

## License

MIT (`LICENSE`, mirrored at `treeminer/LICENSE`), preserving woodysoil/XenblocksMiner
attribution. The reference server repo (jacklevin74/xenminer) is unlicensed: it was read
for protocol semantics only and **no code from it is included**.
