//! Submission layer: breaker, drain scheduler, and `SubmissionManager`.
//!
//! Port of `src/submit/{CircuitBreaker,DrainScheduler,SubmissionManager,ITransport}`.
//! No Crow, cpr, or TUI. Transport is a trait; HTTP comes later via reqwest, not cpr.
//! CUDA stays in C++. Tests drive `run_once` with injectable clocks.

pub mod breaker;
pub mod drain;
pub mod manager;
pub mod time;
pub mod transport;

pub use breaker::{BreakerState, CircuitBreaker, CircuitBreakerConfig};
pub use drain::{DifficultyTrend, DrainScheduler, DrainSchedulerConfig};
pub use manager::{
    ConfirmBodyCheck, DrainHandle, Metrics, StepResult, SubmissionConfig, SubmissionManager,
};
pub use time::{iso_utc, parse_http_date_ms};
pub use transport::{Transport, TransportResult};

#[cfg(test)]
mod manager_tests;
