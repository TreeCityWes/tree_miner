//! Submission manager: one scheduling step, confirmation-aware acks, fatal boundary.
//! Port of `src/submit/SubmissionManager.{h,cpp}`. No Crow, cpr, or TUI.

use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use treeminer_journal::{FindJournalApi, JournalError};
use treeminer_protocol::{
    classify_with_retry_after, compute_margin, extract_json_field, parse_difficulty_hint,
    parse_retry_after_seconds, xuni_window_at, Classification, FindKind, FindRecord, FindStatus,
    MarginConfig, MarginInputs, MarginMode, TRANSPORT_ERROR,
};

use crate::breaker::{BreakerState, CircuitBreaker, CircuitBreakerConfig, Clock};
use crate::drain::{DifficultyTrend, DrainScheduler, DrainSchedulerConfig};
use crate::time::{iso_utc, parse_http_date_ms};
use crate::transport::{Transport, TransportResult};

pub type MonoClock = Clock;
pub type WallClock = Clock;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StepResult {
    Idle,
    Probed,
    Submitted,
    BreakerBlocked,
    ConfirmRetried,
}

/// How a `/get_block` 200 body relates to the record it is supposed to confirm.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfirmBodyCheck {
    Confirmed,
    Malformed,
    KeyMismatch,
    HashMismatch,
}

#[derive(Clone, Debug)]
pub struct SubmissionConfig {
    pub fetch_limit: usize,
    pub confirm_fetch_limit: usize,
    pub backoff_base_ms: i64,
    pub backoff_cap_ms: i64,
    pub xuni_max_windows: i32,
    pub idle_poll_ms: i64,
    pub breaker: CircuitBreakerConfig,
    pub drain: DrainSchedulerConfig,
    pub margin: MarginConfig,
    pub margin_eval_interval_ms: i64,
}

impl Default for SubmissionConfig {
    fn default() -> Self {
        Self {
            fetch_limit: 16,
            confirm_fetch_limit: 4,
            backoff_base_ms: 2000,
            backoff_cap_ms: 300_000,
            xuni_max_windows: 3,
            idle_poll_ms: 250,
            breaker: CircuitBreakerConfig::default(),
            drain: DrainSchedulerConfig::default(),
            margin: MarginConfig::default(),
            margin_eval_interval_ms: 5000,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Metrics {
    pub submitted: u64,
    pub resubmitted: u64,
    pub acked: u64,
    pub accepted_unconfirmed: u64,
    pub reconciled_via_get_block: u64,
    pub confirmation_retries: u64,
    pub lying_200_detected: u64,
    pub parked_difficulty: u64,
    pub parked_xuni: u64,
    pub quarantined: u64,
    pub permanently_invalid: u64,
    pub transport_failures: u64,
    pub probes: u64,
    pub margin_changes: u64,
    pub confirm_body_rejected: u64,
    pub thread_loop_exceptions: u64,
}

pub type OutcomeCallback = Arc<dyn Fn(&FindRecord, &Classification, Option<i32>) + Send + Sync>;
pub type NetworkStateCallback = Arc<dyn Fn(BreakerState) + Send + Sync>;
pub type FatalCallback = Arc<dyn Fn(String) + Send + Sync>;
pub type DifficultyHintCallback = Arc<dyn Fn(u32) + Send + Sync>;
pub type MarginCallback = Arc<dyn Fn(u32) + Send + Sync>;

struct Shared {
    difficulty_hint_cb: Option<DifficultyHintCallback>,
    outcome_cb: Option<OutcomeCallback>,
    network_state_cb: Option<NetworkStateCallback>,
    fatal_cb: Option<FatalCallback>,
    margin_cb: Option<MarginCallback>,
    last_difficulty: Option<u32>,
    trend: DifficultyTrend,
    server_offset_ms: Option<i64>,
    metrics: Metrics,
}

fn default_monotonic_ms() -> i64 {
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    let start = START.get_or_init(Instant::now);
    start.elapsed().as_millis() as i64
}

fn default_wall_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn is_blank_body(s: &str) -> bool {
    s.bytes().all(|b| b.is_ascii_whitespace())
}

fn confirm_reject_reason(check: ConfirmBodyCheck) -> &'static str {
    match check {
        ConfirmBodyCheck::KeyMismatch => {
            "/get_block 200 body describes a different key — not a confirmation \
             of this record, remaining unconfirmed"
        }
        ConfirmBodyCheck::HashMismatch => {
            "/get_block 200 has our key but a DIFFERENT hash_to_verify — refusing \
             to ack, remaining unconfirmed"
        }
        _ => {
            "/get_block 200 body malformed or missing key — untrusted, remaining \
             unconfirmed"
        }
    }
}

/// Single-threaded drain of the durable journal. Tests drive `run_once` with injectable clocks.
pub struct SubmissionManager<J, T> {
    journal: J,
    transport: T,
    cfg: SubmissionConfig,
    mono: MonoClock,
    wall: WallClock,
    breaker: CircuitBreaker,
    scheduler: DrainScheduler,
    shared: Mutex<Shared>,
    fatal: AtomicBool,
    next_submit_allowed_ms: i64,
    last_window_open: bool,
    margin_kib: AtomicU32,
    outage_started_ms: AtomicI64,
    last_outage_span_ms: AtomicI64,
    last_margin_eval_ms: i64,
    margin_eval_started: bool,
}

impl<J: FindJournalApi, T: Transport> SubmissionManager<J, T> {
    pub fn new(journal: J, transport: T) -> Self {
        Self::with_config(journal, transport, SubmissionConfig::default(), None, None)
    }

    pub fn with_config(
        journal: J,
        transport: T,
        cfg: SubmissionConfig,
        monotonic: Option<MonoClock>,
        wall: Option<WallClock>,
    ) -> Self {
        let mono: MonoClock = monotonic.unwrap_or_else(|| Arc::new(default_monotonic_ms));
        let wall: WallClock = wall.unwrap_or_else(|| Arc::new(default_wall_ms));
        let breaker = CircuitBreaker::new(cfg.breaker, Arc::clone(&mono));
        let scheduler = DrainScheduler::new(cfg.drain);
        Self {
            journal,
            transport,
            cfg,
            mono,
            wall,
            breaker,
            scheduler,
            shared: Mutex::new(Shared {
                difficulty_hint_cb: None,
                outcome_cb: None,
                network_state_cb: None,
                fatal_cb: None,
                margin_cb: None,
                last_difficulty: None,
                trend: DifficultyTrend::Unknown,
                server_offset_ms: None,
                metrics: Metrics::default(),
            }),
            fatal: AtomicBool::new(false),
            next_submit_allowed_ms: 0,
            last_window_open: false,
            margin_kib: AtomicU32::new(0),
            outage_started_ms: AtomicI64::new(0),
            last_outage_span_ms: AtomicI64::new(0),
            last_margin_eval_ms: 0,
            margin_eval_started: false,
        }
    }

    pub fn journal(&self) -> &J {
        &self.journal
    }

    pub fn transport(&self) -> &T {
        &self.transport
    }

    pub fn transport_mut(&mut self) -> &mut T {
        &mut self.transport
    }

    pub fn set_difficulty_hint_callback(&self, cb: DifficultyHintCallback) {
        self.shared.lock().expect("shared").difficulty_hint_cb = Some(cb);
    }

    pub fn set_outcome_callback(&self, cb: OutcomeCallback) {
        self.shared.lock().expect("shared").outcome_cb = Some(cb);
    }

    pub fn set_network_state_callback(&self, cb: NetworkStateCallback) {
        self.shared.lock().expect("shared").network_state_cb = Some(cb);
    }

    pub fn set_fatal_callback(&self, cb: FatalCallback) {
        self.shared.lock().expect("shared").fatal_cb = Some(cb);
    }

    pub fn set_margin_callback(&self, cb: MarginCallback) {
        self.shared.lock().expect("shared").margin_cb = Some(cb);
    }

    pub fn difficulty_trend(&self) -> DifficultyTrend {
        self.shared.lock().expect("shared").trend
    }

    pub fn last_observed_difficulty(&self) -> Option<u32> {
        self.shared.lock().expect("shared").last_difficulty
    }

    pub fn server_clock_offset_ms(&self) -> Option<i64> {
        self.shared.lock().expect("shared").server_offset_ms
    }

    pub fn metrics(&self) -> Metrics {
        self.shared.lock().expect("shared").metrics
    }

    pub fn breaker_state(&self) -> BreakerState {
        self.breaker.state()
    }

    pub fn drain_rate_per_second(&self) -> f64 {
        self.scheduler.rate_per_second()
    }

    pub fn margin_in_effect(&self) -> u32 {
        self.margin_kib.load(Ordering::Relaxed)
    }

    pub fn last_outage_span_ms(&self) -> i64 {
        self.last_outage_span_ms.load(Ordering::Relaxed)
    }

    pub fn outage_duration_ms(&self) -> i64 {
        let started = self.outage_started_ms.load(Ordering::Relaxed);
        if started == 0 {
            return 0;
        }
        let elapsed = (self.mono)() - started;
        if elapsed > 0 {
            elapsed
        } else {
            0
        }
    }

    pub fn is_fatal(&self) -> bool {
        self.fatal.load(Ordering::SeqCst)
    }

    /// First-observation un-park + falling-floor un-park (PLAN §3.3 / §3.4).
    pub fn observe_difficulty(&mut self, difficulty: u32) -> Result<(), JournalError> {
        let mut decreased = false;
        let mut first_observation = false;
        {
            let mut g = self.shared.lock().expect("shared");
            if let Some(prev) = g.last_difficulty {
                if difficulty > prev {
                    g.trend = DifficultyTrend::Rising;
                } else if difficulty < prev {
                    g.trend = DifficultyTrend::Falling;
                    decreased = true;
                } else {
                    g.trend = DifficultyTrend::Flat;
                }
            } else {
                first_observation = true;
            }
            g.last_difficulty = Some(difficulty);
        }
        if decreased || first_observation {
            self.journal.unpark_for_difficulty(difficulty)?;
        }
        Ok(())
    }

    /// Never panics: fatal journal errors halt the drain and later calls return Idle.
    pub fn run_once(&mut self) -> StepResult {
        if self.fatal.load(Ordering::SeqCst) {
            return StepResult::Idle;
        }
        match self.run_step() {
            Ok(r) => r,
            Err(err) => {
                self.handle_fatal(format!("submission step: {err}"));
                StepResult::Idle
            }
        }
    }

    /// A `/get_block` 200 is only trusted when its body proves this record (finding 4).
    pub fn confirmation_matches(record: &FindRecord, body: &str) -> ConfirmBodyCheck {
        let Some(key) = extract_json_field(body, "key") else {
            return ConfirmBodyCheck::Malformed;
        };
        if key != record.payload.key {
            return ConfirmBodyCheck::KeyMismatch;
        }
        if let Some(hash) = extract_json_field(body, "hash_to_verify") {
            if hash != record.payload.hash_to_verify {
                return ConfirmBodyCheck::HashMismatch;
            }
        }
        ConfirmBodyCheck::Confirmed
    }

    fn handle_fatal(&mut self, what: String) {
        if self.fatal.swap(true, Ordering::SeqCst) {
            return;
        }
        {
            let mut g = self.shared.lock().expect("shared");
            g.metrics.thread_loop_exceptions += 1;
        }
        let cb = self.shared.lock().expect("shared").fatal_cb.clone();
        if let Some(cb) = cb {
            cb(what);
        }
    }

    fn emit_outcome(
        &self,
        record: &FindRecord,
        classification: &Classification,
        http_status: Option<i32>,
    ) {
        let cb = self.shared.lock().expect("shared").outcome_cb.clone();
        if let Some(cb) = cb {
            cb(record, classification, http_status);
        }
    }

    fn emit_network_state(&self) {
        let cb = self.shared.lock().expect("shared").network_state_cb.clone();
        if let Some(cb) = cb {
            cb(self.breaker.state());
        }
    }

    fn emit_difficulty_hint(&self, d: u32) {
        let cb = self
            .shared
            .lock()
            .expect("shared")
            .difficulty_hint_cb
            .clone();
        if let Some(cb) = cb {
            cb(d);
        }
    }

    fn run_step(&mut self) -> Result<StepResult, JournalError> {
        self.update_margin()?;
        if self.breaker.state() == BreakerState::Open {
            return self.probe_step();
        }
        let submit_result = self.submit_step()?;
        let confirm_result = self.confirm_step()?;
        if submit_result == StepResult::Idle && confirm_result != StepResult::Idle {
            return Ok(confirm_result);
        }
        Ok(submit_result)
    }

    fn update_margin(&mut self) -> Result<(), JournalError> {
        let now = (self.mono)();
        let open = self.breaker.state() == BreakerState::Open;
        if open {
            if self.outage_started_ms.load(Ordering::Relaxed) == 0 {
                self.outage_started_ms.store(now, Ordering::Relaxed);
            }
        } else {
            let started = self.outage_started_ms.load(Ordering::Relaxed);
            if started != 0 {
                let span = now - started;
                self.last_outage_span_ms
                    .store(if span > 0 { span } else { 0 }, Ordering::Relaxed);
            }
            self.outage_started_ms.store(0, Ordering::Relaxed);
        }

        if self.cfg.margin.mode == MarginMode::Off {
            return Ok(());
        }
        if self.margin_eval_started
            && (now - self.last_margin_eval_ms) < self.cfg.margin_eval_interval_ms
        {
            return Ok(());
        }
        self.last_margin_eval_ms = now;
        self.margin_eval_started = true;

        let mut inputs = MarginInputs {
            breaker_open: open,
            outage_ms: if open {
                now - self.outage_started_ms.load(Ordering::Relaxed)
            } else {
                0
            },
            backlog: 0,
        };
        if self.cfg.margin.mode == MarginMode::Auto {
            let c = self.journal.counts()?;
            inputs.backlog = c.pending + c.parked + c.accepted_unconfirmed + c.quarantined;
        }
        let next = compute_margin(self.cfg.margin, inputs);
        if next == self.margin_kib.load(Ordering::Relaxed) {
            return Ok(());
        }
        self.margin_kib.store(next, Ordering::Relaxed);
        {
            let mut g = self.shared.lock().expect("shared");
            g.metrics.margin_changes += 1;
        }
        let cb = self.shared.lock().expect("shared").margin_cb.clone();
        if let Some(cb) = cb {
            cb(next);
        }
        Ok(())
    }

    fn track_server_date(&self, r: &TransportResult) {
        if !r.transport_ok {
            return;
        }
        let Some(header) = r.date_header.as_deref() else {
            return;
        };
        if let Some(server_ms) = parse_http_date_ms(header) {
            self.shared.lock().expect("shared").server_offset_ms = Some(server_ms - (self.wall)());
        }
    }

    fn handle_difficulty_body(&mut self, body: &str) -> Result<(), JournalError> {
        let Some(field) = extract_json_field(body, "difficulty") else {
            return Ok(());
        };
        let Some(d) = parse_difficulty_hint(&format!("m={field}")) else {
            return Ok(());
        };
        self.observe_difficulty(d)?;
        self.journal.record_difficulty(d, &iso_utc((self.wall)()))?;
        self.emit_difficulty_hint(d);
        Ok(())
    }

    fn backoff_time_iso(&self, attempt_count: i32, retry_after_s: Option<i64>) -> String {
        let mut delay = self.cfg.backoff_base_ms;
        if let Some(secs) = retry_after_s {
            delay = secs * 1000;
        } else {
            let mut i = 0;
            while i < attempt_count && delay < self.cfg.backoff_cap_ms {
                delay *= 2;
                i += 1;
            }
        }
        delay = delay.min(self.cfg.backoff_cap_ms);
        iso_utc((self.wall)() + delay)
    }

    fn probe_step(&mut self) -> Result<StepResult, JournalError> {
        if !self.breaker.probe_due() {
            return Ok(StepResult::Idle);
        }
        let r = self.transport.difficulty();
        self.track_server_date(&r);
        {
            let mut g = self.shared.lock().expect("shared");
            g.metrics.probes += 1;
        }
        if r.transport_ok && r.http_status == 200 && !is_blank_body(&r.body) {
            self.handle_difficulty_body(&r.body)?;
            self.breaker.on_probe_success();
        } else {
            self.breaker.on_probe_failure();
        }
        self.emit_network_state();
        Ok(StepResult::Probed)
    }

    fn submit_step(&mut self) -> Result<StepResult, JournalError> {
        let now_mono = (self.mono)();
        if now_mono < self.next_submit_allowed_ms {
            return Ok(StepResult::Idle);
        }

        let offset = self
            .shared
            .lock()
            .expect("shared")
            .server_offset_ms
            .unwrap_or(0);
        let window = xuni_window_at((self.wall)() + offset);
        if window.open && !self.last_window_open {
            self.journal
                .unpark_xuni_for_window(self.cfg.xuni_max_windows)?;
        }
        self.last_window_open = window.open;

        let now_iso = iso_utc((self.wall)());
        let mut eligible =
            self.journal
                .fetch_eligible_of_kind(FindKind::Xen11, &now_iso, self.cfg.fetch_limit)?;
        let mut xuni_pressure = false;
        if window.open {
            let xuni = self.journal.fetch_eligible_of_kind(
                FindKind::Xuni,
                &now_iso,
                self.cfg.fetch_limit,
            )?;
            xuni_pressure = !xuni.is_empty();
            eligible.extend(xuni);
        }
        self.breaker.set_xuni_pressure(xuni_pressure);
        if eligible.is_empty() {
            return Ok(StepResult::Idle);
        }

        let trend = self.difficulty_trend();
        let rec = match self.scheduler.select_next(&eligible, trend, window) {
            Some(r) => r.clone(),
            None => return Ok(StepResult::Idle),
        };
        if !self.breaker.try_admit() {
            return Ok(StepResult::BreakerBlocked);
        }

        let was_closed = self.breaker.state() == BreakerState::Closed;
        let res = self.transport.submit(&rec.payload);
        self.track_server_date(&res);
        let status = if res.transport_ok {
            res.http_status
        } else {
            TRANSPORT_ERROR
        };
        let mut c = classify_with_retry_after(
            status,
            &res.body,
            rec.payload.kind,
            res.retry_after.as_deref(),
        );
        let known_difficulty_before_response = self.last_observed_difficulty();

        if let Some(hint) = c.server_difficulty_hint {
            self.observe_difficulty(hint)?;
            self.journal
                .record_difficulty(hint, &iso_utc((self.wall)()))?;
            self.emit_difficulty_hint(hint);
        }
        let _ = known_difficulty_before_response;

        let mut next_attempt: Option<String> = None;
        if c.needs_lookup_confirmation {
            let conf = self.transport.confirm(&rec.payload.key);
            self.track_server_date(&conf);
            let body_check = if conf.transport_ok && conf.http_status == 200 {
                Self::confirmation_matches(&rec, &conf.body)
            } else {
                ConfirmBodyCheck::Malformed
            };
            if conf.transport_ok
                && conf.http_status == 200
                && body_check == ConfirmBodyCheck::Confirmed
            {
                c.next_status = FindStatus::Acked;
                c.needs_lookup_confirmation = false;
                c.reason
                    .push_str("; confirmed via /get_block (body matches key)");
                self.shared
                    .lock()
                    .expect("shared")
                    .metrics
                    .reconciled_via_get_block += 1;
            } else if conf.transport_ok && conf.http_status == 200 {
                c.reason.push_str("; ");
                c.reason.push_str(confirm_reject_reason(body_check));
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
                self.shared
                    .lock()
                    .expect("shared")
                    .metrics
                    .confirm_body_rejected += 1;
            } else if conf.transport_ok && conf.http_status == 404 {
                c.next_status = FindStatus::Pending;
                c.needs_lookup_confirmation = false;
                c.reason.push_str(
                    "; /get_block says ABSENT — server 200 was not durable, resubmitting",
                );
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
                self.shared
                    .lock()
                    .expect("shared")
                    .metrics
                    .lying_200_detected += 1;
            } else {
                c.reason
                    .push_str("; /get_block unavailable, remaining unconfirmed");
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
            }
        }

        if c.next_status == FindStatus::Pending && next_attempt.is_none() {
            let retry_after_s = if status == 429 {
                res.retry_after
                    .as_deref()
                    .and_then(parse_retry_after_seconds)
            } else {
                None
            };
            next_attempt = Some(self.backoff_time_iso(rec.attempt_count, retry_after_s));
        }

        let http_status = if res.transport_ok {
            Some(res.http_status)
        } else {
            None
        };
        self.journal.record_attempt(
            rec.id,
            &c,
            http_status,
            &res.body,
            next_attempt.as_deref(),
            &iso_utc((self.wall)()),
        )?;
        self.emit_outcome(&rec, &c, http_status);

        let transport_failure = !res.transport_ok
            || res.http_status >= 500
            || res.http_status == 408
            || res.http_status == 425
            || is_blank_body(&res.body);
        let accepted =
            c.next_status == FindStatus::Acked || c.next_status == FindStatus::AcceptedUnconfirmed;
        if accepted {
            self.breaker.on_verify_success();
            if was_closed {
                self.scheduler.on_healthy_round_trip();
            } else {
                self.scheduler.on_breaker_close();
            }
        } else if transport_failure {
            self.breaker.on_verify_transport_failure();
            self.scheduler.on_throttle();
        } else if res.http_status == 429 {
            self.breaker.on_verify_inconclusive();
            self.scheduler.on_throttle();
        } else {
            self.breaker.on_verify_inconclusive();
            self.scheduler.on_healthy_round_trip();
        }
        self.emit_network_state();
        self.next_submit_allowed_ms = now_mono + self.scheduler.submit_interval_ms();

        {
            let mut g = self.shared.lock().expect("shared");
            g.metrics.submitted += 1;
            if rec.attempt_count > 0 {
                g.metrics.resubmitted += 1;
            }
            if !res.transport_ok {
                g.metrics.transport_failures += 1;
            }
            match c.next_status {
                FindStatus::Acked => g.metrics.acked += 1,
                FindStatus::AcceptedUnconfirmed => g.metrics.accepted_unconfirmed += 1,
                FindStatus::ParkedDifficulty => g.metrics.parked_difficulty += 1,
                FindStatus::ParkedXuniWindow => g.metrics.parked_xuni += 1,
                FindStatus::Quarantined => g.metrics.quarantined += 1,
                FindStatus::PermanentlyInvalid => g.metrics.permanently_invalid += 1,
                _ => {}
            }
        }
        Ok(StepResult::Submitted)
    }

    fn confirm_step(&mut self) -> Result<StepResult, JournalError> {
        if self.breaker.state() == BreakerState::Open {
            return Ok(StepResult::Idle);
        }
        let batch = self
            .journal
            .fetch_awaiting_confirmation(&iso_utc((self.wall)()), self.cfg.confirm_fetch_limit)?;
        if batch.is_empty() {
            return Ok(StepResult::Idle);
        }
        let mut any = false;
        for rec in batch {
            let conf = self.transport.confirm(&rec.payload.key);
            self.track_server_date(&conf);

            let body_check = if conf.transport_ok && conf.http_status == 200 {
                Self::confirmation_matches(&rec, &conf.body)
            } else {
                ConfirmBodyCheck::Malformed
            };

            let mut c = Classification::pending(String::new());
            let mut next_attempt: Option<String> = None;
            if conf.transport_ok
                && conf.http_status == 200
                && body_check == ConfirmBodyCheck::Confirmed
            {
                c.next_status = FindStatus::Acked;
                c.reason = "confirmed via /get_block (retry, body matches key)".into();
                let mut g = self.shared.lock().expect("shared");
                g.metrics.acked += 1;
                g.metrics.reconciled_via_get_block += 1;
            } else if conf.transport_ok && conf.http_status == 200 {
                c.next_status = FindStatus::AcceptedUnconfirmed;
                c.reason = confirm_reject_reason(body_check).into();
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
                self.shared
                    .lock()
                    .expect("shared")
                    .metrics
                    .confirm_body_rejected += 1;
            } else if conf.transport_ok && conf.http_status == 404 {
                c.next_status = FindStatus::Pending;
                c.reason =
                    "/get_block says ABSENT — server 200 was not durable, resubmitting".into();
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
                self.shared
                    .lock()
                    .expect("shared")
                    .metrics
                    .lying_200_detected += 1;
            } else {
                c.next_status = FindStatus::AcceptedUnconfirmed;
                c.reason = "/get_block unavailable, remaining unconfirmed (retry backoff)".into();
                next_attempt = Some(self.backoff_time_iso(rec.attempt_count, None));
            }

            let http_status = if conf.transport_ok {
                Some(conf.http_status)
            } else {
                None
            };
            self.journal.record_attempt(
                rec.id,
                &c,
                http_status,
                &conf.body,
                next_attempt.as_deref(),
                &iso_utc((self.wall)()),
            )?;
            self.emit_outcome(&rec, &c, http_status);
            self.shared
                .lock()
                .expect("shared")
                .metrics
                .confirmation_retries += 1;
            any = true;
            if !conf.transport_ok {
                break;
            }
        }
        Ok(if any {
            StepResult::ConfirmRetried
        } else {
            StepResult::Idle
        })
    }
}

/// Dedicated OS thread + current-thread Tokio runtime driving `run_once`.
pub struct DrainHandle {
    running: Arc<AtomicBool>,
    wake: Arc<tokio::sync::Notify>,
    thread: Option<JoinHandle<()>>,
}

impl DrainHandle {
    pub fn spawn<J, T>(manager: Arc<Mutex<SubmissionManager<J, T>>>) -> Self
    where
        J: FindJournalApi + Send + 'static,
        T: Transport + Send + 'static,
    {
        let running = Arc::new(AtomicBool::new(true));
        let wake = Arc::new(tokio::sync::Notify::new());
        let running_t = Arc::clone(&running);
        let wake_t = Arc::clone(&wake);
        let idle_poll = manager.lock().expect("manager").cfg.idle_poll_ms.max(0) as u64;
        let thread = std::thread::Builder::new()
            .name("treeminer-submit".into())
            .spawn(move || {
                let rt = tokio::runtime::Builder::new_current_thread()
                    .enable_time()
                    .build()
                    .expect("tokio current-thread runtime");
                rt.block_on(async move {
                    while running_t.load(Ordering::SeqCst) {
                        let result = manager.lock().expect("manager").run_once();
                        if !running_t.load(Ordering::SeqCst)
                            || manager.lock().expect("manager").is_fatal()
                        {
                            break;
                        }
                        let wait_ms = if result == StepResult::Idle {
                            idle_poll
                        } else {
                            idle_poll.min(50)
                        };
                        tokio::select! {
                            _ = tokio::time::sleep(Duration::from_millis(wait_ms)) => {}
                            _ = wake_t.notified() => {}
                        }
                    }
                });
            })
            .expect("spawn submit thread");
        Self {
            running,
            wake,
            thread: Some(thread),
        }
    }

    pub fn notify_find_appended(&self) {
        self.wake.notify_one();
    }

    pub fn stop(&mut self) {
        self.running.store(false, Ordering::SeqCst);
        self.wake.notify_waiters();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for DrainHandle {
    fn drop(&mut self) {
        self.stop();
    }
}
