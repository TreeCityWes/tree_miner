# TreeMiner Rust crates

Incremental host-side rewrite. CUDA stays C++ (`kernelrunner.cu`); C++ still owns the GPU
until the Hash API FFI crate exists. Each crate is replaceable behind the current miner.

| Crate | Status | C++ source of truth |
|---|---|---|
| `treeminer-protocol` | done | `ResponseClassifier`, `PhcAssembler`, `MarginPolicy`, `xuniWindowAt` |
| `treeminer-journal` | done | `FindJournal` + `FallbackSink` (WAL + `synchronous=FULL`) |
| `treeminer-submit` | done | `SubmissionManager` / breaker / drain (Tokio one worker) |
| `treeminer-hash` (FFI) | next | Hash API `hash-batch`; kernel still `nvcc` |
| orchestrator | last | replace `main.cpp` |

```sh
cargo test --manifest-path treeminer/rust/Cargo.toml
```

No GPU. Do not wrap Crow, cpr, Boost.ProgramOptions, or the TUI through FFI — replace those
in later crates.
