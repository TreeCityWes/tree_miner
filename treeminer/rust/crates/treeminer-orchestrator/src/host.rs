//! Journal-first host: open + fallback import + recover, then capture.
//! Port of `main.cpp` journal pipeline (~643–849, capture ~873–1018). No TUI, no Crow.

use std::path::{Path, PathBuf};

use treeminer_journal::{
    FallbackSink, FindJournal, FindJournalApi, ImportStats, JournalError, RecoveryStats,
};
use treeminer_protocol::{assemble_phc, FindKind, FoundPayload, HexError};
use treeminer_submit::{iso_utc, SubmissionConfig, SubmissionManager, Transport};

#[derive(Clone, Debug, PartialEq)]
pub struct CaptureInput {
    pub hexsalt: String,
    pub key: String,
    pub hashed_pure: String,
    pub memory_cost: u32,
    pub attempts: u64,
    pub hashes_per_second: f64,
    pub worker: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CaptureOutcome {
    Journaled { id: i64 },
    Fallback,
    Fatal { reason: String },
}

impl CaptureOutcome {
    pub fn durably_captured(&self) -> bool {
        matches!(self, Self::Journaled { .. } | Self::Fallback)
    }

    pub fn is_fatal(&self) -> bool {
        matches!(self, Self::Fatal { .. })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CaptureError {
    BadSalt(HexError),
}

impl std::fmt::Display for CaptureError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::BadSalt(err) => write!(f, "{err}"),
        }
    }
}

impl std::error::Error for CaptureError {}

/// C++ `globalJournalPath + ".fallback.jsonl"`.
pub fn fallback_path_for(journal_path: impl AsRef<Path>) -> PathBuf {
    let path = journal_path.as_ref();
    let mut os = path.as_os_str().to_os_string();
    os.push(".fallback.jsonl");
    PathBuf::from(os)
}

/// Immutable PHC capture from the batch the hasher actually used. Never re-hash.
pub fn build_payload(input: &CaptureInput, now_ms: i64) -> Result<FoundPayload, CaptureError> {
    let hash_to_verify = assemble_phc(input.memory_cost, &input.hexsalt, &input.hashed_pure)
        .map_err(CaptureError::BadSalt)?;
    let kind = if input.hashed_pure.contains("XEN11") {
        FindKind::Xen11
    } else {
        FindKind::Xuni
    };
    Ok(FoundPayload {
        key: input.key.clone(),
        hash_to_verify,
        account: format!("0x{}", input.hexsalt),
        kind,
        memory_cost: input.memory_cost,
        worker: input.worker.clone(),
        attempts: input.attempts,
        hashes_per_second: input.hashes_per_second,
        found_at_utc: iso_utc(now_ms),
    })
}

/// Journal append first. On SQLite failure, JSONL sink. Both fail → fatal halt.
pub fn persist_find(
    journal: &impl FindJournalApi,
    fallback: &FallbackSink,
    payload: &FoundPayload,
) -> CaptureOutcome {
    match journal.append(payload) {
        Ok(id) => CaptureOutcome::Journaled { id },
        Err(err) => {
            if fallback.append(payload) {
                CaptureOutcome::Fallback
            } else {
                CaptureOutcome::Fatal {
                    reason: format!("journal append and fallback sink both failed: {err}"),
                }
            }
        }
    }
}

pub struct Host {
    pub journal: FindJournal,
    pub fallback: FallbackSink,
    pub journal_path: PathBuf,
    pub abs_path: PathBuf,
    pub import: ImportStats,
    pub recovery: RecoveryStats,
}

impl Host {
    /// Open the journal, drain the fallback sink, then `recover_on_startup` — that order.
    pub fn open(journal_path: impl AsRef<Path>) -> Result<Self, JournalError> {
        let journal_path = journal_path.as_ref().to_path_buf();
        let fallback_path = fallback_path_for(&journal_path);
        let journal = FindJournal::open(&journal_path)?;
        let import = FallbackSink::import_into(&journal, &fallback_path);
        let recovery = journal.recover_on_startup()?;
        let fallback = FallbackSink::new(fallback_path);
        let abs_path = std::fs::canonicalize(&journal_path).unwrap_or_else(|_| {
            std::env::current_dir()
                .map(|cwd| cwd.join(&journal_path))
                .unwrap_or_else(|_| journal_path.clone())
        });
        Ok(Self {
            journal,
            fallback,
            journal_path,
            abs_path,
            import,
            recovery,
        })
    }

    pub fn capture(
        &self,
        input: CaptureInput,
        now_ms: i64,
    ) -> Result<CaptureOutcome, CaptureError> {
        let payload = build_payload(&input, now_ms)?;
        Ok(persist_find(&self.journal, &self.fallback, &payload))
    }

    pub fn make_manager<T: Transport>(
        self,
        transport: T,
        margin: treeminer_protocol::MarginConfig,
    ) -> SubmissionManager<FindJournal, T> {
        let mut cfg = SubmissionConfig::default();
        cfg.margin = margin;
        SubmissionManager::with_config(self.journal, transport, cfg, None, None)
    }
}

pub fn format_recovery(path: &Path, import: &ImportStats, rec: &RecoveryStats) -> String {
    let mut out = format!("JOURNAL path={}\n", path.display());
    if import.file_present {
        out.push_str(&format!(
            "JOURNAL fallback sink drained | imported={} | malformed={}\n",
            import.imported, import.malformed
        ));
    }
    out.push_str(&format!(
        "JOURNAL recovered | pending={} | unconfirmed={} | parked={} | acked={} | quarantined={}\n",
        rec.pending,
        rec.accepted_unconfirmed,
        rec.parked_difficulty + rec.parked_xuni,
        rec.acked,
        rec.quarantined
    ));
    out
}
