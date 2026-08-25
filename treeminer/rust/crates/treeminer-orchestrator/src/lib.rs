//! Rust host for TreeMiner: config, journal-first capture, drain, hash CLI.
//!
//! Replaces the *role* of `main.cpp` for persistence and Hash API commands. Does not wrap
//! Crow, cpr, Boost.ProgramOptions, or the TUI. The CUDA mine loop stays in
//! `kernelrunner.cu` / `xenblocksMiner`. This crate is additive; CMake is unchanged.

pub mod cli;
pub mod config;
pub mod hash_cli;
pub mod host;
pub mod http;

pub use cli::{run, Command, RunOutput};
pub use config::{load_config_txt, resolve_config, ConfigError, HostConfig};
pub use hash_cli::{format_hash_result, hash_request_from_flags, HASH_USAGE};
pub use host::{
    build_payload, fallback_path_for, persist_find, CaptureError, CaptureInput, CaptureOutcome,
    Host,
};
pub use http::{verify_json, HttpTransport};

#[cfg(test)]
mod tests;
