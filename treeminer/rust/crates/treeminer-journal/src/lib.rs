//! Durable find journal: SQLite WAL + `synchronous=FULL`, plus a JSONL fallback sink.
//!
//! Port of `src/journal/FindJournal.{h,cpp}` and `src/journal/FallbackSink.{h,cpp}`.
//! No GPU. The C++ miner is unchanged; this crate is additive.

mod error;
mod fallback;
mod journal;
mod jsonl;

pub use error::JournalError;
pub use fallback::{FallbackSink, ImportStats};
pub use journal::{Counts, FindJournal, RecoveryStats};

use treeminer_protocol::{Classification, FindKind, FindRecord, FoundPayload};

/// Abstract journal contract (`IFindJournal`). The submitter talks to this, not SQLite.
pub trait FindJournalApi {
    fn append(&self, payload: &FoundPayload) -> Result<i64, JournalError>;
    fn fetch_eligible(&self, now_utc: &str, limit: usize) -> Result<Vec<FindRecord>, JournalError>;
    fn fetch_eligible_of_kind(
        &self,
        kind: FindKind,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError>;
    fn fetch_awaiting_confirmation(
        &self,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError>;
    fn get_by_id(&self, id: i64) -> Result<Option<FindRecord>, JournalError>;
    fn record_attempt(
        &self,
        id: i64,
        classification: &Classification,
        http_status: Option<i32>,
        response_body: &str,
        next_attempt_at: Option<&str>,
        now_utc: &str,
    ) -> Result<(), JournalError>;
    fn unpark_for_difficulty(&self, current_difficulty: u32) -> Result<usize, JournalError>;
    fn unpark_xuni_for_window(&self, max_windows: i32) -> Result<usize, JournalError>;
    fn recover_on_startup(&self) -> Result<RecoveryStats, JournalError>;
    fn record_difficulty(&self, difficulty: u32, at_utc: &str) -> Result<(), JournalError>;
    fn last_known_difficulty(&self) -> Result<Option<u32>, JournalError>;
    fn counts(&self) -> Result<Counts, JournalError>;
}

impl FindJournalApi for FindJournal {
    fn append(&self, payload: &FoundPayload) -> Result<i64, JournalError> {
        FindJournal::append(self, payload)
    }
    fn fetch_eligible(&self, now_utc: &str, limit: usize) -> Result<Vec<FindRecord>, JournalError> {
        FindJournal::fetch_eligible(self, now_utc, limit)
    }
    fn fetch_eligible_of_kind(
        &self,
        kind: FindKind,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        FindJournal::fetch_eligible_of_kind(self, kind, now_utc, limit)
    }
    fn fetch_awaiting_confirmation(
        &self,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        FindJournal::fetch_awaiting_confirmation(self, now_utc, limit)
    }
    fn get_by_id(&self, id: i64) -> Result<Option<FindRecord>, JournalError> {
        FindJournal::get_by_id(self, id)
    }
    fn record_attempt(
        &self,
        id: i64,
        classification: &Classification,
        http_status: Option<i32>,
        response_body: &str,
        next_attempt_at: Option<&str>,
        now_utc: &str,
    ) -> Result<(), JournalError> {
        FindJournal::record_attempt(
            self,
            id,
            classification,
            http_status,
            response_body,
            next_attempt_at,
            now_utc,
        )
    }
    fn unpark_for_difficulty(&self, current_difficulty: u32) -> Result<usize, JournalError> {
        FindJournal::unpark_for_difficulty(self, current_difficulty)
    }
    fn unpark_xuni_for_window(&self, max_windows: i32) -> Result<usize, JournalError> {
        FindJournal::unpark_xuni_for_window(self, max_windows)
    }
    fn recover_on_startup(&self) -> Result<RecoveryStats, JournalError> {
        FindJournal::recover_on_startup(self)
    }
    fn record_difficulty(&self, difficulty: u32, at_utc: &str) -> Result<(), JournalError> {
        FindJournal::record_difficulty(self, difficulty, at_utc)
    }
    fn last_known_difficulty(&self) -> Result<Option<u32>, JournalError> {
        FindJournal::last_known_difficulty(self)
    }
    fn counts(&self) -> Result<Counts, JournalError> {
        FindJournal::counts(self)
    }
}

#[cfg(test)]
mod fallback_tests;
#[cfg(test)]
mod journal_tests;

#[cfg(test)]
mod test_support {
    use super::*;
    use treeminer_protocol::{FindKind, FindStatus, FoundPayload};

    pub const NOW: &str = "2026-08-09T12:34:56Z";

    pub fn make_payload(key_suffix: &str, kind: FindKind, memory_cost: u32) -> FoundPayload {
        let suffix = if key_suffix.len() >= 2 {
            key_suffix.to_string()
        } else {
            format!("0{key_suffix}")
        };
        FoundPayload {
            key: format!("aabbccddeeff00112233445566778899aabbccddeeff001122334455667788{suffix}"),
            hash_to_verify: format!(
                "$argon2id$v=19$m={memory_cost},t=1,p=1$c29tZXNhbHRzb21lc2FsdDE5$\
                 TFVYRU4xMWFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6QUJDREVGRw"
            ),
            account: "0x1234567890abcdef1234567890abcdef12345678".to_string(),
            kind,
            memory_cost,
            worker: "rig0-gpu0".to_string(),
            attempts: 123456,
            hashes_per_second: 1500.5,
            found_at_utc: "2026-08-09T12:00:00Z".to_string(),
        }
    }

    pub fn classify(status: FindStatus, reason: &str) -> Classification {
        Classification {
            next_status: status,
            server_difficulty_hint: None,
            needs_lookup_confirmation: false,
            reason: reason.to_string(),
        }
    }
}
