//! Difficulty-margin policy. Port of `src/treeminer/MarginPolicy.h` +
//! `src/submit/MarginPolicy.cpp`.
//!
//! Auto mode costs nothing while healthy (breaker closed AND backlog empty). One step
//! immediately when degraded, plus one step per 300 s difficulty-adjustment period of
//! outage, capped at `max_kib`. Fixed mode is never clamped by that ceiling.

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MarginMode {
    Off,
    Fixed,
    Auto,
}

impl MarginMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Fixed => "fixed",
            Self::Auto => "auto",
        }
    }
}

impl Default for MarginMode {
    fn default() -> Self {
        Self::Off
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MarginConfig {
    pub mode: MarginMode,
    pub margin_kib: u32,
    pub max_kib: u32,
    pub adjust_period_ms: i64,
}

impl Default for MarginConfig {
    fn default() -> Self {
        Self {
            mode: MarginMode::Off,
            margin_kib: 1000,
            max_kib: 5000,
            adjust_period_ms: 300_000,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub struct MarginInputs {
    pub breaker_open: bool,
    pub outage_ms: i64,
    pub backlog: usize,
}

pub fn compute_margin(cfg: MarginConfig, inputs: MarginInputs) -> u32 {
    match cfg.mode {
        MarginMode::Off => 0,
        MarginMode::Fixed => cfg.margin_kib,
        MarginMode::Auto => {
            if !inputs.breaker_open && inputs.backlog == 0 {
                return 0;
            }
            let mut steps: u64 = 1;
            if inputs.breaker_open && cfg.adjust_period_ms > 0 && inputs.outage_ms > 0 {
                steps += (inputs.outage_ms / cfg.adjust_period_ms) as u64;
            }
            let margin = u64::from(cfg.margin_kib).saturating_mul(steps);
            margin.min(u64::from(cfg.max_kib)) as u32
        }
    }
}

/// `"off" | "fixed" | "auto"` (case-insensitive), plus documented aliases.
/// Returns `None` on unknown text so a typo is never silently remapped.
pub fn parse_margin_mode(text: &str) -> Option<MarginMode> {
    match text.to_ascii_lowercase().as_str() {
        "off" | "none" | "disabled" => Some(MarginMode::Off),
        "fixed" | "static" | "constant" => Some(MarginMode::Fixed),
        "auto" | "adaptive" => Some(MarginMode::Auto),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn auto_config() -> MarginConfig {
        MarginConfig {
            mode: MarginMode::Auto,
            ..MarginConfig::default()
        }
    }

    fn healthy() -> MarginInputs {
        MarginInputs::default()
    }

    fn outage(ms: i64) -> MarginInputs {
        MarginInputs {
            breaker_open: true,
            outage_ms: ms,
            backlog: 0,
        }
    }

    #[test]
    fn off_mode_never_adds_headroom() {
        let cfg = MarginConfig::default();
        assert_eq!(cfg.mode, MarginMode::Off);
        assert_eq!(compute_margin(cfg, healthy()), 0);
        assert_eq!(compute_margin(cfg, outage(3_600_000)), 0);
        let mut backlogged = healthy();
        backlogged.backlog = 500;
        assert_eq!(compute_margin(cfg, backlogged), 0);
    }

    #[test]
    fn fixed_mode_is_constant_regardless_of_health() {
        let cfg = MarginConfig {
            mode: MarginMode::Fixed,
            margin_kib: 2500,
            ..MarginConfig::default()
        };
        assert_eq!(compute_margin(cfg, healthy()), 2500);
        assert_eq!(compute_margin(cfg, outage(9_999_999)), 2500);
    }

    #[test]
    fn fixed_mode_is_not_clamped_by_the_auto_ceiling() {
        let cfg = MarginConfig {
            mode: MarginMode::Fixed,
            margin_kib: 9000,
            max_kib: 5000,
            ..MarginConfig::default()
        };
        assert_eq!(compute_margin(cfg, healthy()), 9000);
    }

    #[test]
    fn auto_mode_costs_nothing_while_healthy_and_drained() {
        assert_eq!(compute_margin(auto_config(), healthy()), 0);
    }

    #[test]
    fn auto_mode_buys_one_step_the_moment_the_breaker_opens() {
        assert_eq!(compute_margin(auto_config(), outage(0)), 1000);
        assert_eq!(compute_margin(auto_config(), outage(1)), 1000);
    }

    #[test]
    fn auto_ramp_adds_one_step_per_difficulty_adjustment_period() {
        let cfg = auto_config();
        assert_eq!(compute_margin(cfg, outage(299_999)), 1000);
        assert_eq!(compute_margin(cfg, outage(300_000)), 2000);
        assert_eq!(compute_margin(cfg, outage(600_000)), 3000);
        assert_eq!(compute_margin(cfg, outage(900_000)), 4000);
    }

    #[test]
    fn auto_ramp_stops_at_the_ceiling() {
        let cfg = auto_config();
        assert_eq!(compute_margin(cfg, outage(1_200_000)), 5000);
        assert_eq!(compute_margin(cfg, outage(1_500_000)), 5000);
        assert_eq!(compute_margin(cfg, outage(86_400_000)), 5000);
    }

    #[test]
    fn auto_mode_holds_headroom_while_a_backlog_drains() {
        let mut recovering = healthy();
        recovering.backlog = 1;
        assert_eq!(compute_margin(auto_config(), recovering), 1000);
        recovering.backlog = 10_000;
        assert_eq!(compute_margin(auto_config(), recovering), 1000);
    }

    #[test]
    fn auto_mode_returns_to_zero_once_the_backlog_clears() {
        let mut drained = healthy();
        drained.backlog = 0;
        assert_eq!(compute_margin(auto_config(), drained), 0);
    }

    #[test]
    fn step_size_is_configurable_and_multiplies_through_the_ramp() {
        let cfg = MarginConfig {
            mode: MarginMode::Auto,
            margin_kib: 500,
            max_kib: 100_000,
            adjust_period_ms: 300_000,
        };
        assert_eq!(compute_margin(cfg, outage(0)), 500);
        assert_eq!(compute_margin(cfg, outage(300_000)), 1000);
        assert_eq!(compute_margin(cfg, outage(1_500_000)), 3000);
    }

    #[test]
    fn zero_step_size_yields_no_headroom_even_while_degraded() {
        let cfg = MarginConfig {
            margin_kib: 0,
            ..auto_config()
        };
        assert_eq!(compute_margin(cfg, outage(600_000)), 0);
    }

    #[test]
    fn a_huge_step_cannot_overflow_the_returned_headroom() {
        let cfg = MarginConfig {
            mode: MarginMode::Auto,
            margin_kib: 4_000_000_000,
            max_kib: 5000,
            adjust_period_ms: 300_000,
        };
        assert_eq!(compute_margin(cfg, outage(3_000_000)), 5000);
    }

    #[test]
    fn a_zero_adjust_period_does_not_divide_by_zero() {
        let cfg = MarginConfig {
            adjust_period_ms: 0,
            ..auto_config()
        };
        assert_eq!(compute_margin(cfg, outage(600_000)), 1000);
    }

    #[test]
    fn margin_mode_parses_the_documented_spellings() {
        assert_eq!(parse_margin_mode("off"), Some(MarginMode::Off));
        assert_eq!(parse_margin_mode("AUTO"), Some(MarginMode::Auto));
        assert_eq!(parse_margin_mode("Fixed"), Some(MarginMode::Fixed));
        assert_eq!(parse_margin_mode("adaptive"), Some(MarginMode::Auto));
        assert_eq!(parse_margin_mode("none"), Some(MarginMode::Off));
        assert_eq!(parse_margin_mode("static"), Some(MarginMode::Fixed));
    }

    #[test]
    fn an_unknown_margin_mode_is_rejected_never_defaulted() {
        assert!(parse_margin_mode("aggressive").is_none());
        assert!(parse_margin_mode("").is_none());
        assert!(parse_margin_mode("1000").is_none());
    }

    #[test]
    fn mode_round_trips_through_as_str() {
        assert_eq!(MarginMode::Off.as_str(), "off");
        assert_eq!(MarginMode::Fixed.as_str(), "fixed");
        assert_eq!(MarginMode::Auto.as_str(), "auto");
        assert_eq!(
            parse_margin_mode(MarginMode::Auto.as_str()),
            Some(MarginMode::Auto)
        );
    }
}
