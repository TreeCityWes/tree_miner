// MarginPolicy.cpp — see src/treeminer/MarginPolicy.h for the rationale behind the ramp.

#include "treeminer/MarginPolicy.h"

#include <algorithm>
#include <cctype>

namespace treeminer {

namespace {

std::string lowered(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

std::uint32_t computeMargin(const MarginConfig& cfg, const MarginInputs& in) {
    switch (cfg.mode) {
        case MarginMode::Off:
            return 0;

        case MarginMode::Fixed:
            return cfg.margin_kib;

        case MarginMode::Auto: {
            // Healthy and drained: no headroom, no hashrate tax. This is the common case and
            // it must cost exactly nothing.
            if (!in.breaker_open && in.backlog == 0) {
                return 0;
            }
            // Degraded: one step immediately (a find made now cannot be submitted now), plus
            // one step per completed adjustment period the outage has lasted.
            std::uint64_t steps = 1;
            if (in.breaker_open && cfg.adjust_period_ms > 0 && in.outage_ms > 0) {
                steps += static_cast<std::uint64_t>(in.outage_ms / cfg.adjust_period_ms);
            }
            const std::uint64_t margin = static_cast<std::uint64_t>(cfg.margin_kib) * steps;
            return static_cast<std::uint32_t>(
                std::min<std::uint64_t>(margin, static_cast<std::uint64_t>(cfg.max_kib)));
        }
    }
    return 0;
}

bool parseMarginMode(const std::string& text, MarginMode& out) {
    const std::string t = lowered(text);
    if (t == "off" || t == "none" || t == "disabled") {
        out = MarginMode::Off;
        return true;
    }
    if (t == "fixed" || t == "static" || t == "constant") {
        out = MarginMode::Fixed;
        return true;
    }
    if (t == "auto" || t == "adaptive") {
        out = MarginMode::Auto;
        return true;
    }
    return false;
}

const char* to_string(MarginMode mode) {
    switch (mode) {
        case MarginMode::Off:   return "off";
        case MarginMode::Fixed: return "fixed";
        case MarginMode::Auto:  return "auto";
    }
    return "off";
}

}  // namespace treeminer
