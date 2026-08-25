use std::path::Path;
use std::sync::Mutex;
use std::time::Duration;

use rusqlite::{params, Connection, OptionalExtension, TransactionBehavior};
use treeminer_protocol::{Classification, FindKind, FindRecord, FindStatus, FoundPayload};

use crate::error::JournalError;

pub const MAX_HASH_TO_VERIFY_LENGTH: usize = 150;
const SUPPORTED_SCHEMA_VERSION: i64 = 1;
const BUSY_TIMEOUT_MS: u64 = 5000;

const RECORD_COLUMNS: &str = "id, key, hash_to_verify, account, kind, m, worker, attempts, \
     hashes_per_second, found_at, status, status_reason, attempt_count, next_attempt_at, \
     last_attempt_at, last_http_status, last_response, confirmed_at, xuni_windows_tried";

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RecoveryStats {
    pub pending: usize,
    pub accepted_unconfirmed: usize,
    pub parked_difficulty: usize,
    pub parked_xuni: usize,
    pub quarantined: usize,
    pub acked: usize,
    pub dead: usize,
    pub invalid: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub pending: usize,
    pub parked: usize,
    pub parked_difficulty: usize,
    pub parked_xuni: usize,
    pub quarantined: usize,
    pub acked_total: usize,
    pub dead_total: usize,
    pub accepted_unconfirmed: usize,
    pub permanently_invalid: usize,
    pub queued_xen11: usize,
    pub queued_xuni: usize,
}

pub struct FindJournal {
    conn: Mutex<Connection>,
}

impl FindJournal {
    pub fn open(db_path: impl AsRef<Path>) -> Result<Self, JournalError> {
        let path = db_path.as_ref();
        let mut conn = Connection::open(path).map_err(|err| {
            JournalError::new(format!("journal: open '{}': {err}", path.display()))
        })?;
        conn.busy_timeout(Duration::from_millis(BUSY_TIMEOUT_MS))?;
        apply_pragmas(&conn)?;
        ensure_schema(&mut conn)?;
        Ok(Self {
            conn: Mutex::new(conn),
        })
    }

    fn lock(&self) -> Result<std::sync::MutexGuard<'_, Connection>, JournalError> {
        self.conn
            .lock()
            .map_err(|_| JournalError::new("journal: mutex poisoned"))
    }

    /// Durable on return (WAL + synchronous=FULL). Duplicate key returns the existing id.
    pub fn append(&self, payload: &FoundPayload) -> Result<i64, JournalError> {
        let mut conn = self.lock()?;
        let mut status = FindStatus::Pending;
        let mut reason = String::new();
        if payload.hash_to_verify.len() > MAX_HASH_TO_VERIFY_LENGTH {
            status = FindStatus::PermanentlyInvalid;
            reason = format!(
                "hash_to_verify length {} exceeds server limit of {MAX_HASH_TO_VERIFY_LENGTH}",
                payload.hash_to_verify.len()
            );
        } else if payload.account.is_empty() {
            status = FindStatus::PermanentlyInvalid;
            reason = "account is empty".to_string();
        }

        let tx = conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        tx.execute(
            "INSERT INTO finds (key, hash_to_verify, account, kind, m, worker, attempts,
                                hashes_per_second, found_at, status, status_reason)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)
             ON CONFLICT(key) DO NOTHING;",
            params![
                payload.key,
                payload.hash_to_verify,
                payload.account,
                payload.kind.as_str(),
                payload.memory_cost as i64,
                null_if_empty(&payload.worker),
                payload.attempts as i64,
                payload.hashes_per_second,
                payload.found_at_utc,
                status.as_str(),
                null_if_empty(&reason),
            ],
        )?;
        let id = if tx.changes() == 0 {
            tx.query_row(
                "SELECT id FROM finds WHERE key = ?1;",
                params![payload.key],
                |row| row.get(0),
            )
            .map_err(|_| {
                JournalError::new("journal: append hit UNIQUE conflict but key not found")
            })?
        } else {
            tx.last_insert_rowid()
        };
        tx.commit()?;
        Ok(id)
    }

    pub fn fetch_eligible(
        &self,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        let conn = self.lock()?;
        fetch_by_status(&conn, "Pending", now_utc, limit, None)
    }

    pub fn fetch_eligible_of_kind(
        &self,
        kind: FindKind,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        let conn = self.lock()?;
        fetch_by_status(&conn, "Pending", now_utc, limit, Some(kind.as_str()))
    }

    pub fn fetch_awaiting_confirmation(
        &self,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        let conn = self.lock()?;
        fetch_by_status(&conn, "AcceptedUnconfirmed", now_utc, limit, None)
    }

    pub fn get_by_id(&self, id: i64) -> Result<Option<FindRecord>, JournalError> {
        let conn = self.lock()?;
        let sql = format!("SELECT {RECORD_COLUMNS} FROM finds WHERE id = ?1;");
        conn.query_row(&sql, params![id], row_to_record)
            .optional()
            .map_err(JournalError::from)
    }

    pub fn record_attempt(
        &self,
        id: i64,
        classification: &Classification,
        http_status: Option<i32>,
        response_body: &str,
        next_attempt_at: Option<&str>,
        now_utc: &str,
    ) -> Result<(), JournalError> {
        if classification.next_status == FindStatus::Submitting {
            return Err(JournalError::new(
                "journal: recordAttempt with status Submitting — that state is in-process \
                 only and must never be persisted",
            ));
        }
        let conn = self.lock()?;
        let n = conn.execute(
            "UPDATE finds SET
               status = ?1,
               status_reason = ?2,
               attempt_count = attempt_count + 1,
               last_attempt_at = ?3,
               last_http_status = ?4,
               last_response = ?5,
               next_attempt_at = ?6,
               confirmed_at = CASE WHEN ?1 = 'Acked'
                                   THEN COALESCE(confirmed_at, ?3) ELSE confirmed_at END
             WHERE id = ?7;",
            params![
                classification.next_status.as_str(),
                null_if_empty(&classification.reason),
                now_utc,
                http_status,
                response_body,
                next_attempt_at,
                id,
            ],
        )?;
        if n == 0 {
            return Err(JournalError::new(format!(
                "journal: recordAttempt for unknown find id {id}"
            )));
        }
        Ok(())
    }

    pub fn unpark_for_difficulty(&self, current_difficulty: u32) -> Result<usize, JournalError> {
        let conn = self.lock()?;
        let n = conn.execute(
            "UPDATE finds SET status = 'Pending', next_attempt_at = NULL
             WHERE status = 'ParkedDifficulty' AND m >= ?1;",
            params![current_difficulty as i64],
        )?;
        Ok(n)
    }

    pub fn unpark_xuni_for_window(&self, max_windows: i32) -> Result<usize, JournalError> {
        let mut conn = self.lock()?;
        let tx = conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        tx.execute(
            "UPDATE finds SET status = 'Dead',
               status_reason = 'xuni window budget exhausted',
               next_attempt_at = NULL
             WHERE status = 'ParkedXuniWindow' AND xuni_windows_tried >= ?1;",
            params![max_windows],
        )?;
        let unparked = tx.execute(
            "UPDATE finds SET status = 'Pending',
               xuni_windows_tried = xuni_windows_tried + 1,
               next_attempt_at = NULL
             WHERE status = 'ParkedXuniWindow';",
            [],
        )?;
        tx.commit()?;
        Ok(unparked)
    }

    pub fn recover_on_startup(&self) -> Result<RecoveryStats, JournalError> {
        let mut conn = self.lock()?;
        let tx = conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        tx.execute(
            "UPDATE finds SET status = 'Pending' WHERE status = 'Submitting';",
            [],
        )?;
        let mut stats = RecoveryStats::default();
        {
            let mut stmt = tx.prepare("SELECT status, COUNT(*) FROM finds GROUP BY status;")?;
            let rows = stmt.query_map([], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
            })?;
            for row in rows {
                let (status_text, count) = row?;
                let count = count as usize;
                match status_from_db(&status_text)? {
                    FindStatus::Pending => stats.pending += count,
                    FindStatus::AcceptedUnconfirmed => stats.accepted_unconfirmed += count,
                    FindStatus::ParkedDifficulty => stats.parked_difficulty += count,
                    FindStatus::ParkedXuniWindow => stats.parked_xuni += count,
                    FindStatus::Quarantined => stats.quarantined += count,
                    FindStatus::Acked => stats.acked += count,
                    FindStatus::Dead => stats.dead += count,
                    FindStatus::PermanentlyInvalid => stats.invalid += count,
                    FindStatus::Submitting => {}
                }
            }
        }
        tx.commit()?;
        Ok(stats)
    }

    pub fn record_difficulty(&self, difficulty: u32, at_utc: &str) -> Result<(), JournalError> {
        let conn = self.lock()?;
        conn.execute(
            "INSERT INTO difficulty_seen (at, value) VALUES (?1, ?2);",
            params![at_utc, difficulty as i64],
        )?;
        Ok(())
    }

    pub fn last_known_difficulty(&self) -> Result<Option<u32>, JournalError> {
        let conn = self.lock()?;
        let value: Option<i64> = conn
            .query_row(
                "SELECT value FROM difficulty_seen ORDER BY rowid DESC LIMIT 1;",
                [],
                |row| row.get(0),
            )
            .optional()?;
        Ok(value.map(|v| v as u32))
    }

    pub fn counts(&self) -> Result<Counts, JournalError> {
        let conn = self.lock()?;
        let mut result = Counts::default();
        let mut stmt =
            conn.prepare("SELECT status, kind, COUNT(*) FROM finds GROUP BY status, kind;")?;
        let rows = stmt.query_map([], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, i64>(2)?,
            ))
        })?;
        for row in rows {
            let (status_text, kind_text, count) = row?;
            let status = status_from_db(&status_text)?;
            let kind = kind_from_db(&kind_text)?;
            let count = count as usize;
            match status {
                FindStatus::Pending => result.pending += count,
                FindStatus::ParkedDifficulty => {
                    result.parked += count;
                    result.parked_difficulty += count;
                }
                FindStatus::ParkedXuniWindow => {
                    result.parked += count;
                    result.parked_xuni += count;
                }
                FindStatus::Quarantined => result.quarantined += count,
                FindStatus::Acked => result.acked_total += count,
                FindStatus::Dead => result.dead_total += count,
                FindStatus::AcceptedUnconfirmed => result.accepted_unconfirmed += count,
                FindStatus::PermanentlyInvalid => result.permanently_invalid += count,
                FindStatus::Submitting => {}
            }
            if matches!(
                status,
                FindStatus::Pending | FindStatus::AcceptedUnconfirmed
            ) {
                match kind {
                    FindKind::Xen11 => result.queued_xen11 += count,
                    FindKind::Xuni => result.queued_xuni += count,
                }
            }
        }
        Ok(result)
    }
}

impl Drop for FindJournal {
    fn drop(&mut self) {
        if let Ok(conn) = self.conn.lock() {
            let _ = conn.execute_batch("PRAGMA wal_checkpoint(PASSIVE);");
        }
    }
}

fn null_if_empty(s: &str) -> Option<&str> {
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}

fn apply_pragmas(conn: &Connection) -> Result<(), JournalError> {
    conn.pragma_update(None, "journal_mode", "WAL")?;
    let mode: String = conn.query_row("PRAGMA journal_mode;", [], |row| row.get(0))?;
    if mode.to_ascii_lowercase() != "wal" {
        return Err(JournalError::new(
            "journal: could not enable WAL journal mode at this path",
        ));
    }
    conn.execute_batch("PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;")?;
    Ok(())
}

fn ensure_schema(conn: &mut Connection) -> Result<(), JournalError> {
    let tx = conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
    tx.execute_batch(
        "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
         CREATE TABLE IF NOT EXISTS finds (
           id                 INTEGER PRIMARY KEY,
           key                TEXT NOT NULL UNIQUE,
           hash_to_verify     TEXT NOT NULL,
           account            TEXT NOT NULL,
           kind               TEXT NOT NULL CHECK(kind IN ('XEN11','XUNI')),
           m                  INTEGER NOT NULL,
           worker             TEXT,
           attempts           INTEGER,
           hashes_per_second  REAL,
           found_at           TEXT NOT NULL,
           status             TEXT NOT NULL,
           status_reason      TEXT,
           attempt_count      INTEGER NOT NULL DEFAULT 0,
           next_attempt_at    TEXT,
           last_attempt_at    TEXT,
           last_http_status   INTEGER,
           last_response      TEXT,
           confirmed_at       TEXT,
           xuni_windows_tried INTEGER NOT NULL DEFAULT 0
         );
         CREATE INDEX IF NOT EXISTS idx_finds_ready
           ON finds(status, next_attempt_at, kind, id);
         CREATE TABLE IF NOT EXISTS difficulty_seen (at TEXT, value INTEGER);",
    )?;
    let version: Option<i64> =
        tx.query_row("SELECT MAX(version) FROM schema_version;", [], |row| {
            row.get(0)
        })?;
    match version {
        None => {
            tx.execute(
                "INSERT INTO schema_version(version) VALUES (?1);",
                params![SUPPORTED_SCHEMA_VERSION],
            )?;
        }
        Some(v) if v == SUPPORTED_SCHEMA_VERSION => {}
        Some(v) => {
            return Err(JournalError::new(format!(
                "journal: unsupported schema_version {v} (supported: {SUPPORTED_SCHEMA_VERSION})"
            )));
        }
    }
    tx.commit()?;
    Ok(())
}

fn fetch_by_status(
    conn: &Connection,
    status: &str,
    now_utc: &str,
    limit: usize,
    kind: Option<&str>,
) -> Result<Vec<FindRecord>, JournalError> {
    let sql = format!(
        "SELECT {RECORD_COLUMNS} FROM finds
         WHERE status = ?1
           AND (?2 IS NULL OR kind = ?2)
           AND (next_attempt_at IS NULL OR next_attempt_at <= ?3)
         ORDER BY id ASC LIMIT ?4;"
    );
    let mut stmt = conn.prepare(&sql)?;
    let rows = stmt.query_map(params![status, kind, now_utc, limit as i64], row_to_record)?;
    let mut records = Vec::new();
    for row in rows {
        records.push(row?);
    }
    Ok(records)
}

fn conversion_err(column: usize, message: String) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(
        column,
        rusqlite::types::Type::Text,
        Box::new(JournalError::new(message)),
    )
}

fn row_to_record(row: &rusqlite::Row<'_>) -> rusqlite::Result<FindRecord> {
    let kind_text: String = row.get(4)?;
    let status_text: String = row.get(10)?;
    let worker: Option<String> = row.get(6)?;
    let reason: Option<String> = row.get(11)?;
    let last_response: Option<String> = row.get(16)?;
    let attempts: Option<i64> = row.get(7)?;
    let rate: Option<f64> = row.get(8)?;
    Ok(FindRecord {
        id: row.get(0)?,
        payload: FoundPayload {
            key: row.get(1)?,
            hash_to_verify: row.get(2)?,
            account: row.get(3)?,
            kind: FindKind::parse(&kind_text).ok_or_else(|| {
                conversion_err(
                    4,
                    format!("journal: unknown kind string in database: '{kind_text}'"),
                )
            })?,
            memory_cost: row.get::<_, i64>(5)? as u32,
            worker: worker.unwrap_or_default(),
            attempts: attempts.unwrap_or(0) as u64,
            hashes_per_second: rate.unwrap_or(0.0),
            found_at_utc: row.get(9)?,
        },
        status: FindStatus::parse(&status_text).ok_or_else(|| {
            conversion_err(
                10,
                format!("journal: unknown status string in database: '{status_text}'"),
            )
        })?,
        status_reason: reason.unwrap_or_default(),
        attempt_count: row.get::<_, i64>(12)? as i32,
        next_attempt_at: row.get(13)?,
        last_attempt_at: row.get(14)?,
        last_http_status: row.get(15)?,
        last_response: last_response.unwrap_or_default(),
        confirmed_at: row.get(17)?,
        xuni_windows_tried: row.get::<_, i64>(18)? as i32,
    })
}

fn status_from_db(s: &str) -> Result<FindStatus, JournalError> {
    FindStatus::parse(s).ok_or_else(|| {
        JournalError::new(format!("journal: unknown status string in database: '{s}'"))
    })
}

fn kind_from_db(s: &str) -> Result<FindKind, JournalError> {
    FindKind::parse(s).ok_or_else(|| {
        JournalError::new(format!("journal: unknown kind string in database: '{s}'"))
    })
}
