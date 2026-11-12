#pragma once
// RollingRate — recent throughput from a monotonically increasing counter.
//
// The miner's per-stream hashrate was a since-start average (total hashes / uptime). After
// weeks of uptime that number cannot move: a stream that slows or stalls is invisible for
// days. RollingRate keeps a short window of (time, cumulative count) samples and reports
// the rate across that window, so a drop shows within the window length.
//
// Not thread-safe; the owner serializes access. Header-only so tests need no toolchain.

#include <cstdint>
#include <deque>
#include <optional>

namespace treeminer {

class RollingRate {
public:
    explicit RollingRate(std::int64_t window_ms = 60000, std::int64_t min_span_ms = 5000,
                         std::int64_t sample_interval_ms = 1000)
        : window_ms_(window_ms), min_span_ms_(min_span_ms),
          sample_interval_ms_(sample_interval_ms) {}

    // Record the cumulative count at `now_ms`. Calls closer together than the sample
    // interval only refresh the newest sample, so a per-batch caller stays O(window).
    void add(std::int64_t now_ms, std::uint64_t total) {
        if (!samples_.empty() && total < samples_.back().total) {
            samples_.clear();  // counter reset (stream restart): start a fresh window
        }
        if (samples_.size() >= 2 &&
            now_ms - samples_[samples_.size() - 2].t_ms < sample_interval_ms_) {
            samples_.back() = {now_ms, total};
        } else {
            samples_.push_back({now_ms, total});
        }
        while (samples_.size() > 2 && now_ms - samples_[1].t_ms >= window_ms_) {
            samples_.pop_front();
        }
    }

    // Units per second across the window, or nullopt until min_span_ms of history exists.
    std::optional<double> rate() const {
        if (samples_.size() < 2) {
            return std::nullopt;
        }
        const std::int64_t span = samples_.back().t_ms - samples_.front().t_ms;
        if (span < min_span_ms_) {
            return std::nullopt;
        }
        return static_cast<double>(samples_.back().total - samples_.front().total) * 1000.0 /
               static_cast<double>(span);
    }

    // Milliseconds since the newest sample, for staleness checks.
    std::optional<std::int64_t> ageMs(std::int64_t now_ms) const {
        if (samples_.empty()) {
            return std::nullopt;
        }
        return now_ms - samples_.back().t_ms;
    }

private:
    struct Sample {
        std::int64_t t_ms;
        std::uint64_t total;
    };
    std::int64_t window_ms_, min_span_ms_, sample_interval_ms_;
    std::deque<Sample> samples_;
};

}  // namespace treeminer
