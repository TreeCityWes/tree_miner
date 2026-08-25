//! Journal-first mine loop: hash-batch → capture → drain.
//! Replaces the *role* of `main.cpp`'s mine + submit callback. CUDA stays in
//! `kernelrunner.cu` (reached later via the hash C ABI). No Crow, cpr, Boost, or TUI.

use std::collections::BTreeMap;

use treeminer_hash::{HashBackend, HashMatch, HashRequest, MIN_ARGON2_CPU_DIFFICULTY};
use treeminer_journal::{FallbackSink, FindJournalApi};
use treeminer_protocol::{
    compute_margin, extract_json_field, parse_difficulty_hint, MarginConfig, MarginInputs,
};
use treeminer_submit::{
    iso_utc, BreakerState, StepResult, SubmissionManager, Transport, TransportResult,
};

use crate::host::{build_payload_from_hash, persist_find, CaptureError, CaptureOutcome};

pub const FALLBACK_DIFFICULTY: u32 = 42_069;

#[derive(Clone, Debug)]
pub struct MineParams {
    pub hexsalt: String,
    pub worker: String,
    pub batch_size: usize,
    pub backend: String,
    pub pattern: String,
    pub allow_xuni: bool,
    pub difficulty_override: Option<u32>,
    pub margin: MarginConfig,
}

impl Default for MineParams {
    fn default() -> Self {
        Self {
            hexsalt: String::new(),
            worker: String::new(),
            batch_size: 1,
            backend: "cpu".into(),
            pattern: "XEN11".into(),
            allow_xuni: true,
            difficulty_override: None,
            margin: MarginConfig::default(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct MineReport {
    pub difficulty: u32,
    pub margin_kib: u32,
    pub effective_m: u32,
    pub attempts: usize,
    pub hashrate: f64,
    pub captured: usize,
    pub hash_ok: bool,
    pub hash_error: String,
    pub drain: StepResult,
    pub fatal: bool,
}

/// `--donotupload`: journal only, never put a find on the wire.
#[derive(Default)]
pub struct DiscardTransport;

impl Transport for DiscardTransport {
    fn submit(&mut self, _: &treeminer_protocol::FoundPayload) -> TransportResult {
        TransportResult::down("donotupload")
    }
    fn confirm(&mut self, _: &str) -> TransportResult {
        TransportResult::down("donotupload")
    }
    fn difficulty(&mut self) -> TransportResult {
        TransportResult::down("donotupload")
    }
}

/// Strip optional `0x` and lowercase. Miner address is 40 hex chars (20-byte ETH).
pub fn account_hex(addr: &str) -> Result<String, String> {
    let rest = addr
        .strip_prefix("0x")
        .or_else(|| addr.strip_prefix("0X"))
        .unwrap_or(addr);
    if rest.len() != 40 || !rest.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!(
            "miner address must be 40 hex chars (optional 0x prefix), got {addr:?}"
        ));
    }
    Ok(rest.to_ascii_lowercase())
}

/// gpage.py `{"difficulty": "<N>"}` plus a bare integer body.
pub fn parse_difficulty_body(body: &str) -> Option<u32> {
    if let Some(field) = extract_json_field(body, "difficulty") {
        return parse_difficulty_hint(&format!("m={field}")).or_else(|| field.parse().ok());
    }
    let trimmed = body.trim();
    if trimmed.is_empty() {
        return None;
    }
    parse_difficulty_hint(&format!("m={trimmed}")).or_else(|| trimmed.parse().ok())
}

fn unique_matches(matches: &[HashMatch]) -> Vec<&HashMatch> {
    let mut by_key: BTreeMap<&str, &HashMatch> = BTreeMap::new();
    for m in matches {
        match by_key.get(m.key.as_str()) {
            None => {
                by_key.insert(&m.key, m);
            }
            Some(prev) => {
                if prev.matched_pattern != "XEN11" && m.matched_pattern == "XEN11" {
                    by_key.insert(&m.key, m);
                }
            }
        }
    }
    by_key.into_values().collect()
}

pub fn resolve_difficulty<J, T>(
    manager: &mut SubmissionManager<J, T>,
    override_d: Option<u32>,
    now_ms: i64,
) -> u32
where
    J: FindJournalApi,
    T: Transport,
{
    if let Some(d) = override_d {
        let _ = manager.observe_difficulty(d);
        return d;
    }
    let r = manager.transport_mut().difficulty();
    if r.transport_ok && r.http_status == 200 {
        if let Some(d) = parse_difficulty_body(&r.body) {
            let _ = manager.observe_difficulty(d);
            let _ = manager.journal().record_difficulty(d, &iso_utc(now_ms));
            return d;
        }
    }
    if let Some(d) = manager.last_observed_difficulty() {
        return d;
    }
    if let Ok(Some(d)) = manager.journal().last_known_difficulty() {
        return d;
    }
    FALLBACK_DIFFICULTY
}

fn effective_memory_cost(difficulty: u32, margin_kib: u32, backend: &str) -> u32 {
    let mut m = difficulty.saturating_add(margin_kib);
    if (backend == "cpu" || backend == "reference") && m < MIN_ARGON2_CPU_DIFFICULTY {
        m = MIN_ARGON2_CPU_DIFFICULTY;
    }
    if m == 0 {
        m = 1;
    }
    m
}

/// One hash-batch, journal-first capture of matches, then one drain step.
pub fn mine_step<H, J, T>(
    hasher: &mut H,
    manager: &mut SubmissionManager<J, T>,
    fallback: &FallbackSink,
    params: &MineParams,
    now_ms: i64,
) -> Result<MineReport, CaptureError>
where
    H: HashBackend,
    J: FindJournalApi,
    T: Transport,
{
    let difficulty = resolve_difficulty(manager, params.difficulty_override, now_ms);
    let backlog = manager
        .journal()
        .counts()
        .map(|c| c.pending + c.parked + c.accepted_unconfirmed)
        .unwrap_or(0);
    let margin_kib = compute_margin(
        params.margin,
        MarginInputs {
            breaker_open: manager.breaker_state() != BreakerState::Closed,
            outage_ms: manager.outage_duration_ms(),
            backlog,
        },
    );
    let effective_m = effective_memory_cost(difficulty, margin_kib, &params.backend);

    let request = HashRequest {
        salt_hex: params.hexsalt.clone(),
        backend: params.backend.clone(),
        target_pattern: params.pattern.clone(),
        difficulty: effective_m,
        batch_size: params.batch_size.max(1),
        allow_xuni: params.allow_xuni,
        ..HashRequest::default()
    };
    let hashed = hasher.run_batch(&request);
    let mut report = MineReport {
        difficulty,
        margin_kib,
        effective_m,
        attempts: hashed.attempts,
        hashrate: hashed.hashrate,
        captured: 0,
        hash_ok: hashed.ok,
        hash_error: hashed.error.clone(),
        drain: StepResult::Idle,
        fatal: false,
    };
    if !hashed.ok {
        return Ok(report);
    }

    for m in unique_matches(&hashed.matches) {
        let payload = build_payload_from_hash(
            &params.hexsalt,
            &m.key,
            &m.hash,
            effective_m,
            hashed.attempts as u64,
            hashed.hashrate,
            &params.worker,
            now_ms,
        )?;
        let outcome = persist_find(manager.journal(), fallback, &payload);
        match outcome {
            CaptureOutcome::Journaled { .. } | CaptureOutcome::Fallback => {
                report.captured += 1;
            }
            CaptureOutcome::Fatal { .. } => {
                report.fatal = true;
                return Ok(report);
            }
        }
    }

    report.drain = manager.run_once();
    report.fatal = manager.is_fatal();
    Ok(report)
}

pub fn format_mine_report(report: &MineReport, step: usize) -> String {
    let mut line = format!(
        "mine step {} m={} (diff={} +margin={}) attempts={} captured={} drain={:?}\n",
        step,
        report.effective_m,
        report.difficulty,
        report.margin_kib,
        report.attempts,
        report.captured,
        report.drain
    );
    if !report.hash_ok {
        line.push_str(&format!("hash error: {}\n", report.hash_error));
    }
    if report.fatal {
        line.push_str("FATAL durability failure\n");
    }
    line
}
