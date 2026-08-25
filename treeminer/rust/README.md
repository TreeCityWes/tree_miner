# TreeMiner Rust crates

Incremental host-side rewrite. CUDA stays C++ (`kernelrunner.cu`); C++ still owns the GPU
until the orchestrator crate replaces `main.cpp`. Each crate is replaceable behind the current miner.

| Crate | Status | C++ source of truth |
|---|---|---|
| `treeminer-protocol` | done | `ResponseClassifier`, `PhcAssembler`, `MarginPolicy`, `xuniWindowAt` |
| `treeminer-journal` | done | `FindJournal` + `FallbackSink` (WAL + `synchronous=FULL`) |
| `treeminer-submit` | done | `SubmissionManager` / breaker / drain (Tokio one worker) |
| `treeminer-hash` (FFI) | done | C ABI around Hash API `hash-batch`; kernel still `nvcc` |
| orchestrator | last | replace `main.cpp` |

```sh
cargo test --manifest-path treeminer/rust/Cargo.toml
```

No GPU for crate tests. Cargo tests for `treeminer-hash` link a C stub of the same ABI;
production `src/hashapi/treeminer_hash.cpp` dispatches to `CpuHashBackend` / `CudaHashBackend`
(kernel stays `kernelrunner.cu`). Do not wrap Crow, cpr, Boost.ProgramOptions, or the TUI
through FFI — replace those in the orchestrator crate.
