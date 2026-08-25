use std::fs::{self, File};
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;

use treeminer_protocol::{FindKind, FindStatus};

use crate::test_support::{make_payload, NOW};
use crate::{FallbackSink, FindJournal};

struct TempDb {
    path: PathBuf,
}

impl TempDb {
    fn new(name: &str) -> Self {
        let path = std::env::temp_dir().join(format!("treeminer_rs_fallback_db_{name}.db"));
        let tmp = Self { path };
        tmp.cleanup();
        tmp
    }
    fn cleanup(&self) {
        let _ = fs::remove_file(&self.path);
        let _ = fs::remove_file(suffix(&self.path, "-wal"));
        let _ = fs::remove_file(suffix(&self.path, "-shm"));
    }
}

impl Drop for TempDb {
    fn drop(&mut self) {
        self.cleanup();
    }
}

struct TempSink {
    path: PathBuf,
}

impl TempSink {
    fn new(name: &str) -> Self {
        let path = std::env::temp_dir().join(format!("treeminer_rs_fallback_sink_{name}.jsonl"));
        let tmp = Self { path };
        tmp.cleanup();
        tmp
    }
    fn cleanup(&self) {
        let _ = fs::remove_file(&self.path);
        let _ = fs::remove_file(suffix(&self.path, ".imported"));
    }
    fn exists(&self) -> bool {
        self.path.exists()
    }
    fn archive_exists(&self) -> bool {
        suffix(&self.path, ".imported").exists()
    }
    fn read_lines(&self) -> Vec<String> {
        let file = File::open(&self.path).unwrap();
        BufReader::new(file).lines().map(|l| l.unwrap()).collect()
    }
    fn write_lines(&self, lines: &[String]) {
        let mut out = File::create(&self.path).unwrap();
        for line in lines {
            writeln!(out, "{line}").unwrap();
        }
    }
}

impl Drop for TempSink {
    fn drop(&mut self) {
        self.cleanup();
    }
}

fn suffix(path: &PathBuf, extra: &str) -> PathBuf {
    let mut s = path.as_os_str().to_os_string();
    s.push(extra);
    PathBuf::from(s)
}

fn journaled_by_key(
    journal: &FindJournal,
) -> std::collections::BTreeMap<String, treeminer_protocol::FindRecord> {
    journal
        .fetch_eligible(NOW, 1000)
        .unwrap()
        .into_iter()
        .map(|r| (r.payload.key.clone(), r))
        .collect()
}

fn check_payloads_equal(
    actual: &treeminer_protocol::FoundPayload,
    expected: &treeminer_protocol::FoundPayload,
) {
    assert_eq!(actual.key, expected.key);
    assert_eq!(actual.hash_to_verify, expected.hash_to_verify);
    assert_eq!(actual.account, expected.account);
    assert_eq!(actual.kind, expected.kind);
    assert_eq!(actual.memory_cost, expected.memory_cost);
    assert_eq!(actual.worker, expected.worker);
    assert_eq!(actual.attempts, expected.attempts);
    assert_eq!(actual.hashes_per_second, expected.hashes_per_second);
    assert_eq!(actual.found_at_utc, expected.found_at_utc);
}

#[test]
fn append_and_import_round_trip() {
    let sink = TempSink::new("round_trip");
    let tmp = TempDb::new("round_trip");
    let a = make_payload("01", FindKind::Xen11, 1727);
    let mut b = make_payload("02", FindKind::Xuni, 1900);
    b.worker = "rig1-gpu3".into();
    b.attempts = 987654321;
    b.hashes_per_second = 0.125;
    b.found_at_utc = "2026-08-09T12:05:01Z".into();
    let mut c = make_payload("03", FindKind::Xen11, 2048);
    c.hashes_per_second = 1234.56789;

    let fallback = FallbackSink::new(&sink.path);
    assert!(fallback.append(&a));
    assert!(fallback.append(&b));
    assert!(fallback.append(&c));
    assert_eq!(sink.read_lines().len(), 3);

    let journal = FindJournal::open(&tmp.path).unwrap();
    let stats = FallbackSink::import_into(&journal, &sink.path);
    assert_eq!(stats.imported, 3);
    assert_eq!(stats.malformed, 0);
    assert!(stats.file_present);

    let by_key = journaled_by_key(&journal);
    assert_eq!(by_key.len(), 3);
    for expected in [&a, &b, &c] {
        let rec = by_key.get(&expected.key).unwrap();
        assert_eq!(rec.status, FindStatus::Pending);
        check_payloads_equal(&rec.payload, expected);
    }
    assert!(!sink.exists());
    assert!(sink.archive_exists());
}

#[test]
fn import_is_idempotent_by_key() {
    let sink = TempSink::new("idempotent");
    let tmp = TempDb::new("idempotent");
    let payload = make_payload("07", FindKind::Xuni, 1800);
    let fallback = FallbackSink::new(&sink.path);
    assert!(fallback.append(&payload));
    assert!(fallback.append(&payload));
    let journal = FindJournal::open(&tmp.path).unwrap();
    let direct_id = journal.append(&payload).unwrap();
    assert!(direct_id > 0);
    let stats = FallbackSink::import_into(&journal, &sink.path);
    assert_eq!(stats.imported, 2);
    assert_eq!(stats.malformed, 0);
    let by_key = journaled_by_key(&journal);
    assert_eq!(by_key.len(), 1);
    assert_eq!(by_key[&payload.key].id, direct_id);
    check_payloads_equal(&by_key[&payload.key].payload, &payload);
}

#[test]
fn malformed_lines_never_block_recovery() {
    let sink = TempSink::new("malformed");
    let tmp = TempDb::new("malformed");
    let first = make_payload("11", FindKind::Xen11, 1727);
    let second = make_payload("12", FindKind::Xuni, 1900);
    let good_lines = {
        let scratch = TempSink::new("malformed_scratch");
        let generator = FallbackSink::new(&scratch.path);
        assert!(generator.append(&first));
        assert!(generator.append(&second));
        scratch.read_lines()
    };
    assert_eq!(good_lines.len(), 2);
    let mut truncated = good_lines[0].clone();
    truncated.truncate(truncated.len() / 2);
    sink.write_lines(&[
        good_lines[0].clone(),
        "not json".into(),
        truncated,
        good_lines[1].clone(),
    ]);
    let journal = FindJournal::open(&tmp.path).unwrap();
    let stats = FallbackSink::import_into(&journal, &sink.path);
    assert_eq!(stats.imported, 2);
    assert_eq!(stats.malformed, 2);
    assert!(stats.file_present);
    let by_key = journaled_by_key(&journal);
    assert_eq!(by_key.len(), 2);
    check_payloads_equal(&by_key[&first.key].payload, &first);
    check_payloads_equal(&by_key[&second.key].payload, &second);
    assert!(!sink.exists());
    assert!(sink.archive_exists());
}

#[test]
fn missing_file_is_clean_noop() {
    let sink = TempSink::new("missing");
    let tmp = TempDb::new("missing");
    assert!(!sink.exists());
    let journal = FindJournal::open(&tmp.path).unwrap();
    let stats = FallbackSink::import_into(&journal, &sink.path);
    assert_eq!(stats.imported, 0);
    assert_eq!(stats.malformed, 0);
    assert!(!stats.file_present);
    assert!(!sink.exists());
    assert!(!sink.archive_exists());
    assert_eq!(journal.counts().unwrap().pending, 0);
}

#[test]
fn append_survives_weird_content() {
    let sink = TempSink::new("weird");
    let tmp = TempDb::new("weird");
    let mut payload = make_payload("21", FindKind::Xuni, 4096);
    payload.hash_to_verify = "$argon2id$v=19$m=4096,t=1,p=1$c2FsdA$\"quoted\\backslash\"".into();
    payload.worker = "rig\"0\\gpu\nline2\ttab\u{0001}ctrl-é-日本".into();
    payload.found_at_utc = "2026-08-09T12:00:00Z\r\nspliced".into();
    payload.attempts = u64::MAX;
    payload.hashes_per_second = 0.1 + 0.2;
    let fallback = FallbackSink::new(&sink.path);
    assert!(fallback.append(&payload));
    assert_eq!(sink.read_lines().len(), 1);
    let journal = FindJournal::open(&tmp.path).unwrap();
    let stats = FallbackSink::import_into(&journal, &sink.path);
    assert_eq!(stats.imported, 1);
    assert_eq!(stats.malformed, 0);
    let by_key = journaled_by_key(&journal);
    assert_eq!(by_key.len(), 1);
    check_payloads_equal(&by_key[&payload.key].payload, &payload);
}
