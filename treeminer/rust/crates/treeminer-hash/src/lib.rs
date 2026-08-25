//! Hash API `hash-batch` FFI.
//!
//! Port of the host-side Hash API around `IHashBackend::runBatch`. The CUDA kernel stays
//! in `kernelrunner.cu` and is reached through `CudaHashBackend` via
//! `src/hashapi/treeminer_hash.cpp` when built with `--features cuda` (CMake target
//! `treeminer_hash`). Default Cargo tests link a C stub (`native/stub.c`). No Crow, cpr,
//! Boost, or TUI.

pub mod ffi;
pub mod matching;
pub mod stub;
pub mod types;
pub mod validate;

pub use ffi::{hash_batch, FfiBackend};
pub use matching::{append_matches, has_xuni_match, is_superblock_hash};
pub use stub::{make_key, StubBackend};
pub use types::{
    HashBackend, HashMatch, HashRequest, HashResult, HashTimings, DEFAULT_HASH_LENGTH,
    HASH_API_KEY_LENGTH, MAX_CPU_BATCH_SIZE, MAX_TARGET_PATTERN_LENGTH, MIN_ARGON2_CPU_DIFFICULTY,
};
pub use validate::{is_hex_string, is_valid_request, join_errors, normalize_hex, validate_request};
