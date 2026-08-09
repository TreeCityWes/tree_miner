#pragma once
// DrainScheduler — pure ordering + pacing policy for draining the journal backlog
// (PLAN.md §3.2, §10.5-6; SOL-PLAN §7; GROK §2.1, §2.9). No I/O, no clock, no threads.
//
// Ordering (selectNext, pure function of its inputs):
//   1. XUNI preempts XEN11 when the (server-clock estimated) XUNI window is open and
//      near its end — a missed window costs the find, XEN11 can always wait.
//   2. Otherwise oldest eligible XEN11 first; when the difficulty trend is RISING,
//      ascending-m first (lowest margin drains before the floor rises past it), oldest
//      as the tie-break.
//   3. Remaining XUNI (window open, not near the end) after the XEN11 backlog.
//      XUNI with a closed window are never selected — submitting them only burns a
//      guaranteed 401.
//
// Pacing (adaptive drain, PLAN §10.6): start at 1/s when the breaker closes after an
// outage, double per healthy round-trip, halve on 5xx/429; the configured `drain_rate`
// is the ceiling. Never stampede a recovering server.

#include <cstdint>
#include <vector>

#include "../treeminer/Types.h"

namespace treeminer {

enum class DifficultyTrend { Unknown, Flat, Rising, Falling };

// Server-clock-adjusted view of the XUNI :55-:05 window (computed by the caller from the
// tracked HTTP Date offset).
struct XuniWindowState {
    bool open = false;
    std::int64_t ms_until_close = 0;  // meaningful only when open
};

class DrainScheduler {
public:
    struct Config {
        double start_rate_per_s = 1.0;   // rate right after the breaker closes
        double max_rate_per_s = 4.0;     // `drain_rate` config: the ceiling
        double min_rate_per_s = 0.25;    // floor under repeated 5xx/429
        std::int64_t xuni_preempt_window_ms = 120000;  // "near window end" threshold
    };

    explicit DrainScheduler(Config cfg = Config{});

    // Pick the next record to submit from the journal's oldest-first eligible slice.
    // Returns nullptr when nothing should be submitted now (empty, or only closed-window
    // XUNI remain). Pure: same inputs, same answer.
    const FindRecord* selectNext(const std::vector<FindRecord>& eligible,
                                 DifficultyTrend trend,
                                 const XuniWindowState& window) const;

    // --- adaptive pacing ---
    void onBreakerClose();      // reset to start_rate_per_s
    void onHealthyRoundTrip();  // x2, capped at max_rate_per_s
    void onThrottle();          // 5xx/429: /2, floored at min_rate_per_s
    double ratePerSecond() const { return rate_per_s_; }
    std::int64_t submitIntervalMs() const;

private:
    Config cfg_;
    double rate_per_s_;
};

}  // namespace treeminer
