//! Orchestrator tests: config precedence, recover+fallback, journal-first capture, drain, hash CLI.

use std::collections::{BTreeMap, VecDeque};
use std::fs;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use treeminer_hash::{HashBackend, HASH_API_KEY_LENGTH};
use treeminer_journal::{Counts, FallbackSink, FindJournalApi, JournalError, RecoveryStats};
use treeminer_protocol::{
    Classification, FindKind, FindRecord, FindStatus, FoundPayload, MarginMode,
};
use treeminer_submit::{
    iso_utc, StepResult, SubmissionConfig, SubmissionManager, Transport, TransportResult,
};

use crate::config::{load_config_txt, resolve_config, CliOverrides, HostConfig};
use crate::hash_cli::run_hash;
use crate::host::{
    build_payload, fallback_path_for, persist_find, CaptureInput, CaptureOutcome, Host,
};
use crate::http::verify_json;
use crate::run;

const OK_200: &str = r#"{"message": "Hash verified successfully and block saved."}"#;
const SALT: &str = "aabbccddeeff00112233445566778899aabbccdd";
const NOW_MS: i64 = 1_767_225_600_000; // 2026-01-01T00:00:00Z

struct TempPaths {
    dir: PathBuf,
}

impl TempPaths {
    fn new(tag: &str) -> Self {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "treeminer_orch_{tag}_{}_{nanos}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        Self { dir }
    }
    fn journal(&self) -> PathBuf {
        self.dir.join("treeminer-journal.db")
    }
    fn config(&self) -> PathBuf {
        self.dir.join("config.txt")
    }
}

impl Drop for TempPaths {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.dir);
    }
}

fn xen_digest() -> &'static str {
    // C++ classifies kind from `hashed_pure.find("XEN11")` before PHC assembly.
    "XEN11abcdefghijklmnop"
}

fn capture_input(key_suffix: &str) -> CaptureInput {
    CaptureInput {
        hexsalt: SALT.into(),
        key: format!("aabbccddeeff00112233445566778899aabbccddeeff001122334455667788{key_suffix}"),
        hashed_pure: xen_digest().into(),
        memory_cost: 1100,
        attempts: 123456,
        hashes_per_second: 1500.5,
        worker: "rig0".into(),
    }
}

struct FailingJournal;

impl FindJournalApi for FailingJournal {
    fn append(&self, _: &FoundPayload) -> Result<i64, JournalError> {
        Err(JournalError::new("sqlite busy"))
    }
    fn fetch_eligible(&self, _: &str, _: usize) -> Result<Vec<FindRecord>, JournalError> {
        Ok(vec![])
    }
    fn fetch_eligible_of_kind(
        &self,
        _: FindKind,
        _: &str,
        _: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        Ok(vec![])
    }
    fn fetch_awaiting_confirmation(
        &self,
        _: &str,
        _: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        Ok(vec![])
    }
    fn get_by_id(&self, _: i64) -> Result<Option<FindRecord>, JournalError> {
        Ok(None)
    }
    fn record_attempt(
        &self,
        _: i64,
        _: &Classification,
        _: Option<i32>,
        _: &str,
        _: Option<&str>,
        _: &str,
    ) -> Result<(), JournalError> {
        Ok(())
    }
    fn unpark_for_difficulty(&self, _: u32) -> Result<usize, JournalError> {
        Ok(0)
    }
    fn unpark_xuni_for_window(&self, _: i32) -> Result<usize, JournalError> {
        Ok(0)
    }
    fn recover_on_startup(&self) -> Result<RecoveryStats, JournalError> {
        Ok(RecoveryStats::default())
    }
    fn record_difficulty(&self, _: u32, _: &str) -> Result<(), JournalError> {
        Ok(())
    }
    fn last_known_difficulty(&self) -> Result<Option<u32>, JournalError> {
        Ok(None)
    }
    fn counts(&self) -> Result<Counts, JournalError> {
        Ok(Counts::default())
    }
}

struct ScriptedTransport {
    submits: VecDeque<TransportResult>,
    confirms: VecDeque<TransportResult>,
    posted: Arc<Mutex<Vec<FoundPayload>>>,
}

impl ScriptedTransport {
    fn new(submits: Vec<TransportResult>, confirms: Vec<TransportResult>) -> Self {
        Self {
            submits: submits.into(),
            confirms: confirms.into(),
            posted: Arc::new(Mutex::new(Vec::new())),
        }
    }
}

impl Transport for ScriptedTransport {
    fn submit(&mut self, payload: &FoundPayload) -> TransportResult {
        self.posted.lock().unwrap().push(payload.clone());
        self.submits
            .pop_front()
            .unwrap_or_else(|| TransportResult::down("no submit script"))
    }
    fn confirm(&mut self, _: &str) -> TransportResult {
        self.confirms
            .pop_front()
            .unwrap_or_else(|| TransportResult::down("no confirm script"))
    }
    fn difficulty(&mut self) -> TransportResult {
        TransportResult::ok(200, "1100")
    }
}

#[test]
fn config_file_then_cli_overrides() {
    let tmp = TempPaths::new("cfg");
    fs::write(
        tmp.config(),
        "  journal_path  =  /from/file.db  \n\
         difficulty_margin_mode=fixed\n\
         difficulty_margin=2000\n\
         difficulty_margin_max=8000\n\
         rpc_link=http://from-file\n\
         account_address=0xabc\n",
    )
    .unwrap();
    let file = load_config_txt(tmp.config());
    let resolved = resolve_config(&file, &CliOverrides::default()).unwrap();
    assert_eq!(resolved.journal_path, "/from/file.db");
    assert_eq!(resolved.rpc_link, "http://from-file");
    assert_eq!(resolved.miner_addr, "0xabc");
    assert_eq!(resolved.margin.mode, MarginMode::Fixed);
    assert_eq!(resolved.margin.margin_kib, 2000);
    assert_eq!(resolved.margin.max_kib, 8000);

    let cli = CliOverrides {
        journal_path: Some("cli-journal.db".into()),
        rpc_link: Some("http://cli".into()),
        margin_mode: Some("auto".into()),
        margin_kib: Some("1500".into()),
        ..CliOverrides::default()
    };
    let resolved = resolve_config(&file, &cli).unwrap();
    assert_eq!(resolved.journal_path, "cli-journal.db");
    assert_eq!(resolved.rpc_link, "http://cli");
    assert_eq!(resolved.margin.mode, MarginMode::Auto);
    assert_eq!(resolved.margin.margin_kib, 1500);
    assert_eq!(resolved.margin.max_kib, 8000);
}

#[test]
fn invalid_margin_mode_is_fatal() {
    let mut file = BTreeMap::new();
    file.insert("difficulty_margin_mode".into(), "maybe".into());
    let err = resolve_config(&file, &CliOverrides::default()).unwrap_err();
    assert!(err.to_string().contains("off | fixed | auto"));
}

#[test]
fn empty_journal_path_keeps_default() {
    let cfg = resolve_config(&BTreeMap::new(), &CliOverrides::default()).unwrap();
    assert_eq!(cfg.journal_path, HostConfig::default().journal_path);
    assert_eq!(cfg.rpc_link, "http://xenblocks.io");
}

#[test]
fn recover_imports_fallback_before_startup() {
    let tmp = TempPaths::new("recover");
    let journal_path = tmp.journal();
    let fallback = fallback_path_for(&journal_path);
    let payload = build_payload(&capture_input("01"), NOW_MS).unwrap();
    assert!(FallbackSink::new(&fallback).append(&payload));

    let host = Host::open(&journal_path).unwrap();
    assert!(host.import.file_present);
    assert_eq!(host.import.imported, 1);
    assert_eq!(host.import.malformed, 0);
    assert_eq!(host.recovery.pending, 1);
    let rec = host.journal.get_by_id(1).unwrap().unwrap();
    assert_eq!(rec.payload.key, payload.key);
    assert_eq!(rec.status, FindStatus::Pending);
}

#[test]
fn capture_journals_phc_before_any_network() {
    let tmp = TempPaths::new("capture");
    let host = Host::open(tmp.journal()).unwrap();
    let transport = ScriptedTransport::new(vec![], vec![]);
    let posted = Arc::clone(&transport.posted);

    let outcome = host.capture(capture_input("02"), NOW_MS).unwrap();
    assert!(matches!(outcome, CaptureOutcome::Journaled { id } if id > 0));
    assert!(posted.lock().unwrap().is_empty(), "no POST before journal");

    let rec = host.journal.fetch_eligible(&iso_utc(NOW_MS), 8).unwrap();
    assert_eq!(rec.len(), 1);
    assert!(rec[0]
        .payload
        .hash_to_verify
        .starts_with("$argon2id$v=19$m=1100,t=1,p=1$"));
    assert!(rec[0].payload.hash_to_verify.contains(xen_digest()));
    assert_eq!(rec[0].payload.account, format!("0x{SALT}"));
    assert_eq!(rec[0].payload.kind, FindKind::Xen11);
    assert_eq!(rec[0].payload.found_at_utc, iso_utc(NOW_MS));
    drop(transport);
}

#[test]
fn capture_falls_back_when_journal_fails() {
    let tmp = TempPaths::new("fb");
    let sink = FallbackSink::new(tmp.dir.join("fallback.jsonl"));
    let payload = build_payload(&capture_input("03"), NOW_MS).unwrap();
    let outcome = persist_find(&FailingJournal, &sink, &payload);
    assert_eq!(outcome, CaptureOutcome::Fallback);
    assert!(sink.path().exists());
}

#[test]
fn capture_fatal_when_journal_and_fallback_fail() {
    let tmp = TempPaths::new("fatal");
    let blocker = tmp.dir.join("not-a-dir");
    fs::write(&blocker, b"x").unwrap();
    let sink = FallbackSink::new(blocker.join("sink.jsonl"));
    let payload = build_payload(&capture_input("04"), NOW_MS).unwrap();
    let outcome = persist_find(&FailingJournal, &sink, &payload);
    assert!(outcome.is_fatal());
    assert!(!outcome.durably_captured());
}

#[test]
fn xuni_kind_when_digest_lacks_xen11() {
    let mut input = capture_input("05");
    input.hashed_pure = "XUNI7digest".into();
    let payload = build_payload(&input, NOW_MS).unwrap();
    assert_eq!(payload.kind, FindKind::Xuni);
}

#[test]
fn drain_run_once_with_fake_transport() {
    let tmp = TempPaths::new("drain");
    let host = Host::open(tmp.journal()).unwrap();
    let outcome = host.capture(capture_input("06"), NOW_MS).unwrap();
    let CaptureOutcome::Journaled { id } = outcome else {
        panic!("expected journaled");
    };
    let rec = host.journal.get_by_id(id).unwrap().unwrap();
    let confirm_body = format!(
        r#"{{"account":"{}","hash_to_verify":"{}","key":"{}"}}"#,
        rec.payload.account, rec.payload.hash_to_verify, rec.payload.key
    );

    let transport = ScriptedTransport::new(
        vec![TransportResult::ok(200, OK_200)],
        vec![TransportResult::ok(200, confirm_body)],
    );
    let posted = Arc::clone(&transport.posted);
    let mut cfg = SubmissionConfig::default();
    cfg.backoff_base_ms = 2000;
    let mut mgr = SubmissionManager::with_config(
        host.journal,
        transport,
        cfg,
        None,
        Some(Arc::new(|| NOW_MS)),
    );

    assert_eq!(mgr.run_once(), StepResult::Submitted);
    assert_eq!(posted.lock().unwrap().len(), 1);
    assert_eq!(posted.lock().unwrap()[0].key, rec.payload.key);

    let mut last = StepResult::Idle;
    for _ in 0..8 {
        last = mgr.run_once();
        if mgr.journal().get_by_id(id).unwrap().unwrap().status == FindStatus::Acked {
            break;
        }
    }
    let acked = mgr.journal().get_by_id(id).unwrap().unwrap();
    assert_eq!(acked.status, FindStatus::Acked, "last step {last:?}");
}

#[test]
fn verify_json_attempts_and_rate_are_strings() {
    let payload = build_payload(&capture_input("07"), NOW_MS).unwrap();
    let body = verify_json(&payload, "worker-1");
    assert!(body.contains(r#""attempts":"123456""#));
    assert!(body.contains(r#""hashes_per_second":"1500.50""#));
    assert!(body.contains(r#""worker":"worker-1""#));
    assert!(body.contains(&format!(r#""key":"{}""#, payload.key)));
    assert!(!body.contains(r#""attempts":123456"#));
}

#[test]
fn http_transport_posts_verify_and_reads_headers() {
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::thread;

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut buf = [0u8; 8192];
        let n = stream.read(&mut buf).unwrap();
        let req = String::from_utf8_lossy(&buf[..n]).into_owned();
        stream
            .write_all(
                b"HTTP/1.1 200 OK\r\nRetry-After: 7\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok",
            )
            .unwrap();
        req
    });
    let mut transport =
        crate::HttpTransport::new(format!("http://127.0.0.1:{}", addr.port()), "w1");
    let payload = build_payload(&capture_input("09"), NOW_MS).unwrap();
    let result = treeminer_submit::Transport::submit(&mut transport, &payload);
    let req = server.join().unwrap();
    assert!(result.transport_ok, "{}", result.error);
    assert_eq!(result.http_status, 200);
    assert_eq!(result.body, "ok");
    assert_eq!(result.retry_after.as_deref(), Some("7"));
    assert_eq!(
        result.date_header.as_deref(),
        Some("Sun, 06 Nov 1994 08:49:37 GMT")
    );
    assert!(req.contains("POST /verify HTTP/1.1"));
    assert!(req.contains(r#""attempts":"123456""#));
    assert!(req.contains(r#""hashes_per_second":"1500.50""#));
    assert!(req.contains(r#""worker":"w1""#));
}

#[test]
fn http_transport_strips_trailing_slash() {
    let t = crate::HttpTransport::new("http://xenblocks.io/", "w");
    assert_eq!(t.rpc(), "http://xenblocks.io");
}

#[test]
fn hash_batch_cli_against_stub() {
    let out = run([
        "treeminer",
        "hash-batch",
        "--salt",
        "aabbccddeeff0011",
        "--batch-size",
        "2",
        "--difficulty",
        "8",
        "--pattern",
        "stub",
        "--no-xuni",
    ]);
    assert_eq!(out.code, 0, "{}", out.stderr);
    assert!(out.stdout.contains("ok=true"));
    assert!(out.stdout.contains("attempts=2"));
    assert!(out.stdout.contains("backend=cpu-stub"));
}

#[test]
fn hash_one_cli_fixed_key() {
    let key = "ab".repeat(HASH_API_KEY_LENGTH / 2);
    let out = run([
        "treeminer",
        "hash-one",
        "--salt",
        "aabbccddeeff0011",
        "--key",
        &key,
        "--difficulty",
        "8",
        "--pattern",
        "stub",
        "--no-xuni",
        "--json",
    ]);
    assert_eq!(out.code, 0, "{}", out.stderr);
    assert!(out.stdout.contains("\"ok\":true"));
    assert!(out.stdout.contains("\"attempts\":1"));
    assert!(out.stdout.contains(&key));
}

#[test]
fn hash_help_and_host_help() {
    let help = run(["treeminer", "--help"]);
    assert_eq!(help.code, 0);
    assert!(help.stdout.contains("recover"));
    let hash = run(["treeminer", "hash-help"]);
    assert_eq!(hash.code, 0);
    assert!(hash.stdout.contains("hash-batch"));
}

#[test]
fn recover_cli_prints_absolute_path() {
    let tmp = TempPaths::new("cli_recover");
    let journal = tmp.journal();
    let host = Host::open(&journal).unwrap();
    drop(host);
    let out = run([
        "treeminer",
        "recover",
        "--journalPath",
        journal.to_str().unwrap(),
    ]);
    assert_eq!(out.code, 0, "{}", out.stderr);
    assert!(out.stdout.contains("JOURNAL path="));
    assert!(out.stdout.contains("recovered"));
}

#[test]
fn capture_cli_journals_find() {
    let tmp = TempPaths::new("cli_cap");
    let journal = tmp.journal();
    let key = format!("aabbccddeeff00112233445566778899aabbccddeeff00112233445566778808");
    let out = run([
        "treeminer",
        "capture",
        "--journalPath",
        journal.to_str().unwrap(),
        "--salt",
        SALT,
        "--key",
        &key,
        "--digest",
        xen_digest(),
        "--difficulty",
        "1100",
    ]);
    assert_eq!(out.code, 0, "{}", out.stderr);
    assert!(out.stdout.contains("journaled"));
    let host = Host::open(&journal).unwrap();
    assert_eq!(host.journal.counts().unwrap().pending, 1);
}

#[test]
fn hash_request_helper_matches_stub_backend() {
    let mut req = treeminer_hash::HashRequest {
        salt_hex: "aabbccddeeff0011".into(),
        difficulty: 8,
        batch_size: 1,
        target_pattern: "stub".into(),
        allow_xuni: false,
        key: "cd".repeat(32),
        ..treeminer_hash::HashRequest::default()
    };
    let ffi = treeminer_hash::FfiBackend.run_batch(&req);
    let via = run_hash(&mut treeminer_hash::FfiBackend, &req);
    assert_eq!(ffi.hash, via.hash);
    req.key.clear();
    req.batch_size = 3;
    let r = run_hash(&mut treeminer_hash::FfiBackend, &req);
    assert!(r.ok);
    assert_eq!(r.attempts, 3);
}
