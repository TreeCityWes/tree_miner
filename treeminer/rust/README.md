# TreeMiner Rust crates

Incremental host-side rewrite. CUDA stays C++ (`kernelrunner.cu`); C++ still owns the GPU
mine loop (`xenblocksMiner`). Each crate is replaceable behind the current miner. Do not
wrap Crow, cpr, Boost.ProgramOptions, or the TUI through FFI.

| Crate | Status | C++ source of truth |
|---|---|---|
| `treeminer-protocol` | done | `ResponseClassifier`, `PhcAssembler`, `MarginPolicy`, `xuniWindowAt` |
| `treeminer-journal` | done | `FindJournal` + `FallbackSink` (WAL + `synchronous=FULL`) |
| `treeminer-submit` | done | `SubmissionManager` / breaker / drain (Tokio one worker) |
| `treeminer-hash` (FFI) | done | C ABI around Hash API `hash-batch`; kernel still `nvcc` |
| `treeminer-orchestrator` | done | config + journal-first capture + `mine` loop + drain + hash CLI (`treeminer` bin) |

```sh
cargo test --manifest-path treeminer/rust/Cargo.toml
cargo run --manifest-path treeminer/rust/Cargo.toml -p treeminer-orchestrator -- hash-help
cargo run --manifest-path treeminer/rust/Cargo.toml -p treeminer-orchestrator -- mine --help
```

`treeminer mine` is the journal-first host loop (hash-batch → capture → drain). Cargo tests
drive it with the hash stub and `--donotupload`.

## CUDA canary (does not change xenblocksMiner)

Default `cargo test` links `native/stub.c`. Production hashing is CMake target
`treeminer_hash` (`treeminer_hash.cpp` → `CudaHashBackend` → `kernelrunner.cu`), **not**
linked into the `xenblocksMiner` executable.

```sh
cmake -S treeminer -B build -DTREEMINER_BUILD_HASH_FFI=ON
cmake --build build --target treeminer_hash
set -a && source build/treeminer-hash-cuda.env && set +a
# canary journal — never the live runtime-live/ file
cargo run --manifest-path treeminer/rust/Cargo.toml -p treeminer-orchestrator \
  --features cuda -- mine --backend cuda --donotupload \
  --journalPath /tmp/treeminer-canary.db --minerAddr 0xYourAddress --steps 1
```
