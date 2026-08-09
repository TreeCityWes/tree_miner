#pragma once
// CircuitBreaker — protects the /verify path during server outages (PLAN.md §3.2, §10.6;
// SOL-PLAN §6). States:
//
//   CLOSED    normal operation; opens after `failure_threshold` CONSECUTIVE transport/5xx
//             failures on the /verify path.
//   OPEN      no /verify attempts. The owner probes GET /difficulty when probeDue():
//             first probe after 5 s, then x2 + jitter per failed probe, capped at 60 s
//             (capped at ~5 s while an eligible XUNI exists, so an outage backoff cannot
//             consume the remainder of a submission window).
//   HALF_OPEN entered on a successful /difficulty probe. Admits exactly ONE real queued
//             submission. Closes ONLY on a verification-path success or a conclusive
//             duplicate (onVerifySuccess); a transport/5xx failure reopens with an
//             escalated probe interval; a conclusive non-success (401/4xx classification)
//             keeps it HALF_OPEN and releases the admission slot — the transport is
//             healthy, but only real acceptance proves the /verify path.
//
// /difficulty health is deliberately separate from /verify health: a good /difficulty
// response proves connectivity, not that /verify and its database work.
//
// The clock is injectable (monotonic milliseconds) so every transition is unit-testable
// without sleeping. Jitter is injectable for the same reason. Not thread-safe by itself;
// the SubmissionManager drives it from a single thread.

#include <cstdint>
#include <functional>

namespace treeminer {

class CircuitBreaker {
public:
    enum class State { Closed, Open, HalfOpen };

    struct Config {
        int failure_threshold = 3;             // consecutive transport/5xx failures to open
        std::int64_t probe_base_ms = 5000;     // first probe delay after opening
        std::int64_t probe_cap_ms = 60000;     // normal probe-interval ceiling
        std::int64_t probe_cap_xuni_ms = 5000; // ceiling while an eligible XUNI exists
        double jitter_fraction = 0.2;          // adds up to +20% of the interval
    };

    using Clock = std::function<std::int64_t()>;   // monotonic milliseconds
    using Jitter = std::function<double()>;        // uniform [0, 1)

    // `jitter` may be null: a deterministic zero-jitter source is used (tests) unless a
    // real one is supplied (production passes a seeded uniform generator).
    CircuitBreaker(Config cfg, Clock clock, Jitter jitter = nullptr);

    State state() const { return state_; }
    int consecutiveFailures() const { return consecutive_failures_; }

    // --- OPEN: /difficulty probe scheduling ---
    bool probeDue() const;                 // true only in OPEN when the probe time arrived
    std::int64_t nextProbeAtMs() const { return next_probe_at_ms_; }
    void onProbeSuccess();                 // OPEN -> HALF_OPEN (one admission granted)
    void onProbeFailure();                 // stay OPEN; escalate the probe interval

    // --- submission admission ---
    // CLOSED: always true. HALF_OPEN: true exactly once until the outcome is reported.
    // OPEN: always false.
    bool tryAdmit();

    // --- /verify outcomes ---
    void onVerifySuccess();           // verification success or conclusive duplicate -> CLOSED
    void onVerifyTransportFailure();  // transport/5xx: counts toward opening; HALF_OPEN -> OPEN
    void onVerifyInconclusive();      // conclusive non-success (401/4xx): healthy transport,
                                      // resets the failure streak, but never closes from HALF_OPEN

    // While an eligible XUNI exists the probe ceiling drops to probe_cap_xuni_ms; an already
    // scheduled far-future probe is pulled in when the flag turns on.
    void setXuniPressure(bool eligible_xuni_exists);

private:
    void open_();
    void scheduleProbe_();
    std::int64_t activeCapMs_() const;

    Config cfg_;
    Clock clock_;
    Jitter jitter_;
    State state_ = State::Closed;
    int consecutive_failures_ = 0;
    std::int64_t probe_interval_ms_ = 0;
    std::int64_t next_probe_at_ms_ = 0;
    bool admission_available_ = false;  // HALF_OPEN single-admission latch
    bool xuni_pressure_ = false;
};

}  // namespace treeminer
