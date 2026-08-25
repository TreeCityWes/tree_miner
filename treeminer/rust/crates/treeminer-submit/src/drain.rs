//! Pure ordering + pacing policy for draining the journal backlog.
//! Port of `src/submit/DrainScheduler.{h,cpp}`.

use treeminer_protocol::{FindKind, FindRecord, XuniWindowState};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DifficultyTrend {
    Unknown,
    Flat,
    Rising,
    Falling,
}

impl Default for DifficultyTrend {
    fn default() -> Self {
        Self::Unknown
    }
}

#[derive(Clone, Copy, Debug)]
pub struct DrainSchedulerConfig {
    pub start_rate_per_s: f64,
    pub max_rate_per_s: f64,
    pub min_rate_per_s: f64,
    pub xuni_preempt_window_ms: i64,
}

impl Default for DrainSchedulerConfig {
    fn default() -> Self {
        Self {
            start_rate_per_s: 1.0,
            max_rate_per_s: 4.0,
            min_rate_per_s: 0.25,
            xuni_preempt_window_ms: 120_000,
        }
    }
}

/// No I/O, no clock, no threads. Same inputs, same answer.
#[derive(Clone, Debug)]
pub struct DrainScheduler {
    cfg: DrainSchedulerConfig,
    rate_per_s: f64,
}

impl Default for DrainScheduler {
    fn default() -> Self {
        Self::new(DrainSchedulerConfig::default())
    }
}

impl DrainScheduler {
    pub fn new(cfg: DrainSchedulerConfig) -> Self {
        let mut rate = cfg.start_rate_per_s;
        if rate <= 0.0 {
            rate = 1.0;
        }
        Self {
            cfg,
            rate_per_s: rate,
        }
    }

    /// Pick the next record to submit from the journal's oldest-first eligible slice.
    /// Returns `None` when nothing should be submitted now.
    pub fn select_next<'a>(
        &self,
        eligible: &'a [FindRecord],
        trend: DifficultyTrend,
        window: XuniWindowState,
    ) -> Option<&'a FindRecord> {
        let mut oldest_xuni: Option<&'a FindRecord> = None;
        let mut best_xen11: Option<&'a FindRecord> = None;

        for r in eligible {
            if r.payload.kind == FindKind::Xuni {
                if window.open && oldest_xuni.is_none() {
                    oldest_xuni = Some(r);
                }
                continue;
            }
            if best_xen11.is_none() {
                best_xen11 = Some(r);
            } else if trend == DifficultyTrend::Rising
                && r.payload.memory_cost < best_xen11.unwrap().payload.memory_cost
            {
                best_xen11 = Some(r);
            }
        }

        if let Some(xuni) = oldest_xuni {
            if window.ms_until_close <= self.cfg.xuni_preempt_window_ms {
                return Some(xuni);
            }
        }
        if let Some(xen) = best_xen11 {
            return Some(xen);
        }
        oldest_xuni
    }

    pub fn on_breaker_close(&mut self) {
        self.rate_per_s = self.cfg.start_rate_per_s;
    }

    pub fn on_healthy_round_trip(&mut self) {
        self.rate_per_s = (self.rate_per_s * 2.0).min(self.cfg.max_rate_per_s);
    }

    pub fn on_throttle(&mut self) {
        self.rate_per_s = (self.rate_per_s / 2.0).max(self.cfg.min_rate_per_s);
    }

    pub fn rate_per_second(&self) -> f64 {
        self.rate_per_s
    }

    pub fn submit_interval_ms(&self) -> i64 {
        (1000.0 / self.rate_per_s) as i64
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use treeminer_protocol::FoundPayload;

    fn rec(id: i64, kind: FindKind, m: u32) -> FindRecord {
        let mut r = FindRecord::default();
        r.id = id;
        r.payload = FoundPayload {
            key: format!("k{id}"),
            kind,
            memory_cost: m,
            ..FoundPayload::default()
        };
        r
    }

    fn window(open: bool, ms_until_close: i64) -> XuniWindowState {
        XuniWindowState {
            open,
            ms_until_close,
        }
    }

    #[test]
    fn empty_backlog_selects_nothing() {
        let sched = DrainScheduler::default();
        assert!(sched
            .select_next(&[], DifficultyTrend::Unknown, window(false, 0))
            .is_none());
    }

    #[test]
    fn oldest_xen11_first_by_default() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 100_000),
            rec(2, FindKind::Xen11, 90_000),
            rec(3, FindKind::Xen11, 95_000),
        ];
        let p = sched.select_next(&v, DifficultyTrend::Unknown, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(1));
        let p = sched.select_next(&v, DifficultyTrend::Falling, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(1));
        let p = sched.select_next(&v, DifficultyTrend::Flat, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(1));
    }

    #[test]
    fn rising_difficulty_drains_ascending_m_first() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 100_000),
            rec(2, FindKind::Xen11, 90_000),
            rec(3, FindKind::Xen11, 95_000),
        ];
        let p = sched.select_next(&v, DifficultyTrend::Rising, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(2));
    }

    #[test]
    fn rising_difficulty_ties_break_oldest_first() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 90_000),
            rec(2, FindKind::Xen11, 90_000),
        ];
        let p = sched.select_next(&v, DifficultyTrend::Rising, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(1));
    }

    #[test]
    fn xuni_never_selected_while_the_window_is_closed() {
        let sched = DrainScheduler::default();
        let mut v = vec![rec(1, FindKind::Xuni, 100_000)];
        assert!(sched
            .select_next(&v, DifficultyTrend::Unknown, window(false, 0))
            .is_none());
        v.push(rec(2, FindKind::Xen11, 100_000));
        let p = sched.select_next(&v, DifficultyTrend::Unknown, window(false, 0));
        assert_eq!(p.map(|r| r.id), Some(2));
    }

    #[test]
    fn xuni_preempts_xen11_near_the_window_end() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 100_000),
            rec(2, FindKind::Xuni, 100_000),
            rec(3, FindKind::Xuni, 100_000),
        ];
        let p = sched.select_next(&v, DifficultyTrend::Unknown, window(true, 60_000));
        assert_eq!(p.map(|r| r.id), Some(2));
    }

    #[test]
    fn xuni_yields_to_xen11_while_the_window_end_is_far() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 100_000),
            rec(2, FindKind::Xuni, 100_000),
        ];
        let p = sched.select_next(&v, DifficultyTrend::Unknown, window(true, 480_000));
        assert_eq!(p.map(|r| r.id), Some(1));
        let only_xuni = vec![rec(2, FindKind::Xuni, 100_000)];
        let p = sched.select_next(&only_xuni, DifficultyTrend::Unknown, window(true, 480_000));
        assert_eq!(p.map(|r| r.id), Some(2));
    }

    #[test]
    fn select_next_is_repeatable() {
        let sched = DrainScheduler::default();
        let v = vec![
            rec(1, FindKind::Xen11, 100_000),
            rec(2, FindKind::Xen11, 90_000),
        ];
        let a = sched.select_next(&v, DifficultyTrend::Rising, window(false, 0));
        let b = sched.select_next(&v, DifficultyTrend::Rising, window(false, 0));
        assert_eq!(a.map(|r| r.id), b.map(|r| r.id));
    }

    #[test]
    fn rate_starts_at_1_per_s_and_doubles_up_to_the_ceiling() {
        let mut s = DrainScheduler::default();
        assert_eq!(s.rate_per_second() as i32, 1);
        assert_eq!(s.submit_interval_ms(), 1000);
        s.on_healthy_round_trip();
        assert_eq!(s.rate_per_second() as i32, 2);
        s.on_healthy_round_trip();
        assert_eq!(s.rate_per_second() as i32, 4);
        s.on_healthy_round_trip();
        assert_eq!(s.rate_per_second() as i32, 4);
        assert_eq!(s.submit_interval_ms(), 250);
    }

    #[test]
    fn throttle_halves_the_rate_down_to_the_floor() {
        let mut s = DrainScheduler::default();
        s.on_healthy_round_trip();
        s.on_healthy_round_trip();
        s.on_throttle();
        assert_eq!((s.rate_per_second() * 100.0) as i32, 200);
        s.on_throttle();
        s.on_throttle();
        s.on_throttle();
        assert_eq!((s.rate_per_second() * 100.0) as i32, 25);
        assert_eq!(s.submit_interval_ms(), 4000);
    }

    #[test]
    fn breaker_close_resets_to_the_start_rate() {
        let mut s = DrainScheduler::default();
        s.on_healthy_round_trip();
        s.on_healthy_round_trip();
        assert_eq!(s.rate_per_second() as i32, 4);
        s.on_breaker_close();
        assert_eq!(s.rate_per_second() as i32, 1);
    }

    #[test]
    fn ceiling_is_configurable() {
        let mut cfg = DrainSchedulerConfig::default();
        cfg.max_rate_per_s = 8.0;
        let mut s = DrainScheduler::new(cfg);
        for _ in 0..6 {
            s.on_healthy_round_trip();
        }
        assert_eq!(s.rate_per_second() as i32, 8);
    }
}
