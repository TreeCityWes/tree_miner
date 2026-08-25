//! Server-clock XUNI `:55–:05` window.
//! Port of `SubmissionManager::xuniWindowAt` (`src/submit/SubmissionManager.cpp`).

pub const MS_PER_HOUR: i64 = 3600 * 1000;
pub const XUNI_OPEN_BEFORE_HOUR_MS: i64 = 5 * 60 * 1000; // :55
pub const XUNI_OPEN_AFTER_HOUR_MS: i64 = 5 * 60 * 1000; // :05

#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub struct XuniWindowState {
    pub open: bool,
    /// Meaningful only when `open`.
    pub ms_until_close: i64,
}

/// Floor division matching the C++ helper (toward −∞), needed so negative epoch
/// offsets still land in the correct hour bucket.
fn floor_div(a: i64, b: i64) -> i64 {
    let q = a / b;
    if a % b != 0 && (a < 0) != (b < 0) {
        q - 1
    } else {
        q
    }
}

/// `server_epoch_ms` is wall time on the *server* clock (local wall + HTTP Date offset).
pub fn xuni_window_at(server_epoch_ms: i64) -> XuniWindowState {
    let into_hour = server_epoch_ms - floor_div(server_epoch_ms, MS_PER_HOUR) * MS_PER_HOUR;
    if into_hour >= MS_PER_HOUR - XUNI_OPEN_BEFORE_HOUR_MS {
        XuniWindowState {
            open: true,
            ms_until_close: (MS_PER_HOUR - into_hour) + XUNI_OPEN_AFTER_HOUR_MS,
        }
    } else if into_hour < XUNI_OPEN_AFTER_HOUR_MS {
        XuniWindowState {
            open: true,
            ms_until_close: XUNI_OPEN_AFTER_HOUR_MS - into_hour,
        }
    } else {
        XuniWindowState::default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn models_the_55_to_05_server_window() {
        // minute 56 -> open, closes at :05 past the next hour.
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
    fn boundaries_at_hour_edges() {
        assert!(xuni_window_at(0).open);
        assert_eq!(xuni_window_at(0).ms_until_close, 5 * 60_000);
        let just_before_55 = 55 * 60_000 - 1;
        assert!(!xuni_window_at(just_before_55).open);
        let just_before_05 = 5 * 60_000 - 1;
        assert!(xuni_window_at(just_before_05).open);
        assert_eq!(xuni_window_at(just_before_05).ms_until_close, 1);
    }

    #[test]
    fn negative_epoch_uses_floor_div() {
        // One millisecond before Unix epoch is in the previous hour's :59, which is open.
        let w = xuni_window_at(-1);
        assert!(w.open);
        assert_eq!(w.ms_until_close, 1 + XUNI_OPEN_AFTER_HOUR_MS);
    }
}
