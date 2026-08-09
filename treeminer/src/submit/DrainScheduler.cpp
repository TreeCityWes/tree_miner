#include "DrainScheduler.h"

#include <algorithm>

namespace treeminer {

DrainScheduler::DrainScheduler() : DrainScheduler(Config{}) {}

DrainScheduler::DrainScheduler(Config cfg) : cfg_(cfg), rate_per_s_(cfg.start_rate_per_s) {
    if (rate_per_s_ <= 0.0) {
        rate_per_s_ = 1.0;
    }
}

const FindRecord* DrainScheduler::selectNext(const std::vector<FindRecord>& eligible,
                                             DifficultyTrend trend,
                                             const XuniWindowState& window) const {
    const FindRecord* oldest_xuni = nullptr;   // journal order is oldest-first: first hit wins
    const FindRecord* best_xen11 = nullptr;

    for (const FindRecord& r : eligible) {
        if (r.payload.kind == FindKind::XUNI) {
            if (window.open && oldest_xuni == nullptr) {
                oldest_xuni = &r;
            }
            continue;
        }
        // XEN11
        if (best_xen11 == nullptr) {
            best_xen11 = &r;
        } else if (trend == DifficultyTrend::Rising &&
                   r.payload.memory_cost < best_xen11->payload.memory_cost) {
            // Rising difficulty: lowest-margin finds drain first, before the floor
            // climbs past their baked-in m (GROK §2.1). Oldest wins ties because the
            // journal slice is already oldest-first.
            best_xen11 = &r;
        }
    }

    // XUNI preemption: the window is open and closing soon — XEN11 can wait, XUNI cannot.
    if (oldest_xuni != nullptr && window.ms_until_close <= cfg_.xuni_preempt_window_ms) {
        return oldest_xuni;
    }
    if (best_xen11 != nullptr) {
        return best_xen11;
    }
    return oldest_xuni;  // window open but not near its end, and no XEN11 backlog
}

void DrainScheduler::onBreakerClose() {
    rate_per_s_ = cfg_.start_rate_per_s;
}

void DrainScheduler::onHealthyRoundTrip() {
    rate_per_s_ = std::min(rate_per_s_ * 2.0, cfg_.max_rate_per_s);
}

void DrainScheduler::onThrottle() {
    rate_per_s_ = std::max(rate_per_s_ / 2.0, cfg_.min_rate_per_s);
}

std::int64_t DrainScheduler::submitIntervalMs() const {
    return static_cast<std::int64_t>(1000.0 / rate_per_s_);
}

}  // namespace treeminer
