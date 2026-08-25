//! Pure XenBlocks protocol types for TreeMiner.
//!
//! No I/O, no clock, no GPU. C++ still owns hashing and the process until later crates
//! replace journal, submit, and `main.cpp`. This crate is a 1:1 port of:
//! - `src/treeminer/Types.{h,cpp}`
//! - `src/submit/ResponseClassifier.{h,cpp}`
//! - `src/treeminer/PhcAssembler.h` + unpadded PHC base64 from `src/hashapi/HashApiEncoding.cpp`
//! - `src/treeminer/MarginPolicy.h` + `src/submit/MarginPolicy.cpp`
//! - `SubmissionManager::xuniWindowAt` (`src/submit/SubmissionManager.cpp`)

pub mod classifier;
pub mod margin;
pub mod phc;
pub mod types;
pub mod xuni;

pub use classifier::{
    classify, classify_with_retry_after, extract_json_field, extract_json_message,
    parse_difficulty_hint, parse_retry_after_seconds, TRANSPORT_ERROR,
};
pub use margin::{compute_margin, parse_margin_mode, MarginConfig, MarginInputs, MarginMode};
pub use phc::{assemble_phc, hex_to_bytes, phc_base64_encode, HexError};
pub use types::{
    Classification, FindKind, FindRecord, FindStatus, FoundPayload, FIND_STATUS_TERMINAL,
};
pub use xuni::{
    xuni_window_at, XuniWindowState, XUNI_OPEN_AFTER_HOUR_MS, XUNI_OPEN_BEFORE_HOUR_MS,
};
