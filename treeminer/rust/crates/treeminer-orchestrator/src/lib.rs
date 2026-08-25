//! Rust host for TreeMiner: config, journal-first capture, drain, hash CLI, mine loop.
//!
//! Replaces the *role* of `main.cpp` for persistence, Hash API commands, and the
//! journal-first mine loop. Does not wrap Crow, cpr, Boost.ProgramOptions, or the TUI.
//! CUDA stays in `kernelrunner.cu`. CMake / `xenblocksMiner` are unchanged.

pub mod cli;
pub mod config;
pub mod hash_cli;
pub mod host;
pub mod http;
pub mod mine;

pub use cli::{run, Command, RunOutput};
pub use config::{load_config_txt, resolve_config, ConfigError, HostConfig};
pub use hash_cli::{format_hash_result, hash_request_from_flags, HASH_USAGE};
pub use host::{
    build_payload, build_payload_from_hash, fallback_path_for, persist_find, CaptureError,
    CaptureInput, CaptureOutcome, Host,
};
pub use http::{verify_json, HttpTransport};
pub use mine::{
    account_hex, mine_step, parse_difficulty_body, DiscardTransport, MineParams, MineReport,
    FALLBACK_DIFFICULTY,
};

#[cfg(test)]
mod tests;
