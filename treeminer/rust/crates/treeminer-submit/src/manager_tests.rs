//! Port of `tests/unit/submit/test_submission_manager.cpp`.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use treeminer_journal::{Counts, FindJournalApi, JournalError, RecoveryStats};
use treeminer_protocol::{
    xuni_window_at, Classification, FindKind, FindRecord, FindStatus, FoundPayload,
};

use crate::breaker::BreakerState;
use crate::manager::{
    ConfirmBodyCheck, DrainHandle, StepResult, SubmissionConfig, SubmissionManager,
};
use crate::time::iso_utc;
use crate::transport::{Transport, TransportResult};

const OK_200: &str = r#"{"message": "Hash verified successfully and block saved."}"#;
const DUP_400: &str = r#"{"message": "Block already exists, continue"}"#;
const WALL_OPEN: i64 = 1_767_225_600_000; // 2026-01-01T00:00:00Z — XUNI window OPEN
const WALL_CLOSED: i64 = 1_767_227_400_000; // 2026-01-01T00:30:00Z — window closed

fn payload(key: &str, kind: FindKind, m: u32) -> FoundPayload {
    FoundPayload {
        key: key.to_string(),
        hash_to_verify: format!("$argon2id$v=19$m={m},t=1,p=1$saltsalt$XEN11digest"),
        account: "0x1111111111111111111111111111111111111111".into(),
        kind,
        memory_cost: m,
        worker: "w1".into(),
        attempts: 1000,
        hashes_per_second: 1234.5,
        found_at_utc: "2026-01-01T00:00:00Z".into(),
    }
}

fn ok(status: i32, body: &str) -> TransportResult {
    TransportResult::ok(status, body)
}

fn down() -> TransportResult {
    TransportResult::down("connect refused")
}

fn block_row(p: &FoundPayload) -> String {
    format!(
        r#"{{"account": "{}", "block_id": 7, "created_at": "2026-01-01 00:00:00", "hash_to_verify": "{}", "key": "{}"}}"#,
        p.account, p.hash_to_verify, p.key
    )
}

fn test_config() -> SubmissionConfig {
    let mut cfg = SubmissionConfig::default();
    cfg.backoff_base_ms = 2000;
    cfg
}

struct FakeJournalInner {
    records: Vec<FindRecord>,
    next_id: i64,
    unpark_difficulty_calls: Vec<u32>,
    unpark_xuni_calls: i32,
    difficulty_log: Vec<(u32, String)>,
    throw_on_record_attempt: bool,
    throw_count: i32,
}

#[derive(Clone)]
struct FakeJournal {
    inner: Arc<Mutex<FakeJournalInner>>,
}

impl FakeJournal {
    fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(FakeJournalInner {
                records: Vec::new(),
                next_id: 1,
                unpark_difficulty_calls: Vec::new(),
                unpark_xuni_calls: 0,
                difficulty_log: Vec::new(),
                throw_on_record_attempt: false,
                throw_count: 0,
            })),
        }
    }

    fn throwing() -> Self {
        let j = Self::new();
        j.inner.lock().unwrap().throw_on_record_attempt = true;
        j
    }

    fn record(&self, id: i64) -> FindRecord {
        self.inner
            .lock()
            .unwrap()
            .records
            .iter()
            .find(|r| r.id == id)
            .cloned()
            .expect("record")
    }

    fn set_status(&self, id: i64, status: FindStatus) {
        if let Some(r) = self
            .inner
            .lock()
            .unwrap()
            .records
            .iter_mut()
            .find(|r| r.id == id)
        {
            r.status = status;
        }
    }

    fn unpark_difficulty_calls(&self) -> Vec<u32> {
        self.inner.lock().unwrap().unpark_difficulty_calls.clone()
    }

    fn unpark_xuni_calls(&self) -> i32 {
        self.inner.lock().unwrap().unpark_xuni_calls
    }

    fn difficulty_log(&self) -> Vec<(u32, String)> {
        self.inner.lock().unwrap().difficulty_log.clone()
    }

    fn throw_count(&self) -> i32 {
        self.inner.lock().unwrap().throw_count
    }
}

impl FindJournalApi for FakeJournal {
    fn append(&self, payload: &FoundPayload) -> Result<i64, JournalError> {
        let mut g = self.inner.lock().unwrap();
        if let Some(r) = g.records.iter().find(|r| r.payload.key == payload.key) {
            return Ok(r.id);
        }
        let id = g.next_id;
        g.next_id += 1;
        g.records.push(FindRecord {
            id,
            payload: payload.clone(),
            status: FindStatus::Pending,
            ..FindRecord::default()
        });
        Ok(id)
    }

    fn fetch_eligible(&self, now_utc: &str, limit: usize) -> Result<Vec<FindRecord>, JournalError> {
        let g = self.inner.lock().unwrap();
        let mut out = Vec::new();
        for r in &g.records {
            if r.status != FindStatus::Pending {
                continue;
            }
            if r.next_attempt_at.as_deref().is_some_and(|t| t > now_utc) {
                continue;
            }
            out.push(r.clone());
            if out.len() >= limit {
                break;
            }
        }
        Ok(out)
    }

    fn fetch_eligible_of_kind(
        &self,
        kind: FindKind,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        let g = self.inner.lock().unwrap();
        let mut out = Vec::new();
        for r in &g.records {
            if r.status != FindStatus::Pending || r.payload.kind != kind {
                continue;
            }
            if r.next_attempt_at.as_deref().is_some_and(|t| t > now_utc) {
                continue;
            }
            out.push(r.clone());
            if out.len() >= limit {
                break;
            }
        }
        Ok(out)
    }

    fn fetch_awaiting_confirmation(
        &self,
        now_utc: &str,
        limit: usize,
    ) -> Result<Vec<FindRecord>, JournalError> {
        let g = self.inner.lock().unwrap();
        let mut out = Vec::new();
        for r in &g.records {
            if r.status != FindStatus::AcceptedUnconfirmed {
                continue;
            }
            if r.next_attempt_at.as_deref().is_some_and(|t| t > now_utc) {
                continue;
            }
            out.push(r.clone());
            if out.len() >= limit {
                break;
            }
        }
        Ok(out)
    }

    fn get_by_id(&self, id: i64) -> Result<Option<FindRecord>, JournalError> {
        Ok(self
            .inner
            .lock()
            .unwrap()
            .records
            .iter()
            .find(|r| r.id == id)
            .cloned())
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
        let mut g = self.inner.lock().unwrap();
        if g.throw_on_record_attempt {
            g.throw_count += 1;
            return Err(JournalError::new("disk I/O error (simulated)"));
        }
        let Some(r) = g.records.iter_mut().find(|r| r.id == id) else {
            return Ok(());
        };
        r.status = classification.next_status;
        r.status_reason = classification.reason.clone();
        r.attempt_count += 1;
        r.next_attempt_at = next_attempt_at.map(str::to_string);
        r.last_attempt_at = Some(now_utc.to_string());
        r.last_http_status = http_status;
        r.last_response = response_body.to_string();
        if classification.next_status == FindStatus::Acked {
            r.confirmed_at = Some(now_utc.to_string());
        }
        Ok(())
    }

    fn unpark_for_difficulty(&self, current_difficulty: u32) -> Result<usize, JournalError> {
        let mut g = self.inner.lock().unwrap();
        let mut n = 0usize;
        for r in &mut g.records {
            if r.status == FindStatus::ParkedDifficulty
                && r.payload.memory_cost >= current_difficulty
            {
                r.status = FindStatus::Pending;
                r.next_attempt_at = None;
                n += 1;
            }
        }
        g.unpark_difficulty_calls.push(current_difficulty);
        Ok(n)
    }

    fn unpark_xuni_for_window(&self, max_windows: i32) -> Result<usize, JournalError> {
        let mut g = self.inner.lock().unwrap();
        let mut n = 0usize;
        for r in &mut g.records {
            if r.status != FindStatus::ParkedXuniWindow {
                continue;
            }
            if r.xuni_windows_tried >= max_windows {
                r.status = FindStatus::Dead;
                continue;
            }
            r.xuni_windows_tried += 1;
            r.status = FindStatus::Pending;
            r.next_attempt_at = None;
            n += 1;
        }
        g.unpark_xuni_calls += 1;
        Ok(n)
    }

    fn recover_on_startup(&self) -> Result<RecoveryStats, JournalError> {
        let mut g = self.inner.lock().unwrap();
        let mut s = RecoveryStats::default();
        for r in &mut g.records {
            if r.status == FindStatus::Submitting {
                r.status = FindStatus::Pending;
            }
            match r.status {
                FindStatus::Pending => s.pending += 1,
                FindStatus::AcceptedUnconfirmed => s.accepted_unconfirmed += 1,
                FindStatus::ParkedDifficulty => s.parked_difficulty += 1,
                FindStatus::ParkedXuniWindow => s.parked_xuni += 1,
                FindStatus::Quarantined => s.quarantined += 1,
                FindStatus::Acked => s.acked += 1,
                FindStatus::Dead => s.dead += 1,
                FindStatus::PermanentlyInvalid => s.invalid += 1,
                _ => {}
            }
        }
        Ok(s)
    }

    fn record_difficulty(&self, difficulty: u32, at_utc: &str) -> Result<(), JournalError> {
        self.inner
            .lock()
            .unwrap()
            .difficulty_log
            .push((difficulty, at_utc.to_string()));
        Ok(())
    }

    fn last_known_difficulty(&self) -> Result<Option<u32>, JournalError> {
        Ok(self
            .inner
            .lock()
            .unwrap()
            .difficulty_log
            .last()
            .map(|(d, _)| *d))
    }

    fn counts(&self) -> Result<Counts, JournalError> {
        let g = self.inner.lock().unwrap();
        let mut c = Counts::default();
        for r in &g.records {
            if r.status == FindStatus::Pending || r.status == FindStatus::AcceptedUnconfirmed {
                match r.payload.kind {
                    FindKind::Xen11 => c.queued_xen11 += 1,
                    FindKind::Xuni => c.queued_xuni += 1,
                }
            }
            match r.status {
                FindStatus::Pending => c.pending += 1,
                FindStatus::ParkedDifficulty => {
                    c.parked += 1;
                    c.parked_difficulty += 1;
                }
                FindStatus::ParkedXuniWindow => {
                    c.parked += 1;
                    c.parked_xuni += 1;
                }
                FindStatus::Quarantined => c.quarantined += 1,
                FindStatus::Acked => c.acked_total += 1,
                FindStatus::Dead => c.dead_total += 1,
                FindStatus::AcceptedUnconfirmed => c.accepted_unconfirmed += 1,
                FindStatus::PermanentlyInvalid => c.permanently_invalid += 1,
                _ => {}
            }
        }
        Ok(c)
    }
}

struct FakeTransportInner {
    submit_queue: VecDeque<TransportResult>,
    confirm_queue: VecDeque<TransportResult>,
    difficulty_queue: VecDeque<TransportResult>,
    submitted_keys: Vec<String>,
    confirmed_keys: Vec<String>,
    difficulty_calls: i32,
}

#[derive(Clone)]
struct FakeTransport {
    inner: Arc<Mutex<FakeTransportInner>>,
}

impl FakeTransport {
    fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(FakeTransportInner {
                submit_queue: VecDeque::new(),
                confirm_queue: VecDeque::new(),
                difficulty_queue: VecDeque::new(),
                submitted_keys: Vec::new(),
                confirmed_keys: Vec::new(),
                difficulty_calls: 0,
            })),
        }
    }

    fn push_submit(&self, r: TransportResult) {
        self.inner.lock().unwrap().submit_queue.push_back(r);
    }
    fn push_confirm(&self, r: TransportResult) {
        self.inner.lock().unwrap().confirm_queue.push_back(r);
    }
    fn push_difficulty(&self, r: TransportResult) {
        self.inner.lock().unwrap().difficulty_queue.push_back(r);
    }
    fn submitted_keys(&self) -> Vec<String> {
        self.inner.lock().unwrap().submitted_keys.clone()
    }
    fn confirmed_keys(&self) -> Vec<String> {
        self.inner.lock().unwrap().confirmed_keys.clone()
    }
}

impl Transport for FakeTransport {
    fn submit(&mut self, payload: &FoundPayload) -> TransportResult {
        let mut g = self.inner.lock().unwrap();
        g.submitted_keys.push(payload.key.clone());
        g.submit_queue.pop_front().unwrap_or_else(down)
    }
    fn confirm(&mut self, key: &str) -> TransportResult {
        let mut g = self.inner.lock().unwrap();
        g.confirmed_keys.push(key.to_string());
        g.confirm_queue.pop_front().unwrap_or_else(down)
    }
    fn difficulty(&mut self) -> TransportResult {
        let mut g = self.inner.lock().unwrap();
        g.difficulty_calls += 1;
        g.difficulty_queue.pop_front().unwrap_or_else(down)
    }
}

struct Clocks {
    mono: Arc<AtomicI64>,
    wall: Arc<AtomicI64>,
}

impl Clocks {
    fn new(wall: i64) -> Self {
        Self {
            mono: Arc::new(AtomicI64::new(0)),
            wall: Arc::new(AtomicI64::new(wall)),
        }
    }

    fn advance(&self, ms: i64) {
        self.mono.fetch_add(ms, Ordering::SeqCst);
        self.wall.fetch_add(ms, Ordering::SeqCst);
    }

    fn set_wall(&self, wall: i64) {
        self.wall.store(wall, Ordering::SeqCst);
    }

    fn wall(&self) -> i64 {
        self.wall.load(Ordering::SeqCst)
    }

    fn fns(&self) -> (crate::manager::MonoClock, crate::manager::WallClock) {
        let m = Arc::clone(&self.mono);
        let w = Arc::clone(&self.wall);
        (
            Arc::new(move || m.load(Ordering::SeqCst)),
            Arc::new(move || w.load(Ordering::SeqCst)),
        )
    }
}

fn make_mgr(
    j: FakeJournal,
    t: FakeTransport,
    cfg: SubmissionConfig,
    clocks: &Clocks,
) -> SubmissionManager<FakeJournal, FakeTransport> {
    let (mono, wall) = clocks.fns();
    SubmissionManager::with_config(j, t, cfg, Some(mono), Some(wall))
}

#[test]
fn xuni_window_at_models_the_55_to_05_server_window() {
    let w = xuni_window_at(56 * 60_000);
    assert!(w.open);
    assert_eq!(w.ms_until_close, 4 * 60_000 + 5 * 60_000);
    let w = xuni_window_at(3 * 60_000);
    assert!(w.open);
    assert_eq!(w.ms_until_close, 2 * 60_000);
    let w = xuni_window_at(30 * 60_000);
    assert!(!w.open);
    assert!(xuni_window_at(55 * 60_000).open);
    assert!(!xuni_window_at(5 * 60_000).open);
}

#[test]
fn two_hundred_with_confirmed_lookup_becomes_acked() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let outcomes = Arc::new(Mutex::new(Vec::new()));
    let o = Arc::clone(&outcomes);
    m.set_outcome_callback(Arc::new(move |_, c, _| {
        o.lock().unwrap().push(c.next_status);
    }));
    let p = payload("aa11", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    let mut m = m;
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Acked);
    assert!(j.record(id).confirmed_at.is_some());
    assert_eq!(t.confirmed_keys().len(), 1);
    assert_eq!(t.confirmed_keys()[0], "aa11");
    assert_eq!(m.metrics().acked, 1);
    assert_eq!(m.metrics().reconciled_via_get_block, 1);
    assert_eq!(outcomes.lock().unwrap().as_slice(), &[FindStatus::Acked]);
}

#[test]
fn two_hundred_with_absent_lookup_is_resubmitted_lying_200() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("aa22", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(404, r#"{"error": "Data not found for provided key"}"#));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Pending);
    assert!(j.record(id).next_attempt_at.is_some());
    assert_eq!(m.metrics().lying_200_detected, 1);
    clk.advance(10_000);
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Acked);
    assert_eq!(m.metrics().submitted, 2);
    assert_eq!(m.metrics().resubmitted, 1);
}

#[test]
fn duplicate_response_confirms_via_lookup_to_acked() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("aa33", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    t.push_submit(ok(400, DUP_400));
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Acked);
}

#[test]
fn unavailable_lookup_leaves_accepted_unconfirmed() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let id = j
        .append(&payload("aa44", FindKind::Xen11, 100_000))
        .unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(down());
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(m.metrics().accepted_unconfirmed, 1);
    assert!(j.record(id).status_reason.contains("unavailable"));
    assert_eq!(
        j.record(id).next_attempt_at,
        Some(iso_utc(clk.wall() + 2000))
    );
    assert_eq!(t.confirmed_keys().len(), 1);
}

#[test]
fn difficulty_401_parks_and_propagates_the_m_hint() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let hints = Arc::new(Mutex::new(Vec::new()));
    let h = Arc::clone(&hints);
    m.set_difficulty_hint_callback(Arc::new(move |d| h.lock().unwrap().push(d)));
    let outcomes = Arc::new(Mutex::new(Vec::new()));
    let o = Arc::clone(&outcomes);
    m.set_outcome_callback(Arc::new(move |_, c, _| {
        o.lock().unwrap().push(c.next_status);
    }));
    let id = j
        .append(&payload("aa55", FindKind::Xen11, 100_000))
        .unwrap();
    t.push_submit(ok(
        401,
        r#"{"message": "Hash does not contain 'm=104000'. Your memory_cost setting in your miner will be autoadjusted."}"#,
    ));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::ParkedDifficulty);
    assert_eq!(hints.lock().unwrap().as_slice(), &[104000]);
    assert_eq!(m.last_observed_difficulty(), Some(104000));
    assert!(!j.difficulty_log().is_empty());
    assert_eq!(j.difficulty_log().last().unwrap().0, 104000);
    assert_eq!(
        outcomes.lock().unwrap().as_slice(),
        &[FindStatus::ParkedDifficulty]
    );
}

#[test]
fn retry_after_429_is_honored_in_next_attempt_at() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let id = j
        .append(&payload("aa66", FindKind::Xen11, 100_000))
        .unwrap();
    let mut r = ok(429, r#"{"message": "slow down"}"#);
    r.retry_after = Some("30".into());
    t.push_submit(r);
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Pending);
    assert_eq!(
        j.record(id).next_attempt_at,
        Some(iso_utc(clk.wall() + 30_000))
    );
}

#[test]
fn date_headers_feed_the_server_clock_offset() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    assert!(m.server_clock_offset_ms().is_none());
    let p = payload("aa77", FindKind::Xen11, 100_000);
    let _id = j.append(&p).unwrap();
    let mut r = ok(200, OK_200);
    r.date_header = Some("Thu, 01 Jan 2026 00:01:30 GMT".into());
    t.push_submit(r);
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(m.server_clock_offset_ms(), Some(90_000));
}

#[test]
fn outage_opens_the_breaker_recovery_closes_through_a_real_submission() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let network_states = Arc::new(Mutex::new(Vec::new()));
    let ns = Arc::clone(&network_states);
    m.set_network_state_callback(Arc::new(move |s| ns.lock().unwrap().push(s)));
    let p = payload("bb11", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();

    for i in 0..3 {
        if i > 0 {
            clk.advance(60_000);
        }
        t.push_submit(down());
        assert_eq!(m.run_once(), StepResult::Submitted);
    }
    assert_eq!(m.breaker_state(), BreakerState::Open);
    assert_eq!(
        *network_states.lock().unwrap().last().unwrap(),
        BreakerState::Open
    );
    assert_eq!(m.metrics().transport_failures, 3);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(t.submitted_keys().len(), 3);

    clk.advance(6000);
    t.push_difficulty(down());
    assert_eq!(m.run_once(), StepResult::Probed);
    assert_eq!(m.breaker_state(), BreakerState::Open);
    clk.advance(11_000);
    t.push_difficulty(ok(200, r#"{"difficulty": "100000"}"#));
    assert_eq!(m.run_once(), StepResult::Probed);
    assert_eq!(m.breaker_state(), BreakerState::HalfOpen);
    assert_eq!(
        *network_states.lock().unwrap().last().unwrap(),
        BreakerState::HalfOpen
    );
    assert_eq!(m.last_observed_difficulty(), Some(100_000));

    clk.advance(60_000);
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(m.breaker_state(), BreakerState::Closed);
    assert_eq!(
        *network_states.lock().unwrap().last().unwrap(),
        BreakerState::Closed
    );
    assert_eq!(j.record(id).status, FindStatus::Acked);
    assert_eq!(m.drain_rate_per_second() as i32, 1);
}

#[test]
fn adaptive_pacing_gates_back_to_back_submissions() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p1 = payload("cc11", FindKind::Xen11, 100_000);
    let p2 = payload("cc22", FindKind::Xen11, 100_000);
    j.append(&p1).unwrap();
    j.append(&p2).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p1)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(m.run_once(), StepResult::Idle);
    clk.advance(1000);
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p2)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(t.submitted_keys().len(), 2);
}

#[test]
fn window_opening_unparks_xuni_within_budget() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(j.unpark_xuni_calls(), 0);
    clk.set_wall(1_767_228_900_000); // 00:55
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(j.unpark_xuni_calls(), 1);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(j.unpark_xuni_calls(), 1);
}

#[test]
fn closed_window_xuni_backlog_does_not_hide_a_later_xen11() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut cfg = test_config();
    cfg.fetch_limit = 2;
    let mut m = make_mgr(j.clone(), t.clone(), cfg, &clk);
    j.append(&payload("xuni-1", FindKind::Xuni, 100_000))
        .unwrap();
    j.append(&payload("xuni-2", FindKind::Xuni, 100_000))
        .unwrap();
    j.append(&payload("xuni-3", FindKind::Xuni, 100_000))
        .unwrap();
    let xen = payload("xen-1", FindKind::Xen11, 100_000);
    let xen_id = j.append(&xen).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&xen)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(t.submitted_keys()[0], "xen-1");
    assert_eq!(j.record(xen_id).status, FindStatus::Acked);
}

#[test]
fn observed_difficulty_decrease_unparks_records() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    m.observe_difficulty(104000).unwrap();
    assert_eq!(m.difficulty_trend(), crate::DifficultyTrend::Unknown);
    assert_eq!(j.unpark_difficulty_calls(), vec![104000]);
    m.observe_difficulty(106000).unwrap();
    assert_eq!(m.difficulty_trend(), crate::DifficultyTrend::Rising);
    assert_eq!(j.unpark_difficulty_calls().len(), 1);
    m.observe_difficulty(100000).unwrap();
    assert_eq!(m.difficulty_trend(), crate::DifficultyTrend::Falling);
    assert_eq!(j.unpark_difficulty_calls(), vec![104000, 100000]);
}

#[test]
fn confirmation_retry_succeeds_to_acked() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("dd11", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    j.set_status(id, FindStatus::AcceptedUnconfirmed);
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::Acked);
    assert!(j.record(id).confirmed_at.is_some());
    assert!(t.submitted_keys().is_empty());
    assert_eq!(m.metrics().confirmation_retries, 1);
    assert_eq!(m.metrics().reconciled_via_get_block, 1);
}

#[test]
fn confirmation_retry_finds_404_pending() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("dd22", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    j.set_status(id, FindStatus::AcceptedUnconfirmed);
    t.push_confirm(ok(404, r#"{"error": "Data not found for provided key"}"#));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::Pending);
    assert!(j.record(id).next_attempt_at.is_some());
    assert_eq!(m.metrics().lying_200_detected, 1);
    clk.advance(10_000);
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::Acked);
}

#[test]
fn confirmation_retry_transport_down_stays_unconfirmed() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("dd33", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    j.set_status(id, FindStatus::AcceptedUnconfirmed);
    t.push_confirm(down());
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(
        j.record(id).next_attempt_at,
        Some(iso_utc(clk.wall() + 2000))
    );
    assert_eq!(t.confirmed_keys().len(), 1);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(t.confirmed_keys().len(), 1);
    clk.advance(3000);
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::Acked);
}

#[test]
fn breaker_open_suppresses_confirmation_retries() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p1 = payload("ee11", FindKind::Xen11, 100_000);
    j.append(&p1).unwrap();
    for i in 0..3 {
        if i > 0 {
            clk.advance(60_000);
        }
        t.push_submit(down());
        assert_eq!(m.run_once(), StepResult::Submitted);
    }
    assert_eq!(m.breaker_state(), BreakerState::Open);
    let p2 = payload("ee22", FindKind::Xen11, 100_000);
    let u = j.append(&p2).unwrap();
    j.set_status(u, FindStatus::AcceptedUnconfirmed);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert!(t.confirmed_keys().is_empty());
    clk.advance(6000);
    t.push_difficulty(ok(200, r#"{"difficulty": "100000"}"#));
    assert_eq!(m.run_once(), StepResult::Probed);
    clk.advance(60_000);
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p1)));
    t.push_confirm(ok(200, &block_row(&p2)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(u).status, FindStatus::Acked);
}

#[test]
fn xen11_drains_past_a_deep_closed_window_xuni_backlog() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    for i in 0..20 {
        j.append(&payload(&format!("xu{i:02}"), FindKind::Xuni, 100_000))
            .unwrap();
    }
    let xen = payload("xen1", FindKind::Xen11, 100_000);
    j.append(&xen).unwrap();
    t.push_submit(ok(200, r#"{"message": "Block added"}"#));
    t.push_confirm(ok(200, &block_row(&xen)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(t.submitted_keys(), vec!["xen1".to_string()]);
    assert_eq!(j.counts().unwrap().pending, 20);
}

#[test]
fn closing_window_xuni_preempts_past_a_deep_xen11_backlog() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_OPEN + 4 * 60 * 1000);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    for i in 0..20 {
        j.append(&payload(&format!("xe{i:02}"), FindKind::Xen11, 100_000))
            .unwrap();
    }
    let xuni = payload("xuni1", FindKind::Xuni, 100_000);
    j.append(&xuni).unwrap();
    t.push_submit(ok(200, r#"{"message": "Block added"}"#));
    t.push_confirm(ok(200, &block_row(&xuni)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(t.submitted_keys(), vec!["xuni1".to_string()]);
}

#[test]
fn confirmation_matches_validates_key_and_hash_byte_for_byte() {
    let mut rec = FindRecord::default();
    rec.payload = payload("aabb", FindKind::Xen11, 100_000);
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            &block_row(&rec.payload)
        ),
        ConfirmBodyCheck::Confirmed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"{"key": "aabb"}"#
        ),
        ConfirmBodyCheck::Confirmed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(&rec, ""),
        ConfirmBodyCheck::Malformed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            "<html>502</html>"
        ),
        ConfirmBodyCheck::Malformed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(&rec, "OK"),
        ConfirmBodyCheck::Malformed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"[{"key": "aabb"}]"#
        ),
        ConfirmBodyCheck::Malformed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"{"block_id": 7}"#
        ),
        ConfirmBodyCheck::Malformed
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"{"key": "ffff"}"#
        ),
        ConfirmBodyCheck::KeyMismatch
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"{"key": "AABB"}"#
        ),
        ConfirmBodyCheck::KeyMismatch
    );
    assert_eq!(
        SubmissionManager::<FakeJournal, FakeTransport>::confirmation_matches(
            &rec,
            r#"{"key": "aabb", "hash_to_verify": "$argon2id$other"}"#
        ),
        ConfirmBodyCheck::HashMismatch
    );
}

#[test]
fn initial_confirm_wrong_key_stays_unconfirmed_then_recovers() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("ff11", FindKind::Xen11, 100_000);
    let other = payload("attacker", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&other)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(
        j.record(id).next_attempt_at,
        Some(iso_utc(clk.wall() + 2000))
    );
    assert_eq!(m.metrics().acked, 0);
    assert_eq!(m.metrics().reconciled_via_get_block, 0);
    assert_eq!(m.metrics().confirm_body_rejected, 1);
    assert!(j.record(id).status_reason.contains("different key"));
    clk.advance(3000);
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::Acked);
}

#[test]
fn initial_confirm_garbage_body_stays_unconfirmed() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let id = j
        .append(&payload("ff22", FindKind::Xen11, 100_000))
        .unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, "<html><body>captive portal</body></html>"));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert!(j.record(id).next_attempt_at.is_some());
    assert_eq!(m.metrics().acked, 0);
    assert_eq!(m.metrics().confirm_body_rejected, 1);
}

#[test]
fn initial_confirm_json_missing_key_stays_unconfirmed() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let id = j
        .append(&payload("ff33", FindKind::Xen11, 100_000))
        .unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, r#"{"block_id": 7, "account": "0x1111"}"#));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert!(j.record(id).next_attempt_at.is_some());
    assert_eq!(m.metrics().confirm_body_rejected, 1);
}

#[test]
fn initial_confirm_hash_mismatch_is_never_acked() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("ff44", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    let mut impostor = p.clone();
    impostor.hash_to_verify = "$argon2id$v=19$m=100000,t=1,p=1$saltsalt$SOMEONEELSE".into();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&impostor)));
    assert_eq!(m.run_once(), StepResult::Submitted);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert!(j.record(id).next_attempt_at.is_some());
    assert_eq!(m.metrics().acked, 0);
    assert_eq!(m.metrics().confirm_body_rejected, 1);
    assert!(j
        .record(id)
        .status_reason
        .contains("DIFFERENT hash_to_verify"));
}

#[test]
fn confirm_retry_wrong_key_stays_unconfirmed_with_backoff() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("gg11", FindKind::Xen11, 100_000);
    let other = payload("attacker", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    j.set_status(id, FindStatus::AcceptedUnconfirmed);
    t.push_confirm(ok(200, &block_row(&other)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(
        j.record(id).next_attempt_at,
        Some(iso_utc(clk.wall() + 2000))
    );
    assert_eq!(m.metrics().acked, 0);
    assert_eq!(m.metrics().confirm_body_rejected, 1);
    clk.advance(3000);
    t.push_confirm(ok(200, &block_row(&p)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::Acked);
}

#[test]
fn confirm_retry_garbage_and_hash_mismatch_stay_unconfirmed() {
    let j = FakeJournal::new();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let p = payload("gg22", FindKind::Xen11, 100_000);
    let id = j.append(&p).unwrap();
    j.set_status(id, FindStatus::AcceptedUnconfirmed);
    t.push_confirm(ok(200, "not json at all"));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert!(j.record(id).next_attempt_at.is_some());
    let mut impostor = p.clone();
    impostor.hash_to_verify = "$argon2id$v=19$m=100000,t=1,p=1$saltsalt$SOMEONEELSE".into();
    clk.advance(3000);
    t.push_confirm(ok(200, &block_row(&impostor)));
    assert_eq!(m.run_once(), StepResult::ConfirmRetried);
    assert_eq!(j.record(id).status, FindStatus::AcceptedUnconfirmed);
    assert_eq!(m.metrics().acked, 0);
    assert_eq!(m.metrics().confirm_body_rejected, 2);
}

#[test]
fn journal_error_is_contained_fatal_callback_fires_once() {
    let j = FakeJournal::throwing();
    let t = FakeTransport::new();
    let clk = Clocks::new(WALL_CLOSED);
    let mut m = make_mgr(j.clone(), t.clone(), test_config(), &clk);
    let fatals = Arc::new(Mutex::new(Vec::new()));
    let f = Arc::clone(&fatals);
    m.set_fatal_callback(Arc::new(move |what| f.lock().unwrap().push(what)));
    let p = payload("hh11", FindKind::Xen11, 100_000);
    j.append(&p).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    let r = m.run_once();
    assert_eq!(r, StepResult::Idle);
    assert_eq!(j.throw_count(), 1);
    let fatals = fatals.lock().unwrap();
    assert_eq!(fatals.len(), 1);
    assert!(fatals[0].contains("disk I/O error"));
    assert_eq!(m.metrics().thread_loop_exceptions, 1);
    clk.advance(60_000);
    assert_eq!(m.run_once(), StepResult::Idle);
    assert_eq!(j.throw_count(), 1);
    assert_eq!(fatals.len(), 1);
}

#[test]
fn drain_loop_survives_journal_error_and_stop_joins() {
    let j = FakeJournal::throwing();
    let t = FakeTransport::new();
    let p = payload("hh22", FindKind::Xen11, 100_000);
    j.append(&p).unwrap();
    t.push_submit(ok(200, OK_200));
    t.push_confirm(ok(200, &block_row(&p)));
    let m = SubmissionManager::with_config(j.clone(), t.clone(), test_config(), None, None);
    let (tx, rx) = std::sync::mpsc::channel();
    m.set_fatal_callback(Arc::new(move |what| {
        let _ = tx.send(what);
    }));
    let manager = Arc::new(Mutex::new(m));
    let mut handle = DrainHandle::spawn(Arc::clone(&manager));
    let what = rx
        .recv_timeout(Duration::from_secs(10))
        .expect("fatal callback");
    handle.stop();
    assert!(what.contains("disk I/O error"));
    assert_eq!(manager.lock().unwrap().metrics().thread_loop_exceptions, 1);
    assert_eq!(j.throw_count(), 1);
}
