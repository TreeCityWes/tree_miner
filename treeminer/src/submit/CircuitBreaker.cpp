#include "CircuitBreaker.h"

#include <algorithm>

namespace treeminer {

CircuitBreaker::CircuitBreaker(Config cfg, Clock clock, Jitter jitter)
    : cfg_(cfg), clock_(std::move(clock)), jitter_(std::move(jitter)) {
    if (!jitter_) {
        jitter_ = [] { return 0.0; };
    }
}

std::int64_t CircuitBreaker::activeCapMs_() const {
    return xuni_pressure_ ? std::min(cfg_.probe_cap_ms, cfg_.probe_cap_xuni_ms)
                          : cfg_.probe_cap_ms;
}

void CircuitBreaker::scheduleProbe_() {
    const std::int64_t cap = activeCapMs_();
    std::int64_t base = std::min(probe_interval_ms_, cap);
    std::int64_t delay =
        base + static_cast<std::int64_t>(jitter_() * cfg_.jitter_fraction *
                                         static_cast<double>(base));
    if (xuni_pressure_) {
        delay = std::min(delay, cap);  // hard cap while a XUNI window is at stake
    }
    next_probe_at_ms_ = clock_() + delay;
}

void CircuitBreaker::open_() {
    state_ = State::Open;
    admission_available_ = false;
    scheduleProbe_();
}

bool CircuitBreaker::probeDue() const {
    return state_ == State::Open && clock_() >= next_probe_at_ms_;
}

void CircuitBreaker::onProbeSuccess() {
    if (state_ != State::Open) {
        return;
    }
    state_ = State::HalfOpen;
    admission_available_ = true;
}

void CircuitBreaker::onProbeFailure() {
    if (state_ != State::Open) {
        return;
    }
    probe_interval_ms_ = std::min(probe_interval_ms_ * 2, cfg_.probe_cap_ms);
    scheduleProbe_();
}

bool CircuitBreaker::tryAdmit() {
    switch (state_) {
        case State::Closed:
            return true;
        case State::HalfOpen:
            if (admission_available_) {
                admission_available_ = false;
                return true;
            }
            return false;
        case State::Open:
        default:
            return false;
    }
}

void CircuitBreaker::onVerifySuccess() {
    state_ = State::Closed;
    consecutive_failures_ = 0;
    probe_interval_ms_ = cfg_.probe_base_ms;
    admission_available_ = false;
}

void CircuitBreaker::onVerifyTransportFailure() {
    if (state_ == State::HalfOpen) {
        // The half-open probe submission failed: reopen with an escalated interval.
        probe_interval_ms_ = std::min(std::max(probe_interval_ms_, cfg_.probe_base_ms) * 2,
                                      cfg_.probe_cap_ms);
        open_();
        return;
    }
    if (state_ == State::Closed) {
        if (++consecutive_failures_ >= cfg_.failure_threshold) {
            probe_interval_ms_ = cfg_.probe_base_ms;
            open_();
        }
    }
    // State::Open: nothing to do — no /verify traffic should be flowing.
}

void CircuitBreaker::onVerifyInconclusive() {
    // A parsed, conclusive server response (difficulty park, quarantine, ...) proves the
    // transport and the /verify handler are alive — but only real acceptance closes the
    // breaker from HALF_OPEN (SOL-PLAN §6).
    consecutive_failures_ = 0;
    if (state_ == State::HalfOpen) {
        admission_available_ = true;  // keep draining, one admission at a time
    }
}

void CircuitBreaker::setXuniPressure(bool eligible_xuni_exists) {
    const bool was = xuni_pressure_;
    xuni_pressure_ = eligible_xuni_exists;
    if (!was && xuni_pressure_ && state_ == State::Open) {
        // Pull in a far-future probe so the outage backoff cannot eat the window.
        const std::int64_t latest = clock_() + activeCapMs_();
        next_probe_at_ms_ = std::min(next_probe_at_ms_, latest);
    }
}

}  // namespace treeminer
