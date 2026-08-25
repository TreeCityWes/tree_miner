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
| `treeminer-orchestrator` | done | config + journal-first capture + drain + hash CLI (`treeminer` bin) |

```sh
cargo test --manifest-path treeminer/rust/Cargo.toml
cargo run --manifest-path treeminer/rust/Cargo.toml -p treeminer-orchestrator -- hash-help
```

No GPU for crate tests. Cargo tests for `treeminer-hash` / the orchestrator hash CLI link a
C stub of the same ABI; production `src/hashapi/treeminer_hash.cpp` dispatches to
`CpuHashBackend` / `CudaHashBackend` (kernel stays `kernelrunner.cu`).
