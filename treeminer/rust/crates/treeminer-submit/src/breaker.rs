//! Circuit breaker for the `/verify` path.
//! Port of `src/submit/CircuitBreaker.{h,cpp}`.

use std::sync::Arc;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BreakerState {
    Closed,
    Open,
    HalfOpen,
}

#[derive(Clone, Copy, Debug)]
pub struct CircuitBreakerConfig {
    /// Consecutive transport/5xx failures to open.
    pub failure_threshold: i32,
    /// First probe delay after opening.
    pub probe_base_ms: i64,
    /// Normal probe-interval ceiling.
    pub probe_cap_ms: i64,
    /// Ceiling while an eligible XUNI exists.
    pub probe_cap_xuni_ms: i64,
    /// Adds up to +`jitter_fraction` of the interval.
    pub jitter_fraction: f64,
}

impl Default for CircuitBreakerConfig {
    fn default() -> Self {
        Self {
            failure_threshold: 3,
            probe_base_ms: 5000,
            probe_cap_ms: 60_000,
            probe_cap_xuni_ms: 5000,
            jitter_fraction: 0.2,
        }
    }
}

pub type Clock = Arc<dyn Fn() -> i64 + Send + Sync>;
pub type Jitter = Arc<dyn Fn() -> f64 + Send + Sync>;

/// Protects `/verify` during server outages. Not internally synchronized; the
/// `SubmissionManager` drives it from a single thread.
pub struct CircuitBreaker {
    cfg: CircuitBreakerConfig,
    clock: Clock,
    jitter: Jitter,
    state: BreakerState,
    consecutive_failures: i32,
    probe_interval_ms: i64,
    next_probe_at_ms: i64,
    admission_available: bool,
    xuni_pressure: bool,
}

impl CircuitBreaker {
    pub fn new(cfg: CircuitBreakerConfig, clock: Clock) -> Self {
        Self::with_jitter(cfg, clock, Arc::new(|| 0.0))
    }

    pub fn with_jitter(cfg: CircuitBreakerConfig, clock: Clock, jitter: Jitter) -> Self {
        Self {
            cfg,
            clock,
            jitter,
            state: BreakerState::Closed,
            consecutive_failures: 0,
            probe_interval_ms: 0,
            next_probe_at_ms: 0,
            admission_available: false,
            xuni_pressure: false,
        }
    }

    pub fn state(&self) -> BreakerState {
        self.state
    }

    pub fn consecutive_failures(&self) -> i32 {
        self.consecutive_failures
    }

    pub fn next_probe_at_ms(&self) -> i64 {
        self.next_probe_at_ms
    }

    pub fn probe_due(&self) -> bool {
        self.state == BreakerState::Open && (self.clock)() >= self.next_probe_at_ms
    }

    pub fn on_probe_success(&mut self) {
        if self.state != BreakerState::Open {
            return;
        }
        self.state = BreakerState::HalfOpen;
        self.admission_available = true;
    }

    pub fn on_probe_failure(&mut self) {
        if self.state != BreakerState::Open {
            return;
        }
        self.probe_interval_ms = (self.probe_interval_ms * 2).min(self.cfg.probe_cap_ms);
        self.schedule_probe();
    }

    /// CLOSED: always true. HALF_OPEN: true exactly once until the outcome is reported.
    /// OPEN: always false.
    pub fn try_admit(&mut self) -> bool {
        match self.state {
            BreakerState::Closed => true,
            BreakerState::HalfOpen => {
                if self.admission_available {
                    self.admission_available = false;
                    true
                } else {
                    false
                }
            }
            BreakerState::Open => false,
        }
    }

    pub fn on_verify_success(&mut self) {
        self.state = BreakerState::Closed;
        self.consecutive_failures = 0;
        self.probe_interval_ms = self.cfg.probe_base_ms;
        self.admission_available = false;
    }

    pub fn on_verify_transport_failure(&mut self) {
        if self.state == BreakerState::HalfOpen {
            self.probe_interval_ms =
                (self.probe_interval_ms.max(self.cfg.probe_base_ms) * 2).min(self.cfg.probe_cap_ms);
            self.open();
            return;
        }
        if self.state == BreakerState::Closed {
            self.consecutive_failures += 1;
            if self.consecutive_failures >= self.cfg.failure_threshold {
                self.probe_interval_ms = self.cfg.probe_base_ms;
                self.open();
            }
        }
    }

    pub fn on_verify_inconclusive(&mut self) {
        self.consecutive_failures = 0;
        if self.state == BreakerState::HalfOpen {
            self.admission_available = true;
        }
    }

    pub fn set_xuni_pressure(&mut self, eligible_xuni_exists: bool) {
        let was = self.xuni_pressure;
        self.xuni_pressure = eligible_xuni_exists;
        if !was && self.xuni_pressure && self.state == BreakerState::Open {
            let latest = (self.clock)() + self.active_cap_ms();
            self.next_probe_at_ms = self.next_probe_at_ms.min(latest);
        }
    }

    fn open(&mut self) {
        self.state = BreakerState::Open;
        self.admission_available = false;
        self.schedule_probe();
    }

    fn schedule_probe(&mut self) {
        let cap = self.active_cap_ms();
        let base = self.probe_interval_ms.min(cap);
        let mut delay = base + ((self.jitter)() * self.cfg.jitter_fraction * base as f64) as i64;
        if self.xuni_pressure {
            delay = delay.min(cap);
        }
        self.next_probe_at_ms = (self.clock)() + delay;
    }

    fn active_cap_ms(&self) -> i64 {
        if self.xuni_pressure {
            self.cfg.probe_cap_ms.min(self.cfg.probe_cap_xuni_ms)
        } else {
            self.cfg.probe_cap_ms
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicI64, Ordering};

    fn make_breaker(now: &Arc<AtomicI64>, cfg: CircuitBreakerConfig) -> CircuitBreaker {
        let n = Arc::clone(now);
        CircuitBreaker::new(cfg, Arc::new(move || n.load(Ordering::SeqCst)))
    }

    #[test]
    fn opens_after_3_consecutive_transport_failures() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        assert_eq!(b.state(), BreakerState::Closed);
        assert!(b.try_admit());
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Closed);
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Closed);
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Open);
        assert!(!b.try_admit());
    }

    #[test]
    fn a_success_or_conclusive_response_resets_the_failure_streak() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_success();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Closed);
        b.on_verify_inconclusive();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Closed);
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Open);
    }

    #[test]
    fn probe_schedule_doubles_from_5s_and_caps_at_60s() {
        let now = Arc::new(AtomicI64::new(1000));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Open);
        assert_eq!(b.next_probe_at_ms(), 1000 + 5000);
        assert!(!b.probe_due());
        now.store(1000 + 5000, Ordering::SeqCst);
        assert!(b.probe_due());
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 10_000);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 20_000);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 40_000);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 60_000);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 60_000);
    }

    #[test]
    fn jitter_adds_up_to_jitter_fraction_of_the_interval() {
        let now = Arc::new(AtomicI64::new(0));
        let n = Arc::clone(&now);
        let mut cfg = CircuitBreakerConfig::default();
        cfg.jitter_fraction = 0.2;
        let mut b = CircuitBreaker::with_jitter(
            cfg,
            Arc::new(move || n.load(Ordering::SeqCst)),
            Arc::new(|| 0.5),
        );
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        assert_eq!(b.next_probe_at_ms(), 5500);
    }

    #[test]
    fn xuni_pressure_caps_probes_at_5s_and_pulls_in_far_probes() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 20_000);
        b.set_xuni_pressure(true);
        assert!(b.next_probe_at_ms() <= now.load(Ordering::SeqCst) + 5000);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert!(b.next_probe_at_ms() <= now.load(Ordering::SeqCst) + 5000);
        b.set_xuni_pressure(false);
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_failure();
        assert!(b.next_probe_at_ms() > now.load(Ordering::SeqCst) + 5000);
    }

    #[test]
    fn half_open_admits_one_submission() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        assert!(b.probe_due());
        b.on_probe_success();
        assert_eq!(b.state(), BreakerState::HalfOpen);
        assert!(b.try_admit());
        assert!(!b.try_admit());
    }

    #[test]
    fn half_open_closes_only_on_verification_success() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_success();
        assert!(b.try_admit());
        b.on_verify_inconclusive();
        assert_eq!(b.state(), BreakerState::HalfOpen);
        assert!(b.try_admit());
        b.on_verify_success();
        assert_eq!(b.state(), BreakerState::Closed);
        assert!(b.try_admit());
    }

    #[test]
    fn half_open_transport_failure_reopens_with_escalated_interval() {
        let now = Arc::new(AtomicI64::new(0));
        let mut b = make_breaker(&now, CircuitBreakerConfig::default());
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        b.on_verify_transport_failure();
        now.store(b.next_probe_at_ms(), Ordering::SeqCst);
        b.on_probe_success();
        assert!(b.try_admit());
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Open);
        assert!(!b.try_admit());
        assert_eq!(b.next_probe_at_ms(), now.load(Ordering::SeqCst) + 10_000);
    }

    #[test]
    fn custom_threshold_is_honored() {
        let now = Arc::new(AtomicI64::new(0));
        let mut cfg = CircuitBreakerConfig::default();
        cfg.failure_threshold = 1;
        let mut b = make_breaker(&now, cfg);
        b.on_verify_transport_failure();
        assert_eq!(b.state(), BreakerState::Open);
    }
}
