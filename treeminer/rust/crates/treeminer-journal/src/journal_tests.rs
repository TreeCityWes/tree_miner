use std::fs;
use std::path::PathBuf;

use rusqlite::Connection;
use treeminer_protocol::{FindKind, FindStatus};

use crate::test_support::{classify, make_payload, NOW};
use crate::{FindJournal, JournalError};

struct TempDb {
    path: PathBuf,
}

impl TempDb {
    fn new(name: &str) -> Self {
        let path = std::env::temp_dir().join(format!("treeminer_rs_journal_{name}.db"));
        let tmp = Self { path };
        tmp.cleanup();
        tmp
    }
    fn cleanup(&self) {
        let _ = fs::remove_file(&self.path);
        let _ = fs::remove_file(path_with_suffix(&self.path, "-wal"));
        let _ = fs::remove_file(path_with_suffix(&self.path, "-shm"));
    }
}

impl Drop for TempDb {
    fn drop(&mut self) {
        self.cleanup();
    }
}

fn path_with_suffix(path: &PathBuf, suffix: &str) -> PathBuf {
    let mut s = path.as_os_str().to_os_string();
    s.push(suffix);
    PathBuf::from(s)
}

struct RawDb {
    conn: Connection,
}

impl RawDb {
    fn open(path: &PathBuf) -> Self {
        let conn = Connection::open(path).expect("RawDb open");
        conn.busy_timeout(std::time::Duration::from_millis(5000))
            .unwrap();
        Self { conn }
    }
    fn scalar_text(&self, sql: &str) -> Option<String> {
        self.conn.query_row(sql, [], |row| row.get(0)).ok()
    }
    fn scalar_int(&self, sql: &str) -> i64 {
        self.conn
            .query_row(sql, [], |row| row.get(0))
            .expect("scalar int")
    }
    fn exec(&self, sql: &str) {
        self.conn.execute_batch(sql).expect("exec");
    }
}

fn xen(suffix: &str) -> treeminer_protocol::FoundPayload {
    make_payload(suffix, FindKind::Xen11, 1727)
}

#[test]
fn append_durable_and_idempotent() {
    let tmp = TempDb::new("append_durable_and_idempotent");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let mut payload = xen("01");
    let id = journal.append(&payload).unwrap();
    assert!(id > 0);
    payload.worker = "different-worker".into();
    assert_eq!(journal.append(&payload).unwrap(), id);

    let raw = RawDb::open(&tmp.path);
    assert_eq!(raw.scalar_int("SELECT COUNT(*) FROM finds;"), 1);
    assert_eq!(
        raw.scalar_text("SELECT status FROM finds WHERE id=1;")
            .unwrap(),
        "Pending"
    );
    assert_eq!(
        raw.scalar_text("SELECT key FROM finds WHERE id=1;")
            .unwrap(),
        payload.key
    );
    assert_eq!(
        raw.scalar_text("SELECT worker FROM finds WHERE id=1;")
            .unwrap(),
        "rig0-gpu0"
    );
    assert_eq!(
        raw.scalar_text("SELECT kind FROM finds WHERE id=1;")
            .unwrap(),
        "XEN11"
    );
    assert_eq!(
        raw.scalar_text("PRAGMA journal_mode;")
            .unwrap()
            .to_ascii_lowercase(),
        "wal"
    );
    assert_eq!(raw.scalar_int("SELECT version FROM schema_version;"), 1);
}

#[test]
fn invalid_payload_stored() {
    let tmp = TempDb::new("invalid_payload_stored");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let mut oversize = xen("02");
    oversize.hash_to_verify = "x".repeat(151);
    let id_oversize = journal.append(&oversize).unwrap();
    assert!(id_oversize > 0);
    let mut no_account = xen("03");
    no_account.account.clear();
    let id_no_account = journal.append(&no_account).unwrap();
    assert!(id_no_account > 0);
    assert_ne!(id_no_account, id_oversize);

    let raw = RawDb::open(&tmp.path);
    assert_eq!(raw.scalar_int("SELECT COUNT(*) FROM finds;"), 2);
    assert_eq!(
        raw.scalar_int(
            "SELECT COUNT(*) FROM finds WHERE status='PermanentlyInvalid' AND status_reason IS NOT NULL;"
        ),
        2
    );
    assert_eq!(
        raw.scalar_int(&format!(
            "SELECT LENGTH(hash_to_verify) FROM finds WHERE id={id_oversize};"
        )) as usize,
        oversize.hash_to_verify.len()
    );
    assert!(journal.fetch_eligible(NOW, 10).unwrap().is_empty());
}

#[test]
fn fetch_eligible_backoff_and_ordering() {
    let tmp = TempDb::new("fetch_eligible_backoff");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let id1 = journal.append(&xen("11")).unwrap();
    let id2 = journal.append(&xen("12")).unwrap();
    let id3 = journal.append(&xen("13")).unwrap();
    journal
        .record_attempt(
            id2,
            &classify(FindStatus::Pending, "transient 503"),
            Some(503),
            "server sick",
            Some("2026-08-09T13:00:00Z"),
            NOW,
        )
        .unwrap();

    let eligible = journal.fetch_eligible("2026-08-09T12:59:59Z", 10).unwrap();
    assert_eq!(eligible.len(), 2);
    assert_eq!(eligible[0].id, id1);
    assert_eq!(eligible[1].id, id3);

    let eligible = journal.fetch_eligible("2026-08-09T13:00:00Z", 10).unwrap();
    assert_eq!(eligible.len(), 3);
    assert_eq!(eligible[0].id, id1);
    assert_eq!(eligible[1].id, id2);
    assert_eq!(eligible[2].id, id3);

    let eligible = journal.fetch_eligible("2026-08-09T13:00:00Z", 1).unwrap();
    assert_eq!(eligible.len(), 1);
    assert_eq!(eligible[0].id, id1);
}

#[test]
fn record_attempt_round_trip() {
    let tmp = TempDb::new("record_attempt_round_trip");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let payload = xen("21");
    let id = journal.append(&payload).unwrap();
    journal
        .record_attempt(
            id,
            &classify(FindStatus::Pending, "connect timeout"),
            None,
            "",
            Some("2026-08-09T12:40:00Z"),
            NOW,
        )
        .unwrap();
    let eligible = journal.fetch_eligible("2026-08-09T12:40:00Z", 10).unwrap();
    assert_eq!(eligible.len(), 1);
    let record = &eligible[0];
    assert_eq!(record.id, id);
    assert_eq!(record.status, FindStatus::Pending);
    assert_eq!(record.status_reason, "connect timeout");
    assert_eq!(record.attempt_count, 1);
    assert_eq!(
        record.next_attempt_at.as_deref(),
        Some("2026-08-09T12:40:00Z")
    );
    assert_eq!(record.last_attempt_at.as_deref(), Some(NOW));
    assert!(record.last_http_status.is_none());
    assert!(record.confirmed_at.is_none());
    assert_eq!(record.payload.key, payload.key);
    assert_eq!(record.payload.hash_to_verify, payload.hash_to_verify);
    assert_eq!(record.payload.account, payload.account);
    assert_eq!(record.payload.kind, FindKind::Xen11);
    assert_eq!(record.payload.memory_cost, 1727);
    assert_eq!(record.payload.worker, payload.worker);
    assert_eq!(record.payload.attempts, payload.attempts);
    assert_eq!(record.payload.hashes_per_second, payload.hashes_per_second);
    assert_eq!(record.payload.found_at_utc, payload.found_at_utc);

    let ack_time = "2026-08-09T12:45:00Z";
    journal
        .record_attempt(
            id,
            &classify(FindStatus::Acked, "confirmed via get_block"),
            Some(200),
            "OK",
            None,
            ack_time,
        )
        .unwrap();
    let raw = RawDb::open(&tmp.path);
    assert_eq!(
        raw.scalar_text("SELECT status FROM finds WHERE id=1;")
            .unwrap(),
        "Acked"
    );
    assert_eq!(
        raw.scalar_text("SELECT confirmed_at FROM finds WHERE id=1;")
            .unwrap(),
        ack_time
    );
    assert_eq!(
        raw.scalar_int("SELECT attempt_count FROM finds WHERE id=1;"),
        2
    );
    assert_eq!(
        raw.scalar_int("SELECT last_http_status FROM finds WHERE id=1;"),
        200
    );
    assert_eq!(
        raw.scalar_text("SELECT last_response FROM finds WHERE id=1;")
            .unwrap(),
        "OK"
    );
    assert!(raw
        .scalar_text("SELECT next_attempt_at FROM finds WHERE id=1;")
        .is_none());

    journal
        .record_attempt(
            id,
            &classify(FindStatus::Acked, "re-ack"),
            Some(200),
            "OK",
            None,
            "2026-08-09T23:00:00Z",
        )
        .unwrap();
    assert_eq!(
        raw.scalar_text("SELECT confirmed_at FROM finds WHERE id=1;")
            .unwrap(),
        ack_time
    );

    let err = journal
        .record_attempt(
            id,
            &classify(FindStatus::Submitting, ""),
            None,
            "",
            None,
            NOW,
        )
        .unwrap_err();
    assert!(matches!(err, JournalError(_)));
    assert!(journal
        .record_attempt(
            9999,
            &classify(FindStatus::Pending, ""),
            None,
            "",
            None,
            NOW,
        )
        .is_err());
}

#[test]
fn unpark_for_difficulty_boundary() {
    let tmp = TempDb::new("unpark_for_difficulty");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let id_high = journal
        .append(&make_payload("31", FindKind::Xen11, 1500))
        .unwrap();
    let id_low = journal
        .append(&make_payload("32", FindKind::Xen11, 1400))
        .unwrap();
    journal
        .record_attempt(
            id_high,
            &classify(FindStatus::ParkedDifficulty, "difficulty"),
            Some(401),
            "difficulty too low",
            Some("2026-08-09T13:00:00Z"),
            NOW,
        )
        .unwrap();
    journal
        .record_attempt(
            id_low,
            &classify(FindStatus::ParkedDifficulty, "difficulty"),
            Some(401),
            "difficulty too low",
            Some("2026-08-09T13:00:00Z"),
            NOW,
        )
        .unwrap();
    assert_eq!(journal.unpark_for_difficulty(1500).unwrap(), 1);

    let raw = RawDb::open(&tmp.path);
    assert_eq!(
        raw.scalar_text(&format!("SELECT status FROM finds WHERE id={id_high};"))
            .unwrap(),
        "Pending"
    );
    assert_eq!(
        raw.scalar_text(&format!("SELECT status FROM finds WHERE id={id_low};"))
            .unwrap(),
        "ParkedDifficulty"
    );
    let eligible = journal.fetch_eligible(NOW, 10).unwrap();
    assert_eq!(eligible.len(), 1);
    assert_eq!(eligible[0].id, id_high);
    assert!(eligible[0].next_attempt_at.is_none());
    assert_eq!(journal.unpark_for_difficulty(1400).unwrap(), 1);
    assert_eq!(journal.unpark_for_difficulty(1400).unwrap(), 0);
}

#[test]
fn unpark_xuni_budget_exhaustion() {
    let tmp = TempDb::new("unpark_xuni_budget");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let id = journal
        .append(&make_payload("41", FindKind::Xuni, 1727))
        .unwrap();
    let park = || {
        journal
            .record_attempt(
                id,
                &classify(FindStatus::ParkedXuniWindow, "missed window"),
                Some(401),
                "XUNI Submitted outside of proper time frame.",
                None,
                NOW,
            )
            .unwrap();
    };
    let raw = RawDb::open(&tmp.path);
    for window in 1..=3 {
        park();
        assert_eq!(journal.unpark_xuni_for_window(3).unwrap(), 1);
        assert_eq!(
            raw.scalar_int("SELECT xuni_windows_tried FROM finds WHERE id=1;"),
            window
        );
        assert_eq!(
            raw.scalar_text("SELECT status FROM finds WHERE id=1;")
                .unwrap(),
            "Pending"
        );
    }
    park();
    assert_eq!(journal.unpark_xuni_for_window(3).unwrap(), 0);
    assert_eq!(
        raw.scalar_text("SELECT status FROM finds WHERE id=1;")
            .unwrap(),
        "Dead"
    );
    assert_eq!(
        raw.scalar_text("SELECT status_reason FROM finds WHERE id=1;")
            .unwrap(),
        "xuni window budget exhausted"
    );
    assert!(journal.fetch_eligible(NOW, 10).unwrap().is_empty());
}

#[test]
fn recover_on_startup_counts() {
    let tmp = TempDb::new("recover_on_startup");
    {
        let journal = FindJournal::open(&tmp.path).unwrap();
        journal.append(&xen("51")).unwrap();
        journal
            .append(&make_payload("52", FindKind::Xuni, 1727))
            .unwrap();
        let put = |suffix: &str, status: FindStatus| {
            let id = journal.append(&xen(suffix)).unwrap();
            journal
                .record_attempt(id, &classify(status, "test"), None, "", None, NOW)
                .unwrap();
        };
        put("53", FindStatus::AcceptedUnconfirmed);
        put("54", FindStatus::Acked);
        put("55", FindStatus::ParkedDifficulty);
        put("56", FindStatus::ParkedXuniWindow);
        put("57", FindStatus::Quarantined);
        put("58", FindStatus::Dead);
        let mut invalid = xen("59");
        invalid.account.clear();
        journal.append(&invalid).unwrap();
    }
    {
        let raw = RawDb::open(&tmp.path);
        raw.exec(
            "UPDATE finds SET status='Submitting', next_attempt_at='2026-08-09T11:00:00Z' WHERE id=1;",
        );
    }
    let journal = FindJournal::open(&tmp.path).unwrap();
    let stats = journal.recover_on_startup().unwrap();
    assert_eq!(stats.pending, 2);
    assert_eq!(stats.accepted_unconfirmed, 1);
    assert_eq!(stats.parked_difficulty, 1);
    assert_eq!(stats.parked_xuni, 1);
    assert_eq!(stats.quarantined, 1);
    assert_eq!(stats.acked, 1);
    assert_eq!(stats.dead, 1);
    assert_eq!(stats.invalid, 1);

    let raw = RawDb::open(&tmp.path);
    assert_eq!(
        raw.scalar_int("SELECT COUNT(*) FROM finds WHERE status='Submitting';"),
        0
    );
    assert_eq!(
        raw.scalar_text("SELECT next_attempt_at FROM finds WHERE id=1;")
            .unwrap(),
        "2026-08-09T11:00:00Z"
    );
    let counts = journal.counts().unwrap();
    assert_eq!(counts.pending, 2);
    assert_eq!(counts.parked, 2);
    assert_eq!(counts.parked_difficulty, 1);
    assert_eq!(counts.parked_xuni, 1);
    assert_eq!(counts.quarantined, 1);
    assert_eq!(counts.acked_total, 1);
    assert_eq!(counts.dead_total, 1);
    assert_eq!(counts.accepted_unconfirmed, 1);
    assert_eq!(counts.permanently_invalid, 1);
    assert_eq!(counts.queued_xen11, 2);
    assert_eq!(counts.queued_xuni, 1);
}

#[test]
fn fetch_awaiting_confirmation() {
    let tmp = TempDb::new("fetch_awaiting_confirmation");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let id_pending = journal.append(&xen("71")).unwrap();
    let id_no_backoff = journal.append(&xen("72")).unwrap();
    let id_past = journal.append(&xen("73")).unwrap();
    let id_future = journal.append(&xen("74")).unwrap();
    journal
        .record_attempt(
            id_no_backoff,
            &classify(FindStatus::AcceptedUnconfirmed, "lookup down"),
            Some(200),
            "OK",
            None,
            NOW,
        )
        .unwrap();
    journal
        .record_attempt(
            id_past,
            &classify(FindStatus::AcceptedUnconfirmed, "lookup down"),
            Some(200),
            "OK",
            Some("2026-08-09T12:00:00Z"),
            NOW,
        )
        .unwrap();
    journal
        .record_attempt(
            id_future,
            &classify(FindStatus::AcceptedUnconfirmed, "lookup down"),
            Some(200),
            "OK",
            Some("2026-08-09T13:00:00Z"),
            NOW,
        )
        .unwrap();

    let awaiting = journal.fetch_awaiting_confirmation(NOW, 10).unwrap();
    assert_eq!(awaiting.len(), 2);
    assert_eq!(awaiting[0].id, id_no_backoff);
    assert_eq!(awaiting[1].id, id_past);
    assert_eq!(awaiting[0].status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(awaiting[0].status_reason, "lookup down");
    assert_eq!(awaiting[0].attempt_count, 1);
    assert_eq!(awaiting[0].last_http_status, Some(200));
    assert_eq!(awaiting[0].payload.key, xen("72").key);

    let awaiting = journal
        .fetch_awaiting_confirmation("2026-08-09T13:00:00Z", 10)
        .unwrap();
    assert_eq!(awaiting.len(), 3);
    assert_eq!(awaiting[2].id, id_future);
    let awaiting = journal
        .fetch_awaiting_confirmation("2026-08-09T13:00:00Z", 1)
        .unwrap();
    assert_eq!(awaiting.len(), 1);
    assert_eq!(awaiting[0].id, id_no_backoff);

    let eligible = journal.fetch_eligible(NOW, 10).unwrap();
    assert_eq!(eligible.len(), 1);
    assert_eq!(eligible[0].id, id_pending);
}

#[test]
fn get_by_id_hit_and_miss() {
    let tmp = TempDb::new("get_by_id");
    let journal = FindJournal::open(&tmp.path).unwrap();
    let payload = make_payload("81", FindKind::Xuni, 1600);
    let id = journal.append(&payload).unwrap();
    let ack_time = "2026-08-09T12:45:00Z";
    journal
        .record_attempt(
            id,
            &classify(FindStatus::Acked, "confirmed"),
            Some(200),
            "OK",
            None,
            ack_time,
        )
        .unwrap();
    let found = journal.get_by_id(id).unwrap().unwrap();
    assert_eq!(found.id, id);
    assert_eq!(found.status, FindStatus::Acked);
    assert_eq!(found.status_reason, "confirmed");
    assert_eq!(found.attempt_count, 1);
    assert_eq!(found.confirmed_at.as_deref(), Some(ack_time));
    assert_eq!(found.last_attempt_at.as_deref(), Some(ack_time));
    assert_eq!(found.last_response, "OK");
    assert_eq!(found.payload.key, payload.key);
    assert_eq!(found.payload.kind, FindKind::Xuni);
    assert_eq!(found.payload.memory_cost, 1600);
    assert!(journal.get_by_id(9999).unwrap().is_none());
}

#[test]
fn difficulty_seen_round_trip() {
    let tmp = TempDb::new("difficulty_seen");
    let journal = FindJournal::open(&tmp.path).unwrap();
    assert!(journal.last_known_difficulty().unwrap().is_none());
    journal
        .record_difficulty(1727, "2026-08-09T12:00:00Z")
        .unwrap();
    journal
        .record_difficulty(2727, "2026-08-09T12:05:00Z")
        .unwrap();
    journal
        .record_difficulty(1587, "2026-08-09T12:10:00Z")
        .unwrap();
    assert_eq!(journal.last_known_difficulty().unwrap(), Some(1587));
    let raw = RawDb::open(&tmp.path);
    assert_eq!(raw.scalar_int("SELECT COUNT(*) FROM difficulty_seen;"), 3);
    assert_eq!(
        raw.scalar_text("SELECT at FROM difficulty_seen ORDER BY rowid LIMIT 1;")
            .unwrap(),
        "2026-08-09T12:00:00Z"
    );
    assert_eq!(
        raw.scalar_int("SELECT value FROM difficulty_seen ORDER BY rowid LIMIT 1;"),
        1727
    );
}

#[test]
fn fetch_eligible_of_kind() {
    let tmp = TempDb::new("fetch_eligible_of_kind");
    let journal = FindJournal::open(&tmp.path).unwrap();
    journal
        .append(&make_payload("71", FindKind::Xuni, 1727))
        .unwrap();
    journal
        .append(&make_payload("72", FindKind::Xuni, 1727))
        .unwrap();
    journal
        .append(&make_payload("73", FindKind::Xuni, 1727))
        .unwrap();
    let xen_id = journal
        .append(&make_payload("74", FindKind::Xen11, 1727))
        .unwrap();
    journal
        .append(&make_payload("75", FindKind::Xuni, 1727))
        .unwrap();

    let xen = journal
        .fetch_eligible_of_kind(FindKind::Xen11, NOW, 2)
        .unwrap();
    assert_eq!(xen.len(), 1);
    assert_eq!(xen[0].id, xen_id);
    assert_eq!(xen[0].payload.kind, FindKind::Xen11);

    let xuni = journal
        .fetch_eligible_of_kind(FindKind::Xuni, NOW, 3)
        .unwrap();
    assert_eq!(xuni.len(), 3);
    assert!(xuni[0].id < xuni[1].id);
    assert!(xuni[1].id < xuni[2].id);

    journal
        .record_attempt(
            xuni[0].id,
            &classify(FindStatus::Pending, "retry later"),
            Some(503),
            "unavailable",
            Some("2999-01-01T00:00:00Z"),
            NOW,
        )
        .unwrap();
    let after = journal
        .fetch_eligible_of_kind(FindKind::Xuni, NOW, 10)
        .unwrap();
    assert_eq!(after.len(), 3);
    for r in &after {
        assert_ne!(r.id, xuni[0].id);
    }
    journal
        .record_attempt(
            xen_id,
            &classify(FindStatus::Acked, "confirmed"),
            Some(200),
            "OK",
            None,
            NOW,
        )
        .unwrap();
    assert!(journal
        .fetch_eligible_of_kind(FindKind::Xen11, NOW, 10)
        .unwrap()
        .is_empty());
}

#[test]
fn reopen_persistence() {
    let tmp = TempDb::new("reopen_persistence");
    let keep = xen("61");
    let kept_id;
    {
        let journal = FindJournal::open(&tmp.path).unwrap();
        kept_id = journal.append(&keep).unwrap();
        let acked_id = journal.append(&xen("62")).unwrap();
        journal
            .record_attempt(
                acked_id,
                &classify(FindStatus::Acked, "confirmed"),
                Some(200),
                "OK",
                None,
                NOW,
            )
            .unwrap();
        journal
            .record_difficulty(1727, "2026-08-09T12:00:00Z")
            .unwrap();
    }
    let reopened = FindJournal::open(&tmp.path).unwrap();
    let eligible = reopened.fetch_eligible(NOW, 10).unwrap();
    assert_eq!(eligible.len(), 1);
    assert_eq!(eligible[0].id, kept_id);
    assert_eq!(eligible[0].payload.key, keep.key);
    assert_eq!(eligible[0].payload.hash_to_verify, keep.hash_to_verify);
    let counts = reopened.counts().unwrap();
    assert_eq!(counts.pending, 1);
    assert_eq!(counts.acked_total, 1);
    assert_eq!(counts.queued_xen11, 1);
    assert_eq!(counts.queued_xuni, 0);
    assert_eq!(reopened.last_known_difficulty().unwrap(), Some(1727));
    assert_eq!(reopened.append(&keep).unwrap(), kept_id);
}
